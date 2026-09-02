<p align="right">
  <a href="2026-09-03-workbuddy-ai-passport-v1.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# WorkBuddy AI Passport V1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce an installable AI Passport firmware and a tested gateway that support a privacy-safe WorkBuddy inbox, task/output status, voice transcription review, and confirmed replies or task creation.

**Architecture:** The ESP-IDF firmware owns bounded UI/input/audio/network state and talks only to a small versioned gateway contract. A Python standard-library gateway owns Tencent WorkBuddy OAuth, official API normalization, transcription-provider credentials, and idempotent writes; deterministic demo adapters keep clean builds and tests credential-free.

**Tech Stack:** ESP-IDF 5.5.3, C11, FreeRTOS, LVGL 9, cJSON, `esp_http_client`, ESP32 Wi-Fi, Python 3.11 standard library, `unittest`.

**Fixed V1 boundary:** Production task follow-up requires ACP SSE and JSON-RPC plus a
durable asynchronous phase model, permission-request handling, and idempotency across
connection loss. V1 does not implement that path. Demo mode may exercise the firmware
state machine; Live firmware stops before recording and shows a later-release notice. A
direct device-API `task_followup` request returns HTTP 501 `not_supported`.

---

## File map

Firmware production files:

- `main/main.c`: board initialization and direct WorkBuddy launch only.
- `main/Kconfig.projbuild`: credential-free build settings and explicit demo/live mode.
- `main/workbuddy_types.h`: all bounded cross-module types and limits.
- `main/workbuddy_model.[ch]`: pure page, focus, recording, review, notification, and operation state.
- `main/workbuddy_protocol.[ch]`: bounded snapshot parser and action serializer.
- `main/workbuddy_app.[ch]`: queue ownership and worker orchestration.
- `main/workbuddy_wifi.[ch]`: STA lifecycle and reconnect state.
- `main/workbuddy_transport.[ch]`: gateway HTTP polling, transcription upload, action submit, and receipt query.
- `main/workbuddy_audio.[ch]`: one blocking audio owner and bounded PCM pipeline.
- `main/workbuddy_ui.[ch]`: LVGL privacy cover, lists, details, voice review, result, menu, and status.
- `main/CMakeLists.txt`, `sdkconfig.defaults`: component registration and firmware feature settings.

Host tests:

- `tests/test_workbuddy_model.c`: state transitions and controls.
- `tests/test_workbuddy_protocol.c`: valid/invalid/bounded JSON and action bodies.
- `tests/run_host_tests.py`: compile and execute every C host test.

Gateway:

- `tools/workbuddy_gateway/workbuddy_gateway/{config,errors,models,store,demo_adapter,workbuddy_client,transcription,service,server}.py`.
- `tools/workbuddy_gateway/tests/`: config, contract, auth, normalization, idempotency, transcription, and live-client request tests.
- `tools/workbuddy_gateway/README.md`, `.env.example`, `openapi.yaml`.

Documentation:

- `README.md` / `README.zh_CN.md`, `docs/CHANGELOG.md` / `.zh_CN.md`.
- `PROJECT.md`, `MEMORY.md`, `TASKS.md`, `WORKLOG.md` plus Chinese pairs in the firmware repository.

### Task 1: Lock the firmware domain contract with host tests

**Files:**
- Create: `main/workbuddy_types.h`
- Create: `main/workbuddy_model.h`
- Create: `main/workbuddy_model.c`
- Create: `main/workbuddy_protocol.h`
- Create: `main/workbuddy_protocol.c`
- Create: `tests/test_workbuddy_model.c`
- Create: `tests/test_workbuddy_protocol.c`
- Create: `tests/run_host_tests.py`

- [ ] **Step 1: Write failing model tests**

  Cover privacy-cover entry, `OK` to Inbox, list clamping, menu navigation, context-aware
  voice start, demo task-follow-up state, Live task-follow-up blocking before recording,
  five-second timeout, re-record, cancel, one-shot submit lock, stale snapshot, cursor
  deduplication, and WorkBuddy status normalization. Compile before implementations exist
  and confirm missing-symbol failures.

