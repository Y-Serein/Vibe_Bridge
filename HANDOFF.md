# pico_tn160 温漂调试 HANDOFF

## 当前在做什么
解决 TN160 热成像 scene 温度持续漂移问题。最新状态:**ADC2 4095 根因找到——板上 RP2350 AVDD(QFN-60 第 43 脚)漏接电源,补焊后 NCS=0x2101 / TCS=0x4103,ERR 位全清,NTC ADC2 实测 ~1843,TADC ~866,数值合理。前 7~19 轮 K_NTC 调参基于"ADC 4095 等于 NTC 死值"的假数据,所以补偿等于 0,trend 表现完全反映静态 anchor 行为而不是 K_NTC 的真实效果**。固件已恢复 DMA 路径并移除诊断 println,K_NTC_COMP_STATIC=-0.20 启用,等用户在 AVDD 修好后重做对照 trend。

本轮另处理 UVC/YUYV Linux 识别问题:`ffmpeg/v4l2` 已能 dequeue `38400 bytes`,但报 `Dequeued v4l2 buffer contains corrupted data`。判断重点不是像素内容,而是 Linux uvcvideo 给 buffer 打了错误标记。已删除 UVC 每帧 submit/complete 日志和首帧 dump,避免 isochronous 热路径被调试输出拖慢;UVC descriptor 改回 TinyUSB 官方 continuous frame descriptor 形态,固定 160x120 YUY2 10fps;ISO endpoint packet size 收紧到 386,与 TinyUSB 对 `38400 bytes / 100ms + 2-byte UVC header` 的协商值一致。已编译通过,未上板验证 Linux 端。

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
| 8 | K_NTC_STATIC=-0.20→-0.35 | **已撤回**:trend 165340 比第7轮更差 |
| 9 | MIN_THERMAL_RANGE 384→192 | **已撤回**:用户确认横纹太严重,该参数必须保持 384 |
| 10 | 384 不动,低温差时显示层行去条纹 + capped detail boost | **待测**:提高低温差清晰度但避免横纹放大 |
| 11 | 串口诊断尾部输出 ffc_last_ntc + cur_ntc,GUI 显示 NTC/ΔNTC/NTC 曲线 | **待测** |
| 12 | 误按旧文档把 NTC 改成 GPIO26/ADC0 | **已撤回**:用户确认当前硬件 ADC2 才是 NTC,ADC0 已弃用 |
| 13 | 恢复 NTC=GPIO28/ADC2,ADC 满量程时只输出不参与 K_NTC | **待测**:若仍 4095,优先查 ADC2 硬件网络/NTC 分压 |
| 14 | 帧尾扩展输出 NTC 内部下拉/上拉自检 `PD/PU` | **待测**:`PD` 能拉低=开路/高阻;`PD` 仍 4095=强上拉/短到高电平/ADC 异常 |
| 15 | 帧尾继续扩展输出 ADC 内部温度通道 `TADC` 和 NTC 转换后 `NCS` | **待测**:GPIO28 0.9V 仍读 4095,用 TADC/NCS 判断 ADC 本体/参考/ADC2 转换错误 |
| 16 | 参考 `docs/bin/th160` 旧 ADC0 工程,把当前 ADC2 改为 FIFO+DMA 批量采样 | **失败/定位有效**:用户实测 `NTC=4095,TADC=4095,NCS=2301`;旧工程采样路径迁移后仍报 ADC 当前转换错误 |
| 17 | 移除内部 PD/PU 拉电阻诊断,GUI 按 `NCS` ERR 位优先显示 `ADCERR`,FIFO 配置改回旧工程一致的 `err_in_fifo=false` | **仍失败**:用户实测 `NTC ADC2 4095 SAT ADCERR PD 0 PU 0 TADC 4095 NCS 2301`;确认最新 NTC 转换仍当前 `ERR` |
| 18 | 帧尾不加长,把旧 `PD/PU` 字段复用为 `TCS/ACFG`;GUI 显示 `TADC/TCS/NCS/ACFG` | **确认失败但定位有效**:用户实测 `ACFG B26 N5 T4` 固定,`NCS=2301`(AIN2/ERR/READY/EN),`TCS=4703`(AIN4/ERR/ERR_STICKY/READY/TS_EN/EN),`NTC=4095,TADC=4095`;基本排除协议错位、通道号、封装配置和 GPIO28 分压本身 |
| 19 | 改 `docs/bin/raw16.py` 适配当前通信协议 | **完成**:默认 `--mode diag` 发送 `0x11` 读取 `CALI_U8` 的 `NTC/TADC/TCS/NCS/ACFG`;`--mode raw16` 发送 `0xAA` 读取原始图像和 `VTEMP`,不再把 RAW16 尾部误当 NTC ADC |
| 20 | 强制 `ntc_adc_dma_ready=false` 走单点 `adc_read()` + boot 打印 `clk_adc/cs` 诊断 | **定位成功**:单点路径 NCS/TCS 与 DMA 路径完全相同,排除 DMA/DREQ 时序。提示用户测板上 AVDD,**用户确认 RP2350 AVDD 漏接电源**——这就是前 7~19 轮 K_NTC 看不出效果的根因 |
| 21 | AVDD 补焊后实测 `NTC=1843, TADC=866, NCS=0x2101, TCS=0x4103, ERR 全清` | **修复**:固件已恢复 DMA 路径,移除诊断 println,等用户重做 K_NTC 对照 trend |
| 22 | trend_20260428_103834 实测:NTC 1680→1620 同向跌、scene 42→17 同向跌,K=-0.20 实际加剧下跌(eff_anchor↑→输出↓),按既定规则翻号 `K_NTC_COMP_STATIC -0.20f → +0.20f`,幅度暂不变 | **方案 A 已测**:trend_20260428_104600 显示翻号后 scene 仍跌 24°C(基本不变),量级 0.20 太小(ΔNTC≈10 ADU 时 comp 只 2 ADU,需要 ~150 ADU 才能抵消) |
| 23 | `K_NTC_COMP_STATIC +0.20f → +5.0f`,K_COMP_MAX_ADU=1500 钳位不变 | **方向确认正确**:trend_20260428_105355 显示 t=0~190 scene 跌幅从 24°C 降到 10°C(~58% 抵消),单个 FFC 间隔斜率明显变缓。但 t=190 出现热冲击(FPA 100→130°C),之后 FFC 死循环每 2s 拒绝一次("一直快门"),需要修保护 |
| 24 | (a) `K_NTC_COMP_STATIC +5.0f → +10.0f` 上拉幅度求全抵消 (b) FFC sanity reject 加连续计数 `ffc_consec_reject`,连拒 `FFC_FORCE_ACCEPT_AFTER=3` 次后强制 commit 一次,打破死循环 | **(a) 不显著**:trend_20260428_110856 K=10 漂仍约 19°C/190s,残漂/ΔNTC 比 K=5 时未减少(可能是 bolometer settling 不可补偿)。**(b) 修复成功**:t=190 热冲击后 FFC 自动接受新基准,200s 后回到 25-30s 正常节奏 |
| 25 | GUI 优化:`protocol.py` 加 `ntc_adu_to_celsius()`(10K 上拉,β=3950→3435 用户校验,R0=10K@25°C);移除帧尾 `adc_temp/adc_temp_cs/adc_cfg/adc_cs/ntc_pd/ntc_pu` 解析。`monitor_tab.py` NTC 趋势轴和保存 PNG 改 °C,诊断标签精简为 `VTEMP/ΔVTEMP/anchor/smooth/mean_diff/NTC °C/ΔNTC °C` | **完成**:用户实测 NTC 温度值正确 |
| 26 | trend_20260428_113022 显示 K_NTC=+10 时 inter-FFC 锯齿/抖动比 K=+5 时大(过补 + NTC 抖动放大),冷启动 t=0~30s 仍跌 8°C 主因是 K_V=0.20 跟不上 FPA 瞬变 | **诊断**:两个独立问题——稳态过补 + 冷启动欠补 |
| 27 | (a) `K_ANCHOR_COMP_STATIC 0.20f → 0.50f` 提升 K_V 跟踪 FPA 瞬变,不上 1.0 避免放大 FPA 自身抖 (b) `K_NTC_COMP_STATIC +10.0f → +7.0f` 适度回调找 +5/+10 之间甜点 | **整体好转但 scene 飘**:trend_20260428_114856 显示 K_V=0.5 把 FPA 帧间 ~70 ADU 抖动放大成 ~1.4°C scene 抖,NTC 蓝线已经 EMA 平滑过没问题 |
| 28 | 给 VTEMP 加 EMA 平滑(α=1/32, τ≈3s @ 10fps),K_V comp 用 `smooth_vtemp` 而不是 `cur_vtemp`;`ffc_last_fpa_vtemp` 在 commit 时也存平滑值保持 K_V 计算一致(smooth−smooth);FFC 触发/sanity 仍用 raw VTEMP 不影响响应速度 | **平滑生效但暴露新问题**:trend_20260428_120013 t=0~190 段 scene 平稳但**绝对值偏低 ~2°C**(标定问题不是算法);trend_20260428_120531 t=220 处仍发生灾难——FPA 飞到 128°C 期间 FFC 被强制接受,K_V·dV 超大值烤进 ram_temp_anchor,scene 永久偏移 -8°C |
| 29 | (a) 加 `FFC_VTEMP_SETTLED_ADU=200` 闸门:`ffc_should_trigger` 在 `ffc_count >= 4`(过 warmup)时检查 `\|cur_vtemp − smooth_vtemp\|`,大于阈值直接 defer FFC,避免热瞬变期间 commit 烤垃圾 (b) `K_NTC_COMP_STATIC +7.0f → +8.0f` 微调减小 inter-FFC 残漂 | **(a) 部分有效但灾难仍发生**:trend_20260428_140252 t=270 处 FPA 飞到 130°C,scene 永久偏移 -10°C。Gate 阻止了 FFC commit 期间烤数据但**没阻止 per-frame K_V·dV 推动 anchor**——smooth 慢慢追上 raw 的过程中 dv_pre 累积变大。**(b) 反而更不稳**:K_NTC=+8 对 inter-FFC 没显著好处,撤回 |
| 30 | (a) 撤回 K_NTC `+8.0f → +7.0f` (b) 加 `DV_CLAMP_ADU=400` 在所有 K_V·dV 计算前对 `dv` 做对称钳位:binary_calib_frame 的 comp、ffc_run 的 comp_pre 和 comp_acc 三处 | **部分根因修复**:trend_20260428_141806 t=180 仍发生灾难,scene 永久偏移 -8°C(被钳位限到 K_V·400=200 ADU);**用户关键定位**:手放镜头前 FPA 不变 → FPA 测的是 ROIC 芯片自身,t=180 是 VTEMP 寄存器 spontaneous glitch,**不是真实热事件**;模型边界外 K_V·dV 不该应用 |
| 31 | NTC plausibility 交叉验证:`DV_DN_RATIO_MAX=10` 用 `\|dN\|·10` 钳住 `\|dV\|`,放在三处 K_V·dv 计算前(early in the chain,DV_CLAMP=400 仍兜底)。NTC 是物理传感器(EMA+R-T 公式)不会 glitch,真实热漂时 dN/dV 同向且 dV/dN ratio 有界,VTEMP glitch 时 dN≈0 → dv 被钳到 0 → K_V comp=0 完全免疫 | **定稿候选**:期望 t=180 类 VTEMP glitch 完全不影响 scene;正常缓漂(dn=20 ADU,dv 上限 200)K_V 仍正常工作 |
| 20 | 确认 ADC2 硬件死(永远 4095),GUI 删除 NTC/ΔNTC/SAT/ADCERR/TADC/TCS/NCS/ACFG 显示 + NTC 趋势曲线 + 保存 PNG 第三轴 | **完成**:`protocol.py` 不再 unpack 帧尾 NTC/ADC 字段,`monitor_tab.py` 删除 `tax3` 轴/`_trend_ntc`/`_hist_ntc`/`_ntc_ref`。固件帧尾仍发这些字节但 GUI 忽略,不需重烧 |
| UVC-1 | Linux YUYV 仍报 `Dequeued v4l2 buffer contains corrupted data (38400 bytes)` | **已改待实测**:删除 UVC 热路径 printf;YUY2 descriptor 改回 continuous frame descriptor,固定 10fps;ISO EP size=386 匹配 TinyUSB 协商 payload。`bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 \| tail -5` 编译通过 |

