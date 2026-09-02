<p align="right">
  <strong>简体中文</strong> · <a href="2026-09-03-workbuddy-ai-passport-v1.md">English</a>
</p>

# WorkBuddy AI Passport V1 实现计划

> **供执行 Agent 使用：** 实现时应按任务拆分、逐项验证，并使用计划执行或子 Agent 开发工作流。复选框记录原始执行计划，不代表当前验证结果；实际能力边界以根 README 和最终验证报告为准。

**目标：** 交付可安装的 AI Passport 固件与经过测试的网关，支持保护隐私的 WorkBuddy 收件箱、任务与产出物状态、语音转写确认，以及确认后的消息回复或任务创建。

**架构：** ESP-IDF 固件负责有界的 UI、输入、音频与网络状态，只调用小型版本化网关。Python 标准库网关负责腾讯 WorkBuddy OAuth、官方 API 数据归一化、转写服务凭据和幂等写入。确定性的 demo adapter 让干净构建和测试不依赖客户凭据。

**技术栈：** ESP-IDF 5.5.3、C11、FreeRTOS、LVGL 9、cJSON、`esp_http_client`、ESP32 Wi-Fi、Python 3.11 标准库与 `unittest`。

**V1 固定限制：** 生产可用的任务追问需要 ACP v1 的 SSE 与 JSON-RPC、可持久化的异步阶段模型、权限请求处理，以及跨断线与进程重启的幂等保证，本版本明确不实现。Live 固件会在录音前显示后续版本提示；直接调用网关 `task_followup` 返回 HTTP 501 `not_supported`。本版本也不实现 `permission_response`、ACP 流式事件或产物下载。

## 文件清单

固件生产文件：

- `main/main.c`：硬件初始化与 WorkBuddy 直接启动
- `main/Kconfig.projbuild`：不含已提交凭据的 demo/live 构建设置
- `main/workbuddy_types.h`：跨模块有界类型与上限
- `main/workbuddy_model.[ch]`：页面、焦点、录音、确认、通知与 operation 状态的纯模型
- `main/workbuddy_protocol.[ch]`：有界快照解析与动作序列化
- `main/workbuddy_app.[ch]`：队列所有权与 worker 编排
- `main/workbuddy_wifi.[ch]`：STA 生命周期与重连状态
- `main/workbuddy_transport.[ch]`：网关轮询、转写上传、动作提交与 receipt 查询
- `main/workbuddy_audio.[ch]`：单一阻塞音频 owner 与有界 PCM 管线
- `main/workbuddy_ui.[ch]`：LVGL 隐私封面、列表、详情、语音确认、结果、菜单与状态页
- `main/fonts/lv_font_noto_sans_sc_14.[ch]`：可再分发的 14 px、2 bpp 简体中文字库
- `main/CMakeLists.txt`、`sdkconfig.defaults`：组件注册与固件功能设置

主机测试：

- `tests/test_workbuddy_model.c`：状态转换与控制
- `tests/test_workbuddy_protocol.c`：合法、非法及有界 JSON 与动作 body
- `tests/run_host_tests.py`：编译并执行全部 C 主机测试

网关：

- `tools/workbuddy_gateway/workbuddy_gateway/{config,errors,models,store,demo_adapter,workbuddy_client,transcription,service,server}.py`
- `tools/workbuddy_gateway/tests/`：配置、契约、鉴权、归一化、幂等、转写与 Live client 请求测试
- `tools/workbuddy_gateway/README.md`、`README.zh_CN.md`、`.env.example` 与 `openapi.yaml`

文档：

- `README.md` / `README.zh_CN.md`
- `docs/CHANGELOG.md` / `docs/CHANGELOG.zh_CN.md`
- 本设计与计划的中英文配对

仓库外层的 `PROJECT.md`、`MEMORY.md`、`TASKS.md` 与 `WORKLOG.md` 由长期项目工作区维护，不进入本固件仓库。

### 任务 1：用主机测试锁定固件领域契约

**文件：**

