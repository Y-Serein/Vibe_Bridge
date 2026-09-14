# 产品迭代日志

## 2026-09-14 — 保留 NTC 均值并加入有界 FFC 全局漂移候选（待用户构建）

- 目标/用户现象：固定目标下 V1-only 与 V1+NTC 均存在设备自身热漂移；最新日志显示 NTC 可抵消部分漂移，剩余误差与已接受 FFC 的全局 `dg` 累积相关。
- 关键实现：保留 `K_Q8=290`；新增 `ffc_shutter_drift_adu`，仅累计已接受 FFC 的 `dg`，按 `256/256` 转成 anchor 补偿并限幅 ±600 ADU；FFC 连续性检查使用更新前后同一候选快照，拒绝/失败 FFC 不改变该项；不启用 B gain、不改变协议和 Flash。
- 验证结果：Python 全量测试 65 项通过；`main.c` 与 `thermal_drift_model.c` 在 `THERMAL_DRIFT_RUNTIME_ENABLE=0/1` 两种路径下 ARM `-fsyntax-only` 通过；`git diff --check` 通过。
- 当前状态：源码已修改，未运行 CMake 正式构建、未生成 UF2、未烧录、未上板。
- 风险/未验证平台：该系数来自当前固定目标日志的离线残差回归，是第一版有界候选；若真机绝对温度或 FFC 跳变不满足预期，应回退到仅 V1+NTC，不继续盲调系数。

## 2026-09-14 — 增加第二级 NTC / V1-only A/B 编译开关（待用户构建）

- 目标/用户现象：在保持 `K_Q8=290` 不变、暂不启用 B 的前提下，比较当前 V1+NTC 与 V1-only，定位最新日志中的大幅温漂来源。
- 关键实现：新增 CMake 选项 `THERMAL_DRIFT_RUNTIME_ENABLE`，默认 `ON`；关闭时运行时同时停用第二级 NTC 均值和第三级逐机 B，回退到 V1-only，不改变 Flash、USB/NCM、HTTP 或模型数据格式。
- 验证结果：Python 全量测试 65 项通过；`thermal_drift_model.c` 在开关 `1/0` 两种预处理路径下均通过 ARM `-fsyntax-only`；`git diff --check` 通过。
- 当前状态：源码已修改，未运行 CMake 正式构建、未生成 UF2、未烧录、未上板。
- 风险/未验证平台：需要用户分别构建并记录 `ON`/`OFF` UF2 身份，在同一设备和目标下做 A/B；OFF 变体仅用于诊断，不是最终产品默认配置。

## 2026-09-11 — 修复 B 模型回读归因并加入清除 B 回退操作（待用户构建/上板）

- 目标/用户现象：B 写入日志可能显示 `model readback verification failed`，但设备仍保留有效旧 B；用户需要在 B 效果不佳时主动退回第二层均值保底。
- 关键实现：模型 Flash 擦写完成后显式刷新 RP2350 XIP cache，并在同一非活动槽执行一次有界重写/回读重试；Scope 在 POST 或模型 GET 异常时先回读当前模型内容和 state，区分“新写入失败但旧 B 保留”和“B 失败、V1+均值保底”。新增 `POST /api/v1/calibration/v1/thermal-model/clear`，以 `flags=0x0002` 的 CRC/generation tombstone 停用逐机模型，不擦除基础 V1；Scope Context 新增“清除B补偿”，成功后确认 `valid=false, per_device=false, active=true, mean_active=true`。
- 验证结果：ARM 5 个源文件 `-fsyntax-only` 通过；Scope 3 个源文件和状态机头 Qt/MinGW `-fsyntax-only` 通过；Python 全量测试 64 项通过；两仓库 `git diff --check` 和 plugin JSON 校验通过。
- 当前状态：源码和 Scope 组件未提交、未生成新 UF2、未烧录；当前设备之前的 generation 3 B 模型仍有效，尚未用新固件验证 cache 修复或清除操作。
- 风险/未验证平台：真实 Flash 写入/掉电回退、B 清除后的均值状态、Windows Release 组件加载、多平台 NCM、0°C/50°C/冷启动/热 RST 仍未验证；清除按钮只应在确认不需要当前 B 时使用。

## 2026-09-11 — 基础 V1 后启用四台均值保底并统一 T160DM2 契约（待用户构建/上板）