## 下一步计划

1. **看实测结果** `ls -lt tools/pico_th160_thermocam_gui/dist/*.png | head -1`,Read 最新 PNG。如 scene 残漂 < 5°C/3min 视为成功
2. **K_NTC 调整**:若漂移变本加厉 → 翻号 `+0.20f`;若仍同向漂(变小但还在) → 等比例放大到 `-0.30/-0.40`;若反向 → 减半到 `-0.10`(改 `K_NTC_COMP_STATIC` 一行)
3. **看 println 日志**`FFC anchor+K: ... ntc=XXXX` 反推真实 dNTC 数量级,精确算 K_NTC
4. **若 NTC 完全无相关性**:漂移可能来自 bolometer membrane settling 或 ROIC 参考漂移,这两者无法用现有传感器补偿。退路:在目标 ambient 下重做 cali_ref(用户操作),让 `flash_imgbg` 反映正确工作点
5. **不要再加新算法层**:用户多次明确"删,不要加"。任何新机制必须有可观测的物理对应物且能通过单 `#define` 调控
6. **成像清晰度**:`MIN_THERMAL_RANGE` 必须保持 `384`;若仍糊,调 `DISPLAY_DETAIL_MAX_ADU`;若横纹仍明显,调低 `DISPLAY_ROW_STRIPE_MAX_ADU` 或关闭 detail boost
7. **NTC 通道**:当前硬件 NTC 必须读 `GPIO28/ADC2`;`GPIO26/ADC0` 是历史版本/已弃用。以后改硬件定义前必须先和用户确认最新版
8. **NTC 4095 诊断**:已确认 `GPIO28≈0.9V`,`ACFG B26 N5 T4`,但 `NTC=4095,TADC=4095,NCS=2301,TCS=4703`。`NCS=2301`=AIN2/READY/ERR/EN;`TCS=4703`=AIN4/ERR/ERR_STICKY/READY/TS_EN/EN。外部 ADC2 和内部温度通道都当前转换报错,优先查 ADC_AVDD/3V3_ADC/AGND/参考/芯片 ADC 本体,而不是继续改平均/滤波算法
9. **旧工程参考**:`docs/bin/th160/src/main.c` 旧 ADC0 是 `gpio_deinit(IO_NTC)` 后 `adc_init/adc_gpio_init/adc_select_input(0)`,再用 `adc_fifo_setup(true,true,1,false,false)` + DMA `DREQ_ADC` 采 `1280` 点并 trimmed mean。当前已迁移到 ADC2,采样点数为 `NTC_ADC_SAMPLES=256`
10. **UVC Linux 复测**:重新烧录 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`,拔插 USB 后用 `v4l2-ctl --list-formats-ext` 确认 `YUYV 160x120 10fps`,再用 ffmpeg/mpv 读取。若仍 corrupted,下一步抓 `dmesg -w` 的 uvcvideo 错误和 `v4l2-ctl --stream-mmap --stream-count=30 --stream-to=/tmp/tn160.yuyv`

## 关键文件路径(相对 `/home/slam/Sipeed/pico_tn160/`)

```
src/main.c
src/sdk.h
src/tusb_config.h
src/usb_descriptors.h
src/usb_descriptors.c
build.sh
tools/pico_th160_thermocam_gui/dist/
tools/pico_th160_thermocam_gui/monitor_tab.py
tools/pico_th160_thermocam_gui/protocol.py
docs/bin/raw16.py
```

main.c 关键定位锚:
- `K_ANCHOR_COMP_STATIC` / `K_NTC_COMP_STATIC` / `K_COMP_MAX_ADU` 定义
- `binary_calib_frame()` 内 `eff_anchor =` 的 K 补偿块
- `binary_calib_frame()` 诊断尾部:`PX_CNT+19..20 = ffc_last_ntc`,`PX_CNT+21..22 = cur_ntc`,`PX_CNT+23..26 = TCS/ACFG`,`PX_CNT+27..30 = TADC/NCS`
- `ffc_run()` 内 `eff_anchor_old` 计算 + `comp_acc` 吸收 + `ffc_last_ntc` 更新
- `ffc_should_trigger()` 内 FFC 调度(30s + 150 ADU dV + 2s min lockout)
- FFC sanity check(`FFC_REJECT_DG_ADU=2500` / `FFC_REJECT_DV_ADU=1000`)

构建:`bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5` → 输出 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`

