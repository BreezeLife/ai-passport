"""Crash-tolerant, bounded operation receipt storage."""

from __future__ import annotations

import hashlib
import hmac
import json
import math
import os
import tempfile
import threading
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .errors import GatewayError
from .models import MAX_ERROR_BYTES, OperationRecord, require_identifier, truncate_utf8


STORE_VERSION = 1
MAX_FINGERPRINT_BYTES = 128
MAX_REFRESH_TOKEN_BYTES = 8192


class StateStore:
    def __init__(self, path: Path, max_operations: int = 128) -> None:
        self.path = Path(path)
        self.backup_path = self.path.with_name(self.path.name + ".bak")
        if max_operations < 1 or max_operations > 4096:
            raise ValueError("max_operations out of range")
        self.max_operations = max_operations
        self._lock = threading.RLock()
        self._operations: Dict[str, OperationRecord] = {}
        self._refresh_client_hash: Optional[str] = None
        self._refresh_token: Optional[str] = None
        self._healthy = True
        self._load()

    def is_healthy(self) -> bool:
        with self._lock:
            return self._healthy

    def get(self, operation_id: str) -> Optional[OperationRecord]:
        with self._lock:
            self._ensure_healthy()
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
            self._ensure_healthy()
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
            self._ensure_healthy()
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
            self._ensure_healthy()
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

    def release_pending(self, operation_id: str, fingerprint: str) -> None:
        """Remove only a matching reservation known not to have reached WorkBuddy."""

        clean_id = require_identifier(operation_id, "operation_id")
        with self._lock:
            self._ensure_healthy()
            current = self._require_existing(clean_id)
            if current.fingerprint != fingerprint:
                raise GatewayError(
                    "operation_conflict",
                    409,
                    "Operation ID was already used for a different action",
                    False,
                )
            if current.status != "PENDING":
                return
            del self._operations[clean_id]
            try:
                self._save()
            except GatewayError:
                self._operations[clean_id] = current
                raise

    def get_refresh_token(self, client_id: str) -> Optional[str]:
        client_hash = self._client_hash(client_id)
        with self._lock:
            self._ensure_healthy()
            if self._refresh_client_hash is None or self._refresh_token is None:
                return None
            if not hmac.compare_digest(self._refresh_client_hash, client_hash):
                return None
            return self._refresh_token

    def set_refresh_token(self, client_id: str, refresh_token: str) -> None:
        client_hash = self._client_hash(client_id)
        clean_token = self._validate_refresh_token(refresh_token)
        with self._lock:
            self._ensure_healthy()
            self._refresh_client_hash = client_hash
            self._refresh_token = clean_token
            try:
                self._save()
                self._sync_refresh_backup()
            except GatewayError:
                self._healthy = False
                raise

    def _ensure_healthy(self) -> None:
        if not self._healthy:
            raise GatewayError(
                "state_corrupt",
                503,
                "Gateway state requires operator recovery",
                False,
            )

    @staticmethod
    def _client_hash(client_id: str) -> str:
        clean_id = require_identifier(client_id, "client_id", 512)
        return hashlib.sha256(clean_id.encode("utf-8")).hexdigest()

    @staticmethod
    def _validate_refresh_token(value: Any) -> str:
        if not isinstance(value, str) or not value:
            raise GatewayError(
                "invalid_configuration", 500, "WorkBuddy refresh token is invalid", False
            )
        try:
            encoded = value.encode("utf-8", "strict")
        except UnicodeError:
            raise GatewayError(
                "invalid_configuration", 500, "WorkBuddy refresh token is invalid", False
            )
        if len(encoded) > MAX_REFRESH_TOKEN_BYTES or "\x00" in value:
            raise GatewayError(
                "invalid_configuration", 500, "WorkBuddy refresh token is invalid", False
            )
        return value

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
        return self._envelope_for(
            self._operations,
            self._refresh_client_hash,
            self._refresh_token,
        )

    def _envelope_for(
        self,
        operations_by_id: Dict[str, OperationRecord],
        refresh_client_hash: Optional[str],
        refresh_token: Optional[str],
    ) -> Dict[str, Any]:
        operations = sorted(
            (record.to_storage_dict() for record in operations_by_id.values()),
            key=lambda item: item["operation_id"],
        )
        payload: Dict[str, Any] = {"operations": operations}
        if refresh_client_hash is not None and refresh_token is not None:
            payload["oauth_refresh"] = {
                "client_id_sha256": refresh_client_hash,
                "refresh_token": refresh_token,
            }
        checksum = hashlib.sha256(self._payload_bytes(payload)).hexdigest()
        return {"version": STORE_VERSION, "payload": payload, "sha256": checksum}

    def _sync_refresh_backup(self) -> None:
        """Keep the latest one-time rotating credential in both valid state slots."""

        loaded_backup = self._read_file(self.backup_path)
        backup_operations = (
            loaded_backup[0] if loaded_backup is not None else dict(self._operations)
        )
        envelope = self._envelope_for(
            backup_operations,
            self._refresh_client_hash,
            self._refresh_token,
        )
        encoded = self._payload_bytes(envelope) + b"\n"
        temporary_name: Optional[str] = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="wb",
                prefix=self.backup_path.name + ".",
                suffix=".tmp",
                dir=str(self.path.parent),
                delete=False,
            ) as temporary:
                temporary_name = temporary.name
                os.chmod(temporary.name, 0o600)
                temporary.write(encoded)
                temporary.flush()
                os.fsync(temporary.fileno())
            os.replace(temporary_name, str(self.backup_path))
            temporary_name = None
            self._fsync_directory()
        except OSError:
            raise GatewayError(
                "state_write_failed",
                500,
                "Gateway state could not be saved",
                True,
                safe_to_retry_operation=True,
            )
        finally:
            if temporary_name is not None:
                try:
                    os.unlink(temporary_name)
                except OSError:
                    pass

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
            raise GatewayError(
                "state_write_failed",
                500,
                "Gateway state could not be saved",
                True,
                safe_to_retry_operation=True,
            )
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
        primary_exists = self.path.exists()
        backup_exists = self.backup_path.exists()
        loaded = self._read_file(self.path)
        if loaded is not None:
            self._install_loaded_state(loaded)
            return
        backup = self._read_file(self.backup_path)
        if backup is not None:
            self._install_loaded_state(backup)
            try:
                os.replace(str(self.backup_path), str(self.path))
                self._fsync_directory()
            except OSError:
                pass
            return
        if primary_exists or backup_exists:
            self._healthy = False

    def _install_loaded_state(
        self,
        loaded: Tuple[Dict[str, OperationRecord], Optional[str], Optional[str]],
    ) -> None:
        self._operations, self._refresh_client_hash, self._refresh_token = loaded

    def _read_file(
        self, path: Path
    ) -> Optional[Tuple[Dict[str, OperationRecord], Optional[str], Optional[str]]]:
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
            refresh_client_hash: Optional[str] = None
            refresh_token: Optional[str] = None
            raw_oauth = payload.get("oauth_refresh")
            if raw_oauth is not None:
                if not isinstance(raw_oauth, dict) or set(raw_oauth) != {
                    "client_id_sha256",
                    "refresh_token",
                }:
                    return None
                raw_hash = raw_oauth.get("client_id_sha256")
                if (
                    not isinstance(raw_hash, str)
                    or len(raw_hash) != 64
                    or any(character not in "0123456789abcdef" for character in raw_hash)
                ):
                    return None
                refresh_client_hash = raw_hash
                refresh_token = self._validate_refresh_token(raw_oauth.get("refresh_token"))
            return result, refresh_client_hash, refresh_token
        except (OSError, UnicodeError, ValueError, TypeError, KeyError, GatewayError):
            return None

    @staticmethod
    def _parse_record(raw: Any) -> OperationRecord:
        if not isinstance(raw, dict):
            raise ValueError("record must be an object")
        if set(raw) != {
            "operation_id",
            "fingerprint",
            "status",
            "created_at",
            "updated_at",
            "receipt_id",
            "error_code",
            "error_message",
        }:
            raise ValueError("invalid record fields")
        operation_id = require_identifier(raw["operation_id"], "operation_id")
        fingerprint = raw["fingerprint"]
        status = raw["status"]
        if (
            not isinstance(fingerprint, str)
            or not fingerprint
            or len(fingerprint.encode("utf-8", "strict")) > MAX_FINGERPRINT_BYTES
        ):
            raise ValueError("invalid fingerprint")
        if status not in ("PENDING", "SUCCEEDED", "FAILED"):
            raise ValueError("invalid status")
        created_at = raw["created_at"]
        updated_at = raw["updated_at"]
        if (
            isinstance(created_at, bool)
            or not isinstance(created_at, (int, float))
            or not math.isfinite(created_at)
            or isinstance(updated_at, bool)
            or not isinstance(updated_at, (int, float))
            or not math.isfinite(updated_at)
            or updated_at < created_at
        ):
            raise ValueError("invalid timestamps")
        receipt_id = raw.get("receipt_id")
        error_code = raw.get("error_code")
        error_message = raw.get("error_message")
        if receipt_id is not None:
            receipt_id = require_identifier(receipt_id, "receipt_id")
        if error_code is not None:
            error_code = require_identifier(error_code, "error_code")
        if error_message is not None:
            if (
                not isinstance(error_message, str)
                or len(error_message.encode("utf-8", "strict")) > MAX_ERROR_BYTES
            ):
                raise ValueError("invalid error message")
        if status == "PENDING" and any(
            value is not None for value in (receipt_id, error_code, error_message)
        ):
            raise ValueError("pending record has a result")
        if status == "SUCCEEDED" and (
            receipt_id is None or error_code is not None or error_message is not None
        ):
            raise ValueError("successful record is incomplete")
        if status == "FAILED" and (
            receipt_id is not None or error_code is None or error_message is None
        ):
            raise ValueError("failed record is incomplete")
        return OperationRecord(
            operation_id=operation_id,
            fingerprint=fingerprint,
            status=status,
            created_at=float(created_at),
            updated_at=float(updated_at),
            receipt_id=receipt_id,
            error_code=error_code,
            error_message=error_message,
        )
