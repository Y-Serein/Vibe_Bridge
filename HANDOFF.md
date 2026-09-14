# HANDOFF.md

最后更新：2026-09-14 09:20 CST
当前分支：`test-2026-06-09`；HEAD `1093d1e`；固件和 Scope 工作树均有大量未提交改动，禁止 reset/restore/clean。
当前任务：保留 TN160 三级温漂模型，完成 B 模型 Flash 回读修复和“清除B补偿”回退到第二层；源码/静态验证完成，等待用户手动构建、烧录和真机闭环。

## 当前在做什么

- 当前控制结构：`基础 V1 → 四台均值保底 → 可选逐机 B 残差`。均值固定 `K_Q8=290`（`1.1328125 ADU/NTC`）；NTC/V1 参考无效时退回纯 V1。
- B 只修正较晚热状态相对 V1+均值的全局 anchor 残差和 0°C→50°C 全局响应跨度残差；它不是启动前一分钟的时间模型。当前 B 流程在 V1 写后才采集，是否解决启动首分钟温漂仍未证明。
- 用户最新旧固件真机状态：V1 `generation=5`，B `generation=3`，共同 `calibration_id=558161692`；B 状态 `valid=1, active=1, mean_active=1, per_device=1`，`base_ntc=1934`、`learned_ntc=1669`、总 anchor `-399 ADU`、gain delta `3663 Q16`。
- 最新源码修复 RP2350 模型 Flash 写后立即 XIP 回读窗口：擦写后显式 `flash_flush_cache()`，校验失败时只对同一非活动槽再写一次。旧有效槽在新记录验证前保持可用。
- Scope 遇到 B POST/GET 异常时先回读模型内容和 `/thermal-model/state`，区分“新包其实已写入”“旧 B 仍运行”“仅 V1+均值运行”，不再把一次事务错误直接等同于当前无模型。
- 新增 `POST /api/v1/calibration/v1/thermal-model/clear` 和 Scope Context“清除B补偿”。固件提交更高 generation、`flags=0x0002` 的 CRC tombstone；成功后必须为 `valid=0, active=1, mean_active=1, per_device=0`，不擦除 V1。
- 正确训练数据根目录是 `/home/slam/Sipeed/docs/pcio_tn160/logs`；此前使用仓库 `docs/logs` 的结论已撤回。四台均值来自 v1/v2/v4/v5，异常 v3 排除。

## 已经试过的方案和结果（含失败的）

1. 时间 `TC=A×exp(-t/τ)`、boot-relative NTC、VTEMP、TABLE、旧相对 NTC：均有真机失败证据，禁止继续微调或恢复。
2. RR 行参考能把固定 50°C 拉稳，但与场景 ADU 约 0.997 相关；换手掌/环境和 0°C 后严重错误，已退出默认控制。
3. 五个自由 NTC/Anchor/Gain 节点被用户否定并撤销；当前 `T160DM2` 只有均值层和一个 B 残差端点。
4. 当前 B 模型在旧固件写入时曾报 `model readback verification failed`，但 curl 证明 generation 3 旧/当前有效模型仍运行；错误根因候选是底层 `FLASH_OP` 后未明确刷新 XIP cache，最新源码已修但未上板。
5. 当前五步 Scope 流程为 `0°C → 50°C（自动计算/写V1）→ 保持50°C → 回0°C → 写B`。它能辨识较晚热状态的 anchor/gain 残差，但错过启动首分钟；保留现状，不宣称已解决启动温漂。

## 下一步计划（3-5条actionable)

1. 用户手动构建固件和 Scope；确认构建/启动/UI 正常。仅出现 `module "YSereinSwarm" is not installed` 的构建期警告且程序正常时可忽略；若启动实际失败则必须处理。
2. 烧录新 UF2 后连接当前设备，先读 V1 manifest、B manifest/state，确认 `calibration_id` 和模型身份；不要立即重做采集。
3. 使用已保存本轮 B 包重写一次；预期不再出现 `model readback verification failed`。若仍失败，保留完整 Scope 日志和写前/写后 manifest/state，不重复盲写。
4. 点击“清除B补偿”，确认 `valid=0, active=1, mean_active=1, per_device=0`；断电重启后再次确认 tombstone 持久生效且 V1 ID 未变。
5. 在同一设备、同一目标、同一启动条件下比较 B 开启/清除后的 0°C、50°C、已知常温和冷启动前 60 秒；据数据决定保留 B，或重新设计“连接即记录”的启动可观测流程。

