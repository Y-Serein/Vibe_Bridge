# AGENTS.md

本文件是 `/home/slam/Sipeed/pico_tn160` 的项目级规则。
全局行为规则由 `~/.codex/AGENTS.md` 负责；本文件只补充本项目独有的启动、测试、目录、UVC/硬件坑点和交接规则。
如果本文件与全局规则冲突，优先遵守本文件中更具体的项目规则。

---

## 0. 工程控制论式项目执行协议

本文件是项目级规则；全局规则只放在 `/home/rv_nano/.codex/AGENTS.md`。本项目任务默认按闭环推进：目标 -> 状态 -> 误差 -> 控制动作 -> 反馈 -> 修正 -> 验证 -> 沉淀。

- 动手前先说明当前目标、系统状态、目标与现状误差、本轮控制变量和最小闭环验证方式。
- 只调整完成目标所需的最小变量，不把无关优化、重构或格式化混进当前任务。
- 修改后必须用构建结果、UVC 抓帧、主机兼容性反馈、硬件实测或用户反馈验证；反馈不一致时先定位误差来源，再继续改。
- USB 描述符、UVC payload、PIO、DMA、时序和硬件参数必须保持稳定性优先；禁止为了当前任务破坏 Windows/Linux 已跑通基线。
- 长任务或上下文可能丢失时，按项目规则更新 HANDOFF，而不是依赖对话记忆。

辅助工具：

- 开工前优先运行只读 preflight：`python3 /home/rv_nano/Sipeed/T_tools/agent_preflight.py --project pico-tn160`。
- 用户要求接手、先看看、对齐需求、以结果为导向、可反驳、先不要改代码时，优先读取 `/home/rv_nano/Sipeed/.claude/skills/project-intake/SKILL.md`。
- UVC、USB descriptor、Bulk/ISO、PIO/DMA、热成像采集或烧录器任务优先读取 `/home/rv_nano/Sipeed/.claude/skills/pico-tn160-uvc/SKILL.md`。
- 遇到重复问题先查 `/home/rv_nano/Sipeed/C_context/KNOWN_FAILURES.md`。

---

## 1. 项目概览

- 本项目是：基于 RP2350 的 TN160 热成像采集与 USB UVC 输出固件。
- 主要技术栈：Pico SDK、TinyUSB、PIO、DMA、双核处理、UART 协议、UVC YUY2 视频输出。
- 主要运行环境：RP2350 固件；主机侧主要验证 Windows、Linux/WSL V4L2、guvcview。
- 当前核心目标：保持正式热成像固件可构建，并维持 Windows/Linux 可用的 UVC 成功基线。
- 产品/工程优先级：稳定性 > 可验证性 > 小改动 > 跨平台兼容 > 性能 > 重构。

不要把本项目当成：

- 普通桌面/后端项目：它的主要风险在固件、USB 协议、时序、DMA/PIO 和硬件实测。
- 可随便重构的实验项目：很多参数来自硬件验证，不能凭经验改。
- 只看 Windows 的 UVC 项目：Linux/V4L2/guvcview 兼容性也是当前基线的一部分。

---

## 2. 常用命令

如果命令不确定，先查：

- `README.md`
- `HANDOFF.md`
- `build.sh`
- `CMakeLists.txt`
- `tools/pico-sdk-2.2.0/`
- `docs/`

不要凭经验猜命令。

### 安装依赖

本项目当前没有项目内一键安装依赖命令。不要自动联网安装依赖；缺依赖时先说明缺什么、为什么需要。

### 启动开发环境

没有长期运行的开发服务。默认工作流是修改固件源码、构建 UF2、用户上板验证。

### 构建

```bash
bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5
```

构建产物：

```text
/home/slam/build/pico_tn160/pico_tn160.elf.uf2
```

### 运行测试

本项目没有完整自动化测试。固件相关修改优先运行最小构建：

```bash
bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5
```

涉及 UVC 时，硬件侧由用户在 Windows/Linux 上验证。没有上板实测时，必须明确写“未上板验证”。

### 运行单个测试 / 最小相关测试

UVC 当前常用 Linux/WSL 验证使用用户上次那条 `v4l2-ctl` 抓帧命令即可；命令未变化时，不要反复整段发送，直接说“用上次的 v4l2-ctl 命令”。

期望结果：

