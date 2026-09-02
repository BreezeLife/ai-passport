<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport 上的 WorkBuddy

AI Passport 上的 WorkBuddy 把 FoloToy 工卡变成一个保护隐私的腾讯 WorkBuddy 随身收件箱。你可以抬手查看消息与任务状态、阅读产出物摘要、录制一段简短指令、确认转写文字，再通过网关发送。固件默认运行无需凭据的演示模式。Live 模式仍是集成预览，尚未完成真机验收。

这是一个独立原型，不是腾讯 WorkBuddy 或 FoloToy 的官方版本。

## 理解“无需手机”的边界

工卡运行时不以手机作为桥接设备，而是通过 Wi-Fi 连接你维护的网关。但完整流程仍可能依赖电脑：

- WorkBuddy 本地助理的消息读取与回复要求用户电脑上的 WorkBuddy 本地助理在线
- 网关必须能被工卡访问，可以运行在电脑上，也可以部署为托管服务
- 云端任务由 WorkBuddy 执行，但工卡仍需通过网关完成鉴权与数据归一化
- 首次 WorkBuddy 授权、网关配置、固件安装与恢复都在工卡之外完成

[WorkBuddy 硬件接入指南](https://open.workbuddy.cn/docs/third-party-app)说明了应用审核、OAuth 2.1 授权和 PC 端本地助理依赖。[WorkBuddy Open API 文档](https://open.workbuddy.cn/docs/openapi)定义了当前 `/openapi/v2` 接口。

## 确认 V1 能力边界

V1 把远端内容放在隐私封面之后，并把每类列表限制为六条摘要。

| 能力 | V1 行为 | 边界 |
| --- | --- | --- |
| 隐私封面 | 只显示连接状态、未读数量和活跃任务数量 | 物理确认后才显示消息正文 |
| 收件箱 | 读取本地助理消息，并发送确认后的 `text` 消息 | 选中的消息只提供本地上下文；上游接口会新建消息，没有指定回复目标的字段 |
| 任务 | 显示归一化状态、打开详情，并用确认后的语音文字创建任务 | Live 模式需要 `user.task.readable` 与 `user.task.invokable` |
| 任务追问 | Demo 模式可以演示交互状态；Live 任务详情会在录音开始前提示此能力留待后续版本 | **V1 明确 NOT_SUPPORTED**：直接调用网关 `task_followup` 会返回 HTTP 501 `not_supported`，因为尚未实现 Agent Client Protocol（ACP）v1 的流式连接与 JSON-RPC |
| 产出物 | 显示从可见任务中发现的计划、清单、概览、图片和文档摘要 | 不下载文件、不预览图片、不渲染 Office 文件，也不把任意产物 URL 下发到工卡 |
| 语音 | 最多采集 5 秒 16 kHz、16-bit 单声道 PCM，并在提交前显示转写结果 | 没有本地自动语音识别；Live 模式必须配置转写服务 |
| 刷新 | 默认每 30 秒轮询网关；失败时把缓存标为过期 | 不是即时推送，网络中断会延迟状态更新 |

演示模式会模拟完整交互和结果页面。模拟成功不能证明真实 WorkBuddy、转写、Wi-Fi 或 ACP 链路可用。

## 运行默认演示

仓库默认启用 `CONFIG_WB_DEMO_MODE=y`。固件不启动 Wi-Fi，也不需要网关或转写凭据，而是使用确定性的消息、任务、产出物和转写文案。

激活 ESP-IDF 5.5.3 后，运行仓库验证入口：

```sh
./tools/validate.sh --static
./tools/validate.sh --firmware
```

固件验证会生成 `build/FoloToy-AI-Passport-full.bin`。按照[用 Codex 创造 AI Passport 玩法](https://ai-passport.folotoy.cn/guides/create-a-play-with-codex/)把本地构建安装到开发设备。构建通过不能替代真机测试。

必须保留原有安装与恢复契约：

- 应用分区继续位于 `0x10000`，上限为 `0x300000` 字节
- 设备身份继续保存在 `0x356000` 的 `cardid`
- 永久 Recovery 继续位于 `0x700000`
- 开机时持续按住 `UP` 5 秒仍可进入 Recovery
- 不要在已配网设备上运行 `idf.py erase-flash`

安装前请阅读[构建指南](docs/development/engineering/build-and-test.zh_CN.md)与[Recovery 兼容契约](docs/development/engineering/ble-recovery-compatibility.zh_CN.md)。

## 配置 Live 模式

Live 模式依赖审核与外部服务。不要把凭据写入本仓库。

1. 在 WorkBuddy 开放平台注册硬件接入应用，并等待审核通过。
2. 只申请本版本使用的权限：`user.localassistant.readable`、`user.localassistant.invokable`、`user.task.readable` 与 `user.task.invokable`。
3. 在网关之外完成 OAuth 2.1 授权码流程，再向网关提供 access token 或完整刷新凭据。
4. 配置兼容的语音转文字接口。
5. 在工卡可访问的主机上运行纯 Python 3 标准库网关。

网关接受以下 Live 配置：

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

需要刷新时，用 `WORKBUDDY_REFRESH_TOKEN`、`WORKBUDDY_CLIENT_ID` 和 `WORKBUDDY_CLIENT_SECRET` 三项完整配置替代 `WORKBUDDY_ACCESS_TOKEN`。网关向 `/openapi/v2/token` 发起刷新请求。刷新模式会把轮换后的 refresh token 写入仅文件所有者可访问的网关状态文件，并在重启后优先使用这份与 client 绑定的 token。生产环境应把状态文件及其备份放在 checkout 外。

内置网关服务不会终止 TLS。Live 部署应在它前面提供 HTTPS。只有隔离的开发局域网才能使用明文 HTTP，并且必须显式打开固件的开发局域网选项。

运行 `idf.py menuconfig`，打开 **WorkBuddy AI Passport**，关闭演示模式。在本地且已忽略的 `sdkconfig` 中填写 Wi-Fi SSID、Wi-Fi 密码、网关 URL，以及同一份网关设备 token。设备 token 只用于工卡到网关的鉴权，不是 WorkBuddy OAuth token。

WorkBuddy `client_secret`、refresh token、access token、ACP ticket、sandbox link 和转写密钥只能保存在网关侧，不能进入固件、日志、示例文件或提交记录。启用刷新凭据后，网关状态文件也应按 secret 管理。

## 了解尚未验证的部分

V1 未实现 ACP 任务追问、`permission_response`、任务流式事件、产物下载、本地语音识别、抬手唤醒、蜂窝网络或 Office 文件渲染。Live 固件会在录音前拦截任务追问，并提示此能力留待后续版本。生产追问会等到可持久化的异步阶段、ACP 权限处理与跨断线/重启幂等方案一起完成后再开放。当前硬件没有已确认的惯性测量单元（IMU）。

本次仓库交付没有使用真实 WorkBuddy 账号验证 OAuth 审核、公网网关或真实转写服务。屏幕可读性、按键、麦克风、扬声器、Wi-Fi 重连、堆与栈余量、续航、安装和 Recovery 入口也都需要在 AI Passport 真机上验收。

## 查找实现

- 固件应用：`main/workbuddy_*` 与 `main/fonts/`
- 设备网关与测试：[网关指南](tools/workbuddy_gateway/README.zh_CN.md)
- V1 设计：[WorkBuddy AI Passport V1 设计](docs/superpowers/specs/2026-09-03-workbuddy-ai-passport-v1-design.zh_CN.md)
- 实现计划：[WorkBuddy AI Passport V1 实现计划](docs/superpowers/plans/2026-09-03-workbuddy-ai-passport-v1.zh_CN.md)

本 fork 保留上游 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) 硬件基线与仓库 [MIT 许可证](LICENSE)。生成的 Noto Sans SC 字库子集使用 `assets/fonts/` 中单独附带的 SIL Open Font License。
