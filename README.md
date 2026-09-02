<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# WorkBuddy for AI Passport

WorkBuddy for AI Passport turns the FoloToy badge into a private wearable inbox for Tencent WorkBuddy. You can glance at message and task status, review output summaries, record a short instruction, confirm its transcript, and send it through a gateway. The default firmware is a credential-free demo. Live mode remains an integration preview and has not passed physical-device acceptance.

This is an independent prototype. It is not an official Tencent WorkBuddy or FoloToy release.

## Understand the phone-free boundary

The badge does not use a phone as its runtime bridge. It connects over Wi-Fi to a gateway that you operate. This does not make the complete flow computer-free:

- WorkBuddy local-assistant messages and replies require the user's WorkBuddy PC local assistant to be online
- The gateway must remain reachable from the badge, either on a computer or a hosted service
- Cloud tasks run through WorkBuddy, but the badge still needs the gateway for authentication and normalization
- Initial WorkBuddy authorization, gateway configuration, firmware installation, and recovery happen outside the badge

The [WorkBuddy hardware integration guide](https://open.workbuddy.cn/en/docs/third-party-app) documents app review, OAuth 2.1 authorization, and the PC local-assistant dependency. The [WorkBuddy Open API reference](https://open.workbuddy.cn/en/docs/openapi) defines the current `/openapi/v2` endpoints.

## Check the V1 capability boundary

V1 keeps all remote content behind a privacy cover and limits every collection to six summaries.

| Capability | V1 behavior | Boundary |
| --- | --- | --- |
| Privacy cover | Shows connectivity, unread count, and active-task count | Message text appears only after physical confirmation |
| Inbox | Reads local-assistant messages and sends confirmed `text` messages | The selected message supplies local context; the upstream API creates a new message and has no reply-target field |
| Tasks | Lists normalized status, opens details, and creates a task from confirmed speech | Live access needs `user.task.readable` and `user.task.invokable` |
| Task follow-up | Demo mode can exercise the interaction state; Live task details show that follow-up is reserved for a later release before recording starts | **NOT_SUPPORTED in V1**: a direct gateway `task_followup` request returns HTTP 501 `not_supported` because Agent Client Protocol (ACP) v1 streaming and JSON-RPC are not implemented |
| Outputs | Shows bounded plan, checklist, overview, image, and document summaries discovered for visible tasks | No file download, image preview, Office rendering, or arbitrary artifact URL reaches the badge |
| Voice | Captures up to 5 seconds of 16 kHz, 16-bit mono PCM and displays the returned transcript before submission | There is no local automatic speech recognition; live transcription needs a configured provider |
| Refresh | Polls the gateway every 30 seconds by default and marks failed snapshots stale | Delivery is not instant, and outages can delay visible updates |

Demo mode simulates the interaction, including result screens. A simulated result does not prove live WorkBuddy, transcription, Wi-Fi, or ACP behavior.

## Use the default demo

The tracked configuration enables `CONFIG_WB_DEMO_MODE=y`. The firmware starts without Wi-Fi, gateway credentials, or transcription credentials. It uses deterministic messages, tasks, outputs, and transcripts.

Activate ESP-IDF 5.5.3, then run the repository gates:

```sh
./tools/validate.sh --static
./tools/validate.sh --firmware
```

The firmware gate produces `build/FoloToy-AI-Passport-full.bin`. Follow the [AI Passport Codex play guide](https://ai-passport.folotoy.cn/en/guides/create-a-play-with-codex/) to install a local build on a development device. Do not treat a successful build as a device test.

Preserve the existing install and recovery contract:

- The application partition stays at `0x10000` with a `0x300000` byte limit
- Device identity stays in `cardid` at `0x356000`
- Permanent Recovery stays at `0x700000`
- Holding `UP` for 5 seconds during boot remains the Recovery entry path
- Never run `idf.py erase-flash` on a provisioned device

Read the [build guide](docs/development/engineering/build-and-test.md) and [Recovery compatibility contract](docs/development/engineering/ble-recovery-compatibility.md) before installing firmware.

## Configure live mode

Live mode requires approval and external services. Do not put credentials in this repository.

1. Register a hardware-access application in the WorkBuddy Open Platform and wait for approval.
2. Request only the scopes used by this build: `user.localassistant.readable`, `user.localassistant.invokable`, `user.task.readable`, and `user.task.invokable`.
3. Complete the OAuth 2.1 authorization-code flow outside this gateway. Supply either an access token or the complete refresh-credential set.
4. Configure a compatible speech-to-text endpoint.
5. Run the Python 3 standard-library gateway on a host reachable from the badge.

The gateway accepts these live settings:

```sh
export WORKBUDDY_GATEWAY_MODE=live
export WORKBUDDY_GATEWAY_HOST=0.0.0.0
export WORKBUDDY_GATEWAY_DEVICE_TOKEN='generate_a_random_value_at_least_16_characters'
export WORKBUDDY_BASE_URL='https://www.workbuddy.cn'
export WORKBUDDY_ACCESS_TOKEN='workbuddy_access_token_here'
export WORKBUDDY_TRANSCRIPTION_URL='https://provider.example/v1/audio/transcriptions'
export WORKBUDDY_TRANSCRIPTION_API_KEY='transcription_api_key_here'
PYTHONPATH=tools/workbuddy_gateway python3 -m workbuddy_gateway.server
```

Replace `WORKBUDDY_ACCESS_TOKEN` with all three refresh settings when needed: `WORKBUDDY_REFRESH_TOKEN`, `WORKBUDDY_CLIENT_ID`, and `WORKBUDDY_CLIENT_SECRET`. The gateway posts refresh requests to `/openapi/v2/token`. In refresh mode it saves a rotated refresh token in the owner-only gateway state file and prefers that client-scoped token after restart. Keep the state file and its backup outside the checkout in production.

The built-in gateway server does not terminate TLS. Put it behind HTTPS for a live deployment. Plain HTTP requires the firmware's explicit development-LAN option and must stay on an isolated network.

Run `idf.py menuconfig`, open **WorkBuddy AI Passport**, and disable demo mode. Enter the Wi-Fi SSID, Wi-Fi password, gateway URL, and the same gateway device token in the local ignored `sdkconfig`. The device token authenticates only to your gateway. It is not a WorkBuddy OAuth token.

The WorkBuddy `client_secret`, refresh token, access token, ACP ticket, sandbox link, and transcription key stay on the gateway. None belongs in firmware, logs, example files, or commits. Treat the gateway state file as a secret when refresh credentials are enabled.

## Know what remains unverified

V1 does not implement ACP task follow-up, `permission_response`, streamed task events, artifact downloads, local speech recognition, raise-to-wake, cellular access, or Office-file rendering. Live firmware blocks task follow-up before recording and shows that it is reserved for a later release. Production follow-up remains deferred until durable asynchronous phases, ACP permission handling, and idempotency across disconnects and restarts are designed together. The board has no confirmed inertial measurement unit (IMU).

No live WorkBuddy account, OAuth approval, public gateway deployment, or real transcription provider has been validated as part of this repository delivery. Physical display readability, buttons, microphone, speaker, Wi-Fi reconnection, heap and stack margins, battery life, installation, and Recovery entry also remain unverified until tested on an AI Passport.

## Find the implementation

- Firmware application: `main/workbuddy_*` and `main/fonts/`
- Device gateway and tests: [gateway guide](tools/workbuddy_gateway/README.md)
- V1 design: [WorkBuddy AI Passport V1 design](docs/superpowers/specs/2026-09-03-workbuddy-ai-passport-v1-design.md)
- Implementation plan: [WorkBuddy AI Passport V1 implementation plan](docs/superpowers/plans/2026-09-03-workbuddy-ai-passport-v1.md)

This fork retains the upstream [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) hardware baseline and repository [MIT license](LICENSE). The generated Noto Sans SC font subset carries its separate SIL Open Font License in `assets/fonts/`.
