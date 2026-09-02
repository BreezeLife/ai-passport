from __future__ import annotations

import json
import unittest
from urllib.parse import urlsplit

from support import PACKAGE_ROOT, RecordingServer  # noqa: F401

from workbuddy_gateway.errors import GatewayError
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
                                "content": [{"type": "text", "text": "New message"}],
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
                {"tasks": [{"task_id": "task-1", "name": "Analysis", "status": "completed"}], "total": 1, "pagination": {"page": 1, "size": 6, "total": 1}},
            )
            upstream.json_route(
                "GET",
                "/openapi/v2/tasks/task-1",
                {
                    "task_id": "task-1",
                    "name": "Analysis",
                    "status": "completed",
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
                        ]
                    }
                },
            )
            adapter = LiveWorkBuddyAdapter(
                WorkBuddyClient(upstream.base_url, "access-secret", allow_insecure_http=True)
            )

            snapshot = adapter.fetch_snapshot().to_dict()

        self.assertEqual("Launch plan", snapshot["artifacts"][0]["title"])
        self.assertEqual("checklist", snapshot["artifacts"][1]["kind"])
        self.assertEqual("image", snapshot["artifacts"][2]["kind"])
        artifact_request = upstream.requests[-1]
        self.assertEqual("Bearer task-ticket-secret", artifact_request.headers["authorization"])
        rendered = json.dumps(snapshot)
        for forbidden in ("task-ticket-secret", "/acp", "sandbox", "file:///", "private-data"):
            self.assertNotIn(forbidden, rendered)

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


if __name__ == "__main__":
    unittest.main()