- 目标/用户确认：基础两点标定完成后必须有可用保底；逐机 B 流程是可选补偿层，不能阻断 V1 温准或在失败时擦除原有效标定。
- 关键实现：基础 V1 写入、重启和全量回读后，固件以保存的 `base_ntc_adu` 驱动编译期四台均值 `K_Q8=290`；无有效逐机包时状态为 `active=1, mean_active=1, per_device=0`。逐机 `T160DM2` 仅保存 B 端相对均值的 anchor/gain 残差，绑定当前 V1 `calibration_id`；写入/回读失败保留 V1+均值，NTC/V1 参考无效退回纯 V1。协议文档、README 和只读验证器已同步到 T160DM2 字段与输出。
- 验证结果：`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py'` 通过（62 项）；ARM Release 参数下 5 个固件源 `-fsyntax-only` 通过；`python3 -m py_compile scripts/prepare_thermal_drift_model.py scripts/validate_thermal_protocol_v1.py` 通过；两仓库 `git diff --check` 通过。
- 当前状态：源码、Scope 组件、脚本和文档未提交；未运行 CMake/Qt 正式构建、UF2、Scope Release、烧录或真机 HTTP/黑体验证。
- 风险/未验证平台：四台均值目前是保底候选，不是量产 PASS；需先验证一台设备 V1 回读后的均值状态，再验证 B 成功/失败两条路径及 0°C、50°C、常温、冷启动、热 RST 和 NCM 兼容性。

## 2026-09-10 — 接入逐机 L(N)/H(N) anchor+gain 正式候选（待用户构建/上板）

- 目标/用户现象：时间+boot-relative NTC 与 RR 都只能把单条固定 50°C 轨迹拉准；V5 两热状态的 0→50°C 响应跨度约从 `1392` 变到 `1521 ADU`（约 `+9.25%`），证明只改 anchor 会破坏低温和常温温准。
- 关键实现：`binary_calib_frame()` 仅在 64-byte `T160DM2` 包 CRC、Flash 记录 CRC 和当前 V1 `calibration_id` 全部匹配时，按绝对 NTC 同时应用均值保底、逐机 `anchor_comp_adu` 与 `gain_scale_q16`；无逐机包但 V1 基准 NTC 有效时仍运行均值层，NTC 无效时保持 V1-only，超出学习区间时钳位并上报。FFC 前后使用同一模型快照，动态项不写入基础 anchor。模型生成器先将低/高采样分别减去自身 FFC anchor 再计算跨度，并拒绝混合 V1 及相同 NTC 下超过 0.5°C 的持续升/降温路径差异；有效 NTC 区间限制在两个完整低/高配对状态之间，不能用更宽的高温单轨迹冒充 gain 覆盖。RR、TN、TC、启动时间和 VTEMP 只保留诊断。
- 验证结果：Release 既有 ARM 参数下对 `main.c`、`thermal_calibration_v1.c`、`thermal_drift_model.c`、`thermal_ncm.c`、`thermal_ota.c` 执行 `-fsyntax-only` 通过；全量 Python 测试 64 项通过；温漂 Skill `quick_validate.py` 通过；`git diff --check` 通过。未运行 CMake 构建、未生成新 UF2、未烧录、未上板。
- 当前状态：源码和离线门禁完成，未提交、未发布；等待用户手动构建后，用当前设备同一 V1 的两个 0°C 热状态和固定 50°C 轨迹生成逐机参数，再做独立 0°C/已知常温/50°C 验证。
- 风险/未验证平台：当前 V1 尚无可信成对 L(N)/H(N) 参数，不能上传其他设备 V5 的模型。相同 NTC 的冷/热路径、瞬时 NTC 无效降级、模型 Flash 写入/断电恢复、冷启动/热 RST、Windows/Linux/Android/iOS NCM 均未真机验证；任何失败不得退回调时间常数或 RR。

## 2026-09-10 — 接入当前 V1 的启动时间 + boot-relative NTC fallback（待用户构建/上板）

- 目标/用户现象：V1-only 最新记录从约 62°C持续漂到约 41°C；RR 版固定 50°C约 49.5°C但手掌/环境仍约 14～21°C，不能继续作为默认控制。
- 关键实现：关闭 RR 运行时控制；仅对当前 V1 指纹（`mean_diff≈1078.487`、anchor `4250..4300`）接入 `A·exp(-t/τ)+bias+K_NTC·(NTC-boot_NTC)`，参数为 `A=334 ADU、τ=12 s、bias=155 ADU、K_NTC=1.234`，总限幅 `±600 ADU`。FFC 前后使用同一补偿快照，避免重复累计；其他 V1 保持不补偿。
- 验证结果：ARM `-fsyntax-only` 通过；全量 Python 测试 54 项通过；最新 9 月 9 日固定高温 CSV 离线回放 3～164 s 约 49～50°C；未构建、未烧录、未上板。
- 当前状态：源码候选完成，等待用户手动构建和 50°C/0°C/手掌/环境实测；不能宣称已解决全量程温准。
- 风险/未验证平台：参数由当前设备固定高温轨迹拟合，尚未证明跨设备或低温绝对准确；若低温仍错误，下一步必须联合拟合 anchor+gain，不继续调这组常数。

## 2026-09-09 — 撤掉单条 50°C 轨迹 RR 运行时控制（待用户构建/上板）

