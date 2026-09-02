from __future__ import annotations

import json
import hashlib
import math
import tempfile
import unittest
from pathlib import Path

from support import PACKAGE_ROOT  # noqa: F401

from workbuddy_gateway.errors import GatewayError
from workbuddy_gateway.store import StateStore


class StateStoreTests(unittest.TestCase):
    def test_reservation_and_finished_receipt_survive_restart(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            store = StateStore(path)
            record, created = store.reserve("op-1", "fingerprint-1", 10.0)
            self.assertTrue(created)
            self.assertEqual("PENDING", record.status)
            store.finish_success("op-1", "receipt-1", 11.0)

            restored = StateStore(path).get("op-1")

        self.assertIsNotNone(restored)
        assert restored is not None
        self.assertEqual("SUCCEEDED", restored.status)
        self.assertEqual("receipt-1", restored.receipt_id)

    def test_same_fingerprint_is_idempotent_and_collision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = StateStore(Path(directory) / "state.json")
            original, created = store.reserve("op-1", "same", 1.0)
            repeated, repeated_created = store.reserve("op-1", "same", 2.0)

            self.assertTrue(created)
            self.assertFalse(repeated_created)
            self.assertEqual(original, repeated)
            with self.assertRaises(GatewayError) as caught:
                store.reserve("op-1", "different", 3.0)
            self.assertEqual("operation_conflict", caught.exception.code)

    def test_corrupt_primary_recovers_last_valid_atomic_backup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            store = StateStore(path)
            store.reserve("op-old", "old", 1.0)
            store.reserve("op-new", "new", 2.0)
            path.write_text("{corrupt", encoding="utf-8")

            recovered = StateStore(path)

            self.assertIsNotNone(recovered.get("op-old"))
            self.assertIsNone(recovered.get("op-new"))

    def test_checksum_mismatch_is_not_silently_loaded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            store = StateStore(path)
            store.reserve("op-1", "fingerprint", 1.0)
            envelope = json.loads(path.read_text(encoding="utf-8"))
            envelope["payload"]["operations"][0]["status"] = "SUCCEEDED"
            path.write_text(json.dumps(envelope), encoding="utf-8")

            restored = StateStore(path)

            self.assertFalse(restored.is_healthy())
            with self.assertRaises(GatewayError) as caught:
                restored.get("op-1")
            self.assertEqual("state_corrupt", caught.exception.code)

    def test_double_corruption_fails_closed_instead_of_starting_an_empty_store(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            path.write_text("{broken", encoding="utf-8")
            path.with_name(path.name + ".bak").write_text("[]", encoding="utf-8")

            store = StateStore(path)

            self.assertFalse(store.is_healthy())
            with self.assertRaises(GatewayError) as caught:
                store.reserve("op-1", "fingerprint", 1.0)
            self.assertEqual("state_corrupt", caught.exception.code)

    def test_receipt_count_is_bounded_and_oldest_is_pruned(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = StateStore(Path(directory) / "state.json", max_operations=2)
            store.reserve("op-1", "a", 1.0)
            store.finish_success("op-1", "receipt-1", 1.1)
            store.reserve("op-2", "b", 2.0)
            store.finish_success("op-2", "receipt-2", 2.1)
            store.reserve("op-3", "c", 3.0)

            self.assertIsNone(store.get("op-1"))
            self.assertIsNotNone(store.get("op-2"))
            self.assertIsNotNone(store.get("op-3"))

    def test_pending_operations_are_never_pruned_to_make_room(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = StateStore(Path(directory) / "state.json", max_operations=2)
            store.reserve("op-1", "a", 1.0)
            store.reserve("op-2", "b", 2.0)

            with self.assertRaises(GatewayError) as caught:
                store.reserve("op-3", "c", 3.0)

            self.assertEqual("operation_capacity", caught.exception.code)
            self.assertIsNotNone(store.get("op-1"))
            self.assertIsNotNone(store.get("op-2"))
            self.assertIsNone(store.get("op-3"))

    def test_known_unsubmitted_operation_can_release_its_pending_reservation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            store = StateStore(Path(directory) / "state.json")
            store.reserve("op-1", "same", 1.0)

            store.release_pending("op-1", "same")
            replacement, created = store.reserve("op-1", "same", 2.0)

            self.assertTrue(created)
            self.assertEqual("PENDING", replacement.status)

    def test_rotated_refresh_token_is_scoped_and_survives_restart_with_private_mode(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            store = StateStore(path)

            store.set_refresh_token("client-a", "rotated-refresh-secret")
            restored = StateStore(path)

            self.assertEqual(
                "rotated-refresh-secret",
                restored.get_refresh_token("client-a"),
            )
            self.assertIsNone(restored.get_refresh_token("client-b"))
            self.assertEqual(0o600, path.stat().st_mode & 0o777)

    def test_latest_rotated_refresh_token_survives_primary_corruption(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            store = StateStore(path)
            store.set_refresh_token("client-a", "rotated-refresh-1")
            store.set_refresh_token("client-a", "rotated-refresh-2")
            path.write_text("{corrupt", encoding="utf-8")

            restored = StateStore(path)

            self.assertTrue(restored.is_healthy())
            self.assertEqual(
                "rotated-refresh-2",
                restored.get_refresh_token("client-a"),
            )

    def test_checksum_valid_but_invalid_operation_records_fail_closed(self) -> None:
        invalid_mutations = (
            lambda record: record.update({"unexpected": True}),
            lambda record: record.update({"fingerprint": "x" * 129}),
            lambda record: record.update({"created_at": math.nan}),
            lambda record: record.update({"status": "SUCCEEDED", "receipt_id": None}),
            lambda record: record.update(
                {
                    "status": "FAILED",
                    "receipt_id": "must-not-exist",
                    "error_code": "bad",
                    "error_message": "bad",
                }
            ),
            lambda record: record.update(
                {
                    "status": "PENDING",
                    "receipt_id": "must-not-exist",
                }
            ),
        )
        for index, mutate in enumerate(invalid_mutations):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "state.json"
                store = StateStore(path)
                store.reserve("op-1", "fingerprint", 1.0)
                envelope = json.loads(path.read_text(encoding="utf-8"))
                mutate(envelope["payload"]["operations"][0])
                payload_bytes = json.dumps(
                    envelope["payload"],
                    ensure_ascii=False,
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode("utf-8")
                envelope["sha256"] = hashlib.sha256(payload_bytes).hexdigest()
                path.write_text(json.dumps(envelope), encoding="utf-8")

                restored = StateStore(path)

                self.assertFalse(restored.is_healthy())

    def test_valid_checksum_with_invalid_record_schema_recovers_backup(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            store = StateStore(path)
            store.reserve("op-old", "old", 1.0)
            store.reserve("op-new", "new", 2.0)
            envelope = json.loads(path.read_text(encoding="utf-8"))
            envelope["payload"]["operations"][0]["operation_id"] = ""
            payload_bytes = json.dumps(
                envelope["payload"],
                ensure_ascii=False,
                sort_keys=True,
                separators=(",", ":"),
            ).encode("utf-8")
            envelope["sha256"] = hashlib.sha256(payload_bytes).hexdigest()
            path.write_text(json.dumps(envelope), encoding="utf-8")

            recovered = StateStore(path)

            self.assertIsNotNone(recovered.get("op-old"))
            self.assertIsNone(recovered.get("op-new"))


if __name__ == "__main__":
    unittest.main()
