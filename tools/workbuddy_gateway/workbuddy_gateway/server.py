"""Small authenticated HTTP server exposing the bounded device contract."""

from __future__ import annotations

import copy
import hmac
import json
import math
import re
import socket
import sys
import threading
import time
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional, Tuple, Type

from .config import GatewayConfig
from .demo_adapter import DemoAdapter
from .errors import GatewayError
from .models import truncate_utf8
from .service import GatewayService
from .store import StateStore
from .transcription import DemoTranscriber, MAX_PCM_BYTES, TranscriptionClient
from .workbuddy_client import LiveWorkBuddyAdapter, OAuthTokenProvider, WorkBuddyClient


MAX_JSON_BODY_BYTES = 4096
MAX_DEVICE_RESPONSE_BYTES = 12 * 1024
MAX_REQUEST_ID_BYTES = 64
MAX_HEADER_BYTES = 16 * 1024
MAX_CONTENT_LENGTH_DIGITS = 10
DEFAULT_INBOUND_TIMEOUT_SECONDS = 5.0
DEFAULT_MAX_CONNECTIONS = 8
MAX_DISCARD_BYTES = 256 * 1024
MAX_DISCARD_GRACE_SECONDS = 0.1
PCM_CONTENT_TYPE = "audio/l16;rate=16000;channels=1"
_REQUEST_ID = re.compile(r"^[A-Za-z0-9._:-]{1,64}$")


def _reject_duplicate_keys(pairs: Any) -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


class GatewayHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 8

    def __init__(
        self,
        server_address: Tuple[str, int],
        handler_class: Type[BaseHTTPRequestHandler],
        *,
        inbound_timeout_seconds: float,
        max_connections: int,
    ) -> None:
        self.inbound_timeout_seconds = inbound_timeout_seconds
        self.max_connections = max_connections
        self._connection_slots = threading.BoundedSemaphore(max_connections)
        super().__init__(server_address, handler_class)

    def process_request(self, request: Any, client_address: Any) -> None:
        if not self._connection_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self._connection_slots.release()
            raise

    def process_request_thread(self, request: Any, client_address: Any) -> None:
        try:
            super().process_request_thread(request, client_address)
        finally:
            self._connection_slots.release()

    def handle_error(self, request: Any, client_address: Any) -> None:
        error = sys.exc_info()[1]
        if isinstance(
            error,
            (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, socket.timeout),
        ):
            return
        super().handle_error(request, client_address)


