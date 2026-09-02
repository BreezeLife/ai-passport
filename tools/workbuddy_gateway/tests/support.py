from __future__ import annotations

import contextlib
import json
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, List, Optional, Tuple


PACKAGE_ROOT = Path(__file__).resolve().parents[1]
if str(PACKAGE_ROOT) not in sys.path:
    sys.path.insert(0, str(PACKAGE_ROOT))


Response = Tuple[int, Dict[str, str], bytes]
Route = Callable[["RecordedRequest"], Response]


class RecordedRequest:
    def __init__(self, method: str, path: str, headers: Dict[str, str], body: bytes):
        self.method = method
        self.path = path
        self.headers = headers
        self.body = body

    def json(self) -> Any:
        return json.loads(self.body.decode("utf-8"))


class RecordingServer:
    def __init__(self) -> None:
        self.routes: Dict[Tuple[str, str], Route] = {}
        self.requests: List[RecordedRequest] = []
        self._httpd: Optional[ThreadingHTTPServer] = None
        self._thread: Optional[threading.Thread] = None

    @property
    def base_url(self) -> str:
        assert self._httpd is not None
        host, port = self._httpd.server_address[:2]
        return "http://%s:%d" % (host, port)

    def json_route(self, method: str, path: str, payload: Any, status: int = 200) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.routes[(method, path)] = lambda _request: (
            status,
            {"Content-Type": "application/json"},
            body,
        )

    def route(self, method: str, path: str, callback: Route) -> None:
        self.routes[(method, path)] = callback

    def __enter__(self) -> "RecordingServer":
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def _dispatch(self) -> None:
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length) if length else b""
                request = RecordedRequest(
                    self.command,
                    self.path,
                    {key.lower(): value for key, value in self.headers.items()},
                    body,
                )
                owner.requests.append(request)
                callback = owner.routes.get((self.command, self.path))
                if callback is None:
                    status, headers, response = (404, {"Content-Type": "text/plain"}, b"missing")
                else:
                    status, headers, response = callback(request)
                self.send_response(status)
                for key, value in headers.items():
                    self.send_header(key, value)
                self.send_header("Content-Length", str(len(response)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(response)

            do_GET = _dispatch
            do_POST = _dispatch

            def log_message(self, _format: str, *_args: Any) -> None:
                return

        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *_args: Any) -> None:
        assert self._httpd is not None
        self._httpd.shutdown()
        self._httpd.server_close()
        assert self._thread is not None
        self._thread.join(timeout=2)


def request_json(
    url: str,
    *,
    method: str = "GET",
    token: Optional[str] = None,
    body: Optional[bytes] = None,
    content_type: Optional[str] = None,
    request_id: Optional[str] = None,
) -> Tuple[int, Dict[str, str], Dict[str, Any]]:
    headers: Dict[str, str] = {}
    if token is not None:
        headers["Authorization"] = "Bearer " + token
    if content_type is not None:
        headers["Content-Type"] = content_type
    if request_id is not None:
        headers["X-Request-ID"] = request_id
    request = urllib.request.Request(url, data=body, headers=headers, method=method)
    try:
        response = urllib.request.urlopen(request, timeout=3)
    except urllib.error.HTTPError as exc:
        response = exc
    with contextlib.closing(response):
        raw = response.read()
        return response.status, {key.lower(): value for key, value in response.headers.items()}, json.loads(raw)
