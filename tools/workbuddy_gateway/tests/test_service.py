from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import List, Optional

from support import PACKAGE_ROOT, RecordingServer  # noqa: F401

from workbuddy_gateway.demo_adapter import DemoAdapter
from workbuddy_gateway.errors import GatewayError
from workbuddy_gateway.models import MAX_TRANSCRIPT_BYTES, Snapshot
from workbuddy_gateway.service import GatewayService
from workbuddy_gateway.store import StateStore
from workbuddy_gateway.workbuddy_client import (
    LiveWorkBuddyAdapter,
    OAuthTokenProvider,
    WorkBuddyClient,
)


class FakeAdapter:
    def __init__(self) -> None:
        self.calls: List[tuple] = []
        self.retryable_error = False
        self.safe_retryable_error = False
        self.nonretryable_error = False

    def fetch_snapshot(self, after_cursor: Optional[str] = None) -> Snapshot:
        self.calls.append(("snapshot", after_cursor))
        return DemoAdapter().fetch_snapshot(after_cursor)

    def reply(self, message_id: str, text: str, operation_id: str) -> str:
        self.calls.append(("reply", message_id, text, operation_id))
        if self.safe_retryable_error:
            raise GatewayError(
                "workbuddy_auth_failed",
                502,
                "WorkBuddy authorization failed",
                True,
                safe_to_retry_operation=True,
            )
        if self.retryable_error:
            raise GatewayError("upstream_timeout", 504, "WorkBuddy timed out", True)
        if self.nonretryable_error:
            raise GatewayError("upstream_rejected", 422, "WorkBuddy rejected the action", False)
        return "reply-receipt"

    def create_task(self, prompt: str, operation_id: str) -> str:
        self.calls.append(("task_create", prompt, operation_id))
        return "task-receipt"

    def followup_task(self, task_id: str, text: str, operation_id: str) -> str:
        self.calls.append(("task_followup", task_id, text, operation_id))
        raise GatewayError("not_supported", 501, "Task follow-up requires ACP and is not supported", False)


class FakeTranscriber:
    def __init__(self, result: str = "transcribed") -> None:
        self.result = result
        self.calls: List[bytes] = []

    def transcribe(self, pcm: bytes) -> str:
        self.calls.append(pcm)
        return self.result


class GatewayServiceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.adapter = FakeAdapter()
        self.transcriber = FakeTranscriber()
        self.service = GatewayService(
            self.adapter,
            self.transcriber,
            StateStore(Path(self.temp.name) / "state.json"),
            clock=lambda: 100.0,
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_snapshot_preserves_normalized_allowlisted_device_shape(self) -> None:
        result = self.service.get_snapshot("cursor-before")

        self.assertEqual("demo-1", result["cursor"])
        self.assertEqual(("snapshot", "cursor-before"), self.adapter.calls[0])
        rendered = repr(result).lower()
        for forbidden in ("token", "sandbox", "access_secret", "link"):
            self.assertNotIn(forbidden, rendered)

    def test_transcription_is_a_draft_and_does_not_mutate_workbuddy(self) -> None:
        pcm = b"\x00\x00" * 16000

        result = self.service.create_transcription("op-transcribe", pcm)

        self.assertEqual("transcribed", result["text"])
        self.assertEqual([pcm], self.transcriber.calls)
        self.assertEqual([], self.adapter.calls)

    def test_transcription_is_utf8_bounded(self) -> None:
        self.transcriber.result = "转" * 1000

        result = self.service.create_transcription("op-transcribe", b"\x00\x00" * 16000)

        self.assertLessEqual(len(result["text"].encode("utf-8")), MAX_TRANSCRIPT_BYTES)

    def test_repeated_operation_returns_same_receipt_without_second_mutation(self) -> None:
        action = {
            "version": 1,
            "operation_id": "op-reply-1",
            "type": "reply",
            "message_id": "message-1",
            "text": "Yes, ship it",
        }

        first = self.service.perform_action(action)
        second = self.service.perform_action(dict(action))

        self.assertEqual(first, second)
        self.assertEqual("SUCCEEDED", first["status"])
        self.assertEqual(1, len([call for call in self.adapter.calls if call[0] == "reply"]))

    def test_same_operation_id_with_different_action_is_conflict(self) -> None:
        self.service.perform_action(
            {"version": 1, "operation_id": "op-1", "type": "task_create", "prompt": "first"}
        )

        with self.assertRaises(GatewayError) as caught:
            self.service.perform_action(
                {"version": 1, "operation_id": "op-1", "type": "task_create", "prompt": "different"}
            )

        self.assertEqual("operation_conflict", caught.exception.code)

    def test_retryable_upstream_error_leaves_queryable_pending_receipt(self) -> None:
        self.adapter.retryable_error = True
        action = {
            "version": 1,
            "operation_id": "op-pending",
            "type": "reply",
            "message_id": "m-1",
            "text": "hello",
        }

        with self.assertRaises(GatewayError):
            self.service.perform_action(action)

        result = self.service.get_operation("op-pending")
        self.assertEqual("PENDING", result["status"])
        self.service.perform_action(action)
        self.assertEqual(1, len([call for call in self.adapter.calls if call[0] == "reply"]))

    def test_known_preflight_failure_releases_reservation_for_same_id_retry(self) -> None:
        self.adapter.safe_retryable_error = True
        action = {
            "version": 1,
            "operation_id": "op-auth-retry",
            "type": "reply",
            "message_id": "m-1",
            "text": "hello",
        }

        with self.assertRaises(GatewayError) as caught:
            self.service.perform_action(action)

        self.assertTrue(caught.exception.retryable)
        with self.assertRaises(GatewayError) as missing:
            self.service.get_operation("op-auth-retry")
        self.assertEqual("not_found", missing.exception.code)

        self.adapter.safe_retryable_error = False
        result = self.service.perform_action(action)

        self.assertEqual("SUCCEEDED", result["status"])
        self.assertEqual(
            2,
            len([call for call in self.adapter.calls if call[0] == "reply"]),
        )

    def test_real_oauth_preflight_failure_allows_same_operation_id_after_recovery(self) -> None:
        token_attempts = []
        with RecordingServer() as upstream:
            def token_route(request):
                token_attempts.append(request)
                if len(token_attempts) == 1:
                    return 503, {"Content-Type": "application/json"}, b"{}"
                return (
                    200,
                    {"Content-Type": "application/json"},
                    b'{"access_token":"access","token_type":"Bearer","expires_in":3600}',
                )

            upstream.route("POST", "/openapi/v2/token", token_route)
            upstream.json_route(
                "POST",
                "/openapi/v2/localassistant/message",
                {"code": 0, "data": {"message_id": "reply-receipt"}},
            )
            provider = OAuthTokenProvider(
                upstream.base_url + "/openapi/v2/token",
                "client-id",
                "client-secret",
                "refresh-token",
                allow_insecure_http=True,
            )
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(
                    upstream.base_url,
                    token_provider=provider,
                    allow_insecure_http=True,
                )
            )
            service = GatewayService(
                adapter,
                self.transcriber,
                StateStore(Path(self.temp.name) / "oauth-retry-state.json"),
                clock=lambda: 100.0,
            )
            action = {
                "version": 1,
                "operation_id": "op-real-auth-retry",
                "type": "reply",
                "message_id": "m-1",
                "text": "hello",
            }

            with self.assertRaises(GatewayError) as first:
                service.perform_action(action)
            second = service.perform_action(action)

        self.assertTrue(first.exception.retryable)
        self.assertEqual("SUCCEEDED", second["status"])
        self.assertEqual(2, len(token_attempts))
        self.assertEqual(
            1,
            len(
                [
                    request
                    for request in upstream.requests
                    if request.path == "/openapi/v2/localassistant/message"
                ]
            ),
        )

    def test_post_429_keeps_pending_reservation_and_is_not_resubmitted(self) -> None:
        with RecordingServer() as upstream:
            upstream.json_route(
                "POST",
                "/openapi/v2/localassistant/message",
                {"code": 429, "msg": "busy"},
                status=429,
            )
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(
                    upstream.base_url,
                    "access",
                    allow_insecure_http=True,
                )
            )
            service = GatewayService(
                adapter,
                self.transcriber,
                StateStore(Path(self.temp.name) / "post-429-state.json"),
                clock=lambda: 100.0,
            )
            action = {
                "version": 1,
                "operation_id": "op-post-429",
                "type": "reply",
                "message_id": "m-1",
                "text": "hello",
            }

            with self.assertRaises(GatewayError) as caught:
                service.perform_action(action)
            repeated = service.perform_action(action)

        self.assertTrue(caught.exception.retryable)
        self.assertEqual("PENDING", repeated["status"])
        self.assertEqual(
            1,
            len(
                [
                    request
                    for request in upstream.requests
                    if request.path == "/openapi/v2/localassistant/message"
                ]
            ),
        )

    def test_unhealthy_state_store_keeps_readiness_false_and_blocks_mutation(self) -> None:
        corrupt_path = Path(self.temp.name) / "corrupt-state.json"
        corrupt_path.write_text("not-json", encoding="utf-8")
        corrupt_path.with_name(corrupt_path.name + ".bak").write_text(
            "also-not-json", encoding="utf-8"
        )
        service = GatewayService(
            self.adapter,
            self.transcriber,
            StateStore(corrupt_path),
            clock=lambda: 100.0,
        )

        self.assertFalse(service.is_ready())
        with self.assertRaises(GatewayError) as caught:
            service.perform_action(
                {
                    "version": 1,
                    "operation_id": "op-blocked",
                    "type": "task_create",
                    "prompt": "must not be submitted",
                }
            )

        self.assertEqual("state_corrupt", caught.exception.code)
        self.assertEqual([], self.adapter.calls)

    def test_nonretryable_upstream_error_records_public_failure(self) -> None:
        self.adapter.nonretryable_error = True
        action = {
            "version": 1,
            "operation_id": "op-failed",
            "type": "reply",
            "message_id": "m-1",
            "text": "hello",
        }

        with self.assertRaises(GatewayError):
            self.service.perform_action(action)

        result = self.service.get_operation("op-failed")
        self.assertEqual("FAILED", result["status"])
        self.assertEqual("upstream_rejected", result["error_code"])

    def test_task_followup_fails_explicitly_instead_of_faking_success(self) -> None:
        with self.assertRaises(GatewayError) as caught:
            self.service.perform_action(
                {
                    "version": 1,
                    "operation_id": "op-followup",
                    "type": "task_followup",
                    "task_id": "task-1",
                    "text": "continue",
                }
            )

        self.assertEqual("not_supported", caught.exception.code)
        self.assertEqual("FAILED", self.service.get_operation("op-followup")["status"])

    def test_invalid_actions_are_rejected_before_reservation(self) -> None:
        invalid = (
            {},
            {"version": 1, "operation_id": "op", "type": "unknown", "text": "x"},
            {"version": 1, "operation_id": "op", "type": "reply", "text": "x"},
            {"version": 1, "operation_id": "op", "type": "task_create", "prompt": ""},
            {"version": 1, "operation_id": "op", "type": "task_create", "text": "legacy-field"},
            {"version": 1, "operation_id": "op", "type": "task_create", "prompt": "x", "token": "bad"},
            {"version": 2, "operation_id": "op", "type": "task_create", "prompt": "x"},
            {"operation_id": "op", "type": "task_create", "prompt": "missing version"},
        )
        for action in invalid:
            with self.subTest(action=action):
                with self.assertRaises(GatewayError):
                    self.service.perform_action(action)

    def test_demo_adapter_is_deterministic_and_supports_reply_and_create(self) -> None:
        demo = DemoAdapter()

        first = demo.fetch_snapshot().to_dict()
        second = demo.fetch_snapshot().to_dict()

        self.assertEqual(first, second)
        self.assertTrue(demo.reply("demo-message-1", "ok", "op-1"))
        self.assertTrue(demo.create_task("make a deck", "op-2"))


if __name__ == "__main__":
    unittest.main()