- `VIDIOC_STREAMON returned 0`
- 连续 `DQBUF`
- `bytesused=38400`
- 5 帧输出文件大小为 `192000` bytes

### Lint / Format / Type Check

当前没有统一 lint/format/typecheck 命令。不要为了文档或小修引入格式化噪声。

------

## 3. 项目结构

- `src/`：固件主线源码，包含采集、处理、UVC、USB 描述符、模式配置。
- `src/main.c`：主入口、双核处理、UVC 发送、校准/FFC 主逻辑。
- `src/init.c`：SPI/PIO/DMA 初始化和原始数据接收链路。
- `src/sdk.h`：GPIO、模式开关、Flash 布局和全局结构定义。
- `src/tusb_config.h`：TinyUSB 配置，UVC payload/buffer 等关键参数在这里。
- `src/usb_descriptors.c` / `src/usb_descriptors.h`：USB/UVC 描述符。
- `tools/pico-sdk-2.2.0/`：本项目实际使用的 Pico SDK/TinyUSB 副本；不要改 `/home/slam/pico-sdk` 这类旧路径。
- `tools/backup/`：历史脚本、抓包、分析 notebook、参考数据。
- `docs/`：开发文档、接口文档、日志和 TODO。
- `build.sh`：当前固定构建入口。
- `HANDOFF.md`：当前 session 交接状态；顶部当前状态优先。
- `C_context/`：本地背景资料和 QA 日志，不上传，已在 `.gitignore` 中忽略。
- `D_deliverables/`：最终交付物。
- `R_raw/`：临时分析、实验结果、草稿。
- `T_tools/`：可复用的小工具或脚本。

规则：

- 代码、测试、配置、资源文件优先遵守项目原有目录结构。
- 文档、交付物、草稿、临时分析、工具脚本才使用 CDTR。
- 不要把临时文件、测试输出、分析草稿平铺到项目根目录。
- `AGENTS.md` 是用户明确要求的项目根配置文件，属于根目录例外。
- 不确定新文件放哪里时，先看已有结构；仍不确定再问用户。

------

## 4. 代码修改规则

- 保持现有 C 代码风格、目录结构和命名习惯。
- 优先复用已有实现，不重新造一套并行逻辑。
- 不做无关重构。
- 不做无关格式化；当前工作区存在一些历史换行/格式噪声，不能顺手纳入。
- 不引入新依赖，除非用户明确要求或现有方案无法解决。
- 不改变 USB 描述符、UVC payload、端点、帧率、UART 协议、Flash 格式、GPIO/ADC 定义，除非当前任务明确要求。
- 不把 bug fix 扩大成架构改造。
- 不为了“高级写法”牺牲稳定性、可读性和调试便利。
- 如果发现无关但重要的问题，只报告，不直接修改。

------

## 5. 验证规则

修改完成后，优先运行最小相关验证。

推荐顺序：

1. 文档或规则文件修改：读回文件并检查 diff。
2. 固件源码修改：运行最小构建命令。
3. UVC/USB/硬件相关修改：构建通过后等待用户上板验证 Windows/Linux/guvcview。
4. GUI/工具脚本修改：运行对应脚本或最小启动检查。
5. 如果无法实测硬件，明确标注“未上板验证”。

最终回复必须说明：

- 已运行什么命令。
- 结果是通过、失败还是未运行。
- 如果未验证，说明原因和建议下一步命令。
- 不要用“应该可以”代替验证结果。

------

## 6. 不要自动做的事

除非用户明确要求，不要自动：

- commit。
- push。
- 升级 Pico SDK、TinyUSB 或其他依赖。
- 修改全局配置。
- 修改系统环境。
- 删除、移动、覆盖大量文件。
- 重排目录结构。
- 生成生产镜像。
- 烧录、擦除、格式化设备。
- 修改 `.env`、密钥、证书、账号配置、私密配置。
- 把调试日志、临时输出、构建产物混进提交。
- 把 unrelated fix 混进当前任务。

------

## 7. 常见坑点

