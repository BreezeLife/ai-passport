"""Server-only client for the 2026-09-03 WorkBuddy Open Platform contract."""

from __future__ import annotations

import hashlib
import http.client
import json
import queue
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Callable, Dict, List, Optional, Tuple

from .errors import GatewayError
from .models import (
    MAX_ACTION_TEXT_BYTES,
    MAX_COLLECTION_ITEMS,
    ArtifactSummary,
    MessageSummary,
    Snapshot,
    TaskSummary,
    require_identifier,
    require_text,
)


MAX_UPSTREAM_RESPONSE_BYTES = 64 * 1024
MAX_TOKEN_RESPONSE_BYTES = 16 * 1024
DEFAULT_SNAPSHOT_TIMEOUT_SECONDS = 10.0
DEFAULT_ARTIFACT_CACHE_SECONDS = 90.0
DEFAULT_MAX_SNAPSHOT_CALLS = 7
READ_CHUNK_BYTES = 8192
DNS_RESOLVER_WORKERS = 2
DNS_RESOLVER_QUEUE_SIZE = 2


class _UpstreamCallBudget:
    """Shared per-snapshot budget charged immediately before every HTTP attempt."""

    def __init__(self, maximum: int) -> None:
        self._remaining = maximum
        self._lock = threading.Lock()

    @property
    def remaining(self) -> int:
        with self._lock:
            return self._remaining

    def consume(self) -> None:
        with self._lock:
            if self._remaining <= 0:
                raise GatewayError(
                    "upstream_budget",
                    504,
                    "WorkBuddy snapshot exceeded its request budget",
                    True,
                    safe_to_retry_operation=True,
                )
            self._remaining -= 1


def _deadline_timeout(deadline: Optional[float], maximum: float) -> float:
    if deadline is None:
        return maximum
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise GatewayError(
            "upstream_deadline",
            504,
            "WorkBuddy snapshot exceeded its time limit",
            True,
            safe_to_retry_operation=True,
        )
    return min(maximum, remaining)


def _read_bounded_response(
    response: Any,
    maximum: int,
    *,
    deadline: Optional[float],
    timeout_seconds: float,
) -> bytes:
    """Read incrementally so a trickling peer cannot extend a wall-clock deadline."""

    chunks: List[bytes] = []
    size = 0
    while size <= maximum:
        chunk = _read_response_chunk(
            response,
            min(READ_CHUNK_BYTES, maximum + 1 - size),
            deadline=deadline,
            timeout_seconds=timeout_seconds,
        )
        if not chunk:
            break
        chunks.append(chunk)
        size += len(chunk)
    return b"".join(chunks)


def _read_response_chunk(
    response: Any,
    maximum: int,
    *,
    deadline: Optional[float],
    timeout_seconds: float,
) -> bytes:
    timeout = _deadline_timeout(deadline, timeout_seconds)
    raw_stream = getattr(getattr(response, "fp", None), "raw", None)
    response_socket = getattr(raw_stream, "_sock", None)
    if response_socket is not None:
        response_socket.settimeout(timeout)
    reader = getattr(response, "read1", response.read)
    try:
        chunk = reader(maximum)
    except (socket.timeout, TimeoutError):
        if deadline is not None and time.monotonic() >= deadline:
            _deadline_timeout(deadline, timeout_seconds)
        raise
    if deadline is not None and time.monotonic() >= deadline:
        _deadline_timeout(deadline, timeout_seconds)
    return chunk


def _validate_remote_url(url: str, label: str, allow_insecure_http: bool) -> urllib.parse.SplitResult:
    if not isinstance(url, str):
        raise GatewayError("invalid_configuration", 500, "%s is invalid" % label, False)
    parsed = urllib.parse.urlsplit(url)
    allowed_http = allow_insecure_http and parsed.scheme == "http"
    if (
        (parsed.scheme != "https" and not allowed_http)
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.fragment
    ):
        raise GatewayError(
            "invalid_configuration",
            500,
            "%s must be HTTPS without embedded credentials" % label,
            False,
        )
    return parsed


class _ResolveJob:
    """One bounded DNS request shared with the fixed resolver workers."""

    def __init__(self, host: str, port: int, deadline: float) -> None:
        self.host = host
        self.port = port
        self.deadline = deadline
        self.finished = threading.Event()
        self.result: Optional[List[Tuple[Any, ...]]] = None
        self.error: Optional[Exception] = None
        self._cancelled = False
        self._lock = threading.Lock()

    def can_start(self) -> bool:
        with self._lock:
            if self._cancelled or time.monotonic() >= self.deadline:
                self._cancelled = True
                self.finished.set()
                return False
            return True

    def complete(
        self,
        result: Optional[List[Tuple[Any, ...]]],
        error: Optional[Exception],
    ) -> None:
        with self._lock:
            if not self._cancelled:
                self.result = result
                self.error = error
            self.finished.set()

    def cancel(self) -> None:
        with self._lock:
            self._cancelled = True
            self.finished.set()


