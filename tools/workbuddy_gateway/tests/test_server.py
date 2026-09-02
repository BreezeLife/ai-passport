from __future__ import annotations

import json
import tempfile
import threading
import unittest
from pathlib import Path
from typing import Iterator, Tuple

from support import PACKAGE_ROOT, request_json  # noqa: F401

from workbuddy_gateway.demo_adapter import DemoAdapter
from workbuddy_gateway.server import MAX_JSON_BODY_BYTES, create_http_server
from workbuddy_gateway.service import GatewayService
from workbuddy_gateway.store import StateStore
from workbuddy_gateway.transcription import DemoTranscriber, MAX_PCM_BYTES


class RunningGateway:
    def __init__(self, token: str, service: GatewayService) -> None:
        self.httpd = create_http_server("127.0.0.1", 0, token, service)
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


if __name__ == "__main__":
    unittest.main()
