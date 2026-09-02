<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 运行 WorkBuddy 设备网关

WorkBuddy 设备网关为 AI Passport 提供小型且有明确容量上限的 HTTP API。它把腾讯 WorkBuddy OAuth 与转写凭据留在工卡之外，归一化上游数据，并保存幂等 operation receipt。服务只使用 Python 3.11 标准库，默认进入确定性的 demo 模式。

内置服务只提供明文 HTTP。它只能直接用于隔离的开发网络。任何 Live 部署都应在前面配置 HTTPS 反向代理。

## 启动 demo 模式

从仓库根目录启动服务。进程直接读取环境变量，不会加载 `.env` 文件。

```sh
export WORKBUDDY_GATEWAY_DEVICE_TOKEN='replace_with_a_local_demo_device_token'
export WORKBUDDY_GATEWAY_STATE_PATH='/tmp/workbuddy_gateway_demo_state.json'
PYTHONPATH=tools/workbuddy_gateway python3 -m workbuddy_gateway.server
```

没有覆盖配置时，demo 模式监听 `127.0.0.1:8787`。它不需要 WorkBuddy 或转写凭据，快照、转写文字、消息 receipt 与任务 receipt 都是确定性的。任务追问仍返回 HTTP 501 `not_supported`。

在另一个终端检查服务：

```sh
curl --fail --silent --show-error http://127.0.0.1:8787/healthz
curl --fail --silent --show-error \
  -H "Authorization: Bearer ${WORKBUDDY_GATEWAY_DEVICE_TOKEN}" \
  http://127.0.0.1:8787/v1/snapshot
```

固件默认 demo 模式不会调用这个服务，而是使用固件内置快照与转写，让干净构建不依赖凭据。

## 配置 Live 模式

Live 模式要求腾讯审核并启用你的 WorkBuddy 硬件接入应用。请在本服务之外完成 OAuth 2.1 授权码流程。网关接收已有 access token，或一套完整的刷新凭据。

只申请产品路径需要的 scope：

- `user.localassistant.readable`：本地助理在线状态与消息历史
- `user.localassistant.invokable`：发送确认后的文字消息
- `user.task.readable`：任务列表、任务详情、ACP link 与 ticket，以及产出物摘要
- `user.task.invokable`：创建云端任务

把 [`.env.example`](.env.example) 当作变量清单，再把选中的值导出到进程环境。仓库会忽略 `.env` 与 `.env.*`（受版本控制的示例除外），但网关不会读取 dotenv 文件。生产凭据应放在 secret manager 或服务环境中，不要依赖一个被忽略的文件。

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

需要刷新 access token 时，用三项刷新变量替代 `WORKBUDDY_ACCESS_TOKEN`：

```sh
export WORKBUDDY_REFRESH_TOKEN='workbuddy_refresh_token_here'
export WORKBUDDY_CLIENT_ID='workbuddy_client_id_here'
export WORKBUDDY_CLIENT_SECRET='workbuddy_client_secret_here'
unset WORKBUDDY_ACCESS_TOKEN
```

网关向 `https://www.workbuddy.cn/openapi/v2/token` 发送 `application/x-www-form-urlencoded` 刷新请求。它使用 `expires_in`，提前 60 秒刷新，在上游返回 401 后刷新并重试一次，并接收轮换后的 refresh token。采用轮换 token 前，网关会先把它写入状态文件，并用 client ID 的 SHA-256 作为作用域；重启后优先使用这份持久化 token。Access token 不会持久化。

`client_secret`、refresh token、access token、ACP ticket、sandbox link 与转写 key 都必须留在网关信任边界内，不能复制进固件设置或设备响应。刷新模式下的状态文件与备份含 refresh token，必须按 secret 管理。

## 逐项核对环境变量

下表与 `GatewayConfig.from_env()` 一致：

