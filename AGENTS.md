# AGENTS.md

本文件是 `/home/slam/dev/pico_tn160` 的项目级规则。
全局行为规则由 `~/.codex/AGENTS.md` 负责；本文件只补充本项目独有的启动、测试、目录、USB NCM/硬件坑点和交接规则。
如果本文件与全局规则冲突，优先遵守本文件中更具体的项目规则。

---

## 0. 工程控制论式项目执行协议

本文件是项目级规则；全局规则只放在 `/home/slam/.codex/AGENTS.md`。本项目任务默认按闭环推进：目标 -> 状态 -> 误差 -> 控制动作 -> 反馈 -> 修正 -> 验证 -> 沉淀。

- 动手前先说明当前目标、系统状态、目标与现状误差、本轮控制变量和最小闭环验证方式。
- 只调整完成目标所需的最小变量，不把无关优化、重构或格式化混进当前任务。
- 修改后必须用构建结果、NCM/HTTP 帧读取、主机兼容性反馈、硬件实测或用户反馈验证；反馈不一致时先定位误差来源，再继续改。
- USB NCM 描述符、TinyUSB 网络缓冲、PIO、DMA、时序和 Flash/OTA 参数必须保持稳定性优先；禁止为了当前任务破坏已跑通的多平台 NCM 基线。
- 长任务或上下文可能丢失时，按项目规则更新 HANDOFF，而不是依赖对话记忆。

辅助工具：

- 开工前优先运行只读 preflight：`python3 /home/slam/tools/agent_preflight.py --project pico-tn160`。
- 用户要求接手、先看看、对齐需求、以结果为导向、可反驳、先不要改代码时，优先读取 `/home/slam/Sipeed/.agents/skills/project-intake/SKILL.md`。
- UVC、USB descriptor、Bulk/ISO、PIO/DMA、热成像采集或烧录器任务优先读取 `/home/slam/Sipeed/.agents/skills/pico-tn160-uvc/SKILL.md`。
- 遇到重复问题先查 `/home/slam/Sipeed/C_context/KNOWN_FAILURES.md`。

---

## 1. 项目概览

- 本项目是：基于 RP2350 的 TN160 热成像采集与 USB NCM/HTTP 输出固件。
- 主要技术栈：Pico SDK、TinyUSB CDC-NCM、lwIP、PIO、DMA、双核处理、HTTP、Flash/OTA。
- 主要运行环境：RP2350 固件；主机侧验证 Windows、Linux、Android/iOS 的 USB NCM 与 HTTP。
- 当前核心目标：保持正式热成像固件可构建，并维持多平台可用的 NCM 成功基线。
- 产品/工程优先级：稳定性 > 可验证性 > 小改动 > 跨平台兼容 > 性能 > 重构。

不要把本项目当成：

- 普通桌面/后端项目：它的主要风险在固件、USB 协议、时序、DMA/PIO 和硬件实测。
- 可随便重构的实验项目：很多参数来自硬件验证，不能凭经验改。
- 只看单一主机系统的 USB 项目：Windows、Linux、Android/iOS 的 NCM 枚举和 HTTP 可达性都是当前基线的一部分。

### 文档与日志默认位置

- 本项目主要文档、日志、图片和现场资料的默认根目录是 `/home/slam/Sipeed/wsl/pcio_tn160/docs/`，对应用户侧路径 `Sipeed\wsl\pcio_tn160\docs`。
- 用户明确给出 `Sipeed\wsl\pcio_tn160\docs\...`、提到该目录下的文件，或只说 `logs.txt`、最新日志、图片、看图时，优先从该外部文档根目录定位。
- 不得把明确的外部文档路径替换成当前仓库 `/home/slam/dev/pico_tn160/docs/` 下的同名文件；目标不存在时报告真实缺失，不静默读取另一份同名文件。
- 当前仓库 `docs/` 只用于仓库内已有或明确指定的版本化工程文档。源码搜索、编辑和构建仍在 `/home/slam/dev/pico_tn160` 进行；外部文档默认只读，除非用户明确要求修改。

---

## 2. 常用命令

如果命令不确定，先查：

- `README.md`
- `HANDOFF.md`
- `CMakePresets.json`
- `../env.sh`
- `CMakeLists.txt`
- `tools/pico-sdk-2.2.0/`
- `docs/`

不要凭经验猜命令。

### 安装依赖

本项目当前没有项目内一键安装依赖命令。不要自动联网安装依赖；缺依赖时先说明缺什么、为什么需要。

### 启动开发环境

没有长期运行的开发服务。默认工作流是代理修改固件源码、用户手动构建 UF2、用户上板验证。

### 构建权限（用户固定要求）

