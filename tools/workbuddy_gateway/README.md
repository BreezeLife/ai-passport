<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Run the WorkBuddy device gateway

The WorkBuddy device gateway gives AI Passport a small, bounded HTTP API. It keeps Tencent WorkBuddy OAuth and transcription credentials off the badge, normalizes upstream data, and stores idempotent operation receipts. The service uses the Python 3.11 standard library and defaults to deterministic demo mode.

The built-in server speaks plain HTTP. Use it directly only on an isolated development network. Put it behind an HTTPS reverse proxy for any live deployment.

## Start demo mode

Run the service from the repository root. The process reads environment variables directly and does not load `.env` files.

```sh
export WORKBUDDY_GATEWAY_DEVICE_TOKEN='replace_with_a_local_demo_device_token'
export WORKBUDDY_GATEWAY_STATE_PATH='/tmp/workbuddy_gateway_demo_state.json'
PYTHONPATH=tools/workbuddy_gateway python3 -m workbuddy_gateway.server
```

Demo mode binds to `127.0.0.1:8787` unless you override the host or port. It needs no WorkBuddy or transcription credentials. Its snapshot, transcript, reply receipt, and task receipt are deterministic. Task follow-up still returns HTTP 501 `not_supported`.

Check the running service from another terminal:

```sh
curl --fail --silent --show-error http://127.0.0.1:8787/healthz
curl --fail --silent --show-error \
  -H "Authorization: Bearer ${WORKBUDDY_GATEWAY_DEVICE_TOKEN}" \
  http://127.0.0.1:8787/v1/snapshot
```

The firmware's default demo mode does not call this service. It uses an in-firmware snapshot and transcript so a clean firmware build remains credential-free.

## Configure live mode

Live mode is not available until Tencent approves and enables your WorkBuddy hardware-access application. Complete the OAuth 2.1 authorization-code flow outside this service. The gateway accepts an existing access token or a complete refresh-credential set.

Request only the scopes needed by your product path:

- `user.localassistant.readable`: local-assistant availability and message history
- `user.localassistant.invokable`: sending a confirmed text message
- `user.task.readable`: task list, task detail, ACP link and ticket, and output summaries
- `user.task.invokable`: creating a cloud task

Use [`.env.example`](.env.example) as a variable inventory, then export the selected values into the process environment. The repository ignores `.env` and `.env.*` files except for the tracked example, but the gateway never loads dotenv files. Keep production credentials in a secret manager or service environment rather than relying on an ignored file.

```sh
export WORKBUDDY_GATEWAY_MODE=live
export WORKBUDDY_GATEWAY_HOST=0.0.0.0
export WORKBUDDY_GATEWAY_PORT=8787
export WORKBUDDY_GATEWAY_DEVICE_TOKEN='replace_with_a_random_device_token_at_least_16_characters'
export WORKBUDDY_GATEWAY_STATE_PATH='/absolute/path/outside/the/checkout/workbuddy_gateway_state.json'
export WORKBUDDY_BASE_URL='https://www.workbuddy.cn'
export WORKBUDDY_ACCESS_TOKEN='workbuddy_access_token_here'
export WORKBUDDY_TRANSCRIPTION_URL='https://provider.example/v1/audio/transcriptions'
export WORKBUDDY_TRANSCRIPTION_API_KEY='transcription_api_key_here'
PYTHONPATH=tools/workbuddy_gateway python3 -m workbuddy_gateway.server
```

Use all three refresh variables instead of `WORKBUDDY_ACCESS_TOKEN` when the gateway must refresh access:

```sh
export WORKBUDDY_REFRESH_TOKEN='workbuddy_refresh_token_here'
export WORKBUDDY_CLIENT_ID='workbuddy_client_id_here'
export WORKBUDDY_CLIENT_SECRET='workbuddy_client_secret_here'
unset WORKBUDDY_ACCESS_TOKEN
```

The gateway posts an `application/x-www-form-urlencoded` refresh request to `https://www.workbuddy.cn/openapi/v2/token`. It uses `expires_in`, refreshes 60 seconds early, retries one upstream request after a 401, and accepts a rotated refresh token. Before adopting a rotated token, it writes the token to the gateway state file under a SHA-256 scope for the configured client ID. It prefers that persisted token after restart. An access token is never persisted.