- 目标/用户现象：最新记录显示固定 50°C约 49.5°C，但换手掌/环境后仍约 14～21°C；确认上一版只是把单条 50°C训练轨迹拉准，不能作为全量程模型。
- 关键实现：关闭 `THERMAL_ROW_REF_RUNTIME_ENABLE`，恢复 `A_eff=A_FFC`、`D_eff=D_V1`；继续保留 row-reference/RR 参数和采样字段用于诊断，正式 RR 参数不再参与当前测温输出。
- 验证结果：ARM `-fsyntax-only` 通过；未构建、未生成 UF2、未烧录、未上板。
- 当前状态：V1-only 回退候选，等待用户手动构建及真机确认；下一步使用低温+高温配对数据联合拟合动态 anchor 与 gain。
- 风险/未验证平台：50°C上一版动态改善会消失；这是有意回退，避免继续输出高温准、低温错的假成功。联合模型尚未接入。

## 2026-09-09 — 修正 RR 启动偏置并加入场景不变量门控（待用户构建/上板）

- 目标/用户现象：上一版 `Δrow_ref` 从开机第一帧累计，9 月 9 日固定 50°C 数据在第一次 FFC 后已带入约 `+553 ADU`，显示约 40°C；需要保留约 1 分钟启动收敛窗口，不能再次把绝对温准搬偏。
- 关键实现：前 60 秒严格使用 V1；仅对当前 V1 指纹（`mean_diff≈1078.487`、FFC anchor 4250～4300）绑定已有 `row_ref` 参考 `4987 ADU`、`K=1`、限幅 `±600 ADU`。运行时使用 `center_roi_adu-row_ref_mean_adu` 作为场景不变量：变化超过 32 ADU 时冻结，连续 8 帧变化不超过 6 ADU 后无跳变重绑；不修改 V1 `mean_diff`、FFC 候选 anchor、USB/NCM 或 Flash 布局。
- 验证结果：ARM Release 参数下 `main.c`、`thermal_calibration_v1.c`、`thermal_drift_model.c`、`thermal_ncm.c`、`thermal_ota.c` 执行 `-fsyntax-only` 通过；全量 Python 测试 54 项通过；固定 50°C 日志离线回放 60～280 s 中位 `49.945°C`、P5～P95 `49.283～50.312°C`；v6 场景切换回放低温段中位 `-0.682°C`、环境段中位 `28.405°C`；`git diff --check` 通过。
- 当前状态：源码已修改，未构建、未生成 UF2、未烧录、未上板；离线回放不能替代真实黑体验证。
- 风险/未验证平台：当前 RR 参数只绑定 9 月 9 日这份 V1 指纹；其他无正式 RR 参数的设备保持 V1-only。低温和环境回放没有同步真值，只能证明未再次发生补偿饱和搬移，不能宣称已达到量产温准。需用户冷启动固定 50°C、0°C 和环境实测。

## 2026-09-09 — V1 绝对温准 + 受场景门控的 row-reference 动态 anchor（待用户构建/上板）

- 目标/用户现象：在不增加每台设备 4 分钟 `0°C→50°C→0°C` 额外采集的前提下，同时保留 V1 绝对温准并改善启动后动态温漂；此前 L(N)/H(N) 逐机模型尚无真机证据，且部分日志出现补偿失控。
- 关键实现：默认 `binary_calib_frame()` 保持 V1 `ram_temp_anchor`/`ram_mean_diff` 不变，仅叠加运行时累计 `Δrow_ref`；固定 `K=256/256=1.0`、限幅 `±600 ADU`。`row_ref_span > 900 ADU` 或参考无效时冻结最后动态补偿，恢复后重新绑定 row-reference 基线；FFC 连续性两侧使用同一动态快照，但不将动态项写回持久 FFC anchor。未验证的 L(N)/H(N) 控制路径不再参与默认测温。
- 验证结果：ARM Release 参数下对 `main.c`、`thermal_calibration_v1.c`、`thermal_drift_model.c`、`thermal_ncm.c`、`thermal_ota.c` 执行 `-fsyntax-only` 通过；相关 Python 契约/模型测试 51 项通过；`git diff --check` 通过。未运行固件构建、UF2、烧录或真机验证。
- 当前状态：源码候选完成，等待用户手动构建和固定 50°C/0°C/场景切换验证；未提交、未发布。
- 风险/未验证平台：row-reference 已被证明能稳定固定 50°C，但其原始值与场景辐射耦合；`900 ADU` 门限和冻结/重绑定策略尚未在多板、热机 RST、缓慢场景变化下验证。若 0°C 绝对温准被搬移或场景切换仍误补，应回退到 V1-only 并停止继续调 `K`/限幅。

## 2026-09-08 — 行参考驱动的逐机动态 anchor（50°C通过、跨温区失败）