- [ ] **Step 2: Implement the bounded types and model**

  Use fixed arrays and explicit caps:

  ```c
  #define WB_MAX_ITEMS 6
  #define WB_ID_BYTES 65
  #define WB_TITLE_BYTES 129
  #define WB_PREVIEW_BYTES 385
  #define WB_TRANSCRIPT_BYTES 513

  bool wb_model_dispatch(wb_model_t *model, const wb_event_t *event,
                         wb_effect_t *effect);
  ```

  No ESP-IDF or LVGL header may appear in these files.

- [ ] **Step 3: Verify model tests pass**

  Run `python3 tests/run_host_tests.py --test model`; expect every model case to pass with
  `-std=c11 -Wall -Wextra -Werror`.

- [ ] **Step 4: Write failing protocol tests**

  Test a full snapshot, missing version, wrong types, over-limit arrays/strings, invalid
  UTF-8, unknown task states, duplicate cursor, reply serialization, task-create
  serialization, the reserved task-follow-up action shape, escaping, and fixed operation
  IDs.

- [ ] **Step 5: Implement protocol parsing and serialization**

  Wrap cJSON behind a small allocation-injected interface for firmware and a no-dependency
  minimal test shim for host builds. Reject, truncate, or normalize exactly as specified by
  the public API contract; never copy with an unchecked string function.

- [ ] **Step 6: Verify all firmware host tests pass**

  Run `python3 tests/run_host_tests.py`; expect zero compile warnings and zero failed cases.

### Task 2: Build the gateway contract test-first

**Files:**
- Create: `tools/workbuddy_gateway/workbuddy_gateway/__init__.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/config.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/errors.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/models.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/store.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/demo_adapter.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/workbuddy_client.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/transcription.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/service.py`
- Create: `tools/workbuddy_gateway/workbuddy_gateway/server.py`
- Create: `tools/workbuddy_gateway/tests/test_config.py`
- Create: `tools/workbuddy_gateway/tests/test_service.py`
- Create: `tools/workbuddy_gateway/tests/test_server.py`
- Create: `tools/workbuddy_gateway/tests/test_workbuddy_client.py`
- Create: `tools/workbuddy_gateway/tests/test_transcription.py`

- [ ] **Step 1: Write failing configuration and model tests**

  Assert demo mode starts without secrets; live mode rejects missing device token,
  WorkBuddy base URL, access/refresh configuration, or transcription configuration. Assert
  all public DTOs enforce collection and Unicode-byte limits.

- [ ] **Step 2: Implement typed configuration and errors**

  Centralize environment reads in `GatewayConfig.from_env()`. Use `GatewayError(code,
  status, public_message, retryable)` and one JSON error shape. Redact every token value.

- [ ] **Step 3: Write failing adapter/service tests**

  Specify normalized snapshots from official message/task fixtures, status mapping,
  artifact summaries, incremental cursor behavior, reply/task-create requests, direct
  task-follow-up HTTP 501 with a stored failed operation, duplicate operation receipts, and
  atomic state-file recovery.

- [ ] **Step 4: Implement adapters and service**

  `WorkBuddyClient` must be the only class that knows official paths:

  ```text
  GET/POST /openapi/v2/localassistant/message
  GET/POST /openapi/v2/tasks
  GET      /openapi/v2/tasks/{task_id}
  GET      {sandbox_url}/api/session/artifacts
  ```

  OAuth client secret, refresh token, WorkBuddy access token, ACP token, and sandbox URL
  never appear in the normalized device response. Do not add a partial production ACP
  adapter: V1 must keep `task_followup` explicitly unsupported until durable async phases,
  permission handling, and cross-connection idempotency are implemented together.

- [ ] **Step 5: Write failing transcription tests**

  Cover exact PCM/WAV sizes, one/five-second bounds, unsupported media type, provider
  timeout, malformed JSON, Unicode result cap, and demo transcription.