The `client_secret`, refresh token, access token, ACP ticket, sandbox link, and transcription key stay inside the gateway trust boundary. Never copy them into firmware settings or device responses. In refresh mode the state file and backup contain a refresh token and must be handled as secrets.

## Review every environment setting

The following table matches `GatewayConfig.from_env()`:

| Variable | Demo default | Live rule |
| --- | --- | --- |
| `WORKBUDDY_GATEWAY_MODE` | `demo` | Must be `demo` or `live` |
| `WORKBUDDY_GATEWAY_HOST` | `127.0.0.1` | Non-empty bind host; choose a reachable interface only behind the intended network boundary |
| `WORKBUDDY_GATEWAY_PORT` | `8787` | Integer from 0 through 65535; use 0 only for tests that need an ephemeral port |
| `WORKBUDDY_GATEWAY_INBOUND_TIMEOUT_SECONDS` | `5` | Connection read timeout greater than 0 and no more than 60 seconds |
| `WORKBUDDY_GATEWAY_MAX_CONNECTIONS` | `8` | Concurrent connection limit from 1 through 64 |
| `WORKBUDDY_GATEWAY_DEVICE_TOKEN` | `workbuddy-demo-device-token` | Required and at least 16 characters |
| `WORKBUDDY_GATEWAY_STATE_PATH` | `.workbuddy-gateway-state.json` | Operation and rotated-token state; the default and its backup/temp files are ignored, but production should use a protected path outside the checkout |
| `WORKBUDDY_REQUEST_TIMEOUT_SECONDS` | `15` | Outbound request timeout greater than 0 and no more than 120 seconds |
| `WORKBUDDY_BASE_URL` | Unused | Required HTTPS origin without credentials or an API path; use `https://www.workbuddy.cn` |
| `WORKBUDDY_ACCESS_TOKEN` | Unused | One supported WorkBuddy credential mode |
| `WORKBUDDY_REFRESH_TOKEN` | Unused | Required with both client variables when using refresh mode |
| `WORKBUDDY_CLIENT_ID` | Unused | Required with the refresh token and client secret |
| `WORKBUDDY_CLIENT_SECRET` | Unused | Required with the refresh token and client ID; gateway only |
| `WORKBUDDY_TRANSCRIPTION_URL` | Unused | Required HTTPS endpoint compatible with `/v1/audio/transcriptions` |
| `WORKBUDDY_TRANSCRIPTION_API_KEY` | Unused | Required and gateway only |
| `WORKBUDDY_TRANSCRIPTION_MODEL` | `gpt-4o-mini-transcribe` | Optional non-empty model passed to the provider |

Configuration failures stop startup. URL validation rejects embedded usernames and passwords. The WorkBuddy client also rejects a base URL with a path, query, or fragment and never follows redirects.

The state store writes its primary, backup, and temporary files with owner-only permissions (`0600`). It stores bounded operation fingerprints and receipts, but not action text. In refresh mode it also keeps the latest rotated refresh token in both valid state slots, scoped to the SHA-256 hash of the client ID. A valid backup can recover a corrupt primary. If state material exists but neither slot is valid, the store fails closed: readiness stays false and mutations return HTTP 503 `state_corrupt` until an operator restores trusted state or deliberately starts from a new path. Do not discard an ambiguous `PENDING` operation without first reconciling it with WorkBuddy.

## Understand the device API

The complete machine-readable contract is [`openapi.yaml`](openapi.yaml). Every JSON response includes `version: 1` and a `request_id`. A valid incoming `X-Request-ID` may contain 1 to 64 ASCII letters, digits, `.`, `_`, `:`, or `-`; otherwise the gateway creates one.

`GET /healthz` and `GET /readyz` are public. Every `/v1/*` route requires an exact `Authorization: Bearer <device token>` header.