- 目标/用户现象：固定50°C黑体冷启动359.5秒内，设备在6秒约58.7°C、末段约40.5°C，6秒后峰峰值约19.2°C；继续调整固定上电时间模型无法表达当前真实热状态。
- 关键实现：TNC1既有8字节可选区新增兼容 `RR(0x5252)` tag，保存逐机行参考值、Q8系数和限幅；每个通过场景门禁的完整帧按当前120行参考均值修正 effective anchor。RR有效时不叠加旧TC时间项和NTC fallback；异常参考保持最后有效补偿，未取得有效样本则fail-open；无RR的旧包行为不变。新增诊断字段及本地TNC1生成工具，不改包长度、Flash布局、120-byte metadata、USB/NCM或FFC时序。
- 验证结果：首台写后50°C冷启动359.7秒中，RR全程有效，6秒后P-P为1.3°C、首尾10秒均值差-0.42°C、末段均值50.18°C，达到50°C动态门槛。随后v6跨场景实测中，RR reference为7072，环境场景的row_ref降至6060，补偿饱和到-600 ADU，anchor 6006被拉为effective_anchor 5406，环境约20多°C被读成36.6°C；用户同时确认0°C约读17°C。
- 当前状态：RR只证明固定50°C温漂可被行参考跟踪，但跨温区已经真机证伪，不能量产；本提交仅作为可回滚的观测/50°C候选检查点，下一步先分离行参考中的场景分量和真实热漂移，不继续调reference、K或limit。
- 风险/未验证平台：行参考与场景ADU近似同向变化，单轴 `anchor += K×(row_ref-reference)` 会抵消真实温差信号；0°C、环境和手掌误差不是重新标定能解决的问题。热机RST、跨板和量产Scope流程不再是当前首要矛盾。

## 2026-09-07 — 拒绝无效两点响应并收紧 TNC1 接收（待用户构建/上板）

- 目标/用户现象：一次错误采集得到 `mean_diff=1.000 ADU`，仍被生成并写入设备，导致温度比旧稳定版更不准；有效历史设备的 `mean_diff` 为约 `752～1689 ADU`。
- 关键实现：TNC1 生成、导入、只读契约和固件 Flash 事务统一要求 `mean_diff >= 100 ADU`；低于门槛时不形成可写结果，Scope 保留冷启动与 0°C 数据并提示只重采 50°C。该门槛只排除没有真实两点响应的致命数据，不增加温漂残差、样本跨度或使用状态限制。
- 验证结果：协议契约单测 22/22 通过，精确覆盖 `1 ADU` 拒绝和 `100 ADU` 边界接受；源码 whitespace 检查通过。按用户分工未编译、未生成 UF2、未烧录。
- 当前状态：源码未提交；旧的有效 TNC1 不受影响，`mean_diff < 100 ADU` 的坏包将不再启动或写入。
- 风险/未验证平台：100 ADU 是基于当前有效样本分布设置的保守致命门槛，仍需量产多机数据确认；这不解决现有启动温漂模型残差，只防止伪标定破坏温准。

## 2026-09-07 — 修复 Windows NCM DHCP 续租双峰延迟（待用户 fresh 构建/上板）

- 目标/用户现象：同一固件、同一设备重枚举后，Scope 首个有效 ADC16 有时为 `2.3～4.7 s`，有时稳定落在 `22.0～22.8 s`，后者触发“未记录到冷启动起点”；慢连接时固件帧号已超过 600，排除传感器未出帧。
- 关键实现：扩展 build-local TinyUSB 0.18 DHCP 补丁：SELECTING/INIT-REBOOT 继续读取 Option 50；BOUND/RENEWING/REBINDING 在没有 Option 50 时读取 `ciaddr` 并直接 ACK；旧子网、越界或冲突租约立即广播 NAK，不再让 Windows 等待续租退避。CMake 新增 NCM 与 DHCP overlay marker 断言，补丁没有真正进入生成副本时配置直接失败。
- 验证结果：当前 `build/tinyusb-ios26` 和 DHCP 目标对象确认仍是 9 月 4 日未打补丁的旧副本，解释了最新 UF2 未包含既有租约修复；新补丁已通过 `git apply --check`，CMake/patch `diff --check` 通过。按用户分工未配置、未编译、未生成 UF2、未上板。
- 当前状态：源码未提交；必须执行一次 fresh 配置和构建，普通增量构建不能证明新 DHCP overlay 已生效。
- 风险/未验证平台：尚缺 Windows 抓包直接证明慢样本使用 `ciaddr` 续租；修复依据是 RFC 2131、当前 DHCP server 明确忽略无 Option 50 REQUEST，以及真机 `2～4 s/22 s` 双峰。需复测连续多次冷启动，不能只测一次快样本。

## 2026-09-07 — 逐机冷启动状态模型接入正式 TNC1（待用户构建/上板）

