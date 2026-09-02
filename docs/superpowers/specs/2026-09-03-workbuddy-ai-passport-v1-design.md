<p align="right">
  <a href="2026-09-03-workbuddy-ai-passport-v1-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# WorkBuddy AI Passport V1 Design

## Goal

Build a fourth, isolated AI Passport firmware application that turns the badge into a
small WorkBuddy companion. The first usable release must boot directly into a wearable
inbox, let the user browse WorkBuddy messages and tasks, capture a short voice response,
show the transcription for confirmation, and submit it without a phone acting as the
runtime bridge.

The target service is Tencent WorkBuddy at `workbuddy.cn`. Its published Open API is the
source of truth for local-assistant messages, cloud tasks, ACP task state, and artifacts.

## Product boundary

V1 is one vertical slice with a privacy cover and four views backed by one normalized
snapshot:

0. **Privacy cover** — the idle screen shows only WorkBuddy connectivity and unread/task
   counts. It never exposes message text until `OK` is pressed.

1. **Inbox** — recent local-assistant messages, unread marker, sender role, and a bounded
   preview. Selecting an item starts a reply recording.
2. **Tasks** — recent cloud tasks with normalized state (`QUEUED`, `RUNNING`,
   `NEEDS_INPUT`, `COMPLETED`, or `FAILED`). Selecting a task opens its detail. In Live
   mode, `OK` shows that cloud-task follow-up is reserved for a later release and does not
   start recording; demo mode may exercise the follow-up state machine.
3. **Outputs** — bounded titles and short descriptions for recent plan, checklist,
   overview, image, or document artifacts. The badge does not parse Office files.
4. **New task** — a short voice instruction is transcribed, reviewed, and then sent to
   WorkBuddy's task creation endpoint.

The badge polls for a fresh snapshot and gives a short visual notification only
when a cursor advances or a task changes to an actionable/terminal state. There is no
claim of instant delivery while Wi-Fi, the gateway, or the PC local assistant is offline.

V1 does not implement raise-to-wake (the board has no confirmed IMU), cellular access,
local speech recognition, arbitrary text editing, Office rendering, or a production
always-on power target.

V1 also does not implement production ACP task follow-up. A safe implementation needs a
durable asynchronous phase model, ACP permission-request handling, and idempotency across
connection loss and process restart. The Live firmware therefore blocks before recording.
The device API keeps a forward-compatible `task_followup` shape, but a direct request to
either V1 gateway adapter returns HTTP 501 `not_supported`. Demo success is only a state-
machine demonstration and does not prove a production follow-up path.

## Interaction design

The interaction follows the glance/select/speak/confirm pattern used by constrained
wearables. It avoids hidden simultaneous-button chords.

### Browse state

- `UP CLICK` / `DOWN CLICK`: move focus through the current list.
- `OK CLICK`: open the focused item. In Inbox this goes directly to voice reply; in Tasks
  and Outputs it opens detail.
- `OK LONG`: open the global navigation sheet; on the privacy cover it opens New task.
- A fixed footer always shows the currently valid actions.

### Navigation sheet

- `UP CLICK` / `DOWN CLICK`: select Inbox, Tasks, New task, Outputs, Sync, or Status.
- `OK CLICK`: activate the selection.
- `OK LONG`: close the sheet.

### Voice state

- Entering voice mode is always an explicit button action. Live mode first shows a preparation
  state while the bounded HTTP upload is opened; it does not start the recording clock yet.
- An operation-ID-matched audio-ready acknowledgement changes the screen to a red recording
  indicator with context/recipient and elapsed time. Already queued preparation inputs are
  processed first; the UI renders and waits for its queued SPI pixel transfer to finish before an
  operation-tagged start control is sent. The audio worker then rearms RX DMA, discarding
  preparation pre-roll before its first read.
- `OK CLICK`: finish early. Recording also stops at five seconds. If a fixed-length HTTP
  upload is already open, the remaining samples are zero-filled rather than retaining more
  microphone data.
- `OK LONG`: cancel; no audio or text is submitted.
- Audio is 16 kHz, signed 16-bit, mono PCM and is streamed/buffered in bounded chunks by
  the audio worker. The firmware never owns a whole unbounded recording.
