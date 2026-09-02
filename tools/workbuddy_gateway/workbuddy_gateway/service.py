"""Normalized snapshot, draft transcription, and idempotent action service."""

from __future__ import annotations

import hashlib
import json
import threading
import time
from typing import Any, Callable, Dict, Optional, Protocol

from .errors import GatewayError
from .models import (
    MAX_ACTION_TEXT_BYTES,
    MAX_CURSOR_BYTES,
    MAX_TRANSCRIPT_BYTES,
    Snapshot,
    require_identifier,
    require_text,
    truncate_utf8,
)
from .store import StateStore


class WorkBuddyAdapter(Protocol):
    def fetch_snapshot(self, after_cursor: Optional[str] = None) -> Snapshot:
        ...

    def reply(self, message_id: str, text: str, operation_id: str) -> str:
        ...

    def create_task(self, prompt: str, operation_id: str) -> str:
        ...

    def followup_task(self, task_id: str, text: str, operation_id: str) -> str:
        ...


class Transcriber(Protocol):
    def transcribe(self, pcm: bytes) -> str:
        ...


class GatewayService:
    def __init__(
        self,
        adapter: WorkBuddyAdapter,
        transcriber: Transcriber,
        store: StateStore,
        *,
        clock: Callable[[], float] = time.time,
    ) -> None:
        self._adapter = adapter
        self._transcriber = transcriber
        self._store = store
        self._clock = clock
        self._last_upstream_ok = bool(getattr(adapter, "initially_ready", False))
        self._readiness_lock = threading.Lock()

    def get_snapshot(self, after_cursor: Optional[str] = None) -> Dict[str, Any]:
        if after_cursor is not None:
            require_identifier(after_cursor, "cursor", MAX_CURSOR_BYTES)
        try:
            snapshot = self._adapter.fetch_snapshot(after_cursor)
            if not isinstance(snapshot, Snapshot):
                raise GatewayError(
                    "upstream_schema", 502, "WorkBuddy returned an invalid snapshot", True
                )
            with self._readiness_lock:
                self._last_upstream_ok = True
            return snapshot.to_dict()
        except GatewayError:
            with self._readiness_lock:
                self._last_upstream_ok = False
            raise
        except Exception:
            with self._readiness_lock:
                self._last_upstream_ok = False
            raise GatewayError(
                "upstream_failed", 502, "WorkBuddy snapshot could not be loaded", True
            )

    def create_transcription(self, operation_id: str, pcm: bytes) -> Dict[str, Any]:
        clean_id = require_identifier(operation_id, "operation_id")
        try:
            text = self._transcriber.transcribe(pcm)
        except GatewayError:
            raise
        except Exception:
            raise GatewayError(
                "transcription_failed", 502, "Audio transcription failed", True
            )
        if not isinstance(text, str) or not text.strip():
            raise GatewayError(
                "transcription_schema", 502, "Transcription provider returned no text", True
            )
        return {
            "operation_id": clean_id,
            "text": truncate_utf8(text, MAX_TRANSCRIPT_BYTES),
        }

    def perform_action(self, raw_action: Any) -> Dict[str, Any]:
        action = self._validate_action(raw_action)
        operation_id = action["operation_id"]
        fingerprint_payload = {key: value for key, value in action.items() if key != "operation_id"}
        fingerprint = hashlib.sha256(
            json.dumps(
                fingerprint_payload,
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")
        ).hexdigest()
        record, created = self._store.reserve(operation_id, fingerprint, self._clock())
        if not created:
            return record.to_device_dict()

        try:
            action_type = action["type"]
            if action_type == "reply":
                receipt_id = self._adapter.reply(
                    action["message_id"], action["text"], operation_id
                )
            elif action_type == "task_create":
                receipt_id = self._adapter.create_task(action["prompt"], operation_id)
            else:
                receipt_id = self._adapter.followup_task(
                    action["task_id"], action["text"], operation_id
                )
            receipt_id = require_identifier(receipt_id, "receipt_id")
        except GatewayError as error:
            if error.retryable and error.safe_to_retry_operation:
                self._store.release_pending(operation_id, fingerprint)
            elif not error.retryable:
                self._store.finish_failure(
                    operation_id,
                    error.code,
                    error.public_message,
                    self._clock(),
                )
            raise
        except Exception:
            raise GatewayError(
                "upstream_failed",
                502,
                "WorkBuddy action result is unknown; check the operation status",
                True,
            )

        return self._store.finish_success(
            operation_id, receipt_id, self._clock()
        ).to_device_dict()

    def get_operation(self, operation_id: str) -> Dict[str, Any]:
        clean_id = require_identifier(operation_id, "operation_id")
        record = self._store.get(clean_id)
        if record is None:
            raise GatewayError("not_found", 404, "Operation was not found", False)
        return record.to_device_dict()

    def is_ready(self) -> bool:
        with self._readiness_lock:
            upstream_ready = self._last_upstream_ok
        return upstream_ready and self._store.is_healthy()

    @staticmethod
    def _validate_action(raw_action: Any) -> Dict[str, Any]:
        if not isinstance(raw_action, dict):
            raise GatewayError("invalid_request", 400, "Action must be a JSON object", False)
        if type(raw_action.get("version")) is not int or raw_action["version"] != 1:
            raise GatewayError("invalid_request", 400, "Unsupported action version", False)
        action_type = raw_action.get("type")
        if action_type not in ("reply", "task_create", "task_followup"):
            raise GatewayError("invalid_request", 400, "Unsupported action type", False)
        allowed = {
            "reply": {"version", "operation_id", "type", "message_id", "text"},
            "task_create": {"version", "operation_id", "type", "prompt"},
            "task_followup": {"version", "operation_id", "type", "task_id", "text"},
        }[action_type]
        if set(raw_action) != allowed:
            raise GatewayError("invalid_request", 400, "Action fields are invalid", False)
        result: Dict[str, Any] = {
            "version": 1,
            "operation_id": require_identifier(
                raw_action.get("operation_id"), "operation_id"
            ),
            "type": action_type,
        }
        if action_type == "task_create":
            result["prompt"] = require_text(
                raw_action.get("prompt"), "prompt", MAX_ACTION_TEXT_BYTES
            )
        else:
            result["text"] = require_text(
                raw_action.get("text"), "text", MAX_ACTION_TEXT_BYTES
            )
        if action_type == "reply":
            result["message_id"] = require_identifier(
                raw_action.get("message_id"), "message_id"
            )
        elif action_type == "task_followup":
            result["task_id"] = require_identifier(raw_action.get("task_id"), "task_id")
        return result