- 目标/用户现象：最新同机记录中 VTEMP 已退出控制，但固定目标下开机仍从高温读数向下收敛；0～60 秒 RAW 是全局启动漂移，单靠相对 NTC fallback 无法覆盖。旧“50°C 手动加热学习”训练域与冷启动不一致，不能作为量产最终流程。
- 关键实现：TNC1 头 `52..59` 新增兼容 tag `TC(0x4354)`，保存逐机 NTC 冷/热启动门槛、有符号启动幅度和 0.1 秒时间常数；运行时使用 `base_anchor + A×exp(-t/τ) - 0.20×ΔNTC`。启动时间项只作用于 effective anchor、经过 `6τ` 归零且不写入 FFC 基础 anchor；相对 NTC 项仍在 FFC 时承接。VTEMP 继续只作诊断，旧 `TN` 参数可读回但不参与控制；无效/未门控模型不阻断出图。
- 验证结果：用 `20260907_102542_333` 的 80 个有效冷启动 ADC16/遥测配对和对应 V1 离线复算，得到 `A≈201 ADU、τ≈16.2 s`，量化补偿在 0/10/30/60 秒约为 `201/108/32/5 ADU`；相对无启动项，残差 RMSE 从 `32.3` 降到 `13.0 ADU`、约下降 60%。协议/边界单测 20/20 通过，固件与 Scope `diff --check` 通过；未编译、未生成 UF2、未烧录。
- 当前状态：固件、Scope 自动采样/拟合、TNC1 写入回读和 diagnostics 契约已接通，源码未提交；等待用户手动构建并用同一设备冷启动复测。
- 风险/未验证平台：模型证据当前只来自一台设备的一次有效冷启动；真实 0°C/50°C 温准、冷启动峰峰值、热机重启门控、早期 FFC 连续性、Windows NCM 回读以及跨板分布均未上板验证。离线拟合改善趋势不等于真机已经达到温漂指标。

## 2026-09-07 — VTEMP 退出温度控制与 FFC anchor（待用户构建/上板）

- 目标/用户现象：fallback 真机日志中 VTEMP 在约 55～60 秒从约 `14066` 突降到 `13581 ADU`，而 NTC 基本连续；其变化被放大为 `total_comp -8→-104 ADU`，并在下一次 FFC 写入 anchor，制造约 4°C 错误回升和后续 anchor 搬移。
- 关键实现：VTEMP 保留原值、平滑值和 FFC dV 诊断，但完全退出逐帧补偿、补偿有效性判断、恢复参考和 FFC 累计写入。fallback 只保留现有 `-0.20×ΔNTC` 小幅相对补偿；TNC1 继续仅存储/回读且 `thermal_ntc_model_active=false`。
- 验证结果：已完成 VTEMP 控制引用残留审计、`git diff --check` 和协议 Python 语法/契约检查；未编译、未生成 UF2、未烧录。
- 当前状态：源码未提交，等待用户手动构建后用同一设备、同一 V1 做 0～90 秒冷启动记录。
- 风险/失败线：本改动只消除已经证实的 VTEMP 错误补偿，不声称解决剩余约 223 ADU 的原始冷启动漂移。如果新 CSV 仍出现约 100 ADU 的补偿突变，说明退出不完整；若补偿平稳但温度仍下降，则进入冷启动状态模型，不再恢复 VTEMP。

## 2026-09-07 — 暂停逐机单轴 TNC1 控制，恢复稳定 fallback（待用户构建/上板）

- 目标/用户现象：真机 q1 每次冷启动均先偏高再下降，约 60 秒才主要收敛；用户确认历史无逐机模型时通常 6～30 秒内收敛且稳准只略差。
- 关键实现：保留 TNC1 `reference/k_q8/limit` 的写入、Flash 校验和 diagnostics 回读，但删除其运行时 anchor 计算路径；帧处理和 FFC 始终使用 `+0.20×ΔVTEMP-0.20×ΔNTC` 稳定 fallback。diagnostics 新增 `thermal_ntc_model_active=false`，明确区分“参数合法存储”和“参与控制”。
- 验证结果：基于 q1 对应 991 条 ADC16 自动记录离线重放：0～60 秒原始中心约下降 289 ADU，当前模型仅抵消约 46 ADU，无法解决主启动漂移；后段 `-116 ADU` 会使绝对温度抬高约 4.5～5°C。已完成旧模型运行引用审计和 `git diff --check`；未编译、未生成 UF2、未烧录。
- 当前状态：源码未提交，等待用户手动构建和同机 0～90 秒验证。
- 风险/未验证平台：恢复 fallback 不能保证首分钟直接回到历史 6～30 秒，因为当前 V1、FFC 和旧稳定整包并非同一 artifact；前两次 FFC 各约 1.1°C 的持续变化仍需后续单变量处理。

## 2026-09-04 — 基础 V1 恢复稳定补偿并收敛生产路径（待用户构建/上板）