## 关键文件路径（相对路径，一行一个）

```text
AGENTS.md
HANDOFF.md
C_context/MEMORY.md
.agents/skills/pico-tn160-thermal-drift/SKILL.md
src/main.c
src/thermal_calibration_v1.c
src/thermal_calibration_v1.h
src/thermal_drift_model.c
src/thermal_drift_model.h
src/thermal_ncm.c
src/thermal_ota.c
src/thermal_ota.h
scripts/prepare_thermal_drift_model.py
scripts/validate_thermal_protocol_v1.py
tests/test_tn160_thermal_drift_model.py
tests/test_tn160_protocol_metadata.py
docs/SIPEED_THERMAL_USB_NCM_HTTP_PROTOCOL_V1.md
docs/design/tn160_thermal_drift_model_v1.md
docs/PRODUCT_ITERATION_LOG.md
../../Project/yserein-scope/src/plugins/tn160_ncm/ProductionCalibrationPage.cpp
../../Project/yserein-scope/src/plugins/tn160_ncm/ProductionCalibrationPage.h
../../Project/yserein-scope/src/plugins/tn160_ncm/Tn160NcmTransport.cpp
../../Project/yserein-scope/src/plugins/tn160_ncm/Tn160NcmTransport.h
../../Project/yserein-scope/src/plugins/tn160_ncm/Tn160ProductionWorkflow.h
../../Project/yserein-scope/src/plugins/tn160_ncm/Tn160Plugin.cpp
../../Project/yserein-scope/src/plugins/tn160_ncm/plugin.json
```

## 当前验证状态

- 已运行 pico_tn160 ARM Release 参数下 `main.c`、`thermal_calibration_v1.c`、`thermal_drift_model.c`、`thermal_ncm.c`、`thermal_ota.c` 的 `-fsyntax-only`；5/5 通过。
- 已运行 `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py'`；64 项通过。
- 已运行 Scope `ProductionCalibrationPage.cpp`、`Tn160NcmTransport.cpp`、`Tn160Plugin.cpp` 的 Windows Qt 6.11.1/MinGW 13.1.0 `-fsyntax-only -Wall -Wextra -Wpedantic`；通过。单独检查状态机头仅有 `#pragma once in main file`，可忽略。
- 已运行两仓库 `git diff --check`、Scope plugin JSON 校验；通过。
- 未运行最新源码的正式 CMake 构建、UF2 生成、Scope Release、烧录、真实 B 重写、B 清除、掉电恢复和多平台 NCM 回归；现有 build/UF2 是旧产物。
- 用户手动构建：`cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --build --preset pico2-release --parallel "$(nproc)"`。
- 用户手动 Scope 构建：`powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\Serein_Y\Project\yserein-scope\tools\windows_user_action.ps1"`。

## 还没搞清楚的问题

1. XIP cache flush + 单次重写是否彻底消除当前板的模型回读错误，尚无新固件真机证据。
2. 当前有效 generation 3 B 是报错事务实际写成的新包，还是报错前已有包；缺少该次写前 generation/包哈希，无法追溯。
3. B 对较晚热状态的 anchor/gain 是否有可感知收益，以及是否损害 0°C/常温，尚未做同条件 B on/off 对照。
4. 主要温漂若集中在启动前一分钟，当前 V1 写后 B 采集时序是否值得保留为量产步骤仍未决；连接即记录只能先辨识固定场景的组合漂移，不能凭单条轨迹独立得到完整 gain。
5. `docs/` 被 ignore，设计文档虽存在但普通 `git add -A` 不会纳入；未来提交必须显式处理。Scope 工作树还包含大量用户修改和已删除 tests，禁止顺手恢复或提交。