- 固件构建和最终 UF2 生成默认全部由用户手动执行。
- 代理不得主动运行 `cmake --build`、`cmake --fresh`、配置加构建或其他会更新 `build/`、ELF、UF2 的命令。
- 用户说“继续”、“尝试一下”、“修改”、“验证”或认可方案，都不等于授权代理构建。
- 只有用户在当轮明确说“请你构建”或等价精确指令时，代理才可例外执行该次构建。
- 代理修改固件后，只做不生成固件的源码、diff 和静态检查；然后提供一行用户可直接执行的真实构建命令，并等待用户反馈。
- 未收到用户构建结果前，必须标注“未经用户最终构建”；代理曾生成的产物不得当作最终交付件。

下列命令是用户手动构建的参考合同，它们出现在本文中不构成对代理的构建授权。

### 增量构建

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --build --preset pico2-release --parallel "$(nproc)"
```

### 配置并构建

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --preset pico2-release && cmake --build --preset pico2-release --parallel "$(nproc)"
```

### 彻底刷新

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --fresh --preset pico2-release && cmake --build --preset pico2-release --parallel "$(nproc)"
```

构建产物：

```text
/home/slam/dev/pico_tn160/build/pico_tn160.elf.uf2
```

### 运行测试

本项目没有完整自动化测试。固件相关修改的最小构建由用户手动运行：

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --build --preset pico2-release --parallel "$(nproc)"
```

涉及 NCM/HTTP/OTA 时，硬件侧由用户在 Windows、Linux、Android/iOS 上验证。没有上板实测时，必须明确写“未上板验证”。

### 运行单个测试 / 最小相关测试

NCM 当前最小真机校验使用 `python3 scripts/validate_thermal_protocol_v1.py http://192.168.17.1`；必须在设备已通过 DHCP 枚举且 HTTP 可达时执行。

期望结果：

- `/api/v1/device` 返回协议版本和 NCM 地址。
- `/frame.temp8` 与 `/frame.adc16` 的 payload、metadata 和 CRC 满足 HTTP v1。
- 拔插后主机重新得到 DHCP 地址并能恢复访问。

### Lint / Format / Type Check

当前没有统一 lint/format/typecheck 命令。不要为了文档或小修引入格式化噪声。

------

## 3. 项目结构

- `src/`：固件主线源码，包含采集、处理、NCM/HTTP、USB 描述符、Flash/OTA 和模式配置。
- `src/main.c`：主入口、双核处理、NCM 帧发布、校准/FFC 主逻辑。
- `src/init.c`：SPI/PIO/DMA 初始化和原始数据接收链路。
- `src/sdk.h`：GPIO、模式开关、Flash 布局和全局结构定义。
- `src/tusb_config.h`：TinyUSB 配置，NCM NTB 缓冲等关键参数在这里。
- `src/usb_descriptors.c` / `src/usb_descriptors.h`：USB CDC-NCM 描述符。
- `src/thermal_ncm.c` / `src/thermal_ncm.h`：NCM、DHCP/DNS、HTTP 与热帧协议。
- `src/thermal_ota.c` / `src/thermal_ota.h`：OTA 和 Flash 事务。
- `tools/pico-sdk-2.2.0/`：本项目实际使用的 Pico SDK/TinyUSB 副本；不要改 `/home/slam/pico-sdk` 这类旧路径。
- `tools/backup/`：历史脚本、抓包、分析 notebook、参考数据。
- `docs/`：开发文档、接口文档、日志和 TODO。
- `docs/design/`：设计文档、架构、方案。
- `docs/adr/`：架构决策记录，可选。
- `docs/reference/`：芯片资料、协议资料、外部参考。
- `docs/runbooks/`：操作手册、排障流程。
- `docs/release/`：发布说明、验收记录。
- `scripts/`：项目脚本，偏工程流程。
- `tools/`：独立小工具，偏可复用工具；本项目已有 Pico SDK 副本也在此目录，按现有说明处理。
- `tests/`：测试。
- `examples/`：示例。
- `assets/`：正式资源。
- `build/`：构建产物，通常忽略。
- `out/`：临时输出或生成结果。
- `CMakePresets.json`：当前固定配置与构建预设。
- `HANDOFF.md`：当前 session 交接状态；顶部当前状态优先。

规则：

- 代码、测试、配置、资源文件优先遵守项目原有目录结构。
- 正式文档优先放 `docs/`；设计文档、架构、方案优先放 `docs/design/`。
- 项目脚本优先放 `scripts/`；独立可复用工具优先放 `tools/`。
- 测试、示例和正式资源分别优先放 `tests/`、`examples/`、`assets/`。
- 构建产物和临时输出优先放 `build/` 或 `out/`，并按项目规则忽略。
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
- 不改变 USB NCM 描述符、NTB 缓冲、端点、HTTP 协议、Flash/OTA 格式、GPIO/ADC 定义，除非当前任务明确要求。
- 不把 bug fix 扩大成架构改造。
- 不为了“高级写法”牺牲稳定性、可读性和调试便利。
- 如果发现无关但重要的问题，只报告，不直接修改。