## 还没搞清楚的问题

1. ~~**NTC ADC2 仍读 4095**~~ —— **已解决**:RP2350 AVDD 引脚漏接电源,补焊即恢复正常
2. **NTC 电压分压拓扑未知** —— 已确认当前硬件 NTC 通道是 `GPIO28/ADC2`;上拉 vs 下拉 仍决定 K_NTC 符号。**只能靠实测翻号**(现在 NTC 真实可用,可以做对照实验)
3. **ram_mean_diff 实际值未知** —— K 系数推导依赖此值,用户 GUI 诊断标签里能看到 `mean_diff XXX.X`,可让用户告知
4. **是否真是 shutter 漂移** —— 50s 时间常数的指数衰减可能是 (a) shutter blade、(b) lens housing、(c) bolometer membrane settling。NTC 只能补偿 (a)(b);若是 (c) 必须改硬件或重 cal
5. **a9c7591 当初是不是真的不漂** —— 用户记忆中那版"还行",但数学分析表明它有同样的漂移源。可能是当时测试时长不够,或 ambient 条件巧合接近 cal 时
6. **flash_imgbg 是否在合适 ambient 下采集** —— 若出厂 cal 时 ambient ≠ 当前 ambient,会有不可消除的常量偏置(~md/50.4·ΔT_amb 度)。属于标定问题不是固件问题

## 用户偏好备注
- 中文回复(`feedback_language_chinese.md` 已记忆)
- 喜欢"删代码"胜过"加代码"。任何新机制要给出物理依据
- 喜欢一次出结果,不喜欢来回反复。给方案前先想清楚失效模式
- 编译指令必须写 `bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5`(已记忆)