- 新建：`main/workbuddy_types.h`
- 新建：`main/workbuddy_model.h`
- 新建：`main/workbuddy_model.c`
- 新建：`main/workbuddy_protocol.h`
- 新建：`main/workbuddy_protocol.c`
- 新建：`tests/test_workbuddy_model.c`
- 新建：`tests/test_workbuddy_protocol.c`
- 修改：`tests/run_host_tests.py`

- [ ] **步骤 1：先写失败的模型测试**

  覆盖隐私封面入口、`OK` 进入收件箱、列表焦点边界、全局导航、上下文语音、5 秒超时、重录、取消、一次性提交锁、过期快照、游标去重和 WorkBuddy 状态归一化。实现在缺失时先编译，确认测试会因缺少符号失败。

- [ ] **步骤 2：实现有界类型与模型**

  使用固定数组与明确上限：

  ```c
  #define WB_MAX_ITEMS 6
  #define WB_ID_CAP 65
  #define WB_TITLE_CAP 129
  #define WB_PREVIEW_CAP 385
  #define WB_TRANSCRIPT_CAP 513
  ```

  这些文件不能包含 ESP-IDF 或 LVGL 头文件。所有复制都必须保留 NUL 结尾并在 UTF-8 边界停止。

- [ ] **步骤 3：验证模型测试通过**

  运行 `python3 tests/run_host_tests.py --test model`。测试应在 `-std=c11 -Wall -Wextra -Werror` 下零警告通过。

- [ ] **步骤 4：先写失败的协议测试**

  覆盖完整快照、缺少版本、类型错误、数组或字符串超限、非法 UTF-8、未知任务状态、重复游标、reply 序列化、task-create 序列化、task-followup 契约、转义和固定 operation ID。

- [ ] **步骤 5：实现协议解析与序列化**

  在固件中封装 cJSON，并为主机测试提供最小兼容层。按公开设备契约拒绝、截断或归一化输入，禁止使用不检查容量的字符串复制。

- [ ] **步骤 6：验证全部固件主机测试**

  运行 `python3 tests/run_host_tests.py`，要求零编译警告和零失败用例。

### 任务 2：测试先行实现网关契约

**文件：**

- 新建：`tools/workbuddy_gateway/workbuddy_gateway/__init__.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/config.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/errors.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/models.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/store.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/demo_adapter.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/workbuddy_client.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/transcription.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/service.py`
- 新建：`tools/workbuddy_gateway/workbuddy_gateway/server.py`
- 新建：`tools/workbuddy_gateway/tests/`

- [ ] **步骤 1：先写失败的配置与模型测试**

  断言 demo 模式无需外部凭据即可启动。断言 Live 模式会拒绝缺少设备 token、WorkBuddy base URL、WorkBuddy access token 或完整刷新凭据、转写 URL 与转写 key 的配置。验证所有设备 DTO 的集合与 UTF-8 字节上限。

- [ ] **步骤 2：实现类型化配置与错误**

  在 `GatewayConfig.from_env()` 中集中读取环境变量。使用 `GatewayError(code, status, public_message, retryable)` 和单一 JSON 错误结构。所有日志与 `repr` 必须隐藏 token。

- [ ] **步骤 3：先写失败的 adapter 与 service 测试**

  使用官方消息、任务和产物 fixture 规定归一化快照、状态映射、摘要白名单、消息游标、reply、task-create、明确失败的 task-followup、重复 operation receipt 与状态文件原子恢复。

- [ ] **步骤 4：实现 WorkBuddy client 与 service**

  `WorkBuddyClient` 是唯一了解官方接口路径的类：

  ```text
  GET      /openapi/v2/localassistant
  GET/POST /openapi/v2/localassistant/message
  GET/POST /openapi/v2/tasks
  GET      /openapi/v2/tasks/{task_id}
  POST     /openapi/v2/token
  GET      {verified_base}/api/session/artifacts
  ```

  只有任务 `link` 路径精确以 `/acp` 结尾时，才能推导产物 endpoint。WorkBuddy `client_secret`、refresh token、access token、ACP token、sandbox link 与产物 URI 不得进入设备响应。`permission_response` 和 ACP 会话请求不在 V1 内；不能加入只有内存状态的部分实现。必须等可持久化异步阶段、权限处理与跨断线幂等一起设计完成后，才可开放生产追问。