- [ ] **Step 6: Implement transcription adapter**

  Convert 16 kHz/16-bit/mono PCM to an in-memory WAV capped at five seconds, then issue a
  multipart request to the configured `/v1/audio/transcriptions` endpoint. The default
  model is `gpt-4o-mini-transcribe`; the provider key is environment-only.

- [ ] **Step 7: Write failing HTTP contract tests**

  Start the real threaded server on an ephemeral port. Test `/healthz`, `/readyz`, bearer
  auth, body caps, content types, request IDs, snapshot, transcription, actions, receipts,
  404, 405, malformed JSON, and graceful shutdown.

- [ ] **Step 8: Implement and verify the server**

  Run `python3 -m unittest discover -s tools/workbuddy_gateway/tests -v`; expect all tests to
  pass without third-party packages or network calls.

### Task 3: Integrate firmware UI and controls

**Files:**
- Create: `main/workbuddy_ui.h`
- Create: `main/workbuddy_ui.c`
- Create: `main/workbuddy_app.h`
- Create: `main/workbuddy_app.c`
- Modify: `main/main.c`
- Modify: `main/CMakeLists.txt`
- Modify: `sdkconfig.defaults`
- Create: `main/Kconfig.projbuild`
- Test: `tests/test_workbuddy_model.c`

- [ ] **Step 1: Extend failing model tests for every physical-key path**

  Enumerate `UP`, `DOWN`, `OK` × click/long for privacy cover, browse, detail, menu,
  recording, transcribing, review, sending, result, and error states. Assert unsupported
  events are no-ops and no action is emitted twice.

- [ ] **Step 2: Implement direct-launch app orchestration**

  Initialize I2C/display/LVGL first; button, audio, battery, storage, and network degrade
  independently. Button callbacks enqueue only `wb_event_t`. A UI timer drains immutable
  snapshots under the LVGL lock.

- [ ] **Step 3: Implement the wearable UI**

  Build fixed 240×320 regions: 24 px status bar, 248 px content, 48 px action/footer. Use
  Source Han Sans SC 14 CJK for remote text and Montserrat 20 for the WorkBuddy title/count.
  Show privacy cover, list, detail, nav sheet, recording, transcription review,
  sending/sent/failed, stale marker, and a clear Demo badge.

  Demo task details may enter the follow-up interaction to exercise the state machine. In
  Live mode, `OK` on a task detail must stop before recording, show “Cloud task follow-up
  will be available in a later release,” label the footer as reserved, and emit no action.

- [ ] **Step 4: Re-run host tests and static repository checks**

  Run `python3 tests/run_host_tests.py` and `./tools/validate.sh --static`. Fix warnings,
  documentation-pair failures, and secret scans before continuing.

### Task 4: Add bounded Wi-Fi, transport, and audio workers

**Files:**
- Create: `main/workbuddy_wifi.h`
- Create: `main/workbuddy_wifi.c`
- Create: `main/workbuddy_transport.h`
- Create: `main/workbuddy_transport.c`
- Create: `main/workbuddy_audio.h`
- Create: `main/workbuddy_audio.c`
- Modify: `main/workbuddy_app.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/Kconfig.projbuild`
- Modify: `sdkconfig.defaults`
- Test: `tests/test_workbuddy_model.c`
- Test: `tests/test_workbuddy_protocol.c`

- [ ] **Step 1: Add failing retry/timeout/cancellation model tests**

  Cover Wi-Fi offline/stale behavior, exponential reconnect caps, poll coalescing, audio
  queue backpressure, early stop with silence padding, network interruption, ambiguous
  action timeout, receipt lookup, and explicit cancellation.

- [ ] **Step 2: Implement Wi-Fi lifecycle**

  Use one STA netif, one registered event pair, NVS-backed ESP Wi-Fi credentials, and capped
  reconnect backoff. Demo mode does not start the radio. Never log SSID passwords.

