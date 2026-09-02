from __future__ import annotations

import io
import json
import math
import socket
import struct
import tempfile
import threading
import time
import unittest
import urllib.error
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

from support import PACKAGE_ROOT, request_json  # noqa: F401

from workbuddy_gateway.demo_adapter import DemoAdapter
from workbuddy_gateway.config import GatewayConfig
from workbuddy_gateway.models import ArtifactSummary, MessageSummary, Snapshot, TaskSummary
from workbuddy_gateway.server import (
    MAX_DEVICE_RESPONSE_BYTES,
    MAX_JSON_BODY_BYTES,
    build_service,
    create_http_server,
)
from workbuddy_gateway.service import GatewayService
from workbuddy_gateway.store import StateStore
from workbuddy_gateway.transcription import DemoTranscriber, MAX_PCM_BYTES


class RunningGateway:
    def __init__(self, token: str, service: GatewayService, **server_options: object) -> None:
        self.httpd = create_http_server(
            "127.0.0.1", 0, token, service, **server_options
        )
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    @property
    def base_url(self) -> str:
        host, port = self.httpd.server_address[:2]
        return "http://%s:%d" % (host, port)

    def __enter__(self) -> "RunningGateway":
        self.thread.start()
        return self

    def __exit__(self, *_args: object) -> None:
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            raise AssertionError("gateway did not shut down")


def raw_exchange(port: int, payload: bytes, timeout: float = 1.0) -> bytes:
    connection = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    try:
        connection.settimeout(timeout)
        connection.sendall(payload)
        chunks = []
        while True:
            try:
                chunk = connection.recv(4096)
            except (ConnectionResetError, socket.timeout):
                break
            if not chunk:
                break
            chunks.append(chunk)
        return b"".join(chunks)
    finally:
        connection.close()


class GatewayHTTPContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.token = "device-token-secret"
        self.service = GatewayService(
            DemoAdapter(),
            DemoTranscriber(),
            StateStore(Path(self.temp.name) / "state.json"),
            clock=lambda: 100.0,
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_health_and_readiness_are_public_but_reveal_no_secrets(self) -> None:
        with RunningGateway(self.token, self.service) as gateway:
            health = request_json(gateway.base_url + "/healthz")
            ready = request_json(gateway.base_url + "/readyz")

        self.assertEqual(200, health[0])
        self.assertEqual("ok", health[2]["status"])
        self.assertEqual(200, ready[0])
        self.assertTrue(ready[2]["ready"])
        self.assertNotIn(self.token, json.dumps([health[2], ready[2]]))

    def test_every_response_has_version_and_bounded_request_id(self) -> None:
        with RunningGateway(self.token, self.service) as gateway:
            status, headers, payload = request_json(
                gateway.base_url + "/v1/snapshot",
                token=self.token,
                request_id="device-request-1",
            )

        self.assertEqual(200, status)
        self.assertEqual(1, payload["version"])
        self.assertEqual("device-request-1", payload["request_id"])
        self.assertEqual("application/json; charset=utf-8", headers["content-type"])

    def test_device_routes_require_exact_bearer_auth(self) -> None:
        with RunningGateway(self.token, self.service) as gateway:
            missing = request_json(gateway.base_url + "/v1/snapshot")
            wrong = request_json(gateway.base_url + "/v1/snapshot", token="wrong")
            valid = request_json(gateway.base_url + "/v1/snapshot", token=self.token)

        self.assertEqual(401, missing[0])
        self.assertEqual(401, wrong[0])
        self.assertEqual(200, valid[0])
        rendered = json.dumps([missing[2], wrong[2]])
        self.assertNotIn(self.token, rendered)
        self.assertNotIn("wrong", rendered)

    def test_snapshot_supports_after_cursor_and_is_bounded(self) -> None:
        with RunningGateway(self.token, self.service) as gateway:
            status, _headers, payload = request_json(
                gateway.base_url + "/v1/snapshot?after=demo-0",
                token=self.token,
            )

        self.assertEqual(200, status)
        self.assertEqual("demo-1", payload["cursor"])
        self.assertLessEqual(len(json.dumps(payload, ensure_ascii=False).encode("utf-8")), 12 * 1024)

    def test_transcription_requires_exact_media_type_and_audio_bounds(self) -> None:
        pcm = b"\x00\x00" * 16000
        with RunningGateway(self.token, self.service) as gateway:
            valid = request_json(
                gateway.base_url + "/v1/transcriptions?operation_id=audio-op-1",
                method="POST",
                token=self.token,
                body=pcm,
                content_type="audio/L16;rate=16000;channels=1",
            )
            wrong_type = request_json(
                gateway.base_url + "/v1/transcriptions?operation_id=audio-op-2",
                method="POST",
                token=self.token,
                body=pcm,
                content_type="audio/wav",
            )
            too_large = request_json(
                gateway.base_url + "/v1/transcriptions?operation_id=audio-op-3",
                method="POST",
                token=self.token,
                body=b"\x00" * (MAX_PCM_BYTES + 2),
                content_type="audio/L16;rate=16000;channels=1",
            )

        self.assertEqual(200, valid[0])
        self.assertTrue(valid[2]["text"])
        self.assertEqual(415, wrong_type[0])
        self.assertEqual(413, too_large[0])

    def test_action_and_operation_lookup_form_an_idempotent_http_contract(self) -> None:
        action = json.dumps(
            {"version": 1, "operation_id": "op-http-1", "type": "task_create", "prompt": "Build report"}
        ).encode()
        with RunningGateway(self.token, self.service) as gateway:
            first = request_json(
                gateway.base_url + "/v1/actions",
                method="POST",
                token=self.token,
                body=action,
                content_type="application/json",
            )
            second = request_json(
                gateway.base_url + "/v1/actions",
                method="POST",
                token=self.token,
                body=action,
                content_type="application/json",
            )
            lookup = request_json(
                gateway.base_url + "/v1/operations/op-http-1",
                token=self.token,
            )

        self.assertEqual(200, first[0])
        self.assertEqual(first[2]["receipt_id"], second[2]["receipt_id"])
        self.assertEqual("SUCCEEDED", lookup[2]["status"])

    def test_json_content_type_size_and_shape_are_strict(self) -> None:
        with RunningGateway(self.token, self.service) as gateway:
            wrong_type = request_json(
                gateway.base_url + "/v1/actions",
                method="POST",
                token=self.token,
                body=b"{}",
                content_type="text/plain",
            )
            oversized = request_json(
                gateway.base_url + "/v1/actions",
                method="POST",
                token=self.token,
                body=b"x" * (MAX_JSON_BODY_BYTES + 1),
                content_type="application/json",
            )
            malformed = request_json(
                gateway.base_url + "/v1/actions",
                method="POST",
                token=self.token,
                body=b"{",
                content_type="application/json",
            )
            array = request_json(
                gateway.base_url + "/v1/actions",
                method="POST",
                token=self.token,
                body=b"[]",
                content_type="application/json",
            )
            duplicate_key = request_json(
                gateway.base_url + "/v1/actions",
                method="POST",
                token=self.token,
                body=b'{"version":1,"operation_id":"op-1","operation_id":"op-2","type":"task_create","prompt":"x"}',
                content_type="application/json",
            )

        self.assertEqual(415, wrong_type[0])
        self.assertEqual(413, oversized[0])
        self.assertEqual(400, malformed[0])
        self.assertEqual(400, array[0])
        self.assertEqual(400, duplicate_key[0])
        self.assertEqual("invalid_json", duplicate_key[2]["error"]["code"])

    def test_unknown_route_unknown_operation_and_wrong_method_are_typed(self) -> None:
        with RunningGateway(self.token, self.service) as gateway:
            missing_route = request_json(gateway.base_url + "/missing")
            missing_operation = request_json(
                gateway.base_url + "/v1/operations/no-such-op", token=self.token
            )
            wrong_method = request_json(
                gateway.base_url + "/v1/snapshot", method="POST", token=self.token, body=b""
            )

        self.assertEqual(404, missing_route[0])
        self.assertEqual(404, missing_operation[0])
        self.assertEqual(405, wrong_method[0])
        self.assertEqual("not_found", missing_route[2]["error"]["code"])

    def test_errors_never_echo_authorization_or_body_content(self) -> None:
        body_secret = "body-secret-must-not-leak"
        body = json.dumps(
            {"version": 1, "operation_id": "op-secret", "type": "task_create", "prompt": body_secret, "token": "bad"}
        ).encode()
        with RunningGateway(self.token, self.service) as gateway:
            response = request_json(
                gateway.base_url + "/v1/actions",
                method="POST",
                token=self.token,
                body=body,
                content_type="application/json",
            )

        rendered = json.dumps(response[2])
        self.assertNotIn(self.token, rendered)
        self.assertNotIn(body_secret, rendered)

    def test_build_service_prefers_persisted_refresh_token_and_persists_next_rotation(self) -> None:
        state_path = Path(self.temp.name) / "oauth-state.json"
        initial_store = StateStore(state_path)
        initial_store.set_refresh_token("client-id", "persisted-refresh")
        config = GatewayConfig(
            mode="live",
            host="127.0.0.1",
            port=0,
            device_token=self.token,
            state_path=state_path,
            workbuddy_base_url="https://www.workbuddy.cn",
            workbuddy_refresh_token="environment-refresh",
            workbuddy_client_id="client-id",
            workbuddy_client_secret="client-secret",
            transcription_url="https://speech.example/v1/audio/transcriptions",
            transcription_api_key="speech-secret",
        )

        with mock.patch("workbuddy_gateway.server.OAuthTokenProvider") as provider_class:
            build_service(config)

        args = provider_class.call_args.args
        kwargs = provider_class.call_args.kwargs
        self.assertEqual("persisted-refresh", args[3])
        kwargs["on_refresh_token"]("next-rotated-refresh")
        self.assertEqual(
            "next-rotated-refresh",
            StateStore(state_path).get_refresh_token("client-id"),
        )

    def test_oversize_declared_body_returns_413_without_waiting_for_body(self) -> None:
        with RunningGateway(
            self.token,
            self.service,
            inbound_timeout_seconds=1.0,
            max_connections=2,
        ) as gateway:
            port = gateway.httpd.server_address[1]
            started = time.monotonic()
            response = raw_exchange(
                port,
                (
                    "POST /v1/actions HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Authorization: Bearer %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n\r\n"
                )
                .__mod__((self.token, MAX_JSON_BODY_BYTES + 1))
                .encode("ascii"),
                timeout=0.5,
            )
            elapsed = time.monotonic() - started

        self.assertIn(b" 413 ", response.split(b"\r\n", 1)[0])
        self.assertLess(elapsed, 0.5)

    def test_aggregate_headers_and_content_length_digits_are_bounded(self) -> None:
        with RunningGateway(self.token, self.service) as gateway:
            port = gateway.httpd.server_address[1]
            oversized_headers = raw_exchange(
                port,
                b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nX-Fill: "
                + b"x" * (17 * 1024)
                + b"\r\n\r\n",
            )
            excessive_digits = raw_exchange(
                port,
                (
                    "POST /v1/actions HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Authorization: Bearer %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: 10000000000\r\n\r\n"
                )
                .__mod__(self.token)
                .encode("ascii"),
            )

        self.assertIn(b" 431 ", oversized_headers.split(b"\r\n", 1)[0])
        self.assertIn(b" 400 ", excessive_digits.split(b"\r\n", 1)[0])

    def test_idle_and_partial_header_connections_timeout_and_release_slots(self) -> None:
        with RunningGateway(
            self.token,
            self.service,
            inbound_timeout_seconds=0.15,
            max_connections=2,
        ) as gateway:
            port = gateway.httpd.server_address[1]
            idle = socket.create_connection(("127.0.0.1", port), timeout=1)
            partial = socket.create_connection(("127.0.0.1", port), timeout=1)
            try:
                idle.settimeout(1)
                partial.settimeout(1)
                partial.sendall(b"GET /healthz HTTP/1.1\r\nHost: unfinished")
                started = time.monotonic()
                self.assertEqual(b"", idle.recv(1))
                self.assertEqual(b"", partial.recv(1))
                self.assertLess(time.monotonic() - started, 0.8)
                health = request_json(gateway.base_url + "/healthz")
            finally:
                idle.close()
                partial.close()

        self.assertEqual(200, health[0])

    def test_connection_cap_rejects_excess_client_and_recovers(self) -> None:
        with RunningGateway(
            self.token,
            self.service,
            inbound_timeout_seconds=1.0,
            max_connections=2,
        ) as gateway:
            port = gateway.httpd.server_address[1]
            blockers = [
                socket.create_connection(("127.0.0.1", port), timeout=1)
                for _index in range(2)
            ]
            try:
                for blocker in blockers:
                    blocker.sendall(b"GET /healthz HTTP/1.1\r\nHost: unfinished")
                time.sleep(0.05)
                excess = raw_exchange(
                    port,
                    b"GET /healthz HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n",
                    timeout=0.3,
                )
                self.assertNotIn(b" 200 ", excess.split(b"\r\n", 1)[0])
            finally:
                for blocker in blockers:
                    blocker.close()
            time.sleep(0.05)
            health = request_json(gateway.base_url + "/healthz")

        self.assertEqual(200, health[0])

    def test_partial_body_times_out_without_internal_error_or_traceback(self) -> None:
        captured = io.StringIO()
        with redirect_stderr(captured), RunningGateway(
            self.token,
            self.service,
            inbound_timeout_seconds=0.15,
            max_connections=2,
        ) as gateway:
            response = raw_exchange(
                gateway.httpd.server_address[1],
                (
                    "POST /v1/actions HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Authorization: Bearer %s\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: 100\r\n\r\n{}"
                )
                .__mod__(self.token)
                .encode("ascii"),
                timeout=0.8,
            )

        status_line, _headers, body = response.partition(b"\r\n\r\n")
        self.assertIn(b" 408 ", status_line.split(b"\r\n", 1)[0])
        self.assertEqual("request_timeout", json.loads(body)["error"]["code"])
        self.assertNotIn("Traceback", captured.getvalue())
        self.assertNotIn("BrokenPipe", captured.getvalue())

    def test_server_rejects_non_finite_inbound_timeout(self) -> None:
        with self.assertRaises(Exception) as caught:
            create_http_server(
                "127.0.0.1",
                0,
                self.token,
                self.service,
                inbound_timeout_seconds=math.nan,
            )

        self.assertEqual("invalid_configuration", caught.exception.code)

    def test_reset_client_does_not_log_traceback_and_releases_connection_slot(self) -> None:
        class BlockingService:
            initially_ready = True

            def __init__(self):
                self.entered = threading.Event()
                self.release = threading.Event()

            def get_snapshot(self, _after=None):
                self.entered.set()
                self.release.wait(timeout=1)
                return DemoAdapter().fetch_snapshot().to_dict()

            def is_ready(self):
                return True

        service = BlockingService()
        captured = io.StringIO()
        with redirect_stderr(captured), RunningGateway(
            self.token,
            service,
            inbound_timeout_seconds=0.5,
            max_connections=1,
        ) as gateway:
            connection = socket.create_connection(
                ("127.0.0.1", gateway.httpd.server_address[1]), timeout=1
            )
            connection.sendall(
                (
                    "GET /v1/snapshot HTTP/1.1\r\n"
                    "Host: localhost\r\n"
                    "Authorization: Bearer %s\r\n\r\n"
                )
                .__mod__(self.token)
                .encode("ascii")
            )
            self.assertTrue(service.entered.wait(timeout=0.5))
            connection.setsockopt(
                socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0)
            )
            connection.close()
            service.release.set()
            deadline = time.monotonic() + 1.0
            health = None
            while time.monotonic() < deadline:
                try:
                    health = request_json(gateway.base_url + "/healthz")
                    break
                except (urllib.error.URLError, ConnectionError):
                    time.sleep(0.02)

        self.assertIsNotNone(health)
        self.assertEqual(200, health[0])
        self.assertNotIn("Traceback", captured.getvalue())
        self.assertNotIn("BrokenPipe", captured.getvalue())

    def test_escape_heavy_snapshot_is_semantically_trimmed_below_device_limit(self) -> None:
        noisy_preview = '\\\"\n' * 128
        noisy_title = '\\\"' * 64

        class LargeAdapter:
            initially_ready = True

            def fetch_snapshot(self, _after=None):
                return Snapshot.create(
                    "c" * 64,
                    True,
                    True,
                    [
                        MessageSummary.create("m-%d" % index, "assistant", noisy_preview, True)
                        for index in range(6)
                    ],
                    [
                        TaskSummary.create(
                            "t-%d" % index,
                            noisy_title,
                            "working",
                            noisy_preview,
                        )
                        for index in range(6)
                    ],
                    [
                        ArtifactSummary.create(
                            "a-%d" % index,
                            "t-%d" % index,
                            noisy_title,
                            "overview",
                            noisy_preview,
                        )
                        for index in range(6)
                    ],
                )

            def reply(self, *_args):
                raise AssertionError("not used")

            create_task = reply
            followup_task = reply

        service = GatewayService(
            LargeAdapter(),
            DemoTranscriber(),
            StateStore(Path(self.temp.name) / "large-state.json"),
        )
        with RunningGateway(self.token, service) as gateway:
            status, headers, payload = request_json(
                gateway.base_url + "/v1/snapshot", token=self.token
            )

        self.assertEqual(200, status)
        self.assertLessEqual(int(headers["content-length"]), MAX_DEVICE_RESPONSE_BYTES)
        self.assertEqual("m-0", payload["messages"][0]["id"])
        self.assertEqual("t-0", payload["tasks"][0]["id"])
        self.assertEqual("a-0", payload["artifacts"][0]["id"])
        self.assertTrue(
            any(item["preview"] != noisy_preview for item in payload["messages"])
            or any(item["preview"] != noisy_preview for item in payload["tasks"])
            or any(item["description"] != noisy_preview for item in payload["artifacts"])
        )


if __name__ == "__main__":
    unittest.main()