- [ ] **步骤 5：先写失败的转写测试**

  覆盖精确 PCM/WAV 大小、1 到 5 秒边界、不支持的媒体类型、服务超时、非法 JSON、Unicode 结果上限与 demo 转写。

- [ ] **步骤 6：实现转写 adapter**

  把 16 kHz、16-bit、单声道 PCM 转为最多 5 秒的内存 WAV，再向配置的转写 endpoint 发送 multipart 请求。实际默认模型为 `gpt-4o-mini-transcribe`。服务 key 只存在于网关环境变量。

- [ ] **步骤 7：先写失败的 HTTP 契约测试**

  在临时端口启动真实 threaded server。测试 `/healthz`、`/readyz`、bearer 鉴权、请求体上限、content type、request ID、snapshot、transcription、actions、receipt、404、405、非法 JSON 和正常关闭。

- [ ] **步骤 8：实现并验证网关**

  运行 `python3 -m unittest discover -s tools/workbuddy_gateway/tests -v`，要求全部测试无需第三方包或网络即可通过。

### 任务 3：集成固件 UI 与按键

**文件：**

- 新建：`main/workbuddy_ui.h`
- 新建：`main/workbuddy_ui.c`
- 新建：`main/workbuddy_app.h`
- 新建：`main/workbuddy_app.c`
- 修改：`main/main.c`
- 修改：`main/CMakeLists.txt`
- 修改：`sdkconfig.defaults`
- 新建：`main/Kconfig.projbuild`
- 测试：`tests/test_workbuddy_model.c`

- [ ] **步骤 1：为每条实体按键路径扩展失败测试**

  枚举隐私封面、浏览、详情、菜单、录音、转写、确认、发送、结果与错误状态下的 `UP`、`DOWN`、`OK` 点击或长按。断言不支持的输入无作用，任何动作都不会发送两次。

- [ ] **步骤 2：实现直接启动的应用编排**

  先初始化 I2C、显示与 LVGL；按键、音频、电量、存储与网络分别降级。按键回调只把 `wb_event_t` 放入有界队列。UI timer 在 LVGL 锁内消费状态快照。

- [ ] **步骤 3：实现可穿戴 UI**

  使用固定 240×320 布局：24 px 状态栏、248 px 内容区和 48 px 操作区。远端文字使用 Noto Sans SC 14 px、2 bpp 压缩子集，WorkBuddy 标题与计数使用 Montserrat 20。显示隐私封面、列表、详情、导航、录音、转写确认、发送中、成功、失败、过期标记与明确的 Demo 标记。

  Demo 任务详情可以进入追问交互以覆盖状态机；Live 任务详情按 `OK` 时必须在录音前显示“云任务追问将在后续版本开放”，footer 显示“云任务追问预留”，且不提交动作。不要把这个入口写成 V1 已支持能力。

- [ ] **步骤 4：重新运行主机测试与静态检查**

  运行 `python3 tests/run_host_tests.py` 和 `./tools/validate.sh --static`。继续前修复警告、双语文档、链接与 secret scan 错误。

### 任务 4：增加有界 Wi-Fi、传输与音频 worker

**文件：**

- 新建：`main/workbuddy_wifi.h`
- 新建：`main/workbuddy_wifi.c`
- 新建：`main/workbuddy_transport.h`
- 新建：`main/workbuddy_transport.c`
- 新建：`main/workbuddy_audio.h`
- 新建：`main/workbuddy_audio.c`
- 修改：`main/workbuddy_app.c`
- 修改：`main/CMakeLists.txt`
- 修改：`main/Kconfig.projbuild`
- 修改：`sdkconfig.defaults`
- 测试：`tests/test_workbuddy_model.c`
- 测试：`tests/test_workbuddy_protocol.c`

- [ ] **步骤 1：增加重试、超时与取消测试**

  覆盖 Wi-Fi 离线与过期状态、指数退避上限、轮询合并、音频背压、提前结束后的静音补齐、网络中断、不确定动作超时、receipt 查询与明确取消。

