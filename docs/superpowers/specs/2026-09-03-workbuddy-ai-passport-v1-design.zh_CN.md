<p align="right">
  <strong>简体中文</strong> · <a href="2026-09-03-workbuddy-ai-passport-v1-design.md">English</a>
</p>

# WorkBuddy AI Passport V1 设计

## 目标

构建第四个相互隔离的 AI Passport 固件应用，把随身工卡变成小型 WorkBuddy 助手。第一版应直接进入保护隐私的收件箱，让用户浏览 WorkBuddy 消息与任务、录制简短语音、确认转写文字，并在没有手机运行时桥接的情况下回复消息或创建任务。

目标服务是腾讯 WorkBuddy。官方 [WorkBuddy Open API 文档](https://open.workbuddy.cn/docs/openapi)定义本地助理消息、云端任务、Agent Client Protocol（ACP）与会话产物接口。本设计按 2026-09-03 的 `/openapi/v2` 文档实现。

“无需手机”不等于“无需电脑”。本地助理消息依赖用户电脑上的 WorkBuddy 本地助理在线。AI Passport 还需要通过 Wi-Fi 访问运行在电脑或托管环境中的网关。

## V1 实现边界

V1 交付保留了任务追问的固件状态与设备动作类型。Demo 模式可以演示这段交互；Live 固件则在录音开始前提示“云任务追问将在后续版本开放”，不会提交动作。直接调用 Live 或 demo 网关的 `task_followup` 仍明确返回 HTTP 501 `not_supported`。生产可用的任务追问需要 ACP v1 的 Server-Sent Events（SSE）接收通道、JSON-RPC 发送通道、可持久化的异步阶段模型、权限请求处理，以及跨断线与进程重启的幂等保证，本版本未实现这些能力。演示模式中的模拟成功不代表 Live 任务追问可用。

V1 也未实现本地助理 `permission_response`、ACP 流式事件、产物下载或 Office 文件渲染。消息“回复”使用官方普通 `text` 消息接口；选中的消息 ID 只绑定本地上下文和幂等操作，因为上游请求没有 `reply_to` 字段。

## 产品范围

V1 由隐私封面和四个视图组成，全部读取同一份归一化快照：

0. **隐私封面**：待机画面只显示 WorkBuddy 连接状态、未读数量和任务数量。按下 `OK` 前不显示消息文字。
1. **收件箱**：显示最近的本地助理消息、未读标记、发送角色和有长度上限的预览。选中消息后可以录制文字回复。
2. **任务**：显示最近的云端任务，并把状态归一为 `QUEUED`、`RUNNING`、`NEEDS_INPUT`、`COMPLETED` 或 `FAILED`；上游 `pending` 表示等待输入，映射为 `NEEDS_INPUT`，archived 与 deleted 项会被过滤而不是伪装成失败。用户可以打开任务详情；Live 模式按 `OK` 只显示后续版本提示，不进入追问录音。
3. **产出物**：显示最近计划、清单、概览、图片或文档产物的有限标题与摘要。工卡不解析 Office 文件，也不下载媒体。
4. **新任务**：录制简短语音指令，显示转写文字，确认后调用 WorkBuddy 任务创建接口。

固件默认每 30 秒轮询一次快照。消息游标变化时显示简短视觉通知；轮询失败时把缓存明确标为过期。Wi-Fi、网关或 PC 本地助理离线时，不承诺即时送达。

V1 不实现抬手唤醒，因为当前硬件没有已确认的惯性测量单元（IMU）。本版本也不包含蜂窝网络、本地语音识别、任意文本编辑、Office 渲染或生产级常开功耗目标。

## 交互设计

交互遵循“查看、选择、说话、确认”的可穿戴流程，不使用隐藏的多键组合。

### 浏览状态

- `UP CLICK` / `DOWN CLICK`：移动当前列表焦点
- `OK CLICK`：打开焦点项；收件箱会直接进入语音回复，任务和产出物会打开详情
- `OK LONG`：打开全局导航；在隐私封面上会直接创建新任务
- 固定页脚显示当前状态下有效的操作

### 导航菜单

- `UP CLICK` / `DOWN CLICK`：选择收件箱、任务、新任务、产出物、同步或状态
- `OK CLICK`：执行所选操作
- `OK LONG`：关闭菜单

### 语音状态

- 语音模式只能由明确的按键操作进入；Live 模式先显示“准备语音”，在有界 HTTP 上传通道建立完成前不启动录音时钟
- 音频 worker 返回与当前 operation ID 匹配的就绪确认后，UI 先处理准备阶段已排队的输入，再渲染红色录音状态、上下文或目标和已录时间，并等待排队的 SPI 像素传输完成，然后发送带 operation ID 的开始控制；音频 worker 在首次读取前重启 RX DMA，丢弃准备阶段的预录数据
- `OK CLICK`：提前结束；录音也会在 5 秒后自动结束。HTTP 上传采用固定长度时，剩余样本以静音补齐，不继续读取麦克风
- `OK LONG`：取消，不提交音频或文字
- 音频格式为 16 kHz、16-bit、有符号单声道 PCM；音频 worker 以固定 2 KiB 分块采集并上传
- 音频就绪、转写和动作完成事件可靠投递，并按 operation ID 匹配；开始、结束与取消控制也绑定 operation ID，延迟控制不能改变下一次录音；队列满时只允许丢弃按键或可合并的轮询提示

### 确认状态

- 网关返回最多 512 UTF-8 字节的转写文字
- WorkBuddy 写操作发生前，屏幕先显示上下文与转写文字
- `OK CLICK`：使用稳定的 operation ID 提交一次
- `DOWN CLICK`：重新录制
- `OK LONG`：取消并返回上一视图
- 超时不当作成功；固件会按 operation ID 查询结果，避免不确定超时造成重复写入

### 结果与失败状态

- 成功时显示 receipt ID，然后返回对应列表
- Wi-Fi、网关、转写和 WorkBuddy 失败使用有界错误状态；日志不输出凭据、消息正文或 token
- 轮询失败时保留缓存摘要，但明确标注为过期
- Live 任务详情会在录音前拦截追问并显示后续版本提示；只有直接调用网关设备 API 才会得到不可重试的 `not_supported`

## 架构

```text
AI Passport 固件
  UI owner + 纯模型 + 有界 worker
      |  版本化 REST + device bearer token
      v
本地或托管的 WorkBuddy 网关
  鉴权 + 归一化 + 幂等 + 语音转写
      |  OAuth bearer + 官方数据结构
      v
腾讯 WorkBuddy Open API / 可选转写服务
```

### 固件边界

- `main/workbuddy_model.*`：与硬件无关的页面、焦点、录音、确认、通知、operation ID、游标、任务状态与状态转换
- `main/workbuddy_protocol.*`：有明确容量上限的设备 JSON 解析与序列化
- `main/workbuddy_app.*`：队列、轮询调度、worker 所有权与不可变 UI 快照
- `main/workbuddy_wifi.*`：STA 生命周期与最多 30 秒的重连退避
- `main/workbuddy_transport.*`：有界 HTTP 轮询、转写上传、动作提交与 receipt 查询
- `main/workbuddy_audio.*`：阻塞式 BSP 音频调用的唯一 owner，以及固定长度 PCM 管线
- `main/workbuddy_ui.*`：LVGL 对象的唯一 owner；消费快照并发送轻量命令
- `main/main.c`：BSP 初始化、依赖降级与应用直接启动

显示或 LVGL 失败会终止应用启动。音频、电量、Wi-Fi 与远端服务失败分别降级。网络、音频和按键回调不会直接操作 LVGL。现有 BSP、分区表、Recovery boot hook 与固件验证脚本保持不变。

### 网关边界

网关是位于 `tools/workbuddy_gateway/` 的 Python 3 标准库服务：

- `config.py`：启动时验证环境变量，并隐藏敏感值
- `workbuddy_client.py`：唯一调用 WorkBuddy `/openapi/v2` 的模块，处理 token 刷新、消息、任务与产物摘要
- `transcription.py`：把有界 PCM 转为 WAV，并调用可配置的 OpenAI 兼容转写接口
- `service.py`：实现归一化快照、转写草稿、动作、receipt 与幂等规则
- `server.py`：实现 HTTP 解析、设备 bearer 鉴权、请求 ID、响应结构与关闭流程
- `demo_adapter.py`：提供不依赖客户凭据的确定性数据

V1 不需要数据库。小型本地状态文件保存 operation receipt、错误码和请求指纹，不保存动作正文。刷新凭据模式会在采用轮换 refresh token 前，把它按 client ID 的 SHA-256 作用域写入两个有效的 `0600` 状态槽；access token 与 `client_secret` 不持久化。有效备份可恢复损坏主文件；已有状态材料但两个槽都无法校验时，readiness 保持 false，mutation 以 `state_corrupt` 关闭失败。生产部署必须把状态文件当作 secret，并在内置 HTTP 服务前终止 TLS；明文 HTTP 只允许显式启用的隔离开发局域网。

## 设备 API 契约

所有 JSON 响应都包含 `version: 1` 和 `request_id`。受保护接口使用 `Authorization: Bearer <device token>`。这个 token 只鉴权工卡到网关，不是 WorkBuddy OAuth token。

### `GET /v1/snapshot`

可选查询参数为 `after`。响应包含最长 64 字节的稳定快照指纹、新鲜度、本地助理在线状态、未读数、活跃任务数，以及最多六条消息、六个任务和六个产出物摘要。Live adapter 另行维护 WorkBuddy `message_id` 游标，不会把设备快照指纹误传为上游消息 ID。网关按 UTF-8 边界截断；如果 JSON 转义后仍超过 12 KiB，会继续缩短摘要与标题，必要时从列表尾部移除项目。固件再次执行自己的容量检查。

### `POST /v1/transcriptions`

查询参数必须包含 `operation_id`。请求类型必须为 `audio/L16;rate=16000;channels=1`，长度为 1 到 5 秒。响应只返回转写草稿，不写入 WorkBuddy。

### `POST /v1/actions`

请求必须带 `version: 1`、`operation_id` 与以下一种类型：

- `reply`：已确认文字与上下文 message ID
- `task_create`：已确认的任务 prompt
- `task_followup`：设备契约可解析，但 Live 和 demo 网关均返回 HTTP 501 `not_supported`；当前 Live 固件不会提交此动作

相同 operation ID 与相同正文会返回已保存 receipt，不重复调用上游。相同 operation ID 配合不同正文会返回 HTTP 409。能够证明 mutation 尚未提交的可重试前置失败会释放预留，允许原 ID 再次提交；请求可能已经到达 WorkBuddy 但结果不明时，operation 保持 `PENDING`，重复提交只返回该状态而不会再次写入。

### `GET /v1/operations/{operation_id}`

返回 `PENDING`、`SUCCEEDED` 或 `FAILED`，以及有界 receipt 或错误。固件在不确定超时后用它查询结果。

### `GET /healthz` 与 `GET /readyz`

Liveness 不依赖上游。Readiness 表示配置和最近一次 WorkBuddy 访问是否有效，但不泄露敏感信息。

## WorkBuddy 映射

- 本地助理在线状态：`GET /openapi/v2/localassistant`
- 收件箱：`GET /openapi/v2/localassistant/message`，使用网关内部 `message_id` 增量轮询并合并最近六条；兼容官方 string-array history 与 text block
- 文字回复：`POST /openapi/v2/localassistant/message`，只发送 `content` 与 `msg_type: text`
- 任务列表与创建：`GET/POST /openapi/v2/tasks`
- 任务详情与 ACP ticket：`GET /openapi/v2/tasks/{task_id}`
- 产出物：只有任务详情 `link` 的路径精确以 `/acp` 结尾时，网关才推导同源 `{base}/api/session/artifacts?sessionId=...&limit=6` 并使用任务 `token` 鉴权；QUEUED、RUNNING、NEEDS_INPUT 与 COMPLETED 任务按调用预算轮转刷新并缓存 90 秒，deleted 事件被过滤，含糊的 link 会关闭失败
- OAuth 刷新：表单方式 `POST /openapi/v2/token`，携带 `grant_type=refresh_token`、`refresh_token`、`client_id` 与 `client_secret`；轮换 token 会先安全持久化，重启后优先使用

网关只向工卡返回经过白名单筛选的文字摘要。WorkBuddy `client_secret`、refresh token、access token、ACP token、link、URI、下载 URL 与 sandbox 信息都不会下发到工卡。

## 配置与演示模式

固件构建配置不提交凭据。默认 `CONFIG_WB_DEMO_MODE=y`，不启动 Wi-Fi，使用确定性的消息、任务、产出物与转写。关闭演示模式后，只在已忽略的本地 `sdkconfig` 中填写 Wi-Fi SSID、密码、网关 URL、设备 token 与 SNTP 服务器。HTTPS 流量必须等本次启动完成校时后才能发出；Mbed TLS 随后同时检查证书有效期、主机名和信任链。

网关默认也是 demo 模式，不需要外部凭据。Live 网关需要已审核并启用的 WorkBuddy 硬件接入应用、用户 OAuth 授权、设备 token 与转写服务。`client_secret` 只存在于网关环境变量，不得编译进固件。

## 资源与安全上限

- ESP32-C3、8 MB Flash、无 PSRAM；应用镜像不得超过 `0x300000` 字节
- 保留上游单 LVGL draw buffer，不引入未经本项目验证的双缓冲方案
- 快照正文不超过 12 KiB；Live 快照总预算从锁等待前开始，覆盖 DNS 与网络读取，总计 10 秒和七次物理 HTTP 尝试（含 OAuth 刷新与 401 后重试）；转写与动作文字不超过 512 UTF-8 字节；标题不超过 128 字节；预览不超过 384 字节；每类最多六项
- 入站 header 不超过 16 KiB；连接读取默认 5 秒超时；默认最多并发处理八个连接
- 录音最多 5 秒；分块固定为 2 KiB；只有一个音频 owner
- 固件 NVS 不保存消息正文、任务 prompt、录音、OAuth token、ACP token 或产物 payload
- 所有远端内容都按不可信显示数据处理，必须经过物理确认后才能产生写操作
- `cardid@0x356000`、`recovery@0x700000`、3 MB 应用上限和开机持续按 `UP` 5 秒的 Recovery 路径保持不变

## 验证

主机测试覆盖模型状态转换、导航、状态映射、游标去重、有界协议解析、operation 幂等、网关鉴权、上游归一化、OAuth 刷新与轮换 token 恢复、状态损坏关闭失败、产物轮转与端点限制、转写上限、HTTP 超时/并发/容量限制及 demo 请求。

仓库验证入口为：

```sh
./tools/validate.sh --static
./tools/validate.sh --firmware
./tools/validate.sh
```

固件门会同时编译并验证 Demo 与只含占位配置的 Live profile，并断言生成配置中的模式、蓝牙关闭、HTTPS 策略、根证书包和证书日期校验设置。真机验收必须独立完成，包括屏幕与中文字库可读性、全部按键路径、Wi-Fi 重连、SNTP 与 TLS 证书日期校验、5 秒录音、中文转写、通知去重、持续轮询、堆栈余量、Recovery 入口和合并镜像安装。本次交付不宣称这些真机项目已经通过。

## V1 交付

交付内容包括 `feature/workbuddy-ai-passport` 上的源码、网关说明、主机测试和构建后生成的 `build/FoloToy-AI-Passport-full.bin`。构建与主机测试结果不能报告为真机结果。Live WorkBuddy OAuth、真实 API、转写服务与公网部署也必须在获得审批和凭据后另行验证。