| Route | Request | Success |
| --- | --- | --- |
| `GET /healthz` | No auth | `status: ok` |
| `GET /readyz` | No auth | `ready: true`; returns 503 with `ready: false` until a Live upstream snapshot succeeds |
| `GET /v1/snapshot?after=...` | Optional stable snapshot cursor | `fresh`, local-assistant availability, unread and active-task counts, plus bounded message, task, and artifact collections |
| `POST /v1/transcriptions?operation_id=...` | Exact `audio/L16;rate=16000;channels=1` body | A transcription draft; no WorkBuddy mutation |
| `POST /v1/actions` | Strict JSON action | An operation status and receipt when successful |
| `GET /v1/operations/{operation_id}` | Existing operation ID | `PENDING`, `SUCCEEDED`, or `FAILED` record |

The gateway rejects chunked request bodies. POST requests require exactly one ASCII-decimal `Content-Length` of at most 10 digits. JSON accepts only `application/json` with an optional UTF-8 charset. Duplicate JSON keys, extra action fields, and unsupported versions fail closed. It also limits request headers to 16 KiB, times out idle or partial connections, and drops connections above the configured concurrency cap.

### Request and response limits

| Value | Limit |
| --- | --- |
| Action JSON request | 4,096 bytes |
| PCM request | 32,000 through 160,000 bytes, even length |
| Parsed request headers | 16 KiB |
| Device JSON response | 12 KiB |
| Collections | 6 messages, 6 tasks, and 6 artifacts |
| Identifier | 64 UTF-8 bytes |
| Cursor | 64 UTF-8 bytes; Live mode uses a stable SHA-256 snapshot fingerprint |
| Title | 128 UTF-8 bytes |
| Preview or artifact description | 384 UTF-8 bytes |
| Transcript, reply text, or task prompt | 512 UTF-8 bytes |
| Public operation error text | 160 UTF-8 bytes |
| Stored operations | 128 by default; completed records are pruned before pending records |
| Live snapshot budget | 10 seconds total and at most 7 physical HTTP attempts, including OAuth refresh and a 401 retry |
| Artifact cache | 90 seconds per eligible task |
| WorkBuddy upstream response | 64 KiB |
| OAuth token response | 16 KiB |
| Transcription-provider response | 16 KiB |

Text truncation stops on a UTF-8 boundary. Identifiers and action text reject oversize input instead of truncating it. If JSON escaping would push a snapshot beyond 12 KiB, the server progressively shortens previews, descriptions, and task/output titles; it drops tail items only if text trimming is insufficient. The count fields continue to describe the normalized pre-trim snapshot.

### Action shapes

Reply to the local-assistant conversation:

```json
{
  "version": 1,
  "operation_id": "device_operation_123",
  "type": "reply",
  "message_id": "message_123",
  "text": "Proceed with the reviewed draft."
}
```

Create a cloud task:

```json
{
  "version": 1,
  "operation_id": "device_operation_124",
  "type": "task_create",
  "prompt": "Prepare a one-page project update."
}
```

`task_followup` has a defined request shape for forward compatibility, but every V1 adapter rejects it. The current Live firmware stops at an informational notice before recording and does not submit this action; the contract below protects direct API clients:

```json
{
  "version": 1,
  "operation_id": "device_operation_125",
  "type": "task_followup",
  "task_id": "task_123",
  "text": "Add the key risks."
}
```

The first request returns HTTP 501 with `error.code: not_supported` and records a `FAILED` operation. Repeating the same operation ID and body returns that stored record without another mutation. Reusing an operation ID with different content returns HTTP 409 `operation_conflict`. Production follow-up remains deferred until durable asynchronous phases, ACP permission handling, and idempotency across disconnects and process restarts are designed together.

The gateway distinguishes a known preflight failure from an ambiguous write. If OAuth refresh or another operation fails before a WorkBuddy mutation can be submitted, the error is retryable and the reservation is released, so the device may submit the same operation ID again. If a request may have reached WorkBuddy but its result is unknown, the reservation remains `PENDING`; an identical repeat returns that record and does not submit another mutation. Query the operation and reconcile it with WorkBuddy instead of inventing a new ID.

