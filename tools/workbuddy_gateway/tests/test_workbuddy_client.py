from __future__ import annotations

import json
import socket
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest import mock
from urllib.parse import urlsplit

from support import PACKAGE_ROOT, RecordingServer  # noqa: F401

from workbuddy_gateway import workbuddy_client as workbuddy_client_module
from workbuddy_gateway.errors import GatewayError
from workbuddy_gateway.store import StateStore
from workbuddy_gateway.workbuddy_client import (
    LiveWorkBuddyAdapter,
    OAuthTokenProvider,
    WorkBuddyClient,
    resolve_artifact_endpoint,
)



class WorkBuddyClientTests(unittest.TestCase):
    def test_live_client_uses_only_documented_message_and_task_paths(self) -> None:
        with RecordingServer() as upstream:
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant",
                {"code": 0, "msg": "ok", "request_id": "status-1", "data": {"online": True}},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant/message?message_id=m-old&limit=6",
                {
                    "code": 0,
                    "msg": "ok",
                    "request_id": "upstream-1",
                    "data": {
                        "messages": [
                            {
                                "message_id": "m-new",
                                "role": "assistant",
                                "content": ["New message"],
                                "is_read": False,
                            }
                        ]
                    },
                },
            )
            upstream.json_route(
                "POST",
                "/openapi/v2/localassistant/message",
                {"code": 0, "data": {"message_id": "reply-1"}},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                {
                    "tasks": [{"task_id": "task-1", "name": "Deck", "status": "working"}],
                    "total": 1,
                    "pagination": {"page": 1, "size": 6, "total": 1},
                },
            )
            upstream.json_route(
                "POST",
                "/openapi/v2/tasks",
                {
                    "task_id": "task-new",
                    "status": "pending",
                    "name": "New task",
                    "link": "https://secret.example/acp",
                    "token": "task-ticket-secret",
                    "sandboxLink": "https://secret.example",
                },
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks/task-1",
                {"task_id": "task-1", "status": "working"},
            )
            client = WorkBuddyClient(
                upstream.base_url,
                "upstream-access-secret",
                allow_insecure_http=True,
            )

            online = client.local_assistant_online()
            messages = client.list_messages("m-old", 6)
            reply_id = client.send_message("Ship it")
            tasks = client.list_tasks(size=6)
            created = client.create_task("Make a deck")
            detail = client.get_task("task-1")

        self.assertTrue(online)
        self.assertEqual("m-new", messages["messages"][0]["message_id"])
        self.assertEqual("reply-1", reply_id)
        self.assertEqual("task-1", tasks["tasks"][0]["task_id"])
        self.assertEqual("task-new", created["task_id"])
        self.assertEqual("task-1", detail["task_id"])
        self.assertEqual(
            {
                "content": "Ship it",
                "msg_type": "text",
            },
            upstream.requests[2].json(),
        )
        self.assertEqual({"prompt": "Make a deck"}, upstream.requests[4].json())
        self.assertEqual(
            {"Bearer upstream-access-secret"},
            {request.headers["authorization"] for request in upstream.requests},
        )
        self.assertEqual(
            {
                "/openapi/v2/localassistant/message",
                "/openapi/v2/localassistant",
                "/openapi/v2/tasks",
                "/openapi/v2/tasks/task-1",
            },
            {urlsplit(request.path).path for request in upstream.requests},
        )

    def test_content_must_be_an_array_and_only_text_blocks_become_preview(self) -> None:
        with RecordingServer() as upstream:
            upstream.json_route("GET", "/openapi/v2/localassistant", {"code": 0, "data": {"online": True}})
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant/message?limit=6",
                {
                    "code": 0,
                    "data": {
                        "messages": [
                            {
                                "message_id": "m-1",
                                "role": "assistant",
                                "content": [
                                    {"type": "text", "text": "First"},
                                    {"type": "image", "url": "https://private.example/x"},
                                    {"type": "text", "text": "second"},
                                ],
                            }
                        ]
                    },
                },
            )
            upstream.json_route("GET", "/openapi/v2/tasks?page=1&size=6", {"tasks": [], "total": 0, "pagination": {"page": 1, "size": 6, "total": 0}})
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(upstream.base_url, "access", allow_insecure_http=True)
            )

            snapshot = adapter.fetch_snapshot().to_dict()

        self.assertEqual("First\nsecond", snapshot["messages"][0]["preview"])
        self.assertNotIn("private.example", repr(snapshot))

        with RecordingServer() as upstream:
            upstream.json_route("GET", "/openapi/v2/localassistant", {"code": 0, "data": {"online": True}})
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant/message?limit=6",
                {
                    "code": 0,
                    "data": {
                        "messages": [
                            {"message_id": "m-1", "role": "assistant", "content": "not-an-array"}
                        ]
                    },
                },
            )
            upstream.json_route("GET", "/openapi/v2/tasks?page=1&size=6", {"tasks": [], "total": 0, "pagination": {"page": 1, "size": 6, "total": 0}})
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(upstream.base_url, "access", allow_insecure_http=True)
            )
            with self.assertRaises(GatewayError) as caught:
                adapter.fetch_snapshot()
        self.assertEqual("upstream_schema", caught.exception.code)

    def test_incremental_history_keeps_recent_messages_and_uses_an_internal_upstream_cursor(self) -> None:
        with RecordingServer() as upstream:
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant",
                {"code": 0, "data": {"online": True}},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant/message?limit=6",
                {
                    "code": 0,
                    "data": {
                        "messages": [
                            {
                                "message_id": "m-1",
                                "role": "assistant",
                                "content": ["first"],
                            }
                        ]
                    },
                },
            )
            increments = [
                {"code": 0, "data": {"messages": []}},
                {
                    "code": 0,
                    "data": {
                        "messages": [
                            {
                                "message_id": "m-2",
                                "role": "assistant",
                                "content": ["second"],
                            }
                        ]
                    },
                },
            ]

            def incremental(_request):
                body = json.dumps(increments.pop(0)).encode("utf-8")
                return 200, {"Content-Type": "application/json"}, body

            upstream.route(
                "GET",
                "/openapi/v2/localassistant/message?message_id=m-1&limit=6",
                incremental,
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                {"tasks": [], "total": 0, "pagination": {"page": 1, "size": 6, "total": 0}},
            )
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(upstream.base_url, "access", allow_insecure_http=True)
            )

            first = adapter.fetch_snapshot("device-cursor-is-not-a-message-id").to_dict()
            unchanged = adapter.fetch_snapshot(first["cursor"]).to_dict()
            merged = adapter.fetch_snapshot(unchanged["cursor"]).to_dict()

        self.assertEqual(["m-1"], [item["id"] for item in first["messages"]])
        self.assertEqual(["m-1"], [item["id"] for item in unchanged["messages"]])
        self.assertEqual(["m-1", "m-2"], [item["id"] for item in merged["messages"]])
        self.assertEqual(first["cursor"], unchanged["cursor"])
        self.assertNotEqual(unchanged["cursor"], merged["cursor"])
        self.assertLessEqual(len(merged["cursor"].encode("utf-8")), 64)
        message_paths = [
            request.path
            for request in upstream.requests
            if urlsplit(request.path).path == "/openapi/v2/localassistant/message"
        ]
        self.assertEqual(
            [
                "/openapi/v2/localassistant/message?limit=6",
                "/openapi/v2/localassistant/message?message_id=m-1&limit=6",
                "/openapi/v2/localassistant/message?message_id=m-1&limit=6",
            ],
            message_paths,
        )

    def test_task_only_change_advances_the_device_snapshot_cursor(self) -> None:
        task_payloads = [
            {"tasks": [{"task_id": "task-1", "name": "Draft", "status": "failed"}]},
            {"tasks": [{"task_id": "task-1", "name": "Revised", "status": "failed"}]},
        ]
        with RecordingServer() as upstream:
            upstream.json_route("GET", "/openapi/v2/localassistant", {"code": 0, "data": {"online": True}})
            upstream.json_route("GET", "/openapi/v2/localassistant/message?limit=6", {"code": 0, "data": {"messages": []}})

            def tasks(_request):
                payload = task_payloads.pop(0)
                payload.update({"total": 1, "pagination": {"page": 1, "size": 6, "total": 1}})
                return 200, {"Content-Type": "application/json"}, json.dumps(payload).encode()

            upstream.route("GET", "/openapi/v2/tasks?page=1&size=6", tasks)
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(upstream.base_url, "access", allow_insecure_http=True)
            )

            first = adapter.fetch_snapshot().to_dict()
            second = adapter.fetch_snapshot(first["cursor"]).to_dict()

        self.assertNotEqual(first["cursor"], second["cursor"])
        self.assertEqual("Revised", second["tasks"][0]["title"])

    def test_artifacts_use_task_ticket_server_side_and_secrets_are_filtered(self) -> None:
        with RecordingServer() as upstream:
            link = upstream.base_url + "/acp?sessionId=session-1"
            upstream.json_route("GET", "/openapi/v2/localassistant", {"code": 0, "data": {"online": True}})
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant/message?limit=6",
                {"code": 0, "data": {"messages": []}},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                {
                    "tasks": [
                        {"task_id": "task-archived", "name": "Old", "status": "archived"},
                        {"task_id": "task-deleted", "name": "Gone", "status": "deleted"},
                        {"task_id": "task-1", "name": "Analysis", "status": "working"},
                    ],
                    "total": 3,
                    "pagination": {"page": 1, "size": 6, "total": 3},
                },
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks/task-1",
                {
                    "task_id": "task-1",
                    "name": "Analysis",
                    "status": "working",
                    "link": link,
                    "token": "task-ticket-secret",
                    "sandboxDataLink": upstream.base_url + "/private-data",
                },
            )
            upstream.json_route(
                "GET",
                "/api/session/artifacts?sessionId=session-1&limit=6",
                {
                    "data": {
                        "artifacts": [
                            {
                                "id": "entry-1",
                                "artifact": {
                                    "type": "plan",
                                    "uri": "file:///private/plan.md",
                                    "name": "plan.md",
                                    "title": "Launch plan",
                                    "description": "Ready to review",
                                    "mimeType": "text/markdown",
                                },
                            },
                            {
                                "id": "entry-2",
                                "artifact": {
                                    "type": "tasks",
                                    "uri": "agent:///tasks.json",
                                    "title": "Checklist",
                                    "description": "Three items",
                                    "mimeType": "application/json",
                                },
                            },
                            {
                                "id": "entry-3",
                                "artifact": {
                                    "type": "media",
                                    "uri": "agent:///chart.png",
                                    "title": "Chart",
                                    "description": "Latest result",
                                    "mimeType": "image/png",
                                },
                            },
                            {
                                "id": "entry-deleted",
                                "event": "deleted",
                                "artifact": {
                                    "type": "overview",
                                    "uri": "agent:///deleted.md",
                                    "title": "Deleted output",
                                    "description": "Must not be shown",
                                },
                            },
                        ]
                    }
                },
            )
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(upstream.base_url, "access-secret", allow_insecure_http=True)
            )

            snapshot = adapter.fetch_snapshot().to_dict()
            cached_snapshot = adapter.fetch_snapshot(snapshot["cursor"]).to_dict()

        self.assertEqual(["task-1"], [item["id"] for item in snapshot["tasks"]])
        self.assertEqual("Launch plan", snapshot["artifacts"][0]["title"])
        self.assertEqual("checklist", snapshot["artifacts"][1]["kind"])
        self.assertEqual("image", snapshot["artifacts"][2]["kind"])
        self.assertNotIn("Deleted output", repr(snapshot))
        self.assertEqual(snapshot["artifacts"], cached_snapshot["artifacts"])
        artifact_requests = [
            request
            for request in upstream.requests
            if urlsplit(request.path).path == "/api/session/artifacts"
        ]
        self.assertEqual(1, len(artifact_requests))
        artifact_request = artifact_requests[0]
        self.assertEqual("Bearer task-ticket-secret", artifact_request.headers["authorization"])
        rendered = json.dumps(snapshot)
        for forbidden in ("task-ticket-secret", "/acp", "sandbox", "file:///", "private-data"):
            self.assertNotIn(forbidden, rendered)

    def test_snapshot_call_budget_eventually_covers_outputs_for_all_active_tasks(self) -> None:
        class BudgetClient:
            def __init__(self):
                self.calls = []

            def local_assistant_online(self, *, deadline=None, request_budget=None):
                request_budget.consume()
                self.calls.append(("online", deadline))
                return True

            def list_messages(
                self, after_message_id=None, limit=6, *, deadline=None, request_budget=None
            ):
                request_budget.consume()
                self.calls.append(("messages", deadline, after_message_id, limit))
                return {"messages": []}

            def list_tasks(
                self, page=1, size=6, *, deadline=None, request_budget=None
            ):
                request_budget.consume()
                self.calls.append(("tasks", deadline, page, size))
                return {
                    "tasks": [
                        {"task_id": "task-%d" % index, "name": "Task %d" % index, "status": "working"}
                        for index in range(6)
                    ]
                }

            def get_task(self, task_id, *, deadline=None, request_budget=None):
                request_budget.consume()
                self.calls.append(("detail", deadline, task_id))
                return {
                    "task_id": task_id,
                    "status": "working",
                    "link": "https://sandbox.example/acp",
                    "token": "ticket",
                }

            def list_artifacts(
                self,
                link,
                task_id,
                ticket,
                limit=6,
                *,
                deadline=None,
                request_budget=None,
            ):
                del link, ticket
                request_budget.consume()
                self.calls.append(("artifacts", deadline, task_id, limit))
                return [
                    {
                        "artifact": {
                            "type": "overview",
                            "uri": "agent:///%s.md" % task_id,
                            "title": "Output %s" % task_id,
                            "description": "ready",
                        }
                    }
                ]

        client = BudgetClient()
        adapter = LiveWorkBuddyAdapter(
            client,
            clock=lambda: now[0],
            snapshot_timeout_seconds=10.0,
            artifact_cache_seconds=0.5,
            max_snapshot_calls=7,
        )

        now = [100.0]
        snapshots = []
        previous_calls = 0
        for _index in range(3):
            snapshots.append(adapter.fetch_snapshot().to_dict())
            self.assertLessEqual(len(client.calls) - previous_calls, 7)
            previous_calls = len(client.calls)
            now[0] += 1.0

        self.assertEqual(6, len(snapshots[-1]["artifacts"]))
        self.assertEqual(
            {"task-%d" % index for index in range(6)},
            {item["task_id"] for item in snapshots[-1]["artifacts"]},
        )

    def test_snapshot_fetches_are_serialized(self) -> None:
        class ConcurrentClient:
            def __init__(self):
                self.active = 0
                self.maximum_active = 0
                self.lock = threading.Lock()

            def local_assistant_online(self, *, deadline=None, request_budget=None):
                del deadline, request_budget
                with self.lock:
                    self.active += 1
                    self.maximum_active = max(self.maximum_active, self.active)
                time.sleep(0.03)
                with self.lock:
                    self.active -= 1
                return True

            def list_messages(
                self, after_message_id=None, limit=6, *, deadline=None, request_budget=None
            ):
                del after_message_id, limit, deadline, request_budget
                return {"messages": []}

            def list_tasks(
                self, page=1, size=6, *, deadline=None, request_budget=None
            ):
                del page, size, deadline, request_budget
                return {"tasks": []}

        client = ConcurrentClient()
        adapter = LiveWorkBuddyAdapter(client)
        barrier = threading.Barrier(3)
        failures = []

        def fetch():
            barrier.wait()
            try:
                adapter.fetch_snapshot()
            except Exception as error:  # pragma: no cover - assertion reports the error
                failures.append(error)

        threads = [threading.Thread(target=fetch) for _index in range(2)]
        for thread in threads:
            thread.start()
        barrier.wait()
        for thread in threads:
            thread.join(timeout=1)

        self.assertTrue(all(not thread.is_alive() for thread in threads))
        self.assertEqual([], failures)
        self.assertEqual(1, client.maximum_active)

    def test_snapshot_deadline_includes_waiting_for_the_adapter_lock(self) -> None:
        entered = threading.Event()
        release = threading.Event()

        class BlockingClient:
            def local_assistant_online(self, *, deadline=None, request_budget=None):
                del deadline, request_budget
                entered.set()
                release.wait(timeout=1)
                return True

            def list_messages(
                self, after_message_id=None, limit=6, *, deadline=None, request_budget=None
            ):
                del after_message_id, limit, deadline, request_budget
                return {"messages": []}

            def list_tasks(
                self, page=1, size=6, *, deadline=None, request_budget=None
            ):
                del page, size, deadline, request_budget
                return {"tasks": []}

        adapter = LiveWorkBuddyAdapter(
            BlockingClient(),
            snapshot_timeout_seconds=0.08,
        )
        first = threading.Thread(target=adapter.fetch_snapshot)
        first.start()
        self.assertTrue(entered.wait(timeout=0.5))
        started = time.monotonic()
        try:
            with self.assertRaises(GatewayError) as caught:
                adapter.fetch_snapshot()
        finally:
            release.set()
            first.join(timeout=1)

        self.assertEqual("upstream_deadline", caught.exception.code)
        self.assertLess(time.monotonic() - started, 0.3)
        self.assertFalse(first.is_alive())

    def test_expired_snapshot_deadline_prevents_an_upstream_call(self) -> None:
        with RecordingServer() as upstream:
            client = WorkBuddyClient(upstream.base_url, "access", allow_insecure_http=True)

            with self.assertRaises(GatewayError) as caught:
                client.list_tasks(size=6, deadline=time.monotonic() - 1.0)

        self.assertEqual("upstream_deadline", caught.exception.code)
        self.assertEqual([], upstream.requests)

    def test_slow_stream_cannot_extend_total_upstream_deadline(self) -> None:
        body = json.dumps(
            {"tasks": [], "total": 0, "pagination": {"page": 1, "size": 6, "total": 0}}
        ).encode("utf-8")

        class SlowHandler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                for byte in body:
                    try:
                        self.wfile.write(bytes((byte,)))
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError):
                        break
                    time.sleep(0.02)

            def log_message(self, _format, *_args):
                return

        upstream = ThreadingHTTPServer(("127.0.0.1", 0), SlowHandler)
        thread = threading.Thread(target=upstream.serve_forever, daemon=True)
        thread.start()
        host, port = upstream.server_address[:2]
        client = WorkBuddyClient(
            "http://%s:%d" % (host, port),
            "access",
            allow_insecure_http=True,
        )
        started = time.monotonic()
        try:
            with self.assertRaises(GatewayError) as caught:
                client.list_tasks(size=6, deadline=started + 0.15)
        finally:
            upstream.shutdown()
            upstream.server_close()
            thread.join(timeout=2)

        self.assertEqual("upstream_deadline", caught.exception.code)
        self.assertLess(time.monotonic() - started, 0.8)

    def test_slow_dns_resolution_is_bounded_by_the_snapshot_deadline(self) -> None:
        real_getaddrinfo = socket.getaddrinfo
        with RecordingServer() as upstream:
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                {"tasks": [], "total": 0, "pagination": {"page": 1, "size": 6, "total": 0}},
            )
            base_url = upstream.base_url.replace("127.0.0.1", "localhost")
            client = WorkBuddyClient(base_url, "access", allow_insecure_http=True)

            def slow_getaddrinfo(host, *args, **kwargs):
                time.sleep(0.3)
                return real_getaddrinfo(host, *args, **kwargs)

            started = time.monotonic()
            with mock.patch(
                "workbuddy_gateway.workbuddy_client.socket.getaddrinfo",
                side_effect=slow_getaddrinfo,
            ):
                with self.assertRaises(GatewayError) as caught:
                    client.list_tasks(size=6, deadline=started + 0.08)
                elapsed = time.monotonic() - started

        self.assertEqual("upstream_deadline", caught.exception.code)
        self.assertLess(elapsed, 0.25)
        resolver = workbuddy_client_module._DNS_RESOLVER
        self.assertEqual(
            workbuddy_client_module.DNS_RESOLVER_WORKERS,
            len(resolver._workers),
        )
        self.assertTrue(all(worker.daemon for worker in resolver._workers))
        self.assertEqual(
            workbuddy_client_module.DNS_RESOLVER_QUEUE_SIZE,
            resolver._jobs.maxsize,
        )

    def test_snapshot_network_budget_counts_oauth_and_every_http_attempt(self) -> None:
        with RecordingServer() as upstream:
            upstream.json_route(
                "POST",
                "/openapi/v2/token",
                {"access_token": "access", "token_type": "Bearer", "expires_in": 3600},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant",
                {"code": 0, "data": {"online": True}},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant/message?limit=6",
                {"code": 0, "data": {"messages": []}},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                {
                    "tasks": [
                        {"task_id": "task-1", "name": "One", "status": "working"},
                        {"task_id": "task-2", "name": "Two", "status": "working"},
                    ],
                    "total": 2,
                    "pagination": {"page": 1, "size": 6, "total": 2},
                },
            )
            for task_id in ("task-1", "task-2"):
                upstream.json_route(
                    "GET",
                    "/openapi/v2/tasks/%s" % task_id,
                    {
                        "task_id": task_id,
                        "status": "working",
                        "link": upstream.base_url + "/acp",
                        "token": "ticket",
                    },
                )
            upstream.json_route(
                "GET",
                "/api/session/artifacts?sessionId=task-1&limit=6",
                {"data": {"artifacts": []}},
            )
            upstream.json_route(
                "GET",
                "/api/session/artifacts?sessionId=task-2&limit=6",
                {"data": {"artifacts": []}},
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
                ),
                max_snapshot_calls=7,
            )

            adapter.fetch_snapshot()

        self.assertLessEqual(len(upstream.requests), 7)

    def test_direct_create_shape_is_supported_but_secret_fields_do_not_escape_adapter(self) -> None:
        with RecordingServer() as upstream:
            upstream.json_route(
                "POST",
                "/openapi/v2/tasks",
                {
                    "task_id": "task-9",
                    "status": "pending",
                    "name": "Secret-bearing task",
                    "link": "https://sandbox.example/acp",
                    "token": "ticket",
                    "expire_at": 123,
                    "sandboxLink": "https://sandbox.example",
                    "sandboxDataLink": "https://sandbox.example/data",
                },
            )
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(upstream.base_url, "access", allow_insecure_http=True)
            )

            receipt = adapter.create_task("Do work", "operation-id")

        self.assertEqual("task-9", receipt)
        self.assertNotIn("ticket", receipt)

    def test_followup_is_explicitly_not_supported_without_acp(self) -> None:
        client = WorkBuddyClient("https://www.workbuddy.cn", "secret")
        adapter = LiveWorkBuddyAdapter(client)

        with self.assertRaises(GatewayError) as caught:
            adapter.followup_task("task-1", "continue", "op-1")

        self.assertEqual("not_supported", caught.exception.code)


    def test_snapshot_uses_online_status_instead_of_inferring_from_other_requests(self) -> None:
        with RecordingServer() as upstream:
            upstream.json_route("GET", "/openapi/v2/localassistant", {"code": 0, "data": {"online": False}})
            upstream.json_route(
                "GET",
                "/openapi/v2/localassistant/message?limit=6",
                {"code": 0, "data": {"messages": []}},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                {"tasks": [], "total": 0, "pagination": {"page": 1, "size": 6, "total": 0}},
            )
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(upstream.base_url, "access", allow_insecure_http=True)
            )

            snapshot = adapter.fetch_snapshot().to_dict()

        self.assertFalse(snapshot["assistant_available"])

    def test_invalid_online_status_is_a_retryable_upstream_schema_error(self) -> None:
        with RecordingServer() as upstream:
            upstream.json_route("GET", "/openapi/v2/localassistant", {"code": 0, "data": {"online": "yes"}})
            client = WorkBuddyClient(upstream.base_url, "access", allow_insecure_http=True)
            with self.assertRaises(GatewayError) as caught:
                client.local_assistant_online()

        self.assertEqual("upstream_schema", caught.exception.code)
        self.assertTrue(caught.exception.retryable)

    def test_response_size_malformed_json_and_upstream_error_are_bounded_and_redacted(self) -> None:
        access_token = "access-token-must-not-leak"
        with RecordingServer() as upstream:
            upstream.route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                lambda _request: (200, {"Content-Type": "application/json"}, b"x" * 200),
            )
            client = WorkBuddyClient(
                upstream.base_url,
                access_token,
                max_response_bytes=100,
                allow_insecure_http=True,
            )
            with self.assertRaises(GatewayError) as oversized:
                client.list_tasks(size=6)
            self.assertEqual("upstream_too_large", oversized.exception.code)

        with RecordingServer() as upstream:
            upstream.route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                lambda _request: (
                    500,
                    {"Content-Type": "application/json"},
                    json.dumps({"error": access_token}).encode(),
                ),
            )
            client = WorkBuddyClient(upstream.base_url, access_token, allow_insecure_http=True)
            with self.assertRaises(GatewayError) as failed:
                client.list_tasks(size=6)

        self.assertNotIn(access_token, str(failed.exception))
        self.assertNotIn(access_token, failed.exception.public_message)

    def test_insecure_or_credential_bearing_urls_are_rejected_by_default(self) -> None:
        with self.assertRaises(GatewayError):
            WorkBuddyClient("http://www.workbuddy.cn", "secret")
        with self.assertRaises(GatewayError):
            WorkBuddyClient("https://user:pass@www.workbuddy.cn", "secret")

    def test_ambiguous_artifact_link_is_rejected_instead_of_guessing_a_host(self) -> None:
        with self.assertRaises(GatewayError) as caught:
            resolve_artifact_endpoint(
                "https://sandbox.example/sessions/task-1", "task-1", 6
            )
        self.assertEqual("artifact_unavailable", caught.exception.code)

    def test_artifact_endpoint_never_allows_loopback_http_without_explicit_test_opt_in(self) -> None:
        with self.assertRaises(GatewayError):
            resolve_artifact_endpoint(
                "http://127.0.0.1:9999/acp?sessionId=s-1", "task-1", 6
            )
        endpoint = resolve_artifact_endpoint(
            "http://127.0.0.1:9999/acp?sessionId=s-1",
            "task-1",
            6,
            allow_insecure_http=True,
        )
        self.assertEqual(
            "http://127.0.0.1:9999/api/session/artifacts?sessionId=s-1&limit=6",
            endpoint,
        )

    def test_refresh_provider_uses_form_post_and_honors_expires_in(self) -> None:
        now = [1000.0]
        refresh_calls = []
        with RecordingServer() as upstream:
            def token_route(request):
                refresh_calls.append(request)
                sequence = len(refresh_calls)
                return (
                    200,
                    {"Content-Type": "application/json"},
                    json.dumps(
                        {
                            "access_token": "rotated-access-%d" % sequence,
                            "token_type": "Bearer",
                            "expires_in": 120,
                            "refresh_token": "rotated-refresh-%d" % sequence,
                        }
                    ).encode(),
                )

            upstream.route("POST", "/openapi/v2/token", token_route)
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks?page=1&size=6",
                {"tasks": [], "total": 0, "pagination": {"page": 1, "size": 6, "total": 0}},
            )
            provider = OAuthTokenProvider(
                upstream.base_url + "/openapi/v2/token",
                "client-id",
                "client-secret-must-not-leak",
                "refresh-secret-must-not-leak",
                clock=lambda: now[0],
                allow_insecure_http=True,
            )
            client = WorkBuddyClient(
                upstream.base_url,
                token_provider=provider,
                allow_insecure_http=True,
            )

            client.list_tasks(size=6)
            client.list_tasks(size=6)
            now[0] += 121
            client.list_tasks(size=6)

        self.assertEqual(2, len(refresh_calls))
        first_form = refresh_calls[0].body.decode()
        self.assertIn("grant_type=refresh_token", first_form)
        self.assertIn("client_id=client-id", first_form)
        self.assertIn("client_secret=client-secret-must-not-leak", first_form)
        self.assertIn("refresh_token=refresh-secret-must-not-leak", first_form)
        api_auth = [
            request.headers["authorization"]
            for request in upstream.requests
            if request.path.startswith("/openapi/v2/tasks")
        ]
        self.assertEqual(
            ["Bearer rotated-access-1", "Bearer rotated-access-1", "Bearer rotated-access-2"],
            api_auth,
        )
        self.assertNotIn("client-secret-must-not-leak", repr(provider))
        self.assertNotIn("refresh-secret-must-not-leak", repr(provider))

    def test_rotated_refresh_token_is_persisted_and_used_after_restart(self) -> None:
        with tempfile.TemporaryDirectory() as directory, RecordingServer() as upstream:
            state_path = Path(directory) / "state.json"
            forms = []

            def token_route(request):
                forms.append(request.body.decode("ascii"))
                sequence = len(forms)
                return (
                    200,
                    {"Content-Type": "application/json"},
                    json.dumps(
                        {
                            "access_token": "access-%d" % sequence,
                            "token_type": "Bearer",
                            "expires_in": 3600,
                            "refresh_token": "refresh-%d" % sequence,
                        }
                    ).encode(),
                )

            upstream.route("POST", "/openapi/v2/token", token_route)
            first_store = StateStore(state_path)
            first_provider = OAuthTokenProvider(
                upstream.base_url + "/openapi/v2/token",
                "client-id",
                "client-secret",
                "initial-refresh",
                on_refresh_token=lambda token: first_store.set_refresh_token(
                    "client-id", token
                ),
                allow_insecure_http=True,
            )

            self.assertEqual("access-1", first_provider.get_access_token())

            restarted_store = StateStore(state_path)
            restarted_refresh = restarted_store.get_refresh_token("client-id")
            second_provider = OAuthTokenProvider(
                upstream.base_url + "/openapi/v2/token",
                "client-id",
                "client-secret",
                restarted_refresh,
                on_refresh_token=lambda token: restarted_store.set_refresh_token(
                    "client-id", token
                ),
                allow_insecure_http=True,
            )
            self.assertEqual("access-2", second_provider.get_access_token())

        self.assertIn("refresh_token=initial-refresh", forms[0])
        self.assertIn("refresh_token=refresh-1", forms[1])


if __name__ == "__main__":
    unittest.main()
