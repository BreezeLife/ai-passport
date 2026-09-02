"""Server-only client for the 2026-09-03 WorkBuddy Open Platform contract."""

from __future__ import annotations

import hashlib
import json
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


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args: Any, **kwargs: Any) -> None:
        del args, kwargs
        return None


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
        self._access_token: Optional[str] = None
        self._expires_at = 0.0
        self._lock = threading.Lock()
        self._opener = urllib.request.build_opener(_NoRedirectHandler())

    def get_access_token(self) -> str:
        with self._lock:
            if self._access_token is not None and self._clock() < self._expires_at - 60.0:
                return self._access_token
            self._refresh()
            assert self._access_token is not None
            return self._access_token

    def invalidate(self) -> None:
        with self._lock:
            self._expires_at = 0.0

    def _refresh(self) -> None:
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
            response = self._opener.open(request, timeout=self._timeout_seconds)
            with response:
                raw = response.read(MAX_TOKEN_RESPONSE_BYTES + 1)
        except urllib.error.HTTPError as error:
            error.close()
            raise GatewayError(
                "workbuddy_auth_failed", 502, "WorkBuddy authorization failed", True
            )
        except (urllib.error.URLError, socket.timeout, TimeoutError):
            raise GatewayError(
                "workbuddy_auth_failed", 502, "WorkBuddy authorization failed", True
            )
        if len(raw) > MAX_TOKEN_RESPONSE_BYTES:
            raise GatewayError(
                "workbuddy_auth_failed", 502, "WorkBuddy authorization response was invalid", True
            )
        try:
            payload = json.loads(raw.decode("utf-8", "strict"))
        except (UnicodeError, ValueError):
            raise GatewayError(
                "workbuddy_auth_failed", 502, "WorkBuddy authorization response was invalid", True
            )
        if not isinstance(payload, dict):
            raise GatewayError(
                "workbuddy_auth_failed", 502, "WorkBuddy authorization response was invalid", True
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
                "workbuddy_auth_failed", 502, "WorkBuddy authorization response was invalid", True
            )
        rotated_refresh = payload.get("refresh_token")
        if isinstance(rotated_refresh, str) and rotated_refresh:
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
        self._opener = urllib.request.build_opener(_NoRedirectHandler())

    def list_messages(
        self, after_message_id: Optional[str] = None, limit: int = MAX_COLLECTION_ITEMS
    ) -> Dict[str, Any]:
        if limit < 1 or limit > 100:
            raise GatewayError("invalid_request", 400, "Message limit is invalid", False)
        query: List[Tuple[str, Any]] = []
        if after_message_id is not None:
            query.append(("message_id", require_identifier(after_message_id, "message_id")))
        query.append(("limit", limit))
        payload = self._request_json(
            "GET", "/openapi/v2/localassistant/message", query=query
        )
        data = self._unwrap_message_envelope(payload)
        if not isinstance(data.get("messages"), list):
            raise GatewayError(
                "upstream_schema", 502, "WorkBuddy returned an invalid message list", True
            )
        return data

    def local_assistant_online(self) -> bool:
        payload = self._request_json("GET", "/openapi/v2/localassistant")
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

    def list_tasks(self, page: int = 1, size: int = MAX_COLLECTION_ITEMS) -> Dict[str, Any]:
        if isinstance(page, bool) or not isinstance(page, int) or page < 1:
            raise GatewayError("invalid_request", 400, "Task page is invalid", False)
        if isinstance(size, bool) or not isinstance(size, int) or size < 1 or size > 100:
            raise GatewayError("invalid_request", 400, "Task page size is invalid", False)
        payload = self._request_json(
            "GET", "/openapi/v2/tasks", query=(("page", page), ("size", size))
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

    def get_task(self, task_id: str) -> Dict[str, Any]:
        clean_id = require_identifier(task_id, "task_id")
        payload = self._request_json(
            "GET", "/openapi/v2/tasks/" + urllib.parse.quote(clean_id, safe="")
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
        payload = self._request_json("GET", absolute_url=url, bearer_token=task_ticket)
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
            token = bearer_token or self._current_access_token()
            request_headers = dict(headers)
            request_headers["Authorization"] = "Bearer " + token
            request = urllib.request.Request(
                url, data=encoded, method=method, headers=request_headers
            )
            try:
                response = self._opener.open(request, timeout=self._timeout_seconds)
                with response:
                    declared = response.headers.get("Content-Length")
                    if declared is not None and int(declared) > self._max_response_bytes:
                        raise GatewayError(
                            "upstream_too_large",
                            502,
                            "WorkBuddy response exceeded the gateway limit",
                            True,
                        )
                    raw = response.read(self._max_response_bytes + 1)
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
                )
            except GatewayError:
                raise
            except (urllib.error.URLError, socket.timeout, TimeoutError, OSError, ValueError):
                raise GatewayError(
                    "upstream_unavailable", 502, "WorkBuddy could not be reached", True
                )
        else:
            raise GatewayError(
                "upstream_rejected", 502, "WorkBuddy authorization failed", True
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

    def _current_access_token(self) -> str:
        if self._token_provider is not None:
            return self._token_provider.get_access_token()
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

    def __init__(self, client: WorkBuddyClient) -> None:
        self._client = client

    def fetch_snapshot(self, after_cursor: Optional[str] = None) -> Snapshot:
        assistant_available = self._client.local_assistant_online()
        message_data = self._client.list_messages(after_cursor, MAX_COLLECTION_ITEMS)
        task_data = self._client.list_tasks(page=1, size=MAX_COLLECTION_ITEMS)
        raw_messages = message_data["messages"]
        raw_tasks = task_data["tasks"]

        messages: List[MessageSummary] = []
        cursor = after_cursor or ""
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
            messages.append(message)
            cursor = message.message_id

        tasks: List[TaskSummary] = []
        artifacts: List[ArtifactSummary] = []
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
            tasks.append(task)
            if task.status == "COMPLETED" and len(artifacts) < MAX_COLLECTION_ITEMS:
                artifacts.extend(self._load_task_artifacts(task))
                artifacts = artifacts[:MAX_COLLECTION_ITEMS]

        return Snapshot.create(
            cursor,
            True,
            assistant_available,
            messages,
            tasks,
            artifacts,
        )

    def _load_task_artifacts(self, task: TaskSummary) -> List[ArtifactSummary]:
        try:
            detail = self._client.get_task(task.task_id)
            link = detail.get("link")
            ticket = detail.get("token")
            if not isinstance(link, str) or not isinstance(ticket, str):
                return []
            entries = self._client.list_artifacts(
                link,
                task.task_id,
                ticket,
                MAX_COLLECTION_ITEMS,
            )
        except GatewayError:
            return []

        result: List[ArtifactSummary] = []
        for entry in entries:
            try:
                if not isinstance(entry, dict):
                    continue
                artifact = entry.get("artifact")
                if artifact is None and isinstance(entry.get("Entry"), dict):
                    artifact = entry["Entry"].get("artifact")
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