- 目标/用户现象：双点标定后的基础 V1 必须可直接用于设备，不能因为没有逐机 TNC1 参数就退回补偿 OFF；旧实验选择器和无效门禁增加了现场不可预测性。
- 关键实现：无有效 TNC1 时自动使用已提交稳定控制器 `15177d9` 的相对补偿系数 `K_VTEMP=+0.20、K_NTC=-0.20`，并在 FFC 时把累计补偿写入新 anchor；有效 TNC1 仍以逐机绝对 NTC 模型覆盖 fallback。NTC/VTEMP 无效时仅将当帧补偿置零，继续出图。删除 OFF/LEGACY/TABLE/NTC 编译选择器、时间 TABLE、旧 `+2.3/+3.0` NTC 曲线和失效的 VTEMP 触发/门禁分支；保留 TNC1、FFC 连续性保护、Flash/OTA、USB NCM、PIO/DMA 和硬件恢复路径。
- 验证结果：完成旧符号残留审计和 `git diff --check`；按用户要求未编译、未生成 UF2、未烧录、未上板。
- 当前状态：源码未提交；基础 V1 和逐机模型都等待用户构建及真实黑体验证。
- 风险/未验证平台：`15177d9` 是 MaixCam2 历史稳定控制器证据，本次只恢复其补偿系数和 FFC 累积语义，并非复刻当时整份固件；当前 RP2350/NCM 主线的温准、温漂、冷启动、热 RST 和 VTEMP 异常表现仍需实测。

## 2026-09-04 — TN160 单机 NTC global-anchor 正式候选（待用户构建/上板）

- 目标/用户现象：允许每台设备在双点标定后保存自己的 NTC 温漂系数，使同一固件按该机 NTC 状态修正全局测温 anchor；参数或 NTC 异常时不能阻断正常成像。
- 关键实现：新增 `THERMAL_COMP_MODEL_NTC` 并设为当前 selector；复用 TNC1/V1 64-byte 头原有 `52..59` 保留区保存 `tag、ntc_reference_adu、ntc_k_q8、ntc_limit_adu`，不改变包长、Flash slot 和 `FlashDataInfo` 大小；参数由 header CRC、selector record CRC 和边界检查保护。运行时使用 Q8 定点线性模型和逐机限幅，只修改 `eff_anchor`；FFC 前后复用同一 NTC 快照；旧全零 V1 包、非法参数或无效 NTC 均补偿为 0 并继续出图。diagnostics 增加模型有效性和参数回读。
- 验证结果：`git diff --check` 通过；`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py' -v` 13/13 通过，覆盖旧 V1 全零兼容、有效/越界模型、diagnostics 限幅和既有 FFC/回放契约。按用户要求未编译、未生成 UF2、未烧录、未上板。
- 当前状态：源码候选完成，未 commit、未 push；真实 `K_ntc/reference/limit` 尚需由单机受控加热数据拟合并写入 TNC1 后验证，不能宣称温漂已经解决。
- 风险/未验证平台：单轴全局 anchor 假设仍需固定黑体冷启动/热 RST 验证；空间 offset、gain 温漂和升降温滞回不在本候选中。USB/NCM、FFC、0°C/50°C 温准及多设备量产一致性未上板验证。

## 2026-09-04 — TN160 FFC/测温输出基础缺陷修正（待用户构建验证）

- 目标/用户现象：修正当前固件中测温输出与显示增强混用、FFC 触发状态不可靠、无帧重试过密、异常候选强制接受和诊断字段不足等问题；保持设备在补偿失败时继续出图。
- 关键实现：NCM `temp8` 跳过显示去条纹/detail 增强；FFC 调度改用最后完整帧 VTEMP 快照；无帧和预测跳变失败进入冷却并保留旧校准；删除重复拒绝后的强制接受；增加 FFC 帧数、mean/VTEMP/NTC span、行列统计和耗时；补全 public metadata 的 FFC/快门状态和 16-bit 计数，并扩展协议静态校验。
- 验证结果：已运行 `git diff --check` 和 `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py' -v`，8/8 通过；未运行固件构建、未生成 UF2、未烧录、未上板验证。
- 当前状态：源码修改完成，等待用户按项目 preset 手动构建和真机验证；未 commit、未 push。
- 风险/未验证平台：NCM `temp8` 视觉效果相对旧显示增强路径可能变化；FFC 保护、metadata 状态和温度输出需在真实 NCM、黑体、冷启动/热 RST 下验证；既有工作树修改未重新整理。

## 2026-09-03 — 单设备固件标定底层闭环（进行中）

