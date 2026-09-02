"""Deterministic credential-free WorkBuddy demo adapter."""

from __future__ import annotations

from typing import Optional

from .errors import GatewayError
from .models import ArtifactSummary, MessageSummary, Snapshot, TaskSummary


class DemoAdapter:
    initially_ready = True

    def fetch_snapshot(self, after_cursor: Optional[str] = None) -> Snapshot:
        del after_cursor
        return Snapshot.create(
            "demo-1",
            True,
            True,
            (
                MessageSummary.create(
                    "demo-message-1",
                    "assistant",
                    "The quarterly brief is ready for review.",
                    True,
                ),
                MessageSummary.create(
                    "demo-message-2",
                    "user",
                    "Please turn the findings into five slides.",
                    False,
                ),
            ),
            (
                TaskSummary.create(
                    "demo-task-1",
                    "Quarterly brief",
                    "completed",
                    "Research and summary are complete.",
                ),
                TaskSummary.create(
                    "demo-task-2",
                    "Launch slides",
                    "working",
                    "Building the first draft.",
                ),
            ),
            (
                ArtifactSummary.create(
                    "demo-artifact-1",
                    "demo-task-1",
                    "Quarterly overview",
                    "overview",
                    "A concise summary is ready.",
                ),
            ),
        )

    def reply(self, message_id: str, text: str, operation_id: str) -> str:
        del message_id, text
        return "demo-reply-" + operation_id

    def create_task(self, prompt: str, operation_id: str) -> str:
        del prompt
        return "demo-task-" + operation_id

    def followup_task(self, task_id: str, text: str, operation_id: str) -> str:
        del task_id, text, operation_id
        raise GatewayError(
            "not_supported",
            501,
            "Task follow-up requires ACP and is not supported in this release",
            False,
        )
