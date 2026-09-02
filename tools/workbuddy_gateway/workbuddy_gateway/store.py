"""Crash-tolerant, bounded operation receipt storage."""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import tempfile
import threading
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .errors import GatewayError
from .models import MAX_ERROR_BYTES, OperationRecord, require_identifier, truncate_utf8


STORE_VERSION = 1
MAX_FINGERPRINT_BYTES = 128


class StateStore:
    def __init__(self, path: Path, max_operations: int = 128) -> None:
        self.path = Path(path)
        self.backup_path = self.path.with_name(self.path.name + ".bak")
        if max_operations < 1 or max_operations > 4096:
            raise ValueError("max_operations out of range")
        self.max_operations = max_operations
        self._lock = threading.RLock()
        self._operations: Dict[str, OperationRecord] = {}
        self._load()

    def get(self, operation_id: str) -> Optional[OperationRecord]:
        with self._lock:
            return self._operations.get(operation_id)

    def reserve(
        self,
        operation_id: str,
        fingerprint: str,
        now: float,
    ) -> Tuple[OperationRecord, bool]:
        clean_id = require_identifier(operation_id, "operation_id")
        if (
            not isinstance(fingerprint, str)
            or not fingerprint
            or len(fingerprint.encode("utf-8", "strict")) > MAX_FINGERPRINT_BYTES
        ):
            raise GatewayError("invalid_request", 400, "Invalid operation fingerprint", False)
        with self._lock:
            existing = self._operations.get(clean_id)
            if existing is not None:
                if existing.fingerprint != fingerprint:
                    raise GatewayError(
                        "operation_conflict",
                        409,
                        "Operation ID was already used for a different action",
                        False,
                    )
                return existing, False
            before = dict(self._operations)
            record = OperationRecord(
                operation_id=clean_id,
                fingerprint=fingerprint,
                status="PENDING",
                created_at=float(now),
                updated_at=float(now),
            )
            self._operations[clean_id] = record
            try:
                self._prune()
                self._save()
            except GatewayError:
                self._operations = before
                raise
            return record, True

    def finish_success(self, operation_id: str, receipt_id: str, now: float) -> OperationRecord:
        clean_receipt = require_identifier(receipt_id, "receipt_id")
        with self._lock:
            current = self._require_existing(operation_id)
            updated = OperationRecord(
                operation_id=current.operation_id,
                fingerprint=current.fingerprint,
                status="SUCCEEDED",
                created_at=current.created_at,
                updated_at=float(now),
                receipt_id=clean_receipt,
            )
            self._operations[operation_id] = updated
            try:
                self._save()
            except GatewayError:
                self._operations[operation_id] = current
                raise
            return updated

    def finish_failure(
        self,
        operation_id: str,
        error_code: str,
        public_message: str,
        now: float,
    ) -> OperationRecord:
        clean_code = require_identifier(error_code, "error_code")
        with self._lock:
            current = self._require_existing(operation_id)
            updated = OperationRecord(
                operation_id=current.operation_id,
                fingerprint=current.fingerprint,
                status="FAILED",
                created_at=current.created_at,
                updated_at=float(now),
                error_code=clean_code,
                error_message=truncate_utf8(public_message, MAX_ERROR_BYTES),
            )
            self._operations[operation_id] = updated
            try:
                self._save()
            except GatewayError:
                self._operations[operation_id] = current
                raise
            return updated

    def _require_existing(self, operation_id: str) -> OperationRecord:
        current = self._operations.get(operation_id)
        if current is None:
            raise GatewayError("operation_not_found", 404, "Operation was not found", False)
        return current

    def _prune(self) -> None:
        overflow = len(self._operations) - self.max_operations
        if overflow <= 0:
            return
        oldest = sorted(
            (
                record
                for record in self._operations.values()
                if record.status != "PENDING"
            ),
            key=lambda record: (record.updated_at, record.operation_id),
        )
        if len(oldest) < overflow:
            raise GatewayError(
                "operation_capacity",
                503,
                "Too many operations are still pending",
                True,
            )
        oldest = oldest[:overflow]
        for record in oldest:
            del self._operations[record.operation_id]

    @staticmethod
    def _payload_bytes(payload: Dict[str, Any]) -> bytes:
        return json.dumps(
            payload,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")

    def _envelope(self) -> Dict[str, Any]:
        operations = sorted(
            (record.to_storage_dict() for record in self._operations.values()),
            key=lambda item: item["operation_id"],
        )
        payload = {"operations": operations}
        checksum = hashlib.sha256(self._payload_bytes(payload)).hexdigest()
        return {"version": STORE_VERSION, "payload": payload, "sha256": checksum}

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        encoded = self._payload_bytes(self._envelope()) + b"\n"
        temporary_name: Optional[str] = None
        moved_primary = False
        try:
            with tempfile.NamedTemporaryFile(
                mode="wb",
                prefix=self.path.name + ".",
                suffix=".tmp",
                dir=str(self.path.parent),
                delete=False,
            ) as temporary:
                temporary_name = temporary.name
                os.chmod(temporary.name, 0o600)
                temporary.write(encoded)
                temporary.flush()
                os.fsync(temporary.fileno())
            if self.path.exists():
                os.replace(str(self.path), str(self.backup_path))
                moved_primary = True
            os.replace(temporary_name, str(self.path))
            temporary_name = None
            self._fsync_directory()
        except OSError:
            if moved_primary and self.backup_path.exists() and not self.path.exists():
                try:
                    os.replace(str(self.backup_path), str(self.path))
                except OSError:
                    pass
            raise GatewayError("state_write_failed", 500, "Gateway state could not be saved", True)
        finally:
            if temporary_name is not None:
                try:
                    os.unlink(temporary_name)
                except OSError:
                    pass

    def _fsync_directory(self) -> None:
        try:
            descriptor = os.open(str(self.path.parent), os.O_RDONLY)
        except OSError:
            return
        try:
            os.fsync(descriptor)
        except OSError:
            pass
        finally:
            os.close(descriptor)

    def _load(self) -> None:
        loaded = self._read_file(self.path)
        if loaded is not None:
            self._operations = loaded
            return
        backup = self._read_file(self.backup_path)
        if backup is not None:
            self._operations = backup
            try:
                os.replace(str(self.backup_path), str(self.path))
                self._fsync_directory()
            except OSError:
                pass

    def _read_file(self, path: Path) -> Optional[Dict[str, OperationRecord]]:
        if not path.exists():
            return None
        try:
            envelope = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(envelope, dict) or envelope.get("version") != STORE_VERSION:
                return None
            payload = envelope.get("payload")
            checksum = envelope.get("sha256")
            if not isinstance(payload, dict) or not isinstance(checksum, str):
                return None
            expected = hashlib.sha256(self._payload_bytes(payload)).hexdigest()
            if not hmac.compare_digest(checksum, expected):
                return None
            raw_operations = payload.get("operations")
            if not isinstance(raw_operations, list) or len(raw_operations) > self.max_operations:
                return None
            result: Dict[str, OperationRecord] = {}
            for raw in raw_operations:
                record = self._parse_record(raw)
                if record.operation_id in result:
                    return None
                result[record.operation_id] = record
            return result
        except (OSError, UnicodeError, ValueError, TypeError, KeyError, GatewayError):
            return None

    @staticmethod
    def _parse_record(raw: Any) -> OperationRecord:
        if not isinstance(raw, dict):
            raise ValueError("record must be an object")
        operation_id = require_identifier(raw["operation_id"], "operation_id")
        fingerprint = raw["fingerprint"]
        status = raw["status"]
        if not isinstance(fingerprint, str) or not fingerprint:
            raise ValueError("invalid fingerprint")
        if status not in ("PENDING", "SUCCEEDED", "FAILED"):
            raise ValueError("invalid status")
        receipt_id = raw.get("receipt_id")
        error_code = raw.get("error_code")
        error_message = raw.get("error_message")
        if receipt_id is not None:
            receipt_id = require_identifier(receipt_id, "receipt_id")
        if error_code is not None:
            error_code = require_identifier(error_code, "error_code")
        if error_message is not None and not isinstance(error_message, str):
            raise ValueError("invalid error message")
        return OperationRecord(
            operation_id=operation_id,
            fingerprint=fingerprint,
            status=status,
            created_at=float(raw["created_at"]),
            updated_at=float(raw["updated_at"]),
            receipt_id=receipt_id,
            error_code=error_code,
            error_message=error_message,
        )