- 目标/用户现象：将 UF2 烧录、固定 `.17` 连接、实时热图、连续热状态记录、A/B 标定和回读归档合并为一个单设备固件标定流程；当前首个运行 FFC 会造成约 14.6°C 持久突跳，后段仍从 68.6°C 漂到约 47.5°C。
- 关键实现：固件默认网络地址固定为 `192.168.17.1`，保留旧动态网段源码作为回退；首次 FFC 增加同帧中心/稀疏全场预测门，预测变化超过 1.5°C 时拒绝提交；Scope 底层增加烧录成功信号、单设备状态、连接即记录的 ADC16/遥测索引、B1 重放验证及测试件/生产件写入分类。
- 验证结果：本轮仅完成静态检查；`git diff --check` 和新增纯 C++ 校验头的 `g++ -fsyntax-only` 通过。未运行 Scope 构建，未运行固件构建。
- 当前状态：进行中，等待用户手动构建固件和 Scope UI Release；未上板验证，未 commit、未 push。
- 风险/未验证平台：首次 FFC 门的硬件效果、固定 `.17` DHCP/NCM、烧录后自动重连、连续 ADC16 记录配对、B1 生产门和三块板跨板温漂均未完成真机闭环。

## 2026-09-01 — 温漂三板状态辨识 OFF 固件

- 目标/用户现象：现有温漂 TABLE 只按本次上电时间补偿，无法区分冷启动和热机 RST；历史单 NTC 与四温区 V2 路线均已实机失败。为三台真机建立 `NTC + 快门累计 dG` 每机状态模型，第一轮必须先得到不受 TABLE 干预的 0～180 s 原始响应。
- 关键实现：只把 `src/main.c` 的 `THERMAL_COMP_MODEL` 从 `TABLE` 切到 `OFF`，使诊断中的 `total_comp_adu` 预期恒为 0；保留 V1 两点标定、8 帧 FFC、NCM/HTTP、USB 描述符、NTB、Flash/OTA 布局和实时采集路径不变。本固件是三板辨识件，不是温漂成品算法。
- 验证结果：使用固定 `build.sh` 和官方 lwIP `STABLE-2_2_1_RELEASE` 构建通过；V1 标定事务单测 2/2 通过，`git diff --check -- src/main.c` 通过。UF2 为 581120 字节，SHA-256 `aa834144f29ca16a0f3d0b65abd7f7d6052f6780f1a93fd92e1f1ced1436d3b6`，`text/data/bss=292924/1416/397148`。
- 当前状态：唯一 UF2 已生成，源码未提交、未发布，**未上板验证**；只有三板固定 50°C 冷启动 180 s + 立即热 RST 180 s 数据完成留出验证后，才允许实现状态补偿候选。
- 提交/版本：无提交；唯一固件入口为 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。
- 风险/未验证平台：OFF 会暴露真实温漂，显示温度明显变化属于预期，不能用于成品验收。当前 UF2 同时包含尚未真机闭环的 NCM 动态网段修复；三板辨识前应先用一台设备确认枚举、DHCP 和 HTTP 可用。若 `NTC+dG` 在冷/热留出段仍不能同时满足峰峰值 ≤1.5°C、端点漂移 ≤0.5°C、50°C 稳态误差 ≤1°C，必须停止该线性模型并增加物理观测量，不能继续微调常数。

## 2026-09-01 — NCM 动态网段切换后 Windows 快速重获地址

- 目标/用户现象：关联 2026-08-26“TN160 生产地址分配与统一用户地址”。首次真机执行 `.10→.11` 时，Scope 连续报告 `Connection refused` 并清空本轮，Windows 还出现 USB 报错，约 89 秒后页面才显示 `.11`；属于动态网段切换的测试覆盖不足，不是旧固定 `.17` 根因复发。
- 关键实现：保留 VID/PID、USB NCM 描述符、NTB、MAC、HTTP/TNC1 和 Flash 布局不变；网段写入成功后先 `tud_disconnect()` 保持 250 ms 的明确 USB 断开窗口，再 watchdog 重启。通过受版本控制的 build-local TinyUSB DHCP overlay，使新网段 DHCP server 对 Windows INIT-REBOOT 携带的旧子网租约立即返回 DHCPNAK，促使客户端重新 DISCOVER，而不是静默等待旧租约超时。
- 验证结果：官方 lwIP `STABLE-2_2_1_RELEASE` 下固定 `build.sh` 构建通过；生成 UF2 581632 字节，SHA-256 `e42d2f1cf3984fcf7b9982897ae6a8f5a81d633a7d62a8d90c7dfdcb46cbe009`；build-local `dhserver.c` 已读回确认包含 DHCPNAK 路径，V1 契约单测 2/2、两仓库 `diff --check` 通过。
- 当前状态：源码和 UF2 已生成，工作树未提交、未发布，**未上板验证**。
- 提交/版本：无提交；唯一固件入口为 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。
- 风险/未验证平台：必须用一台真实设备复测 `.10→.11` 的 USB 移除/重枚举、DHCP、同 serial 和 HTTP 恢复时间；再继续三台 `.11/.12/.13` 与最终 `.17` 互斥。Android/iOS 和 Linux 尚未复测。

## 2026-08-26 — TN160 生产地址分配与统一用户地址

