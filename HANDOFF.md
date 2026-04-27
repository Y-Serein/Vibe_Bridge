# pico_tn160 温漂调试 HANDOFF

## 当前在做什么
解决 TN160 热成像 scene 温度持续漂移问题。最后一轮(第7轮)改动:**启用沉睡的板载 NTC 通道作为 shutter 温度代理,加入 K_NTC 补偿**。固件已编译完成,**等待用户烧录测试反馈**。

固件位置:`/home/slam/build/pico_tn160/pico_tn160.elf.uf2`
最近一次 trend(待复测对照):`tools/pico_th160_thermocam_gui/dist/trend_20260427_143902.png`(t=0~130s FPA 稳定 130°C,scene 漂 48→17°C)

## 已经试过的方案和结果(含失败的)

| 轮次 | 方案 | 结果 |
|---|---|---|
| 1 | FFC 间隔 120s→30s + MIN_ELAPSED 2s | scene 漂移更剧烈,绿色尖刺密集 |
| 2 | smoother shift bug 修复(eff_anchor_old)+ K 钳位 ±2→±10 + EMA 偏向 history | **灾难**:VTEMP glitch 被放大,scene 崩到 -55°C |
| 3 | FFC 健全性校验(dG=2500/dV=1000)+ K·ΔV cap=1500 + K 钳位 ±5 | 仍崩,且仍漂 |
| 4 | **完全删除** runtime K-fit + eff_anchor 补偿(回归 a9c7591 思路) | 暴露干净的线性温漂(无崩溃) |
| 5 | anchor = g_calib_mean(以 shutter 为基准) | **失败**:挡片 100°C 当温度零点,ambient 26°C 读出 -29°C |
| 6 | 恢复 gain-weighted anchor + smoother shift,加静态 K_VTEMP=0.20 | trend 143902 显示 FPA 稳定时仍漂(K·ΔV=0 无效) |
| 7 | + K_NTC_STATIC=-0.20(NTC 作 shutter 代理) | **待测** |

## 下一步计划

1. **看实测结果** `ls -lt tools/pico_th160_thermocam_gui/dist/*.png | head -1`,Read 最新 PNG。如 scene 残漂 < 5°C/3min 视为成功
2. **K_NTC 调整**:若漂移变本加厉 → 翻号 `+0.20f`;若仍同向漂(变小但还在) → 等比例放大到 `-0.30/-0.40`;若反向 → 减半到 `-0.10`(改 `K_NTC_COMP_STATIC` 一行)
3. **看 println 日志**`FFC anchor+K: ... ntc=XXXX` 反推真实 dNTC 数量级,精确算 K_NTC
4. **若 NTC 完全无相关性**:漂移可能来自 bolometer membrane settling 或 ROIC 参考漂移,这两者无法用现有传感器补偿。退路:在目标 ambient 下重做 cali_ref(用户操作),让 `flash_imgbg` 反映正确工作点
5. **不要再加新算法层**:用户多次明确"删,不要加"。任何新机制必须有可观测的物理对应物且能通过单 `#define` 调控

## 关键文件路径(相对 `/home/slam/Sipeed/pico_tn160/`)

```
src/main.c
src/sdk.h
build.sh
tools/pico_th160_thermocam_gui/dist/
tools/pico_th160_thermocam_gui/monitor_tab.py
tools/pico_th160_thermocam_gui/protocol.py
```

main.c 关键定位锚:
- `K_ANCHOR_COMP_STATIC` / `K_NTC_COMP_STATIC` / `K_COMP_MAX_ADU` 定义
- `binary_calib_frame()` 内 `eff_anchor =` 的 K 补偿块
- `ffc_run()` 内 `eff_anchor_old` 计算 + `comp_acc` 吸收 + `ffc_last_ntc` 更新
- `ffc_should_trigger()` 内 FFC 调度(30s + 150 ADU dV + 2s min lockout)
- FFC sanity check(`FFC_REJECT_DG_ADU=2500` / `FFC_REJECT_DV_ADU=1000`)

构建:`bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5` → 输出 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`

## 还没搞清楚的问题

1. **NTC 电压分压拓扑未知** —— 上拉 vs 下拉 决定 K_NTC 符号。代码中只有 `adc_read()`,无电路注释。**只能靠实测翻号**
2. **ram_mean_diff 实际值未知** —— K 系数推导依赖此值,用户 GUI 诊断标签里能看到 `mean_diff XXX.X`,可让用户告知
3. **是否真是 shutter 漂移** —— 50s 时间常数的指数衰减可能是 (a) shutter blade、(b) lens housing、(c) bolometer membrane settling。NTC 只能补偿 (a)(b);若是 (c) 必须改硬件或重 cal
4. **a9c7591 当初是不是真的不漂** —— 用户记忆中那版"还行",但数学分析表明它有同样的漂移源。可能是当时测试时长不够,或 ambient 条件巧合接近 cal 时
5. **flash_imgbg 是否在合适 ambient 下采集** —— 若出厂 cal 时 ambient ≠ 当前 ambient,会有不可消除的常量偏置(~md/50.4·ΔT_amb 度)。属于标定问题不是固件问题

## 用户偏好备注
- 中文回复(`feedback_language_chinese.md` 已记忆)
- 喜欢"删代码"胜过"加代码"。任何新机制要给出物理依据
- 喜欢一次出结果,不喜欢来回反复。给方案前先想清楚失效模式
- 编译指令必须写 `bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5`(已记忆)