- Audio-ready, transcription, and action completion events are delivered reliably and matched
  to the active operation ID. Start, finish, and cancel controls are also operation-tagged so a
  delayed control cannot alter a later capture; buttons and coalescible poll notices may still be
  dropped when full.

### Review state

- The gateway returns a bounded UTF-8 transcription.
- The screen shows context plus the transcription before any WorkBuddy write occurs.
- `OK CLICK`: submit once with a stable operation ID.
- `DOWN CLICK`: record again.
- `OK LONG`: cancel and return to the previous view.
- A timeout is not treated as success. The operation remains queryable by ID so retrying
  cannot create a duplicate message or task.

### Result and failure states

- Success shows a receipt ID and then returns to the relevant list after confirmation.
- Wi-Fi, authentication, gateway, transcription, and WorkBuddy failures have distinct,
  short user-facing states. Raw server errors, credentials, message bodies, and tokens are
  never logged.
- Cached summaries are explicitly marked stale when a poll fails.
- Live task details stop before recording and explain that cloud-task follow-up is reserved
  for a later release. Only a direct device-API call receives the non-retryable
  `not_supported` result.

## Architecture

```text
AI Passport firmware
  UI owner + pure model + bounded workers
      |  versioned REST, device bearer token
      v
Local/hosted WorkBuddy gateway
  auth + normalization + idempotency + speech-to-text
      |  OAuth bearer + official schemas
      v
Tencent WorkBuddy Open API / optional transcription provider
```

### Firmware boundaries

- `main/workbuddy_model.*`: hardware-independent state, navigation, focus, operation IDs,
  cursor handling, task-status normalization, and transition rules.
- `main/workbuddy_protocol.*`: bounded JSON parsing/serialization for the device contract.
- `main/workbuddy_app.*`: queues, poll scheduling, worker ownership, and immutable UI
  snapshots.
- `main/workbuddy_wifi.*`: STA lifecycle and reconnect backoff.
- `main/workbuddy_transport.*`: bounded HTTP requests and response caps.
- `main/workbuddy_audio.*`: single owner of the blocking BSP audio calls and short PCM
  capture.
- `main/workbuddy_ui.*`: the only owner of LVGL objects; consumes snapshots and posts
  lightweight commands.
- `main/main.c`: BSP initialization, dependency degradation, and direct application launch.

Display and button failures are fatal to the UI. Audio, battery, Wi-Fi, storage, and remote
service failures degrade independently. Network, audio, and button callbacks never touch
LVGL directly. Current `main` BSP, partition table, Recovery boot hook, and firmware
verification scripts remain intact.

### Gateway boundaries

The gateway is a dependency-light Python 3 service in `tools/workbuddy_gateway/`:

- `config.py`: validates environment configuration at startup.
- `workbuddy_client.py`: the only module that speaks the official WorkBuddy Open API.
- `transcription.py`: converts bounded PCM to WAV and calls a configurable transcription
  endpoint. OpenAI `/v1/audio/transcriptions` is the first adapter.
- `service.py`: normalized snapshot, action, receipt, and idempotency rules.
- `server.py`: HTTP parsing, authentication, request IDs, response shaping, and graceful
  shutdown.
- `demo_adapter.py`: deterministic data so the complete UI and HTTP contract can be tested
  without customer credentials.

No database is required in V1. A small local state file stores only refresh credentials
when explicitly configured and operation receipts; writes are atomic and the file is
excluded from Git. Production deployment should place the service behind HTTPS. Plain HTTP
is allowed only for an explicitly enabled development LAN mode.

## Device API contract

All responses include `version: 1` and `request_id`. Authenticated requests use
`Authorization: Bearer <device token>`.

### `GET /v1/snapshot`

Returns a cursor, freshness, assistant availability, up to six messages, six tasks, and six
artifact summaries. Every string and array has a documented maximum; the gateway truncates
at Unicode boundaries and the firmware independently enforces its own caps.

### `POST /v1/transcriptions`

Accepts `audio/L16;rate=16000;channels=1`, an operation ID, and no more than five seconds
of PCM. Returns a transcription draft; it does not write to WorkBuddy.