The Live snapshot deadline starts before waiting for the adapter lock and covers DNS, connect, write, and bounded response reads. DNS uses two fixed daemon workers and a queue of two jobs, so a slow resolver cannot create unbounded threads. Every physical WorkBuddy HTTP attempt consumes the shared seven-attempt budget; token refresh and a retry after 401 count too. A POST network failure or POST response such as 429 is not proof that the mutation was rejected before submission, so its operation stays `PENDING`.

## Check the WorkBuddy mapping

The Live adapter calls only these upstream surfaces:

| Purpose | WorkBuddy request |
| --- | --- |
| PC local-assistant status | `GET /openapi/v2/localassistant` |
| Message history | `GET /openapi/v2/localassistant/message?message_id=...&limit=6`; official `content: string[]` entries and compatible text blocks are normalized |
| Send text | `POST /openapi/v2/localassistant/message` with `content` and `msg_type: text` |
| Task list | `GET /openapi/v2/tasks?page=1&size=6` |
| Create task | `POST /openapi/v2/tasks` with `prompt` |
| Task detail | `GET /openapi/v2/tasks/{task_id}` |
| Token refresh | `POST /openapi/v2/token` with a form body |

The Live adapter keeps its WorkBuddy message cursor separate from the 64-byte device snapshot fingerprint, merges incremental history into the latest six messages, and advances the device cursor when messages, tasks, outputs, or assistant availability change. WorkBuddy `pending` maps to `NEEDS_INPUT`; archived and deleted tasks are filtered rather than reported as failures.

The adapter derives an artifact endpoint only when the task detail `link` path ends exactly in `/acp`. It removes that suffix and requests `/api/session/artifacts?sessionId=...&limit=6` with the task `token`. Any ambiguous link fails closed. Eligible queued, running, needs-input, and completed tasks are scanned within the per-snapshot call budget, with round-robin coverage and a 90-second cache. Deleted artifact events are filtered, and a transient refresh failure preserves the previous cache. Returned URLs, URIs, links, tickets, and sandbox data are filtered out before the device response.

Artifact mapping is closed: `plan` stays `plan`, `tasks` becomes `checklist`, `overview` stays `overview`, `media` with an `image/*` MIME type becomes `image`, and other media becomes `document`. Unknown shapes are skipped.

See the official [WorkBuddy third-party application guide](https://open.workbuddy.cn/en/docs/third-party-app) and [Open API reference](https://open.workbuddy.cn/en/docs/openapi) for upstream registration and schemas. Those documents do not replace this gateway's narrower device contract.

## Interpret status codes

The API uses these HTTP statuses:

| Status | Meaning in this service |
| --- | --- |
| 200 | Successful request, including a repeated operation that returns its stored record |
| 400 | Invalid query, identifier, audio length, JSON, version, or strict action shape |
| 401 | Missing or incorrect device bearer token |
| 404 | Unknown route or operation |
| 405 | Known route with an unsupported method |
| 408 | A partial POST body exceeded the inbound connection timeout |
| 409 | Operation ID reused with different action content |
| 411 | A POST request did not contain exactly one `Content-Length` |
| 413 | Request body exceeds its route limit |
| 415 | Unsupported JSON or PCM content type |
| 431 | Parsed request headers exceed 16 KiB |
| 500 | Invalid runtime state, state-file write failure, or bounded response failure |
| 501 | `task_followup` is not supported |
| 502 | WorkBuddy or transcription provider rejection, outage, or schema failure |
| 503 | Readiness false, corrupt state, or an operation store full of pending records |
| 504 | The 10-second Live snapshot deadline expired |

Error responses use one shape:

```json
{
  "version": 1,
  "request_id": "request_123",
  "error": {
    "code": "invalid_request",
    "message": "Request is invalid",
    "retryable": false
  }
}
```

The server never echoes an authorization header or action body in an error response.

## Run the automated checks

Run the gateway suite without network access:

```sh
python3 -m unittest discover -s tools/workbuddy_gateway/tests -v
```

Run repository checks from the project root:

```sh
./tools/validate.sh --static
```

These checks do not validate a real WorkBuddy application, OAuth approval, transcription account, HTTPS deployment, Wi-Fi network, or AI Passport hardware.
