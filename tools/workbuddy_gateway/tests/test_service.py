from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from typing import List, Optional

from support import PACKAGE_ROOT  # noqa: F401

from workbuddy_gateway.demo_adapter import DemoAdapter
from workbuddy_gateway.errors import GatewayError
from workbuddy_gateway.models import MAX_TRANSCRIPT_BYTES, Snapshot
from workbuddy_gateway.service import GatewayService
from workbuddy_gateway.store import StateStore


class FakeAdapter:
    def __init__(self) -> None:
        self.calls: List[tuple] = []
        self.retryable_error = False
        self.nonretryable_error = False

    def fetch_snapshot(self, after_cursor: Optional[str] = None) -> Snapshot:
        self.calls.append(("snapshot", after_cursor))
        return DemoAdapter().fetch_snapshot(after_cursor)

    def reply(self, message_id: str, text: str, operation_id: str) -> str:
        self.calls.append(("reply", message_id, text, operation_id))
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