- 当前 UVC 成功基线是 full-speed Bulk UVC，不要回退到历史 ISO payload 猜测路线。
- UVC 基线参数：`160x120`、`YUY2`、`10fps`、Bulk endpoint `wMaxPacketSize=64`、UVC payload / TinyUSB epbuf `256`。
- Windows 可见、Linux/V4L2 可连续抓 10fps、guvcview 可用，三者都属于成功条件。
- guvcview 曾只能选择 `1/1 fps`，原因是 continuous frame interval 的 max 暴露到了 1fps；当前已收窄到 10fps。
- `MAIN_FUNC_MODE` 必须保持正式热成像模式 `MAIN_FUNC_MODE_SPI_INIT_40MHZ_4`，不要把 `MAIN_FUNC_MODE_UVC_MAX` 测试模式留在正式源码里。
- `/dev/video0` 是当前 Linux YUYV capture 节点；历史上 `/dev/video1` 不是这路采集入口。
- `tools/pico-sdk-2.2.0/` 是当前实际 SDK。根目录旧 SDK 副本已清理，不要再引用 `/home/slam/pico-sdk` 或 `/home/slam/pico-sdk-2.2.0`。
- `C_context/` 是本地资料目录，不上传；不要把 `C_context/QA_Log.md` 重新纳入 Git。
- 大文件、大目录、生成物不要全文读取，优先用 `rg`、`find`、`sed -n` 精确定位。

------

## 8. 完成标准

一次任务完成时至少满足：

- 改动只覆盖当前需求。
- diff 小而清晰，便于 review 和回滚。
- 没有无关格式化、无关重构、无关文件变更。
- 已运行最小相关验证，或明确说明未验证原因。
- 最终回复说明：
  - 改了什么。
  - 改了哪些文件。
  - 验证情况。
  - 风险、限制或未完成项。

------

## 9. HANDOFF.md 交接规则

`./HANDOFF.md` 是当前任务的 session 交接文件，用于让用户或下一轮 AI 在 3 天后 30 秒内接上工作。
它不是长期项目文档，不要默认更新。

只有以下情况才创建或更新：

- 用户明确要求“写 HANDOFF / 更新 HANDOFF / 交接 / 保存当前状态”。
- 长任务暂停、结束或准备切换 session。
- 多步排查后产生关键上下文、失败尝试或未解决问题。
- 当前上下文复杂，继续对话可能丢失关键线索。
- 用户明确要求 commit 时，提交前必须先确认是否需要同步当前 session 状态。

写入要求：

- 使用全局规则指定的固定结构；不要自创标题。
- 只写当前真实状态。
- 顶部“当前在做什么”代表当前状态；历史表里的“待测/当前/失败”等只代表当轮历史上下文。
- 失败方案必须写结果和线索。
- 下一步计划必须是 3-5 条可执行 action。
- 关键文件路径必须使用相对路径，一行一个。
- 不写 secrets、tokens、API key、私密配置。
- 目标是让用户或下一轮 AI 30 秒内恢复上下文。

------

## 10. 如果本项目是 UI / 桌面项目

本项目不是 UI / 桌面主项目，但仓库内有辅助 GUI/脚本。

- 修改 `tools/pico_th160_thermocam_gui/` 时，保持现有 GUI 风格和串口协议解析方式。
- GUI 改动不能要求固件协议同时变化，除非任务明确要求。
- 不要把 GUI 体验优化混进固件 bug 修复。
- 工具输出、截图、临时分析文件不要放在项目根目录。

------

## 11. 如果本项目是嵌入式 / 硬件项目

本项目属于嵌入式 / 硬件项目。修改驱动、协议、寄存器、GPIO、I2C、SPI、USB、HID、UVC、显示、音频、电源相关代码前，先定位：

- 初始化代码
- 配置文件
- 调用路径
- 相关文档、历史记录或实测日志

硬件相关要求：

- 不凭经验猜寄存器值、USB 描述符、帧率、端点、GPIO 复用或时序参数。
- 涉及 I2C 地址必须区分 7-bit 和 8-bit。
- 涉及 USB / UVC / HID 时，优先保证协议合法性和 Windows / Linux / macOS 兼容性。
- 涉及 UVC Payload Header、endpoint descriptor、commit/probe 参数时，必须说明当前行为、目标行为、修改层级和可能影响的主机系统。
- 涉及实时任务、线程优先级、DMA、USB 中断、显示刷新时，必须说明是否可能影响系统流畅度或资源占用。
- 涉及 GPIO/ADC/NTC/AVDD 时，不要只从软件角度判断；硬件连线和供电问题可能是根因。
- 涉及电源、MOS、充电、OTG、过流、发热时，不要只从软件角度判断，必须提示可能存在硬件风险。
- 无法实测硬件时，明确标注“未上板验证”。