------

## 5. 验证规则

修改完成后，优先运行最小相关验证。

推荐顺序：

1. 文档或规则文件修改：读回文件并检查 diff。
2. 固件源码修改：代理运行不生成固件的静态检查，然后由用户手动运行最小构建命令。
3. NCM/USB/硬件相关修改：收到用户构建通过的反馈后，再等待用户上板验证 Windows、Linux、Android/iOS 的枚举、DHCP 与 HTTP。
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

- 当前成功基线是 full-speed USB CDC-NCM + HTTP v1，不要回退到 UVC、CDC 或串口产品路径。
- NCM 基线参数：`CFG_TUD_NCM=1`、NCM IN/OUT NTB 均为 `8192`、HTTP 默认地址 `192.168.17.1`、`temp8` 和 `adc16` 均带 120-byte `SITHMETA`。
- Windows、Linux、Android/iOS 的 NCM 枚举、DHCP 和 HTTP 帧访问都属于成功条件。
- `MAIN_FUNC_MODE` 必须保持正式热成像模式 `MAIN_FUNC_MODE_SPI_INIT_40MHZ_4`，不要把历史 UVC/CDC 测试模式留在正式源码里。
- 构建环境统一由 `../env.sh` 和 `CMakePresets.json` 提供；缺少 lwIP 时必须失败，不要为构建静默下载未知版本。
- 新 Flash 布局为活动镜像 768 KiB、OTA 镜像 768 KiB、校准数据 504 KiB、OTA 头 4 KiB、设置 4 KiB；未经上板验证不要改变其偏移。
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

- 修改 `tools/pico_th160_thermocam_gui/` 时，保持现有 GUI 风格，并优先适配 NCM/HTTP 协议；不要恢复串口作为产品通道。
- GUI 改动不能擅自要求固件协议同时变化，除非任务明确要求。
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
- 涉及 USB NCM / HTTP 时，优先保证协议合法性和 Windows / Linux / Android/iOS 兼容性。
- 涉及 NCM descriptor、NTB、DHCP/DNS、HTTP 帧格式、OTA 或 Flash 事务时，必须说明当前行为、目标行为、修改层级和可能影响的主机系统。
- 涉及实时任务、线程优先级、DMA、USB 中断、显示刷新时，必须说明是否可能影响系统流畅度或资源占用。
- 涉及 GPIO/ADC/NTC/AVDD 时，不要只从软件角度判断；硬件连线和供电问题可能是根因。
- 涉及电源、MOS、充电、OTG、过流、发热时，不要只从软件角度判断，必须提示可能存在硬件风险。
- 无法实测硬件时，明确标注“未上板验证”。

## 12. TN160 Scope 采集入口（永久约定）

- `/home/slam/Project/yserein-scope` 的 `src/plugins/tn160_ncm/ProductionCalibrationPage` 是 TN160 研发/产测采集的默认入口，优先复用现有 Scope 组件，不另起一套 UI 或手工脚本流程。
- 现有采集槽位已经固定：`captureLow()`、`captureHigh()`、`captureHighB()`、`captureOffset()`；对应 `low/high/high_b/offset` 归档及 manifest、acceptance、monitor 数据。
- 研发 Family characterization 复用这四个槽位：A1=0°C、A2=50°C、B1=升高机芯热状态后的 50°C、B2=热状态下的 0°C。它是研发采集，不等于量产步骤。
- State B 的等待边界固定为：State A 参考建立后最多 180 秒；在 180 秒内 `|ΔNTC| >= 32 ADU` 时立即放行 `captureHighB()`；超过 180 秒仍未达到 32 ADU 时保持未就绪，不继续无限等待，也不自动放行。
- 量产仍遵守已确认的短流程；不得因为 Family/KT/BT/NUC-T 研发需要，把四状态采集强加到每台量产设备。
- `scripts/capture_tn160_family_characterization.py` 和 `scripts/capture_thermal_drift.py` 仅是无 Scope、自动化回放或底层排障备用入口；给用户操作步骤时，默认先写 Scope 页面路径和现有按钮，不要求用户复制脚本命令。
- Scope 工作树有用户既有改动，禁止 reset/restore/clean；本仓库修改温漂模型时不得顺手改 Scope UI、宿主 ABI 或组件状态机，除非用户另行授权。