def _json_bytes(payload: Dict[str, Any]) -> bytes:
    return json.dumps(
        payload, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")


def _trim_snapshot_response(response: Dict[str, Any]) -> Optional[bytes]:
    if not all(isinstance(response.get(key), list) for key in ("messages", "tasks", "artifacts")):
        return None
    candidate = copy.deepcopy(response)

    def encoded_if_fit() -> Optional[bytes]:
        raw_candidate = _json_bytes(candidate)
        return raw_candidate if len(raw_candidate) <= MAX_DEVICE_RESPONSE_BYTES else None

    verbose_fields = (
        ("messages", "preview"),
        ("tasks", "preview"),
        ("artifacts", "description"),
    )
    for maximum in (192, 128, 96, 64, 32, 0):
        for collection_name, field_name in verbose_fields:
            for item in candidate[collection_name]:
                value = item.get(field_name)
                if isinstance(value, str):
                    item[field_name] = truncate_utf8(value, maximum)
        fitted = encoded_if_fit()
        if fitted is not None:
            return fitted

    for maximum in (96, 64, 48, 32, 16):
        for collection_name in ("tasks", "artifacts"):
            for item in candidate[collection_name]:
                value = item.get("title")
                if isinstance(value, str):
                    item["title"] = truncate_utf8(value, maximum)
        fitted = encoded_if_fit()
        if fitted is not None:
            return fitted

    for collection_name in ("artifacts", "tasks", "messages"):
        while candidate[collection_name]:
            candidate[collection_name].pop()
            fitted = encoded_if_fit()
            if fitted is not None:
                return fitted
    return None


def _content_type(value: Optional[str]) -> Tuple[str, Dict[str, str]]:
    if value is None:
        return "", {}
    parts = [part.strip() for part in value.split(";")]
    media_type = parts[0].lower()
    parameters: Dict[str, str] = {}
    for part in parts[1:]:
        if "=" not in part:
            parameters[part.lower()] = ""
            continue
        key, raw_value = part.split("=", 1)
        parameters[key.strip().lower()] = raw_value.strip().strip('"').lower()
    return media_type, parameters


def _handler_class(device_token: str, service: GatewayService) -> Type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def setup(self) -> None:
            self.timeout = getattr(
                self.server, "inbound_timeout_seconds", DEFAULT_INBOUND_TIMEOUT_SECONDS
            )
            super().setup()

        def do_GET(self) -> None:
            self._dispatch("GET")

        def do_POST(self) -> None:
            self._dispatch("POST")

        def do_PUT(self) -> None:
            self._dispatch("PUT")

        def do_PATCH(self) -> None:
            self._dispatch("PATCH")

        def do_DELETE(self) -> None:
            self._dispatch("DELETE")

        def _dispatch(self, method: str) -> None:
            request_id = self._request_id()
            try:
                self._validate_headers()
                parsed = urllib.parse.urlsplit(self.path)
                path = parsed.path
                if method == "GET" and path == "/healthz":
                    self._send(200, {"status": "ok"}, request_id)
                    return
                if method == "GET" and path == "/readyz":
                    ready = service.is_ready()
                    self._send(200 if ready else 503, {"ready": ready}, request_id)
                    return
                if path in ("/healthz", "/readyz"):
                    raise GatewayError("method_not_allowed", 405, "Method is not allowed", False)

                if path == "/v1/snapshot":
                    self._authorize()
                    if method != "GET":
                        raise GatewayError("method_not_allowed", 405, "Method is not allowed", False)
                    query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
                    if set(query) - {"after"} or len(query.get("after", [])) > 1:
                        raise GatewayError("invalid_request", 400, "Snapshot query is invalid", False)
                    after_values = query.get("after")
                    after = after_values[0] if after_values else None
                    self._send(200, service.get_snapshot(after), request_id)
                    return

                operation_prefix = "/v1/operations/"
                if path.startswith(operation_prefix):
                    self._authorize()
                    if method != "GET":
                        raise GatewayError("method_not_allowed", 405, "Method is not allowed", False)
                    if parsed.query:
                        raise GatewayError("invalid_request", 400, "Operation query is invalid", False)
                    encoded_id = path[len(operation_prefix) :]
                    operation_id = urllib.parse.unquote(encoded_id)
                    if not encoded_id or "/" in operation_id:
                        raise GatewayError("not_found", 404, "Route was not found", False)
                    self._send(200, service.get_operation(operation_id), request_id)
                    return

                if path == "/v1/actions":
                    self._authorize()
                    if method != "POST":
                        raise GatewayError("method_not_allowed", 405, "Method is not allowed", False)
                    raw = self._read_body(MAX_JSON_BODY_BYTES)
                    media_type, parameters = _content_type(self.headers.get("Content-Type"))
                    if media_type != "application/json" or set(parameters) - {"charset"}:
                        raise GatewayError(
                            "unsupported_media_type", 415, "Content-Type must be application/json", False
                        )
                    if parameters.get("charset", "utf-8") != "utf-8":
                        raise GatewayError(
                            "unsupported_media_type", 415, "JSON charset must be UTF-8", False
                        )
                    try:
                        action = json.loads(
                            raw.decode("utf-8", "strict"),
                            object_pairs_hook=_reject_duplicate_keys,
                        )
                    except (UnicodeError, ValueError):
                        raise GatewayError("invalid_json", 400, "Request body is invalid JSON", False)
                    if not isinstance(action, dict):
                        raise GatewayError("invalid_request", 400, "Action must be a JSON object", False)
                    self._send(200, service.perform_action(action), request_id)
                    return

                if path == "/v1/transcriptions":
                    self._authorize()
                    if method != "POST":
                        raise GatewayError("method_not_allowed", 405, "Method is not allowed", False)
                    pcm = self._read_body(MAX_PCM_BYTES)
                    media_type, parameters = _content_type(self.headers.get("Content-Type"))
                    if media_type != "audio/l16" or parameters != {
                        "rate": "16000",
                        "channels": "1",
                    }:
                        raise GatewayError(
                            "unsupported_media_type",
                            415,
                            "Content-Type must describe 16 kHz mono L16 audio",
                            False,
                        )
                    query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
                    if set(query) != {"operation_id"} or len(query["operation_id"]) != 1:
                        raise GatewayError(
                            "invalid_request", 400, "Transcription operation ID is required", False
                        )
                    self._send(
                        200,
                        service.create_transcription(query["operation_id"][0], pcm),
                        request_id,
                    )
                    return

                if path in ("/v1/actions", "/v1/transcriptions"):
                    raise GatewayError("method_not_allowed", 405, "Method is not allowed", False)
                raise GatewayError("not_found", 404, "Route was not found", False)
            except GatewayError as error:
                self._send(error.status, {"error": error.to_dict()}, request_id)
            except Exception:
                self._send(
                    500,
                    {
                        "error": GatewayError(
                            "internal_error", 500, "Gateway request failed", True
                        ).to_dict()
                    },
                    request_id,
                )

        def _authorize(self) -> None:
            expected = "Bearer " + device_token
            supplied = self.headers.get("Authorization", "")
            if not hmac.compare_digest(supplied.encode("utf-8"), expected.encode("utf-8")):
                raise GatewayError("unauthorized", 401, "Device authorization failed", False)

        def _validate_headers(self) -> None:
            total = 0
            for name, value in self.headers.items():
                total += len(name.encode("latin-1")) + len(value.encode("latin-1")) + 4
            if total > MAX_HEADER_BYTES:
                self.close_connection = True
                raise GatewayError(
                    "headers_too_large", 431, "Request headers are too large", False
                )

        def _read_body(self, maximum: int) -> bytes:
            if self.headers.get("Transfer-Encoding"):
                self.close_connection = True
                raise GatewayError(
                    "invalid_request", 400, "Chunked request bodies are not supported", False
                )
            declared_values = self.headers.get_all("Content-Length", [])
            if len(declared_values) != 1:
                self.close_connection = True
                raise GatewayError("length_required", 411, "Content-Length is required", False)
            declared = declared_values[0]
            if (
                not declared.isascii()
                or not declared.isdecimal()
                or len(declared) > MAX_CONTENT_LENGTH_DIGITS
            ):
                self.close_connection = True
                raise GatewayError("invalid_request", 400, "Content-Length is invalid", False)
            length = int(declared)
            if length < 1:
                raise GatewayError("invalid_request", 400, "Request body is empty", False)
            if length > maximum:
                self.close_connection = True
                self._discard_buffered_input(min(length, MAX_DISCARD_BYTES))
                raise GatewayError("payload_too_large", 413, "Request body is too large", False)
            try:
                body = self.rfile.read(length)
            except (socket.timeout, TimeoutError, OSError):
                self.close_connection = True
                raise GatewayError(
                    "request_timeout", 408, "Request body timed out", True
                )
            if len(body) != length:
                self.close_connection = True
                raise GatewayError("invalid_request", 400, "Request body was incomplete", False)
            return body

        def _discard_buffered_input(self, maximum: int) -> None:
            """Drain a bounded upload briefly so the typed response is not reset."""

            previous_timeout = self.connection.gettimeout()
            discarded = 0
            deadline = time.monotonic() + MAX_DISCARD_GRACE_SECONDS
            try:
                while discarded < maximum:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        break
                    self.connection.settimeout(remaining)
                    try:
                        chunk = self.rfile.read1(min(8192, maximum - discarded))
                    except (socket.timeout, TimeoutError, OSError):
                        break
                    if not chunk:
                        break
                    discarded += len(chunk)
            finally:
                self.connection.settimeout(previous_timeout)

        def _request_id(self) -> str:
            supplied = self.headers.get("X-Request-ID", "")
            if _REQUEST_ID.fullmatch(supplied) and len(supplied.encode("ascii")) <= MAX_REQUEST_ID_BYTES:
                return supplied
            return uuid.uuid4().hex

        def _send(self, status: int, payload: Dict[str, Any], request_id: str) -> None:
            response = {"version": 1, "request_id": request_id}
            response.update(payload)
            raw = _json_bytes(response)
            if len(raw) > MAX_DEVICE_RESPONSE_BYTES:
                trimmed = _trim_snapshot_response(response) if status == 200 else None
                if trimmed is not None:
                    raw = trimmed
                else:
                    status = 500
                    raw = _json_bytes(
                        {
                            "version": 1,
                            "request_id": request_id,
                            "error": {
                                "code": "response_too_large",
                                "message": "Gateway response exceeded its limit",
                                "retryable": True,
                            },
                        }
                    )
            self.close_connection = True
            try:
                self.send_response(status)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(raw)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(raw)
            except (
                BrokenPipeError,
                ConnectionResetError,
                ConnectionAbortedError,
                socket.timeout,
            ):
                return

        def log_message(self, _format: str, *_args: Any) -> None:
            return

    return Handler


def create_http_server(
    host: str,
    port: int,
    device_token: str,
    service: GatewayService,
    *,
    inbound_timeout_seconds: float = DEFAULT_INBOUND_TIMEOUT_SECONDS,
    max_connections: int = DEFAULT_MAX_CONNECTIONS,
) -> GatewayHTTPServer:
    if not isinstance(device_token, str) or not device_token:
        raise GatewayError(
            "invalid_configuration", 500, "Gateway device token is required", False
        )
    if (
        isinstance(inbound_timeout_seconds, bool)
        or not isinstance(inbound_timeout_seconds, (int, float))
        or not math.isfinite(inbound_timeout_seconds)
        or inbound_timeout_seconds <= 0
        or inbound_timeout_seconds > 60
        or isinstance(max_connections, bool)
        or not isinstance(max_connections, int)
        or max_connections < 1
        or max_connections > 64
    ):
        raise GatewayError(
            "invalid_configuration", 500, "Gateway HTTP limits are invalid", False
        )
    return GatewayHTTPServer(
        (host, port),
        _handler_class(device_token, service),
        inbound_timeout_seconds=float(inbound_timeout_seconds),
        max_connections=max_connections,
    )


def build_service(config: GatewayConfig) -> GatewayService:
    store = StateStore(config.state_path)
    if config.mode == "demo":
        return GatewayService(DemoAdapter(), DemoTranscriber(), store)

    assert config.workbuddy_base_url is not None
    if (
        config.workbuddy_refresh_token
        and config.workbuddy_client_id
        and config.workbuddy_client_secret
    ):
        persisted_refresh = (
            store.get_refresh_token(config.workbuddy_client_id)
            if store.is_healthy()
            else None
        )
        provider = OAuthTokenProvider(
            config.workbuddy_base_url + "/openapi/v2/token",
            config.workbuddy_client_id,
            config.workbuddy_client_secret,
            persisted_refresh or config.workbuddy_refresh_token,
            timeout_seconds=config.request_timeout_seconds,
            on_refresh_token=lambda token: store.set_refresh_token(
                config.workbuddy_client_id, token
            ),
        )
        client = WorkBuddyClient(
            config.workbuddy_base_url,
            token_provider=provider,
            timeout_seconds=config.request_timeout_seconds,
        )
    else:
        assert config.workbuddy_access_token is not None
        client = WorkBuddyClient(
            config.workbuddy_base_url,
            config.workbuddy_access_token,
            timeout_seconds=config.request_timeout_seconds,
        )
    assert config.transcription_url is not None
    assert config.transcription_api_key is not None
    transcriber = TranscriptionClient(
        config.transcription_url,
        config.transcription_api_key,
        config.transcription_model,
        timeout_seconds=config.request_timeout_seconds,
    )
    return GatewayService(LiveWorkBuddyAdapter(client), transcriber, store)


def main() -> None:
    config = GatewayConfig.from_env()
    service = build_service(config)
    server = create_http_server(
        config.host,
        config.port,
        config.device_token,
        service,
        inbound_timeout_seconds=config.inbound_timeout_seconds,
        max_connections=config.max_connections,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