class _BoundedResolver:
    """Fixed daemon workers keep blocking system DNS from escaping deadlines.

    Python's synchronous resolver cannot be interrupted portably. Keeping a
    fixed worker count and a bounded queue lets callers stop at their deadline
    without creating an unbounded number of abandoned threads or jobs.
    """

    def __init__(self, worker_count: int, queue_size: int) -> None:
        self._jobs: "queue.Queue[_ResolveJob]" = queue.Queue(maxsize=queue_size)
        self._workers: Tuple[threading.Thread, ...] = tuple(
            threading.Thread(
                target=self._run,
                name="workbuddy-dns-%d" % (index + 1),
                daemon=True,
            )
            for index in range(worker_count)
        )
        for worker in self._workers:
            worker.start()

    def resolve(self, host: str, port: int, timeout: float) -> List[Tuple[Any, ...]]:
        if timeout <= 0:
            raise socket.timeout("DNS resolution deadline expired")
        job = _ResolveJob(host, port, time.monotonic() + timeout)
        try:
            self._jobs.put_nowait(job)
        except queue.Full:
            raise socket.timeout("DNS resolver capacity is exhausted")
        if not job.finished.wait(timeout):
            job.cancel()
            raise socket.timeout("DNS resolution timed out")
        if job.error is not None:
            raise job.error
        if job.result is None:
            raise socket.timeout("DNS resolution timed out")
        return job.result

    def _run(self) -> None:
        while True:
            job = self._jobs.get()
            try:
                if not job.can_start():
                    continue
                try:
                    result = socket.getaddrinfo(
                        job.host,
                        job.port,
                        0,
                        socket.SOCK_STREAM,
                    )
                except Exception as error:
                    job.complete(None, error)
                else:
                    job.complete(result, None)
            finally:
                self._jobs.task_done()


_DNS_RESOLVER = _BoundedResolver(DNS_RESOLVER_WORKERS, DNS_RESOLVER_QUEUE_SIZE)


def _bounded_create_connection(
    address: Tuple[str, int],
    timeout: Any = socket._GLOBAL_DEFAULT_TIMEOUT,
    source_address: Optional[Tuple[str, int]] = None,
) -> socket.socket:
    """Resolve and connect within one timeout, without synchronous DNS."""

    if timeout is socket._GLOBAL_DEFAULT_TIMEOUT:
        timeout_seconds = 15.0
    else:
        timeout_seconds = float(timeout)
    if timeout_seconds <= 0:
        raise socket.timeout("connection deadline expired")
    deadline = time.monotonic() + timeout_seconds
    host, port = address
    addresses = _DNS_RESOLVER.resolve(host, port, timeout_seconds)
    last_error: Optional[OSError] = None
    for family, socktype, protocol, _canonical_name, socket_address in addresses:
        connection: Optional[socket.socket] = None
        try:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout("connection deadline expired")
            connection = socket.socket(family, socktype, protocol)
            connection.settimeout(remaining)
            if source_address:
                connection.bind(source_address)
            connection.connect(socket_address)
            return connection
        except OSError as error:
            last_error = error
            if connection is not None:
                connection.close()
    if last_error is not None:
        raise last_error
    raise OSError("DNS resolution returned no addresses")


class _BoundedHTTPConnection(http.client.HTTPConnection):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self._create_connection = _bounded_create_connection


class _BoundedHTTPSConnection(http.client.HTTPSConnection):
    def __init__(self, *args: Any, **kwargs: Any) -> None:
        super().__init__(*args, **kwargs)
        self._create_connection = _bounded_create_connection


class _BoundedHTTPHandler(urllib.request.HTTPHandler):
    def http_open(self, request: urllib.request.Request) -> Any:
        return self.do_open(_BoundedHTTPConnection, request)


class _BoundedHTTPSHandler(urllib.request.HTTPSHandler):
    def https_open(self, request: urllib.request.Request) -> Any:
        return self.do_open(
            _BoundedHTTPSConnection,
            request,
            context=self._context,
            check_hostname=self._check_hostname,
        )


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args: Any, **kwargs: Any) -> None:
        del args, kwargs
        return None


def _build_bounded_opener() -> urllib.request.OpenerDirector:
    return urllib.request.build_opener(
        _NoRedirectHandler(),
        _BoundedHTTPHandler(),
        _BoundedHTTPSHandler(),
    )


