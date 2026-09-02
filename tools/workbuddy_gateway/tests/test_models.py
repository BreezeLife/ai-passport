from __future__ import annotations

import unittest

from support import PACKAGE_ROOT  # noqa: F401

from workbuddy_gateway.errors import GatewayError
from workbuddy_gateway.models import (
    MAX_COLLECTION_ITEMS,
    MAX_ID_BYTES,
    MAX_PREVIEW_BYTES,
    MAX_TITLE_BYTES,
    ArtifactSummary,
    MessageSummary,
    Snapshot,
    TaskSummary,
    normalize_task_status,
    require_identifier,
    truncate_utf8,
)


class ModelLimitTests(unittest.TestCase):
    def test_utf8_truncation_never_splits_a_character(self) -> None:
        value = "A" + "你" * 200

        result = truncate_utf8(value, 10)

        self.assertEqual("A你你你", result)
        self.assertLessEqual(len(result.encode("utf-8")), 10)

    def test_identifier_rejects_empty_non_string_and_oversize_values(self) -> None:
        for value in ("", None, 123, "x" * (MAX_ID_BYTES + 1)):
            with self.subTest(value=repr(value)[:20]):
                with self.assertRaises(GatewayError):
                    require_identifier(value, "id")

    def test_task_status_normalization_is_closed(self) -> None:
        cases = {
            "pending": "QUEUED",
            "created": "QUEUED",
            "processing": "RUNNING",
            "working": "RUNNING",
            "needs_input": "NEEDS_INPUT",
            "waiting-user": "NEEDS_INPUT",
            "completed": "COMPLETED",
            "success": "COMPLETED",
            "error": "FAILED",
            "cancelled": "FAILED",
        }
        for raw, expected in cases.items():
            with self.subTest(raw=raw):
                self.assertEqual(expected, normalize_task_status(raw))
        with self.assertRaises(GatewayError):
            normalize_task_status("teleporting")

    def test_snapshot_enforces_collection_and_unicode_byte_caps(self) -> None:
        messages = [
            MessageSummary.create("m-%d" % index, "assistant", "消" * 500, True)
            for index in range(MAX_COLLECTION_ITEMS + 3)
        ]
        tasks = [TaskSummary.create("t-1", "题" * 200, "running", "摘" * 500)]
        artifacts = [
            ArtifactSummary.create("a-1", "t-1", "文" * 200, "plan", "述" * 500)
        ]

        snapshot = Snapshot.create("cursor", True, True, messages, tasks, artifacts).to_dict()

        self.assertEqual(MAX_COLLECTION_ITEMS, len(snapshot["messages"]))
        self.assertLessEqual(len(snapshot["messages"][0]["preview"].encode("utf-8")), MAX_PREVIEW_BYTES)
        self.assertLessEqual(len(snapshot["tasks"][0]["title"].encode("utf-8")), MAX_TITLE_BYTES)
        self.assertLessEqual(len(snapshot["artifacts"][0]["description"].encode("utf-8")), MAX_PREVIEW_BYTES)

    def test_device_dictionaries_have_an_allowlisted_shape(self) -> None:
        snapshot = Snapshot.create(
            "c-1",
            True,
            True,
            [MessageSummary.create("m-1", "assistant", "hello", True)],
            [TaskSummary.create("t-1", "task", "completed", "done")],
            [ArtifactSummary.create("a-1", "t-1", "report", "overview", "ready")],
        ).to_dict()

        rendered = repr(snapshot).lower()
        for forbidden in ("token", "sandbox", "refresh", "client_secret", "link", "url"):
            self.assertNotIn(forbidden, rendered)

    def test_official_artifact_types_map_to_firmware_output_kinds(self) -> None:
        self.assertEqual(
            "checklist",
            ArtifactSummary.create("a-1", "t-1", "Tasks", "tasks", "").kind,
        )
        self.assertEqual(
            "document",
            ArtifactSummary.create("a-2", "t-1", "Media", "media", "").kind,
        )


if __name__ == "__main__":
    unittest.main()