- [ ] **步骤 2：实现 Wi-Fi 生命周期**

  创建一个 STA netif 与一组事件 handler，凭据来自未跟踪的本地 `sdkconfig`，连接失败按最高 30 秒退避重试。Demo 模式不启动无线电，也不记录 SSID 密码。

- [ ] **步骤 3：实现 HTTP 传输**

  配置连接与请求超时，以及 12 KiB 快照响应上限。Live 模式默认只接受 `https://` 网关；只有显式开发设置才能使用 `http://`。每个请求携带版本、设备 bearer token、content type、operation ID 与请求关联信息。日志只输出状态、错误码与长度。

- [ ] **步骤 4：实现音频管线**

  单一音频 task 每次读取 2 KiB PCM 并直接写入固定长度上传。5 秒结束或用户提前停止后，不再读取麦克风；提前结束的剩余部分写入静音。取消时中止请求，错误时显示可重试失败。

- [ ] **步骤 5：集成轮询、动作与 demo worker**

  活跃时默认每 30 秒轮询，选择同步可立即轮询。合并重复快照刷新，不覆盖 mutation。所有 UI 更新都通过模型状态。Demo 模式使用同一模型，但采用确定性本地快照和转写。

- [ ] **步骤 6：验证主机与固件构建**

  依次运行主机测试、`./tools/validate.sh --static`，再激活 ESP-IDF 5.5.3 并运行 `./tools/validate.sh --firmware`。记录镜像大小和最大的静态资源。

### 任务 5：记录配置、API 与产品事实

**文件：**

- 新建：`tools/workbuddy_gateway/.env.example`
- 新建：`tools/workbuddy_gateway/openapi.yaml`
- 新建：`tools/workbuddy_gateway/README.md`
- 新建：`tools/workbuddy_gateway/README.zh_CN.md`
- 新建：`README.md`
- 新建：`README.zh_CN.md`
- 修改：`docs/CHANGELOG.md`
- 修改：`docs/CHANGELOG.zh_CN.md`
- 新建：本设计与计划的中文配对

- [ ] **步骤 1：记录准确的 demo 与 Live 命令**

  包含网关启动、临时端口 smoke test、固件 menuconfig 字段、构建命令、合并镜像路径、Recovery 入口与安全刷写警告。跟踪文件不得包含真实 token、SSID、回调 secret 或设备身份。

- [ ] **步骤 2：记录有来源支持的限制**

  说明运行时无需手机但依赖 Wi-Fi 与网关。本地助理必须在 PC 上在线。首次授权在外部完成。当前没有 IMU、蜂窝网络、本地语音识别或 Office parser。Live 模式还依赖 WorkBuddy 应用审核、OAuth、转写服务和可访问的网关。

- [ ] **步骤 3：检查文档与 OpenAPI 一致性**

  逐项核对中英文、链接、JSON 示例、endpoint、上限、状态码与环境变量是否和生产代码一致。运行 `git diff --check`、仓库静态检查与 secret scan。

### 任务 6：完成验证与交接

**文件：**

- 只修改本任务发现问题所需的文件，不扩大范围。

- [ ] **步骤 1：运行网关测试与 smoke test**

  运行 `python3 -m unittest discover -s tools/workbuddy_gateway/tests -v`。在临时端口启动 demo 网关，以真实 HTTP 请求验证 health、带鉴权 snapshot、transcription、action、重复 action、task-followup 501 与 receipt 查询。

- [ ] **步骤 2：运行完整仓库 gate**

  激活准确的 ESP-IDF 5.5.3 并运行 `./tools/validate.sh`。确认合并镜像验证器保留 `cardid`、Recovery、boot hook、分区 MD5 与 3 MB 应用上限。

- [ ] **步骤 3：复核需求与仓库 diff**

  对照设计逐项检查代码、测试与文档，审阅 `git status`、`git diff`、依赖变化和产物路径。不删除用户文件，不提交凭据。

- [ ] **步骤 4：如实报告证据**

  分别报告 `Environment`、`Build`、`Host tests`、`Device tests` 与 `Unverified`，附安装镜像路径和校验值。屏幕、按键、麦克风、Wi-Fi、功耗、Recovery 与真实 WorkBuddy 集成在真机执行前都必须标为 `NOT RUN`。