| 变量 | Demo 默认值 | Live 规则 |
| --- | --- | --- |
| `WORKBUDDY_GATEWAY_MODE` | `demo` | 只能是 `demo` 或 `live` |
| `WORKBUDDY_GATEWAY_HOST` | `127.0.0.1` | 非空监听地址；只有在网络边界明确时才监听可访问接口 |
| `WORKBUDDY_GATEWAY_PORT` | `8787` | 0 到 65535 的整数；只有需要临时端口的测试才使用 0 |
| `WORKBUDDY_GATEWAY_INBOUND_TIMEOUT_SECONDS` | `5` | 连接读取超时；大于 0 且不超过 60 秒 |
| `WORKBUDDY_GATEWAY_MAX_CONNECTIONS` | `8` | 并发连接上限；取值 1 到 64 |
| `WORKBUDDY_GATEWAY_DEVICE_TOKEN` | `workbuddy-demo-device-token` | 必填，至少 16 个字符 |
| `WORKBUDDY_GATEWAY_STATE_PATH` | `.workbuddy-gateway-state.json` | operation 与轮换 token 状态；默认文件及其备份/临时文件已忽略，但生产环境应使用 checkout 外的受保护路径 |
| `WORKBUDDY_REQUEST_TIMEOUT_SECONDS` | `15` | 出站请求超时；大于 0 且不超过 120 秒 |
| `WORKBUDDY_BASE_URL` | 不使用 | 必须是无内嵌凭据且无 API path 的 HTTPS origin；使用 `https://www.workbuddy.cn` |
| `WORKBUDDY_ACCESS_TOKEN` | 不使用 | 支持的一种 WorkBuddy 凭据模式 |
| `WORKBUDDY_REFRESH_TOKEN` | 不使用 | 刷新模式下必须与两个 client 变量同时提供 |
| `WORKBUDDY_CLIENT_ID` | 不使用 | 必须与 refresh token 和 client secret 同时提供 |
| `WORKBUDDY_CLIENT_SECRET` | 不使用 | 必须与 refresh token 和 client ID 同时提供；仅网关使用 |
| `WORKBUDDY_TRANSCRIPTION_URL` | 不使用 | 必填，兼容 `/v1/audio/transcriptions` 的 HTTPS endpoint |
| `WORKBUDDY_TRANSCRIPTION_API_KEY` | 不使用 | 必填，仅网关使用 |
| `WORKBUDDY_TRANSCRIPTION_MODEL` | `gpt-4o-mini-transcribe` | 可选，传给转写服务的非空模型名 |

配置错误会阻止启动。URL 校验会拒绝内嵌用户名和密码。WorkBuddy client 还会拒绝带 path、query 或 fragment 的 base URL，并且不会跟随重定向。

状态存储会以仅文件所有者可读写的权限（`0600`）写入主文件、备份与临时文件。它保存有界的 operation 指纹与 receipt，不保存 action 文字；刷新模式还会把最新轮换 refresh token 保存在两个有效状态槽中，并以 client ID 的 SHA-256 为作用域。有效备份可以恢复损坏的主文件。如果已有状态材料但两个槽都无法校验，存储会关闭失败：readiness 保持 false，mutation 返回 HTTP 503 `state_corrupt`，直到操作者恢复可信状态或明确改用新路径。没有先去 WorkBuddy 核对前，不要丢弃结果不明的 `PENDING` operation。

## 理解设备 API

完整机器可读契约见 [`openapi.yaml`](openapi.yaml)。每个 JSON 响应都包含 `version: 1` 和 `request_id`。有效的 `X-Request-ID` 可以包含 1 到 64 个 ASCII 字母、数字、`.`、`_`、`:` 或 `-`；其他值会被替换为网关生成的 ID。

`GET /healthz` 和 `GET /readyz` 是公开接口。所有 `/v1/*` 接口都要求严格匹配 `Authorization: Bearer <device token>`。

| 路径 | 请求 | 成功响应 |
| --- | --- | --- |
| `GET /healthz` | 无鉴权 | `status: ok` |
| `GET /readyz` | 无鉴权 | `ready: true`；Live 上游快照成功前以 503 返回 `ready: false` |
| `GET /v1/snapshot?after=...` | 可选稳定快照游标 | `fresh`、本地助理在线状态、未读与活跃任务计数，以及有界消息、任务和产出物集合 |
| `POST /v1/transcriptions?operation_id=...` | 精确的 `audio/L16;rate=16000;channels=1` body | 转写草稿，不写入 WorkBuddy |
| `POST /v1/actions` | 严格 JSON action | operation 状态，成功时包含 receipt |
| `GET /v1/operations/{operation_id}` | 已存在的 operation ID | `PENDING`、`SUCCEEDED` 或 `FAILED` 记录 |

