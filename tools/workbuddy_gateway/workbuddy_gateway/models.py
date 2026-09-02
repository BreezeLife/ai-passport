"""Bounded device-facing gateway models."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence

from .errors import GatewayError


API_VERSION = 1
MAX_COLLECTION_ITEMS = 6
MAX_ID_BYTES = 64
MAX_CURSOR_BYTES = 128
MAX_TITLE_BYTES = 128
MAX_PREVIEW_BYTES = 384
MAX_TRANSCRIPT_BYTES = 512
MAX_ACTION_TEXT_BYTES = 512
MAX_ERROR_BYTES = 160


def _invalid(field: str) -> GatewayError:
    return GatewayError("invalid_request", 400, "Invalid %s" % field, False)


def truncate_utf8(value: str, max_bytes: int) -> str:
    if not isinstance(value, str):
        raise _invalid("text")
    try:
        encoded = value.encode("utf-8", "strict")
    except UnicodeError:
        raise _invalid("text")
    if len(encoded) <= max_bytes:
        return value
    return encoded[:max_bytes].decode("utf-8", "ignore")


def require_identifier(value: Any, field: str, max_bytes: int = MAX_ID_BYTES) -> str:
    if not isinstance(value, str) or not value or value != value.strip():
        raise _invalid(field)
    try:
        size = len(value.encode("utf-8", "strict"))
    except UnicodeError:
        raise _invalid(field)
    if size > max_bytes or any(ord(character) < 0x20 for character in value):
        raise _invalid(field)
    return value


def require_text(value: Any, field: str, max_bytes: int, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise _invalid(field)
    try:
        value.encode("utf-8", "strict")
    except UnicodeError:
        raise _invalid(field)
    if not allow_empty and not value.strip():
        raise _invalid(field)
    if len(value.encode("utf-8")) > max_bytes:
        raise _invalid(field)
    return value


_TASK_STATUSES = {
    "creating": "QUEUED",
    "queued": "QUEUED",
    "pending": "QUEUED",
    "created": "QUEUED",
    "idle": "QUEUED",
    "planning": "RUNNING",
    "running": "RUNNING",
    "working": "RUNNING",
    "processing": "RUNNING",
    "in_progress": "RUNNING",
    "in-progress": "RUNNING",
    "needs_input": "NEEDS_INPUT",
    "needs-input": "NEEDS_INPUT",
    "waiting_user": "NEEDS_INPUT",
    "waiting-user": "NEEDS_INPUT",
    "waiting_for_user": "NEEDS_INPUT",
    "completed": "COMPLETED",
    "complete": "COMPLETED",
    "succeeded": "COMPLETED",
    "success": "COMPLETED",
    "finished": "COMPLETED",
    "failed": "FAILED",
    "error": "FAILED",
    "cancelled": "FAILED",
    "canceled": "FAILED",
    "archived": "FAILED",
    "deleted": "FAILED",
}


def normalize_task_status(value: Any) -> str:
    if not isinstance(value, str):
        raise GatewayError("upstream_schema", 502, "WorkBuddy returned an invalid task status", True)
    normalized = _TASK_STATUSES.get(value.strip().lower())
    if normalized is None:
        raise GatewayError("upstream_schema", 502, "WorkBuddy returned an unknown task status", True)
    return normalized


@dataclass(frozen=True)
class MessageSummary:
    message_id: str
    role: str
    preview: str
    unread: bool

    @classmethod
    def create(cls, message_id: Any, role: Any, preview: Any, unread: Any) -> "MessageSummary":
        clean_id = require_identifier(message_id, "message_id")
        if not isinstance(role, str) or role.lower() not in ("assistant", "user", "system"):
            raise GatewayError("upstream_schema", 502, "WorkBuddy returned an invalid message role", True)
        if not isinstance(preview, str):
            raise GatewayError("upstream_schema", 502, "WorkBuddy returned invalid message content", True)
        return cls(clean_id, role.lower(), truncate_utf8(preview, MAX_PREVIEW_BYTES), bool(unread))

    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.message_id,
            "role": self.role,
            "preview": self.preview,
            "unread": self.unread,
        }


@dataclass(frozen=True)
class TaskSummary:
    task_id: str
    title: str
    status: str
    preview: str

    @classmethod
    def create(cls, task_id: Any, title: Any, status: Any, preview: Any = "") -> "TaskSummary":
        clean_id = require_identifier(task_id, "task_id")
        if not isinstance(title, str) or not isinstance(preview, str):
            raise GatewayError("upstream_schema", 502, "WorkBuddy returned an invalid task", True)
        clean_title = truncate_utf8(title.strip() or "Untitled task", MAX_TITLE_BYTES)
        return cls(
            clean_id,
            clean_title,
            normalize_task_status(status),
            truncate_utf8(preview, MAX_PREVIEW_BYTES),
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.task_id,
            "title": self.title,
            "status": self.status,
            "preview": self.preview,
        }


@dataclass(frozen=True)
class ArtifactSummary:
    artifact_id: str
    task_id: str
    title: str
    kind: str
    description: str

    @classmethod
    def create(
        cls,
        artifact_id: Any,
        task_id: Any,
        title: Any,
        kind: Any,
        description: Any = "",
    ) -> "ArtifactSummary":
        clean_id = require_identifier(artifact_id, "artifact_id")
        clean_task_id = require_identifier(task_id, "task_id")
        if not isinstance(title, str) or not isinstance(description, str):
            raise GatewayError("upstream_schema", 502, "WorkBuddy returned an invalid artifact", True)
        kind_map = {
            "plan": "plan",
            "tasks": "checklist",
            "checklist": "checklist",
            "media": "document",
            "image": "image",
            "document": "document",
            "overview": "overview",
        }
        if not isinstance(kind, str) or kind.lower() not in kind_map:
            raise GatewayError("upstream_schema", 502, "WorkBuddy returned an unknown artifact type", True)
        return cls(
            clean_id,
            clean_task_id,
            truncate_utf8(title.strip() or "Untitled output", MAX_TITLE_BYTES),
            kind_map[kind.lower()],
            truncate_utf8(description, MAX_PREVIEW_BYTES),
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            "id": self.artifact_id,
            "task_id": self.task_id,
            "title": self.title,
            "kind": self.kind,
            "description": self.description,
        }


@dataclass(frozen=True)
class Snapshot:
    cursor: str
    fresh: bool
    assistant_available: bool
    messages: Sequence[MessageSummary]
    tasks: Sequence[TaskSummary]
    artifacts: Sequence[ArtifactSummary]

    @classmethod
    def create(
        cls,
        cursor: Any,
        fresh: Any,
        assistant_available: Any,
        messages: Iterable[MessageSummary],
        tasks: Iterable[TaskSummary],
        artifacts: Iterable[ArtifactSummary],
    ) -> "Snapshot":
        if not isinstance(cursor, str):
            raise GatewayError("upstream_schema", 502, "WorkBuddy returned an invalid cursor", True)
        clean_cursor = truncate_utf8(cursor, MAX_CURSOR_BYTES)
        return cls(
            clean_cursor,
            bool(fresh),
            bool(assistant_available),
            tuple(list(messages)[:MAX_COLLECTION_ITEMS]),
            tuple(list(tasks)[:MAX_COLLECTION_ITEMS]),
            tuple(list(artifacts)[:MAX_COLLECTION_ITEMS]),
        )

    def to_dict(self) -> Dict[str, Any]:
        return {
            "cursor": self.cursor,
            "fresh": self.fresh,
            "assistant_available": self.assistant_available,
            "messages": [message.to_dict() for message in self.messages],
            "tasks": [task.to_dict() for task in self.tasks],
            "artifacts": [artifact.to_dict() for artifact in self.artifacts],
        }


@dataclass(frozen=True)
class OperationRecord:
    operation_id: str
    fingerprint: str
    status: str
    created_at: float
    updated_at: float
    receipt_id: Optional[str] = None
    error_code: Optional[str] = None
    error_message: Optional[str] = None

    def to_storage_dict(self) -> Dict[str, Any]:
        return {
            "operation_id": self.operation_id,
            "fingerprint": self.fingerprint,
            "status": self.status,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
            "receipt_id": self.receipt_id,
            "error_code": self.error_code,
            "error_message": self.error_message,
        }

    def to_device_dict(self) -> Dict[str, Any]:
        result: Dict[str, Any] = {
            "operation_id": self.operation_id,
            "status": self.status,
        }
        if self.receipt_id is not None:
            result["receipt_id"] = self.receipt_id
        if self.error_code is not None:
            result["error_code"] = self.error_code
        if self.error_message is not None:
            result["error"] = truncate_utf8(self.error_message, MAX_ERROR_BYTES)
        return result