- 目标/用户现象：量产需要同时操作多台 TN160，但所有设备固定使用 `192.168.17.1` 会产生地址冲突；完成设备交付用户时又必须只保留一个统一地址。
- 关键实现：无有效网段配置且无有效标定的空白设备默认使用 `192.168.10.1`，Scope 可分配 `192.168.11.1`～`192.168.16.1` 作为临时工位地址；已有持久化地址继续优先，未完成设备拔插后保留临时地址并由 Scope 重新发现，标定完成且回读通过后再切换到 `192.168.17.1`。若网络设置丢失，存在有效标定的设备仍回退到用户地址 `.17`。
- 验证结果：使用官方 lwIP `STABLE-2_2_1_RELEASE` 临时依赖完成固定 `build.sh` 构建；`tests/test_tn160_calibration_v1_contract.py` 2/2 通过，两仓库 `diff --check` 通过。UF2 为 581632 字节，SHA-256 `23b6b4c8f012435815f25d9d0b30cc79c304d2e942d6f6b16add209f26cb2dbf`。
- 当前状态：源码和 UF2 已生成，工作树未提交、未发布，**未上板验证**。
- 提交/版本：无提交；UF2 位于项目约定的 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。
- 风险/未验证平台：尚未实测 `.10→.11～.16→.17` 的 DHCP/重启/拔插闭环，也未验证三台设备并行时 Windows 路由、Scope 自动发现及 Android/iOS 最终 `.17` 访问。

## 2026-08-21 — USB NCM 双点标定写入与 Scope 工位组件

- 目标/用户现象：双点标定数据此前只能从 USB 读取，不能经 USB 安全写入 Flash；生产工具仍依赖历史串口组件。
- 关键实现：新增 TNC1 固定 115274-byte 包的 NCM/HTTP 写入和只读回检接口；设备先写入未激活 payload 槽、逐页回读校验后才切换 selector，启动时还校验 selector 标记、记录 CRC 和 payload CRC 后按有效 generation 选择标定，并拒绝将部分写入 selector 当作旧表加载。新增 Scope tn160_ncm 组件，采用 USB CDC-NCM + HTTP 采集、诊断、生产双点标定、写后重启和 header/三段 payload 回读，不打开串口。
- 验证：此前使用本地 lwIP 的 build.sh 固件构建通过；离线 TNC1 契约单测和既有双状态回放单测通过；Windows Qt6 下 Scope 组件编译成功，`plugin_loading_tests`、`workbench_layout_tests` 和主程序组件生命周期通过。插件直接持有 QtNetwork 时曾在真实 HTTP 请求后卸载崩溃，已改为由常驻 Core 持有 HTTP 执行器。
- 当前状态：软件实现与回归验证完成，未提交、未上板验证。
- 提交/版本：工作树未提交；固件 UF2 生成于 2026-08-21 18:03 CST，SHA-256 为 f5e05b774b506573b6ebc110aa7c5505ec8610016030d9581ded5dd48d364979。
- 风险/未验证平台：未实测 Windows/Linux/macOS 的真实 NCM 重新枚举、真实 Flash 擦写/掉电恢复和生产标定；写接口仅适用于物理受控 USB 工位，不具备网络级认证。

## 2026-08-21 — 产品链路切换为 USB NCM/HTTP

- 目标/用户现象：产品只保留经多平台验证的 USB NCM；预览、产测原始帧、网络设置和 OTA 均走 USB，不再将 UART、UVC 或 USB CDC 作为产品入口。
- 关键实现：采用 `feature/unified-thermal-protocol-v1` 基线，启用 CDC-NCM 和 HTTP v1，关闭 TinyUSB Video/CDC class；引入 NCM 描述符、网页、DHCP/DNS、`temp8`/`adc16` 帧接口、OTA 和 2 MiB Flash 的双 768 KiB 镜像布局。
- 验证：在 Pico SDK 2.2.0 固定的 lwIP 版本下完成标准 `build.sh` 固件构建；UF2 SHA-256 为 `dade5cd83465ff17fbe4ac7a037a22fe3517bd7ff804bc246a9c4366208783d7`，代码段 284316 B，低于 768 KiB OTA 槽限制。协议校验脚本可解析，但本轮未连接设备执行 HTTP 校验。
- 当前状态：已完成代码迁移，待上板验证。
- 提交/版本：本次 NCM-only 基线提交；源基线为 `origin/feature/unified-thermal-protocol-v1` 的 `c8c1779`。
- 风险/未验证平台：当前 UF2 未重新完成 Windows、Linux、Android/iOS 的 NCM 枚举、DHCP、HTTP 流、OTA 或反复拔插验证。`/frame.adc16` 已提供 USB 原始帧读取，但 USB 写入双点标定 Flash 的协议尚未定义或实现；旧串口标定命令不应继续用于产品流程。OTA 当前没有签名、型号/硬件匹配或掉电回滚，不能用于量产升级。