### `POST /v1/actions`

Accepts one of:

- `reply`: context message ID plus confirmed text;
- `task_create`: confirmed task prompt;
- `task_followup`: task ID plus confirmed text, reserved for forward compatibility.

Each request includes a stable operation ID. A repeated operation ID returns the stored
receipt without repeating the upstream mutation.

A direct `task_followup` submission records a failed operation and returns HTTP 501
`not_supported`; repeating the same ID and body returns the stored failure without another
attempt. Current Live firmware never sends this action because it blocks before recording.
Production support is deferred until durable async state, ACP permissions, and cross-
connection idempotency are designed and implemented together.

### `GET /v1/operations/{operation_id}`

Returns `PENDING`, `SUCCEEDED`, or `FAILED` plus a bounded receipt/error. This is used after
ambiguous timeouts.

### `GET /healthz` and `GET /readyz`

Liveness is independent of upstream availability. Readiness reports whether configuration
is valid and whether the last WorkBuddy check succeeded, without leaking secrets.

## WorkBuddy mapping

- Inbox uses `GET /openapi/v2/localassistant/message` and incremental `message_id` polling.
- Reply uses `POST /openapi/v2/localassistant/message` with `msg_type: text`.
- Task list/create/detail use `/openapi/v2/tasks` and `/openapi/v2/tasks/{task_id}`.
- Outputs use the task's returned sandbox/ACP credential and the documented artifact list
  endpoint. The gateway returns summaries only; it never forwards ACP tokens to the badge.
- WorkBuddy OAuth client secret and refresh token remain server-side. The badge receives
  only the gateway's independently revocable device token.

## Configuration and demo mode

Firmware build configuration contains no committed credentials. Local `sdkconfig` values
provide Wi-Fi SSID/password, gateway URL, device token, and an SNTP source. HTTPS traffic is
gated on a fresh boot-time clock synchronization, after which Mbed TLS validates certificate
dates, hostname, and trust chain. If Live configuration is not selected, the firmware enters a
clearly labelled offline demo mode with deterministic sample messages, tasks, outputs, and
transcription so a clean repository build remains installable and testable.

Gateway live mode requires WorkBuddy OAuth configuration and a device token. Gateway demo
mode requires no external credentials. Transcription can use an OpenAI-compatible endpoint;
the API key exists only in gateway environment/state and is never compiled into firmware.

## Resource and safety limits

- ESP32-C3, 8 MB Flash, no PSRAM; application image at most `0x300000` bytes.
- Keep the upstream single LVGL draw buffer. Do not import the unverified double-buffer game
  optimization.
- Snapshot body <= 12 KiB, transcription text <= 512 UTF-8 bytes, item title <= 128 bytes,
  item preview <= 384 bytes, and six items per collection.
- Capture at most five seconds. Audio chunks are fixed-size; only one audio owner exists.
- Do not persist message bodies, task prompts, recordings, access tokens, ACP tokens, or
  artifact payloads in firmware NVS.
- All received content is untrusted display data and can never trigger a tool, reply, or
  task without a physical confirmation.

## Verification

Host tests cover model transitions, navigation, status mapping, cursor deduplication,
bounded protocol parsing, operation idempotency, gateway auth, upstream normalization,
transcription limits, and demo-mode end-to-end requests.

The repository gates are:

```bash
./tools/validate.sh --static
./tools/validate.sh --firmware
./tools/validate.sh
```

The firmware gate builds and verifies both Demo and a placeholder-only Live compile profile,
and asserts the generated profile mode, Bluetooth disablement, HTTPS policy, trust bundle, and
certificate-date settings.

Device acceptance remains separate: display/font readability, every button path, Wi-Fi
reconnect, SNTP and TLS certificate-date validation, five-second capture, Chinese transcription,
notification deduplication, 30-minute polling, heap/stack watermarks, Recovery entry, and
installation of the merged artifact.

## V1 delivery

Delivery consists of the committed source on `feature/workbuddy-ai-passport`, gateway run
instructions, host tests, and the verified `build/FoloToy-AI-Passport-full.bin`. Build and
host-test results must never be reported as device-test results.