class OAuthTokenProvider:
    """Refreshes a server-side WorkBuddy access token using its declared lifetime."""

    def __init__(
        self,
        token_url: str,
        client_id: str,
        client_secret: str,
        refresh_token: str,
        *,
        clock: Callable[[], float] = time.time,
        timeout_seconds: float = 15.0,
        allow_insecure_http: bool = False,
        on_refresh_token: Optional[Callable[[str], None]] = None,
    ) -> None:
        _validate_remote_url(token_url, "WorkBuddy token URL", allow_insecure_http)
        if not all(isinstance(value, str) and value for value in (client_id, client_secret, refresh_token)):
            raise GatewayError(
                "invalid_configuration", 500, "WorkBuddy refresh credentials are incomplete", False
            )
        self._token_url = token_url
        self._client_id = client_id
        self._client_secret = client_secret
        self._refresh_token = refresh_token
        self._clock = clock
        self._timeout_seconds = timeout_seconds
        if on_refresh_token is not None and not callable(on_refresh_token):
            raise GatewayError(
                "invalid_configuration",
                500,
                "WorkBuddy refresh-token persistence is invalid",
                False,
            )
        self._on_refresh_token = on_refresh_token
        self._access_token: Optional[str] = None
        self._expires_at = 0.0
        self._lock = threading.Lock()
        self._opener = _build_bounded_opener()

    def get_access_token(
        self,
        *,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> str:
        with self._lock:
            if self._access_token is not None and self._clock() < self._expires_at - 60.0:
                return self._access_token
            self._refresh(deadline, request_budget)
            assert self._access_token is not None
            return self._access_token

    def invalidate(self) -> None:
        with self._lock:
            self._expires_at = 0.0

    def _refresh(
        self,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> None:
        encoded = urllib.parse.urlencode(
            {
                "grant_type": "refresh_token",
                "refresh_token": self._refresh_token,
                "client_id": self._client_id,
                "client_secret": self._client_secret,
            }
        ).encode("ascii")
        request = urllib.request.Request(
            self._token_url,
            data=encoded,
            method="POST",
            headers={
                "Content-Type": "application/x-www-form-urlencoded",
                "Accept": "application/json",
            },
        )
        try:
            timeout = _deadline_timeout(deadline, self._timeout_seconds)
            if request_budget is not None:
                request_budget.consume()
            response = self._opener.open(
                request,
                timeout=timeout,
            )
            with response:
                raw = _read_bounded_response(
                    response,
                    MAX_TOKEN_RESPONSE_BYTES,
                    deadline=deadline,
                    timeout_seconds=self._timeout_seconds,
                )
        except urllib.error.HTTPError as error:
            error.close()
            raise GatewayError(
                "workbuddy_auth_failed",
                502,
                "WorkBuddy authorization failed",
                True,
                safe_to_retry_operation=True,
            )
        except (urllib.error.URLError, socket.timeout, TimeoutError):
            if deadline is not None and time.monotonic() >= deadline:
                _deadline_timeout(deadline, self._timeout_seconds)
            raise GatewayError(
                "workbuddy_auth_failed",
                502,
                "WorkBuddy authorization failed",
                True,
                safe_to_retry_operation=True,
            )
        if len(raw) > MAX_TOKEN_RESPONSE_BYTES:
            raise GatewayError(
                "workbuddy_auth_failed",
                502,
                "WorkBuddy authorization response was invalid",
                True,
                safe_to_retry_operation=True,
            )
        try:
            payload = json.loads(raw.decode("utf-8", "strict"))
        except (UnicodeError, ValueError):
            raise GatewayError(
                "workbuddy_auth_failed",
                502,
                "WorkBuddy authorization response was invalid",
                True,
                safe_to_retry_operation=True,
            )
        if not isinstance(payload, dict):
            raise GatewayError(
                "workbuddy_auth_failed",
                502,
                "WorkBuddy authorization response was invalid",
                True,
                safe_to_retry_operation=True,
            )
        access_token = payload.get("access_token")
        expires_in = payload.get("expires_in")
        token_type = payload.get("token_type", "Bearer")
        if (
            not isinstance(access_token, str)
            or not access_token
            or len(access_token) > 8192
            or isinstance(expires_in, bool)
            or not isinstance(expires_in, (int, float))
            or expires_in <= 60
            or str(token_type).lower() != "bearer"
        ):
            raise GatewayError(
                "workbuddy_auth_failed",
                502,
                "WorkBuddy authorization response was invalid",
                True,
                safe_to_retry_operation=True,
            )
        rotated_refresh = payload.get("refresh_token")
        if isinstance(rotated_refresh, str) and rotated_refresh:
            if self._on_refresh_token is not None:
                self._on_refresh_token(rotated_refresh)
            self._refresh_token = rotated_refresh
        self._access_token = access_token
        self._expires_at = self._clock() + float(expires_in)

    def __repr__(self) -> str:
        return "OAuthTokenProvider(url=%r, credentials=<redacted>)" % self._token_url


def resolve_artifact_endpoint(
    link: str,
    task_id: str,
    limit: int,
    *,
    allow_insecure_http: bool = False,
) -> str:
    """Derive the documented artifact URL only from an exact `/acp` suffix.

    WorkBuddy examples have historically disagreed about the shape of `link`.
    Guessing from sandboxLink/sandboxDataLink could send a task ticket to an
    unintended host, so ambiguous links fail closed.
    """

    parsed = _validate_remote_url(link, "WorkBuddy ACP link", allow_insecure_http)
    clean_task_id = require_identifier(task_id, "task_id")
    if not parsed.path.endswith("/acp"):
        raise GatewayError(
            "artifact_unavailable",
            424,
            "Task output location is not available in a verified format",
            True,
        )
    if limit < 1 or limit > MAX_COLLECTION_ITEMS:
        raise GatewayError("invalid_request", 400, "Artifact limit is invalid", False)
    query = urllib.parse.parse_qs(parsed.query, keep_blank_values=False)
    session_values = query.get("sessionId")
    session_id = session_values[0] if session_values else clean_task_id
    session_id = require_identifier(session_id, "session_id")
    base_path = parsed.path[: -len("/acp")]
    artifact_path = base_path.rstrip("/") + "/api/session/artifacts"
    return urllib.parse.urlunsplit(
        (
            parsed.scheme,
            parsed.netloc,
            artifact_path,
            urllib.parse.urlencode({"sessionId": session_id, "limit": limit}),
            "",
        )
    )


class WorkBuddyClient:
    def __init__(
        self,
        base_url: str,
        access_token: Optional[str] = None,
        *,
        token_provider: Optional[OAuthTokenProvider] = None,
        timeout_seconds: float = 15.0,
        max_response_bytes: int = MAX_UPSTREAM_RESPONSE_BYTES,
        allow_insecure_http: bool = False,
    ) -> None:
        parsed = _validate_remote_url(base_url, "WorkBuddy base URL", allow_insecure_http)
        if parsed.query or parsed.path not in ("", "/"):
            raise GatewayError(
                "invalid_configuration", 500, "WorkBuddy base URL must not contain a path", False
            )
        if bool(access_token) == bool(token_provider):
            raise GatewayError(
                "invalid_configuration",
                500,
                "Configure exactly one WorkBuddy token source",
                False,
            )
        if timeout_seconds <= 0 or max_response_bytes < 32:
            raise GatewayError(
                "invalid_configuration", 500, "WorkBuddy client limits are invalid", False
            )
        self._base_url = base_url.rstrip("/")
        self._access_token = access_token
        self._token_provider = token_provider
        self._timeout_seconds = timeout_seconds
        self._max_response_bytes = max_response_bytes
        self._allow_insecure_http = allow_insecure_http
        self._opener = _build_bounded_opener()

    def list_messages(
        self,
        after_message_id: Optional[str] = None,
        limit: int = MAX_COLLECTION_ITEMS,
        *,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> Dict[str, Any]:
        if limit < 1 or limit > 100:
            raise GatewayError("invalid_request", 400, "Message limit is invalid", False)
        query: List[Tuple[str, Any]] = []
        if after_message_id is not None:
            query.append(("message_id", require_identifier(after_message_id, "message_id")))
        query.append(("limit", limit))
        payload = self._request_json(
            "GET",
            "/openapi/v2/localassistant/message",
            query=query,
            deadline=deadline,
            request_budget=request_budget,
        )
        data = self._unwrap_message_envelope(payload)
        if not isinstance(data.get("messages"), list):
            raise GatewayError(
                "upstream_schema", 502, "WorkBuddy returned an invalid message list", True
            )
        return data

    def local_assistant_online(
        self,
        *,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> bool:
        payload = self._request_json(
            "GET",
            "/openapi/v2/localassistant",
            deadline=deadline,
            request_budget=request_budget,
        )
        data = self._unwrap_message_envelope(payload)
        online = data.get("online")
        if type(online) is not bool:
            raise GatewayError(
                "upstream_schema",
                502,
                "WorkBuddy returned an invalid assistant status",
                True,
            )
        return online

    def send_message(self, content: str) -> str:
        clean_content = require_text(content, "content", MAX_ACTION_TEXT_BYTES)
        payload = self._request_json(
            "POST",
            "/openapi/v2/localassistant/message",
            body={"content": clean_content, "msg_type": "text"},
        )
        data = self._unwrap_message_envelope(payload)
        return require_identifier(data.get("message_id"), "message_id")

    def list_tasks(
        self,
        page: int = 1,
        size: int = MAX_COLLECTION_ITEMS,
        *,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> Dict[str, Any]:
        if isinstance(page, bool) or not isinstance(page, int) or page < 1:
            raise GatewayError("invalid_request", 400, "Task page is invalid", False)
        if isinstance(size, bool) or not isinstance(size, int) or size < 1 or size > 100:
            raise GatewayError("invalid_request", 400, "Task page size is invalid", False)
        payload = self._request_json(
            "GET",
            "/openapi/v2/tasks",
            query=(("page", page), ("size", size)),
            deadline=deadline,
            request_budget=request_budget,
        )
        if not isinstance(payload.get("tasks"), list):
            raise GatewayError(
                "upstream_schema", 502, "WorkBuddy returned an invalid task list", True
            )
        return payload

    def create_task(self, prompt: str, name: Optional[str] = None) -> Dict[str, Any]:
        clean_prompt = require_text(prompt, "prompt", MAX_ACTION_TEXT_BYTES)
        body: Dict[str, Any] = {"prompt": clean_prompt}
        if name is not None:
            body["name"] = require_text(name, "name", 128)
        payload = self._request_json("POST", "/openapi/v2/tasks", body=body)
        require_identifier(payload.get("task_id"), "task_id")
        if not isinstance(payload.get("status"), str):
            raise GatewayError(
                "upstream_schema", 502, "WorkBuddy returned an invalid created task", True
            )
        return payload

    def get_task(
        self,
        task_id: str,
        *,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> Dict[str, Any]:
        clean_id = require_identifier(task_id, "task_id")
        payload = self._request_json(
            "GET",
            "/openapi/v2/tasks/" + urllib.parse.quote(clean_id, safe=""),
            deadline=deadline,
            request_budget=request_budget,
        )
        if payload.get("task_id") != clean_id or not isinstance(payload.get("status"), str):
            raise GatewayError(
                "upstream_schema", 502, "WorkBuddy returned an invalid task detail", True
            )
        return payload

    def list_artifacts(
        self,
        link: str,
        task_id: str,
        task_ticket: str,
        limit: int = MAX_COLLECTION_ITEMS,
        *,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> List[Dict[str, Any]]:
        if not isinstance(task_ticket, str) or not task_ticket:
            raise GatewayError(
                "artifact_unavailable", 424, "Task output authorization is unavailable", True
            )
        url = resolve_artifact_endpoint(
            link,
            task_id,
            limit,
            allow_insecure_http=self._allow_insecure_http,
        )
        payload = self._request_json(
            "GET",
            absolute_url=url,
            bearer_token=task_ticket,
            deadline=deadline,
            request_budget=request_budget,
        )
        data = payload.get("data")
        if not isinstance(data, dict) or not isinstance(data.get("artifacts"), list):
            raise GatewayError(
                "upstream_schema", 502, "WorkBuddy returned an invalid artifact list", True
            )
        return data["artifacts"][:limit]

    def _request_json(
        self,
        method: str,
        path: Optional[str] = None,
        *,
        query: Optional[Any] = None,
        body: Optional[Dict[str, Any]] = None,
        absolute_url: Optional[str] = None,
        bearer_token: Optional[str] = None,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> Dict[str, Any]:
        if absolute_url is not None:
            _validate_remote_url(
                absolute_url,
                "WorkBuddy artifact URL",
                self._allow_insecure_http,
            )
            url = absolute_url
        else:
            assert path is not None and path.startswith("/openapi/v2/")
            url = self._base_url + path
            if query:
                url += "?" + urllib.parse.urlencode(query)
        encoded: Optional[bytes] = None
        headers = {"Accept": "application/json"}
        if body is not None:
            encoded = json.dumps(
                body, ensure_ascii=False, separators=(",", ":")
            ).encode("utf-8")
            headers["Content-Type"] = "application/json"

        may_refresh = bearer_token is None and self._token_provider is not None
        for attempt in range(2):
            token = bearer_token or self._current_access_token(deadline, request_budget)
            request_headers = dict(headers)
            request_headers["Authorization"] = "Bearer " + token
            request = urllib.request.Request(
                url, data=encoded, method=method, headers=request_headers
            )
            try:
                timeout = _deadline_timeout(deadline, self._timeout_seconds)
                if request_budget is not None:
                    request_budget.consume()
                response = self._opener.open(
                    request,
                    timeout=timeout,
                )
                with response:
                    declared = response.headers.get("Content-Length")
                    if declared is not None:
                        try:
                            declared_size = int(declared)
                        except (TypeError, ValueError):
                            raise GatewayError(
                                "upstream_schema",
                                502,
                                "WorkBuddy returned an invalid response length",
                                True,
                            )
                        if declared_size < 0 or declared_size > self._max_response_bytes:
                            raise GatewayError(
                                "upstream_too_large",
                                502,
                                "WorkBuddy response exceeded the gateway limit",
                                True,
                            )
                    raw = _read_bounded_response(
                        response,
                        self._max_response_bytes,
                        deadline=deadline,
                        timeout_seconds=self._timeout_seconds,
                    )
                break
            except urllib.error.HTTPError as error:
                status = error.code
                error.close()
                if status == 401 and may_refresh and attempt == 0:
                    assert self._token_provider is not None
                    self._token_provider.invalidate()
                    continue
                retryable = status == 429 or status >= 500
                raise GatewayError(
                    "upstream_rejected",
                    502,
                    "WorkBuddy rejected the gateway request",
                    retryable,
                    safe_to_retry_operation=(
                        method in ("GET", "HEAD")
                        and status in (401, 408, 409, 425, 429)
                    ),
                )
            except GatewayError:
                raise
            except (urllib.error.URLError, socket.timeout, TimeoutError, OSError, ValueError):
                if deadline is not None and time.monotonic() >= deadline:
                    _deadline_timeout(deadline, self._timeout_seconds)
                raise GatewayError(
                    "upstream_unavailable",
                    502,
                    "WorkBuddy could not be reached",
                    True,
                    safe_to_retry_operation=method in ("GET", "HEAD"),
                )
        else:
            raise GatewayError(
                "upstream_rejected",
                502,
                "WorkBuddy authorization failed",
                True,
                safe_to_retry_operation=True,
            )

        if len(raw) > self._max_response_bytes:
            raise GatewayError(
                "upstream_too_large",
                502,
                "WorkBuddy response exceeded the gateway limit",
                True,
            )
        try:
            payload = json.loads(raw.decode("utf-8", "strict"))
        except (UnicodeError, ValueError):
            raise GatewayError(
                "upstream_schema", 502, "WorkBuddy returned invalid JSON", True
            )
        if not isinstance(payload, dict):
            raise GatewayError(
                "upstream_schema", 502, "WorkBuddy returned an invalid response", True
            )
        return payload

    def _current_access_token(
        self,
        deadline: Optional[float] = None,
        request_budget: Optional[_UpstreamCallBudget] = None,
    ) -> str:
        if self._token_provider is not None:
            return self._token_provider.get_access_token(
                deadline=deadline,
                request_budget=request_budget,
            )
        assert self._access_token is not None
        return self._access_token

    @staticmethod
    def _unwrap_message_envelope(payload: Dict[str, Any]) -> Dict[str, Any]:
        if payload.get("code") != 0 or not isinstance(payload.get("data"), dict):
            raise GatewayError(
                "upstream_rejected", 502, "WorkBuddy message request failed", True
            )
        return payload["data"]

    def __repr__(self) -> str:
        return "WorkBuddyClient(base_url=%r, token=<redacted>)" % self._base_url


class LiveWorkBuddyAdapter:
    initially_ready = False

    def __init__(
        self,
        client: WorkBuddyClient,
        *,
        clock: Callable[[], float] = time.monotonic,
        snapshot_timeout_seconds: float = DEFAULT_SNAPSHOT_TIMEOUT_SECONDS,
        artifact_cache_seconds: float = DEFAULT_ARTIFACT_CACHE_SECONDS,
        max_snapshot_calls: int = DEFAULT_MAX_SNAPSHOT_CALLS,
    ) -> None:
        if snapshot_timeout_seconds <= 0 or artifact_cache_seconds < 0:
            raise ValueError("snapshot timing limits are invalid")
        if max_snapshot_calls < 3 or max_snapshot_calls > 64:
            raise ValueError("snapshot call budget is invalid")
        self._client = client
        self._clock = clock
        self._snapshot_timeout_seconds = snapshot_timeout_seconds
        self._artifact_cache_seconds = artifact_cache_seconds
        self._max_snapshot_calls = max_snapshot_calls
        self._lock = threading.RLock()
        self._messages: List[MessageSummary] = []
        self._last_message_id: Optional[str] = None
        self._artifact_cache: Dict[str, Tuple[float, Tuple[ArtifactSummary, ...]]] = {}
        self._artifact_scan_offset = 0

    def fetch_snapshot(self, after_cursor: Optional[str] = None) -> Snapshot:
        del after_cursor
        deadline = self._clock() + self._snapshot_timeout_seconds
        remaining = deadline - self._clock()
        if remaining <= 0 or not self._lock.acquire(timeout=remaining):
            raise GatewayError(
                "upstream_deadline",
                504,
                "WorkBuddy snapshot exceeded its time limit",
                True,
                safe_to_retry_operation=True,
            )
        try:
            if self._clock() >= deadline:
                raise GatewayError(
                    "upstream_deadline",
                    504,
                    "WorkBuddy snapshot exceeded its time limit",
                    True,
                    safe_to_retry_operation=True,
                )
            return self._fetch_snapshot_locked(deadline)
        finally:
            self._lock.release()

    def _fetch_snapshot_locked(self, deadline: float) -> Snapshot:
        request_budget = _UpstreamCallBudget(self._max_snapshot_calls)
        assistant_available = self._client.local_assistant_online(
            deadline=deadline,
            request_budget=request_budget,
        )
        message_data = self._client.list_messages(
            self._last_message_id,
            MAX_COLLECTION_ITEMS,
            deadline=deadline,
            request_budget=request_budget,
        )
        task_data = self._client.list_tasks(
            page=1,
            size=MAX_COLLECTION_ITEMS,
            deadline=deadline,
            request_budget=request_budget,
        )
        raw_messages = message_data["messages"]
        raw_tasks = task_data["tasks"]

        incoming_messages: List[MessageSummary] = []
        for raw in raw_messages[:MAX_COLLECTION_ITEMS]:
            if not isinstance(raw, dict):
                raise GatewayError(
                    "upstream_schema", 502, "WorkBuddy returned an invalid message", True
                )
            content = raw.get("content")
            if not isinstance(content, list):
                raise GatewayError(
                    "upstream_schema", 502, "WorkBuddy message content was not an array", True
                )
            text_parts: List[str] = []
            for block in content:
                if isinstance(block, str):
                    text_parts.append(block)
                    continue
                if not isinstance(block, dict):
                    raise GatewayError(
                        "upstream_schema", 502, "WorkBuddy returned an invalid content block", True
                    )
                if block.get("type") == "text":
                    text = block.get("text")
                    if not isinstance(text, str):
                        raise GatewayError(
                            "upstream_schema", 502, "WorkBuddy returned invalid text content", True
                        )
                    text_parts.append(text)
            role = raw.get("role", "assistant")
            is_read = raw.get("is_read")
            unread = not is_read if isinstance(is_read, bool) else role == "assistant"
            message = MessageSummary.create(
                raw.get("message_id"), role, "\n".join(text_parts), unread
            )
            incoming_messages.append(message)

        if incoming_messages:
            merged: Dict[str, MessageSummary] = {
                message.message_id: message for message in self._messages
            }
            for message in incoming_messages:
                merged[message.message_id] = message
            self._messages = list(merged.values())[-MAX_COLLECTION_ITEMS:]
            self._last_message_id = incoming_messages[-1].message_id

        tasks: List[TaskSummary] = []
        for raw in raw_tasks[:MAX_COLLECTION_ITEMS]:
            if not isinstance(raw, dict):
                raise GatewayError(
                    "upstream_schema", 502, "WorkBuddy returned an invalid task", True
                )
            task = TaskSummary.create(
                raw.get("task_id"),
                raw.get("name", "Untitled task"),
                raw.get("status"),
                raw.get("description", ""),
            )
            if task.status == "UNKNOWN":
                continue
            tasks.append(task)

        artifacts = self._refresh_artifacts(tasks, deadline, request_budget)
        cursor = self._snapshot_cursor(
            assistant_available,
            self._messages,
            tasks,
            artifacts,
        )

        return Snapshot.create(
            cursor,
            True,
            assistant_available,
            self._messages,
            tasks,
            artifacts,
        )

    def _refresh_artifacts(
        self,
        tasks: List[TaskSummary],
        deadline: float,
        request_budget: _UpstreamCallBudget,
    ) -> List[ArtifactSummary]:
        eligible = [
            task
            for task in tasks
            if task.status in ("QUEUED", "RUNNING", "NEEDS_INPUT", "COMPLETED")
        ]
        eligible_ids = {task.task_id for task in eligible}
        for task_id in list(self._artifact_cache):
            if task_id not in eligible_ids:
                del self._artifact_cache[task_id]
        if eligible:
            start = self._artifact_scan_offset % len(eligible)
            ordered = eligible[start:] + eligible[:start]
            now = self._clock()
            next_start = start
            for offset, task in enumerate(ordered):
                task_index = (start + offset) % len(eligible)
                cached = self._artifact_cache.get(task.task_id)
                if cached is not None and now - cached[0] < self._artifact_cache_seconds:
                    continue
                if request_budget.remaining < 2 or self._clock() >= deadline:
                    break
                before = request_budget.remaining
                loaded = self._load_task_artifacts(task, deadline, request_budget)
                if request_budget.remaining < before:
                    next_start = (task_index + 1) % len(eligible)
                if loaded is not None:
                    self._artifact_cache[task.task_id] = (self._clock(), tuple(loaded))
            self._artifact_scan_offset = next_start

        artifacts: List[ArtifactSummary] = []
        for task in eligible:
            cached = self._artifact_cache.get(task.task_id)
            if cached is not None:
                artifacts.extend(cached[1])
            if len(artifacts) >= MAX_COLLECTION_ITEMS:
                break
        return artifacts[:MAX_COLLECTION_ITEMS]

    def _load_task_artifacts(
        self,
        task: TaskSummary,
        deadline: float,
        request_budget: _UpstreamCallBudget,
    ) -> Optional[List[ArtifactSummary]]:
        try:
            detail = self._client.get_task(
                task.task_id,
                deadline=deadline,
                request_budget=request_budget,
            )
            link = detail.get("link")
            ticket = detail.get("token")
            if not isinstance(link, str) or not isinstance(ticket, str):
                return []
            entries = self._client.list_artifacts(
                link,
                task.task_id,
                ticket,
                MAX_COLLECTION_ITEMS,
                deadline=deadline,
                request_budget=request_budget,
            )
        except GatewayError:
            return None

        result: List[ArtifactSummary] = []
        for entry in entries:
            try:
                if not isinstance(entry, dict):
                    continue
                nested_entry = entry.get("Entry")
                event = entry.get("event")
                if event is None and isinstance(nested_entry, dict):
                    event = nested_entry.get("event")
                if isinstance(event, str) and event.lower() == "deleted":
                    continue
                artifact = entry.get("artifact")
                if artifact is None and isinstance(nested_entry, dict):
                    artifact = nested_entry.get("artifact")
                if not isinstance(artifact, dict):
                    continue
                raw_identifier = entry.get("id")
                if not isinstance(raw_identifier, str) or not raw_identifier:
                    uri = artifact.get("uri")
                    if not isinstance(uri, str) or not uri:
                        continue
                    raw_identifier = "artifact-" + hashlib.sha256(uri.encode("utf-8")).hexdigest()[:16]
                title = artifact.get("title") or artifact.get("name") or "Untitled output"
                kind = artifact.get("type")
                if isinstance(kind, str) and kind.lower() == "media":
                    mime_type = artifact.get("mimeType")
                    kind = (
                        "image"
                        if isinstance(mime_type, str) and mime_type.lower().startswith("image/")
                        else "document"
                    )
                result.append(
                    ArtifactSummary.create(
                        raw_identifier,
                        task.task_id,
                        title,
                        kind,
                        artifact.get("description", ""),
                    )
                )
            except (GatewayError, UnicodeError):
                continue
        return result

    @staticmethod
    def _snapshot_cursor(
        assistant_available: bool,
        messages: List[MessageSummary],
        tasks: List[TaskSummary],
        artifacts: List[ArtifactSummary],
    ) -> str:
        normalized = {
            "assistant_available": assistant_available,
            "messages": [message.to_dict() for message in messages],
            "tasks": [task.to_dict() for task in tasks],
            "artifacts": [artifact.to_dict() for artifact in artifacts],
        }
        encoded = json.dumps(
            normalized,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()

    def reply(self, message_id: str, text: str, operation_id: str) -> str:
        # The selected ID binds the device confirmation/idempotency context. The
        # official text endpoint itself has no reply_to field.
        require_identifier(message_id, "message_id")
        require_identifier(operation_id, "operation_id")
        return self._client.send_message(text)

    def create_task(self, prompt: str, operation_id: str) -> str:
        require_identifier(operation_id, "operation_id")
        result = self._client.create_task(prompt)
        return require_identifier(result.get("task_id"), "task_id")

    def followup_task(self, task_id: str, text: str, operation_id: str) -> str:
        del task_id, text, operation_id
        raise GatewayError(
            "not_supported",
            501,
            "Task follow-up requires ACP and is not supported in this release",
            False,
        )