网关拒绝 chunked 请求体。POST 必须携带唯一一个 ASCII 十进制 `Content-Length`，且长度字段最多 10 位。JSON 只接受 `application/json`，可带 UTF-8 charset。重复 JSON key、多余 action 字段与不支持的版本都会关闭失败。请求 header 总量限制为 16 KiB；空闲或不完整连接会超时，超过并发上限的连接会被丢弃。

### 请求与响应上限

| 值 | 上限 |
| --- | --- |
| Action JSON 请求 | 4,096 字节 |
| PCM 请求 | 32,000 到 160,000 字节，且长度为偶数 |
| 解析后的请求 header | 16 KiB |
| 设备 JSON 响应 | 12 KiB |
| 集合 | 最多 6 条消息、6 个任务与 6 个产出物 |
| 标识符 | 64 UTF-8 字节 |
| 游标 | 64 UTF-8 字节；Live 模式使用稳定的 SHA-256 快照指纹 |
| 标题 | 128 UTF-8 字节 |
| 预览或产出物描述 | 384 UTF-8 字节 |
| 转写、回复文字或任务 prompt | 512 UTF-8 字节 |
| 公开 operation 错误文字 | 160 UTF-8 字节 |
| 持久化 operation | 默认 128 条；先淘汰已结束记录，不先淘汰 pending 记录 |
| Live 快照预算 | 总计 10 秒，最多 7 次物理 HTTP 尝试，包含 OAuth 刷新与 401 后重试 |
| 产出物缓存 | 每个符合条件的任务缓存 90 秒 |
| WorkBuddy 上游响应 | 64 KiB |
| OAuth token 响应 | 16 KiB |
| 转写服务响应 | 16 KiB |

文字截断会停在 UTF-8 边界。标识符和 action 文字超限时直接拒绝，不会截断。如果 JSON 转义使快照超过 12 KiB，服务会依次缩短预览、描述和任务/产出物标题；只有文字裁剪仍不够时，才从列表尾部移除产出物、任务和消息。计数字段仍描述裁剪前的归一化快照。

### Action 结构

向本地助理会话回复：

```json
{
  "version": 1,
  "operation_id": "device_operation_123",
  "type": "reply",
  "message_id": "message_123",
  "text": "请按确认后的版本继续。"
}
```

创建云端任务：

```json
{
  "version": 1,
  "operation_id": "device_operation_124",
  "type": "task_create",
  "prompt": "生成一页项目进展汇报。"
}
```

`task_followup` 为后续兼容保留了请求结构，但 V1 的所有 adapter 都会拒绝它。当前 Live 固件会在录音前停在说明提示，不会提交此动作；下方契约用于保护直接 API 客户端：

```json
{
  "version": 1,
  "operation_id": "device_operation_125",
  "type": "task_followup",
  "task_id": "task_123",
  "text": "补充关键风险。"
}
```

首次请求返回 HTTP 501，且 `error.code` 为 `not_supported`，同时把 operation 记录为 `FAILED`。使用相同 operation ID 和相同正文重试只返回已保存记录，不会再次产生 mutation。使用相同 operation ID 配合不同正文会返回 HTTP 409 `operation_conflict`。生产追问会等到可持久化的异步阶段、ACP 权限处理与跨断线/进程重启幂等方案一起完成后再开放。

网关会区分确定发生在提交前的失败与结果不明的写入。如果 OAuth 刷新或其他 operation 在能够提交 WorkBuddy mutation 前失败，错误可重试，预留记录会释放，设备可再次提交相同 operation ID。如果请求可能已到达 WorkBuddy、但结果无法确认，预留记录会保持 `PENDING`；重复相同请求只返回该记录，不会再发起 mutation。此时应查询 operation 并与 WorkBuddy 核对，不能改用新 ID 猜测重试。

Live 快照总截止从等待 adapter 锁之前开始，并覆盖 DNS、连接、写入与有界响应读取。DNS 固定使用两个 daemon worker 与两个任务的队列，慢解析不会无限创建线程。每次真实 WorkBuddy HTTP 尝试都会消耗共享的七次预算，token 刷新与 401 后重试也会计数。POST 网络失败或 429 等 POST 响应不能证明 mutation 在提交前已被拒绝，因此对应 operation 会保持 `PENDING`。

