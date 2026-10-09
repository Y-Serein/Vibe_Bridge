# HANDOFF.md

最后更新：2026-09-24 11:40 CST
当前分支：`test-2026-06-09`；本轮 UART/NCM 修复已完成，准备提交；工作区仍有未跟踪诊断生成物，不能 reset/restore/clean。
当前任务：TN160 温漂安全基线和 MaixCAM2 UART 兼容路径已经闭环到可用状态；用户已用最新固件确认 MaixCAM2 可以收到图像。主温漂控制仍是 `V1 + 绝对 NTC 290/256 + accepted FFC dg`，B 不是启动温漂主方案。

## 当前在做什么

- 默认 `pico2-release` 同时启用 USB CDC-NCM 和 UART 镜像；两路可以同时发送，不再由首个传输请求互斥锁定。
- MaixCAM2 端口固定为 `/dev/ttyS2`：2M 启动，发送 `0x44` 后切换 4M；串口帧包含完整 70-byte 诊断尾。
- 本轮无帧根因已经确认：`DOM_UVC` 与 `DOM_NCM` 是同值，旧 `uart_poll2()` 在 NCM 固件中误进入 UVC 过滤分支，把 `0x44` 丢掉，导致主机切到 4M 而固件停留 2M。修复为只在 `THERMAL_NCM_MODE=0` 时编译 UVC 过滤分支；NCM 构建直接接收 UART 命令。
- 当前热模型保持：

  ```text
  A_eff = A_FFC
        + clamp((NTC_current - NTC_base) × 290 / 256, ±600 ADU)
        + clamp(sum(accepted FFC dg) × 256 / 256, ±600 ADU)
  D_eff = D_V1
  ```

## 已经试过的方案和结果（含失败的）

1. `V1-only` 关闭 NTC 后更差；NTC 必须保留。
2. `warmup=-300·t/(t+12s)` + `K_rel=666/256` 在 2026-09-23 数据中过补，已撤回；不要继续调这个模型。
3. `T160DM2` B 不能从单条启动/高温轨迹辨识动态温漂；只保留未来逐机 anchor/gain 残差用途。
4. UART 初版虽然初始化了 `/dev/ttyS2` 和 2M→4M 工具握手，但 MaixCAM2 无帧；日志证明端口正确。真正根因是 NCM/UVC 枚举值别名导致 `0x44` 被丢弃。修复后用户实机确认恢复收帧。

## 下一步计划（3-5条actionable)

1. 保留本次提交作为 UART/NCM 可用基线；后续构建必须记录 UF2 SHA256，代理不代替用户构建、烧录或写 Flash。
2. 在同一 UF2 上分别复核 MaixCAM2 UART 图像、NCM HTTP `/api/v1/device`、`/frame.temp8` 和 `/frame.adc16`，确认串口修复没有影响 NCM。
3. Family KT/BT/NUC-T 研发继续直接使用 Scope `ProductionCalibrationPage` 的 `captureLow/captureHigh/captureHighB/captureOffset` 归档；四状态只用于研发，不强加到量产。
4. 只有基础动态路径稳定后，才用同一 V1 的 0°C/常温/50°C 配对证据评估逐机 B anchor/gain；不要把 B 重新当启动模型。

State B 固定边界：State A 后最多等待 180 秒；`|ΔNTC| >= 32 ADU` 立即放行 `captureHighB()`；超时未达到门槛保持未就绪，不无限等待、不自动放行。

## 关键文件路径（相对路径，一行一个）

```text
AGENTS.md
HANDOFF.md
C_context/MEMORY.md
.agents/skills/pico-tn160-thermal-drift/SKILL.md
CMakeLists.txt
CMakePresets.json
src/main.c
src/thermal_ncm.c
src/thermal_radiometry.c
src/thermal_radiometry_runtime.c
scripts/capture_tn160_family_characterization.py
scripts/fit_tn160_family_profile.py
scripts/fit_tn160_device_residual.py
scripts/prepare_tn160_radiometry_profile.py
tests/test_tn160_family_device_profiles.py
tests/test_tn160_radiometry_profile.py
/home/slam/Project/yserein-scope/src/plugins/tn160_ncm/ProductionCalibrationPage.cpp
/home/slam/Project/yserein-scope/src/plugins/tn160_ncm/ProductionCalibrationPage.h
```

## 当前验证状态

- 用户反馈：使用最新固件和 MaixCAM2 在线运行已恢复收帧，证明 `/dev/ttyS2`、2M→`0x44`→4M 握手及 UART 输出链路可用。
- 已运行：ARM GCC `-fsyntax-only`，`THERMAL_NCM_MODE=1/0` 均通过；`PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_*.py'`，71 项通过；`git diff --check` 通过。
- 未由代理构建或烧录；UF2 由用户执行。NCM 跨 Windows/Linux/Android/iOS 的最新回归尚未在本轮 UART 修复后重新跑完。
- 工作区保留 `androguard.*`、`out/`、`scripts/__pycache__/`、`tests/__pycache__/` 等未跟踪生成物，不能提交。

## 还没搞清楚的问题

1. 本轮修复后 NCM 多平台完整回归和长时间 UART 丢帧率尚未量化。
2. 当前 KT/BT/NUC-T profile 仍是可选、需真实 Family 代表设备数据验证的路径；空 profile 继续安全回退 V1+NTC+FFC dg。
3. `mean_diff` 异常的 V1 span 质量门禁和逐机 B 是否量产保留仍待产品决策；不能用动态温漂测试替代绝对温准验证。