- [ ] **Step 3: Implement HTTP transport**

  Configure connect/read timeouts and a 12 KiB response ceiling. Accept only configured
  `http://` in explicit development mode or verified `https://` in live mode. Include API
  version, bearer token, content type, operation ID, and request correlation. Log only
  status/code/length.

- [ ] **Step 4: Implement audio pipeline**

  One audio task reads 2 KiB PCM chunks; one bounded FIFO feeds the transport. Capture
  stops at five seconds or an explicit stop event, then zero-fills the fixed request length
  without reading more microphone data. Queue overflow cancels the operation and displays a
  retryable failure rather than silently dropping audio.

- [ ] **Step 5: Integrate poll, action, and demo workers**

  Poll every 30 seconds while active and immediately on Sync. Coalesce snapshot refreshes,
  never overwrite mutation commands, and update UI through immutable snapshots only. Demo
  mode uses the same model/effects with deterministic local responses.

- [ ] **Step 6: Verify host and firmware builds**

  Run the host suites, `./tools/validate.sh --static`, then activate ESP-IDF 5.5.3 and run
  `./tools/validate.sh --firmware`. Record image size and inspect largest static consumers.

### Task 5: Document configuration, API, and product truth

**Files:**
- Create: `tools/workbuddy_gateway/.env.example`
- Create: `tools/workbuddy_gateway/openapi.yaml`
- Create: `tools/workbuddy_gateway/README.md`
- Modify: `README.md`
- Modify: `README.zh_CN.md`
- Modify: `docs/CHANGELOG.md`
- Modify: `docs/CHANGELOG.zh_CN.md`
- Create: `PROJECT.md`, `PROJECT.zh_CN.md`
- Create: `MEMORY.md`, `MEMORY.zh_CN.md`
- Create: `TASKS.md`, `TASKS.zh_CN.md`
- Create: `WORKLOG.md`, `WORKLOG.zh_CN.md`

- [ ] **Step 1: Document exact demo and live commands**

  Include gateway start, ephemeral smoke test, firmware menuconfig fields, build command,
  merged image path, Recovery entry, and safe flashing caveats. Never place a real token,
  SSID, callback URL secret, or device identity in a tracked file.

- [ ] **Step 2: Document source-backed limitations**

  State that runtime is phone-free but Wi-Fi/gateway-dependent; first setup is external;
  there is no IMU/cellular/local ASR/Office parser; WorkBuddy PC assistant availability,
  polling latency, OAuth application registration, and physical battery life remain external
  dependencies. State separately that demo follow-up is simulated, Live firmware blocks
  before recording, and direct device-API `task_followup` returns HTTP 501.

- [ ] **Step 3: Run documentation and OpenAPI consistency checks**

  Verify English/Chinese pairs, links, example JSON, endpoint names, caps, and config names
  match production code exactly. Run `git diff --check` and secret scans.

### Task 6: Complete validation and handoff

**Files:**
- Modify only files required by failures found in this task.

- [ ] **Step 1: Run gateway tests and smoke test**

  Run `python3 -m unittest discover -s tools/workbuddy_gateway/tests -v`, start demo gateway
  on an ephemeral port, and exercise health, authenticated snapshot, transcription, action,
  duplicate action, direct task-follow-up HTTP 501, and receipt endpoints with real HTTP
  requests.

- [ ] **Step 2: Run full repository gate**

  Activate exactly ESP-IDF 5.5.3 and run `./tools/validate.sh`. Confirm the merged image
  verifier preserves `cardid`, Recovery, boot hook, partition MD5, and 3 MB application cap.

- [ ] **Step 3: Review requirements and repository diff**

  Check every design requirement against code/tests/docs, inspect `git status`, `git diff`,
  generated dependency changes, and artifact paths. Remove no user files and commit no
  credentials.

- [ ] **Step 4: Report evidence honestly**

  Provide `Environment`, `Build`, `Host tests`, `Device tests`, and `Unverified` separately,
  with the exact installable image path and checksum. Physical display, button, microphone,
  Wi-Fi, power, and Recovery checks remain `NOT RUN` until performed on a board.