## 核对 WorkBuddy 映射

Live adapter 只调用以下上游接口：

| 用途 | WorkBuddy 请求 |
| --- | --- |
| PC 本地助理状态 | `GET /openapi/v2/localassistant` |
| 消息历史 | `GET /openapi/v2/localassistant/message?message_id=...&limit=6`；归一化官方 `content: string[]` 条目与兼容的 text block |
| 发送文字 | `POST /openapi/v2/localassistant/message`，body 包含 `content` 与 `msg_type: text` |
| 任务列表 | `GET /openapi/v2/tasks?page=1&size=6` |
| 创建任务 | `POST /openapi/v2/tasks`，body 包含 `prompt` |
| 任务详情 | `GET /openapi/v2/tasks/{task_id}` |
| Token 刷新 | `POST /openapi/v2/token`，使用 form body |

Live adapter 会把 WorkBuddy 消息游标与 64 字节设备快照指纹分开管理，把增量 history 合并为最近六条消息；消息、任务、产出物或本地助理在线状态变化都会推进设备游标。WorkBuddy `pending` 映射为 `NEEDS_INPUT`；archived 与 deleted 任务会被过滤，不会伪装成失败。

只有任务详情 `link` 的 path 精确以 `/acp` 结尾时，adapter 才会推导产物 endpoint。它移除该后缀，再使用任务 `token` 请求 `/api/session/artifacts?sessionId=...&limit=6`。任何含糊的 link 都会关闭失败。QUEUED、RUNNING、NEEDS_INPUT 与 COMPLETED 任务会在每次快照调用预算内轮转扫描，并使用 90 秒缓存。已删除的产物事件会被过滤，临时刷新失败则保留旧缓存。返回值中的 URL、URI、link、ticket 与 sandbox 数据会在进入设备响应前过滤。

产物类型采用封闭映射：`plan` 保持 `plan`，`tasks` 转为 `checklist`，`overview` 保持 `overview`，MIME 类型为 `image/*` 的 `media` 转为 `image`，其他 media 转为 `document`。未知结构会被跳过。

上游应用注册与数据结构见官方 [WorkBuddy 第三方应用指南](https://open.workbuddy.cn/docs/third-party-app)和 [Open API 文档](https://open.workbuddy.cn/docs/openapi)。这些文档不能替代本设备网关更窄的契约。

## 理解状态码

API 使用以下 HTTP 状态：

| 状态 | 本服务中的含义 |
| --- | --- |
| 200 | 请求成功；也包括重复 operation 返回已保存记录 |
| 400 | query、标识符、音频长度、JSON、版本或严格 action 结构无效 |
| 401 | 缺少或错误的设备 bearer token |
| 404 | 未知路径或 operation |
| 405 | 已知路径使用不支持的方法 |
| 408 | 不完整的 POST body 超过入站连接超时 |
| 409 | 相同 operation ID 被用于不同 action 内容 |
| 411 | POST 未携带唯一一个 `Content-Length` |
| 413 | 请求体超过对应路径上限 |
| 415 | JSON 或 PCM content type 不受支持 |
| 431 | 解析后的请求 header 超过 16 KiB |
| 500 | 运行状态无效、状态文件写入失败或有界响应失败 |
| 501 | 不支持 `task_followup` |
| 502 | WorkBuddy 或转写服务拒绝、不可访问或结构错误 |
| 503 | Readiness 为 false、状态损坏，或 operation store 全部被 pending 记录占满 |
| 504 | 10 秒 Live 快照总时限到期 |

错误响应统一使用以下结构：

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

服务不会在错误响应中回显 Authorization header 或 action body。

## 运行自动化检查

无需网络即可运行网关测试：

```sh
python3 -m unittest discover -s tools/workbuddy_gateway/tests -v
```

从项目根目录运行仓库检查：

```sh
./tools/validate.sh --static
```

这些检查不能验证真实 WorkBuddy 应用、OAuth 审核、转写账号、HTTPS 部署、Wi-Fi 网络或 AI Passport 硬件。
