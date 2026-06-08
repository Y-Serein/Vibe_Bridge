# pico_tn160 温漂调试 HANDOFF

最后更新：2026-06-08 14:20 CST
当前分支：test0416
当前任务：提交当前相对稳定 checkpoint；下一步只加诊断定位冷启动温漂和运行期 -50°C 灾难掉温。

## 接手默认协议

后续接手本项目时，默认按工程控制论闭环推进：目标 -> 状态 -> 误差 -> 控制动作 -> 反馈 -> 修正 -> 验证 -> 沉淀。

- 动固件、USB descriptor、UVC payload、PIO、DMA、时序或文档前，先说明当前目标、当前系统状态、目标与现状误差、本轮控制变量、最小闭环验证方式。
- 修改后必须用真实反馈验证：构建、UVC 抓帧、Windows/Linux/guvcview 兼容性、硬件实测或用户反馈。
- 反馈与预期不一致时，先定位误差来源，再继续改。
- 禁止一次性大范围重构；优先做最小可运行闭环。
- 禁止破坏当前 full-speed Bulk UVC、160x120、YUY2、10fps 的 Windows/Linux 成功基线。
- 长任务暂停或结束时，更新本文件：当前状态、已完成内容、失败尝试、关键文件、验证结果、遗留问题和下一步动作。
- 开工前优先运行：`python3 /home/rv_nano/Sipeed/T_tools/agent_preflight.py --project pico-tn160`；相关流程见 `/home/rv_nano/Sipeed/.claude/skills/pico-tn160-uvc/SKILL.md`。
- 接手/对齐需求/先不改代码时，先用 `/home/rv_nano/Sipeed/.claude/skills/project-intake/SKILL.md`。

## 当前在做什么
当前主线是温漂/运行期 NUC 稳定性。UVC 当前保持成功基线:full-speed Bulk UVC,160x120,YUY2,10fps;Windows 已恢复,Linux/WSL `v4l2-ctl` 连续抓帧通过,guvcview 可用。不要回到 ISO descriptor/payload 猜测路线。

当前相对稳定 checkpoint 固件只允许放在 build 目录:
`/home/slam/build/pico_tn160/pico_tn160.elf.uf2`

SHA256:
`25ec59279ef4bfd7b1aba27ef68d9eb7f28fabfdbf014426129b842e13b61d3e`

项目目录内 `.uf2` 副本已按用户要求删除。之后不要再把固件放到项目根、`docs/` 或 `D_deliverables/`;本项目固件产物只放 `/home/slam/build/pico_tn160`。

用户最新反馈:
1. 当前版本整体相对稳定,需要先提交 checkpoint 再继续尝试修改。
2. 0-50s 冷启动温漂确定存在,约 1 分钟后渐进稳定;MaixCam2 接入会影响热状态/温漂表现。
3. 偶尔运行 7-8 分钟后会突然掉到 -50°C 左右,且短时间内基本不会恢复。这是独立于冷启动 warmup 的灾难级异常,优先判断是否为 anchor/calib 被污染、NTC/ADC 突跳补偿、或 NUC commit guard 漏挡。

本轮温漂控制动作:
1. 保留既有 K/NTC/warmup 参数,不继续随意调历史实测常数。
2. 明确当前 `ffc_run()` 是运行期 shutter-based offset NUC;`K_ANCHOR_COMP_STATIC == 0.0f` 时,VTEMP 不参与 anchor 补偿,也不再用 VTEMP/NTC plausibility gate 阻塞运行期 NUC。
3. 运行期 NUC 先计算候选 shutter frame/candidate anchor,通过 dG limit 和预测画面跳变 guard 后才 commit 到 `ram_calibg`/`ram_temp_anchor`;拒绝时不污染当前基线。
4. 帧尾复用旧 ADC debug 字段输出 NUC event:`decision/count/dg/dv/dn`;GUI 源码解析并显示 `NUC ACC#...`/`REJ_*` 状态。注意 dist 里的 exe 尚未重新打包,要看新状态需运行源码版 GUI 或后续重打包。

本轮已做最小 Bulk UVC 改动:
1. `src/tusb_config.h` 增加 `CFG_TUD_VIDEO_STREAMING_BULK=1`,TinyUSB video buffer / UVC max payload 设为 256。
2. `src/usb_descriptors.h` 增加 uncompressed Bulk descriptor:VS alt0 直接带 1 个 Bulk IN endpoint,不再走 ISO alt1。
3. `src/usb_descriptors.c` 在 `CFG_TUD_VIDEO_STREAMING_BULK` 时选择 Bulk descriptor 和对应总长度。
4. `src/usb_descriptors.h` 的 Bulk endpoint descriptor 固定 `wMaxPacketSize=64`,保持 full-speed Bulk 合法;256 只用于 UVC payload/设备侧 ep buffer。

当前源码保持正式热成像模式:`MAIN_FUNC_MODE_SPI_INIT_40MHZ_4`。当前正式固件已重新构建:
`/home/slam/build/pico_tn160/pico_tn160.elf.uf2`

当前固件用户已验证:Windows 能看到画面;Linux/WSL `v4l2-ctl --stream-count=5` 连续 5 个 `DQBUF`,每帧 `bytesused=38400`,文件 `/tmp/tn160.yuyv=192000` bytes;Linux 直接抓取 video 的软件显示 10fps;guvcview 现在可以正常使用。guvcview 曾只能选择 `1/1 fps`,原因是 continuous frame interval 暴露了 10fps 到 1fps 范围;现已把 max 收窄到 10fps,仍保持 continuous descriptor,没有恢复之前会破坏 Windows 的 discrete/strict commit 方案。

历史排查结论:旧 ISO 路线在 Linux/WSL 下出现短帧、只出首帧或 `STREAMON` 后无 `DQBUF`;多次调 payload/descriptor 后仍不稳定。最终不要再沿 ISO payload 猜测路线继续,当前成功基线是 Bulk UVC。注意:根目录 `/home/slam/pico-sdk` 和 `/home/slam/pico-sdk-2.2.0` 已按用户要求删除,以后只改 `tools/pico-sdk-2.2.0`。

固件位置:`/home/slam/build/pico_tn160/pico_tn160.elf.uf2`

## 已经试过的方案和结果（含失败的）

下表是历史排查记录,其中“当前/待测/待复测”等词只表示当轮上下文,不代表现在状态。现在状态以上方“当前在做什么”为准。

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
| 32 | 运行期 NUC 策略版:在 `K_ANCHOR_COMP_STATIC == 0.0f` 时不再让 VTEMP/NTC plausibility gate 阻塞 shutter NUC;NUC 先算 candidate anchor,通过 dG limit 和预测画面跳变 guard 后再 commit;帧尾输出 `decision/count/dg/dv/dn`,GUI 源码显示 `NUC ACC#...`/`REJ_*` | **当前相对稳定 checkpoint**:`/home/slam/build/pico_tn160/pico_tn160.elf.uf2`,SHA256 `25ec59279ef4bfd7b1aba27ef68d9eb7f28fabfdbf014426129b842e13b61d3e`;项目内旧 `D_deliverables/*.uf2` 副本已删除,以后固件只放 build 目录 |
| 20 | 确认 ADC2 硬件死(永远 4095),GUI 删除 NTC/ΔNTC/SAT/ADCERR/TADC/TCS/NCS/ACFG 显示 + NTC 趋势曲线 + 保存 PNG 第三轴 | **完成**:`protocol.py` 不再 unpack 帧尾 NTC/ADC 字段,`monitor_tab.py` 删除 `tax3` 轴/`_trend_ntc`/`_hist_ntc`/`_ntc_ref`。固件帧尾仍发这些字节但 GUI 忽略,不需重烧 |
| UVC-1 | Linux YUYV 仍报 `Dequeued v4l2 buffer contains corrupted data (38400 bytes)`;后续 UVC 试改导致 Windows Code 10/找不到设备 | **Windows 已出画面**:撤销 `567f761` 及后续 BULK/5fps 试改后,用户确认 Windows 已能看到设备且有画面。当前基线为旧固件 UVC 形态(ISO/30fps/EP1023/原 `fill_uvc_frame2`)。guvcview 仍未解决,不要再用破坏 Windows 的 descriptor/传输参数试改。当前最可疑点:160x120 YUY2 30fps 约 1.152MB/s,超过 full-speed ISO 1023KB/s 上限,Linux/v4l2 可能因此把 buffer 标记为 corrupted;但改帧率/包长会影响 Windows,必须先抓 Linux 协商和内核日志。 |
| UVC-2 | 用户提供 `tools/frame/frame*.raw`,反馈经常卡在第一帧 | **已做最小状态机修复,待上板验证**:6 个 raw 文件均为 38400 bytes,hash 全不同,`frame.raw -> frame2.raw` 有 99.04% 字节不同,`frame2 -> frame3` 有 79.84% 字节不同;按 YUYV 解析无全 0/全 FF/短帧。已修 `uvc_task()` 忽略 `tud_video_n_frame_xfer()` false 的问题:失败时恢复 `uvc_tx_busy=0`,`uvc_state[b]=UVCBUF_READY`,`uvc_sending=-1`;not streaming 时释放发送中 buffer;`find_ready_uvc_buf()` 增加 READY 兜底扫描。该改动不碰 descriptor/帧率/端点。 |
| UVC-3 | 用户提供 WSL `dmesg -w` 日志和 `v4l2-ctl` 输出 | **descriptor 枚举正常,stream 阶段卡住**:`docs/logs.txt` 显示 `new full-speed USB device`,随后 `Found UVC 1.10 device Thermal160 (359f:4202)` 和 `registered new interface driver uvcvideo`;`sudo timeout 5s v4l2-ctl --list-formats-ext -d /dev/video0` 可返回 `YUYV 160x120`、interval `0.033s - 1.000s`。普通用户 stream 报 Permission denied;sudo stream 10s 无输出。当前证据说明 Linux 能读 descriptor,问题在 stream 拿帧阶段;但 `--stream-count=30` + `timeout 10s` 不能排除默认 1fps 导致未等够 30 帧,下一步应强制 30fps 并把 count 降到 1/5。 |
| UVC-4 | 用户强制 stream 后能拿到帧但严重 dropped/不完整 | **已降默认 UVC 帧率到 10fps,待复测**:30fps 时收到 seq 2、116、117,中间 dropped 113。20fps 复测时驱动接受为 20fps,但 10s 只收到 1 帧,输出文件 36864 bytes=`48*768`,像是 20fps 每帧需要 50 个 768B 数据包但少了 2 包。说明 full-speed ISO/WSL USB-IP 下 20fps 仍太贴边。已把 `FRAME_RATE 20 -> 10`,并保留 `dwMaxVideoFrameBufferSize` 字节数修正。保留 ISO/EP1023/原发送格式,不改 BULK。 |
| UVC-5 | 用户在 10fps 固件上仍用 `--set-parm=30`,驱动显示 `Frame rate set to 305.773 fps`;再用 `--set-parm=10` 后 `STREAMON` 成功但 10s 内 0 字节 | **已撤回**:把 YUYV frame descriptor 从 continuous 改为 discrete 单一 10fps,并在 `tud_video_commit_cb()` 里严格拒绝非单一 interval 后,用户反馈 Windows 也识别不到描述符。已撤回 discrete descriptor/严格 commit 校验,回到 continuous frame interval;保留 `FRAME_RATE=10`、`dwMaxVideoFrameBufferSize=38400` 和 UVC 发送状态机修复。构建通过,待上板确认 Windows 枚举。 |
| UVC-6 | Windows 恢复后继续 Linux/guvcview | **已做最小节流修复,待上板验证**:`uvc_inv_ms` 原来只在 commit 回调赋值,但 `uvc_task()` 从未使用,所以实际会在上一帧传完后立刻发下一帧。现新增 `uvc_last_xfer_ms`,按 `uvc_inv_ms` 节流,并把 host commit interval 钳到 10fps~1fps 范围;不改 descriptor、不改 BULK/ISO、不严格拒绝 host commit。构建通过,有 `Clock skew detected` 警告。 |
| UVC-7 | 用户复测节流版:`STREAMON` 成功,但 10s 内仍无 `DQBUF` 输出 | **已加 fallback 测试帧,待上板验证**:如果 streaming 已开、到发送时间且没有 `UVCBUF_READY`,固件会填充一帧内置 `fill_smpte_yuy2()` 测试图并发送。若 Linux 仍 0 字节,基本排除 sensor/norm_buf/YUYV 转换没准备好,重点转向 TinyUSB ISO xfer、WSL USB/IP 或主机端 ISO 兼容性。构建通过。 |
| UVC-8 | 用户复测 fallback 版:`DQBUF seq 0 bytesused 38400`,但落盘 `/tmp/tn160.yuyv=36864` 且 10s 只出 1 帧 | **1023 方案失败后已降到 770,待上板验证**:36864=`96*384`,而 10fps 下 TinyUSB 原算法 `ceil(38400/100)+2=386`,每包数据约 384 bytes,一帧刚好需要 100 个 1ms ISO 包,没有时序余量。实际构建使用的是 `tools/pico-sdk-2.2.0`;曾改 TinyUSB 两处 negotiated `dwMaxPayloadTransferSize` 为 1023,但复测又回到 `STREAMON` 后无 `DQBUF`。当前 `src/tusb_config.h` 定义 `CFG_TUD_VIDEO_STREAMING_MAX_PAYLOAD_SIZE=770`,TinyUSB negotiation 使用该值并 clamp 到 EP 1023;项目 descriptor 不变,固件继续 10fps 节流。已删除旧 object 后重编,构建日志确认重新编译 `video_device.c.obj`,UF2 已更新。 |
| UVC-9 | 用户又测试 `/dev/video0` 和 `/dev/video1` | **只测 `/dev/video0`**:`/dev/video0` 能 `VIDIOC_G_FMT/S_FMT/S_PARM/STREAMON`,是正确 capture 节点;`/dev/video1` 的 `VIDIOC_G_FMT` 返回 `Invalid argument`,`VIDIOC_S_PARM` 返回 `Inappropriate ioctl for device`,不是这路 YUYV 采集入口,后续不要用它判断 UVC 画面。 |
| UVC-10 | 用户复测 770 档:`DQBUF seq 0 bytesused 38400`,但 `/tmp/tn160.yuyv=36864` | **已降到 642,待上板验证**:36864=`48*768`,说明 770 档每帧 50 个包里仍丢 2 个 image payload。`src/tusb_config.h` 现定义 `CFG_TUD_VIDEO_STREAMING_MAX_PAYLOAD_SIZE=642`,即 2-byte UVC header + 640 image bytes,一帧约 60 个包。构建通过,输出 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。不改 descriptor、不改 BULK/ISO、不严格拒绝 host commit。 |
| UVC-11 | 用户复测 642 档:`STREAMON` 成功但 10s 内无 `DQBUF`,文件 0 字节 | **停止 payload 猜测,改 TinyUSB 调度**:`dwMaxPayloadTransferSize` 只影响 UVC commit,endpoint descriptor 仍是 FS ISO 1023,继续降 payload 不能根治。代码查到 TinyUSB 后续分包依赖 `tud_task()` 驱动 `videod_xfer_cb()`,而主循环会执行 `fill_txbuf()`/FFC/`__wfe()`。当前 `src/main.c` 改为 `tud_video_n_frame_xfer()` 成功后同步泵 `tud_task()` 到整帧完成或超时;`src/tusb_config.h` 回到 770。构建通过,输出 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。待上板验证。 |
| UVC-12 | 用户提供 `docs/logs.txt` | **主机日志没有 UVC 协议错误**:最新片段只看到 `Found UVC 1.10 device Thermal160 (359f:4202)` 后多组 `vhci_hcd: unlink->seqnum ...` 和 `urb->status -104`。这些出现在 `timeout/v4l2-ctl` 结束取消 URB 时,不是 `uvcvideo` 报 payload/header/descriptor 非法。当前证据更支持“WSL vhci/USB-IP 没拿到完整 ISO 帧或设备侧没连续推出分包”,不支持继续改 descriptor 或降 payload。 |
| UVC-13 | 同步泵版本复测仍卡在 `STREAMON` 后无 `DQBUF` | **已生成纯 UVC/SMPTE 诊断固件**:`src/sdk.h` 临时切到 `MAIN_FUNC_MODE_UVC_MAX`,不跑 sensor/FFC/双核缓冲/温度算法;`uvc_max_func()` 增加 10fps 节流和 `tud_video_n_frame_xfer()` 失败恢复。构建通过,当前 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2` 是诊断 UF2。若这个固件仍无 `DQBUF`,问题就在 TinyUSB/RP2350 FS ISO/WSL USB-IP 这一层;若能出帧,再回头查 sensor 主模式调度。注意测完要把 `MAIN_FUNC_MODE` 切回 `MAIN_FUNC_MODE_SPI_INIT_40MHZ_4`。 |
| UVC-14 | 继续查为什么 payload 改小无效 | **找到前面试法的错误点**:之前只改 `dwMaxPayloadTransferSize`,但 `TUD_VIDEO_DESC_EP_ISO()` 里的 endpoint `wMaxPacketSize` 仍来自 `CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE=1023`,主机/WSL/vhci 仍按满速 ISO 最大包调度。现把 `CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE` 也改为 770,使 endpoint descriptor 和 UVC commit payload 一致。当前 UF2 为 `MAIN_FUNC_MODE_UVC_MAX` 纯 SMPTE + FS ISO endpoint 770 + payload 770。构建通过,输出 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。置信度:中。 |
| UVC-15 | 用户反馈 Windows UVC 一直闪 RGB | **已恢复正式固件**:RGB 闪烁是因为上一轮 UF2 临时切到 `MAIN_FUNC_MODE_UVC_MAX` 纯 SMPTE 诊断模式,不是正式热成像固件。已把 `src/sdk.h` 恢复为 `MAIN_FUNC_MODE_SPI_INIT_40MHZ_4`,并把 `CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE` 恢复 1023,重新构建 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。当前 UF2 是正式热成像模式,不是诊断 RGB/SMPTE 模式。 |
| UVC-16 | 用户允许继续测试,要求明确说明测试固件 | **已分离测试/正式 UF2**:测试固件统一为 `/home/slam/build/pico_tn160/pico_tn160_UVC_TEST_SMPTE_EP770.uf2`,配置为 `MAIN_FUNC_MODE_UVC_MAX` 纯 SMPTE + endpoint 770 + payload 770,只用于验证 WSL/Linux UVC,Windows 会显示 RGB/SMPTE 测试图。默认正式固件仍是 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`,源码已恢复 `MAIN_FUNC_MODE_SPI_INIT_40MHZ_4` 和 endpoint 1023。以后测试/正式 UF2 都放 `/home/slam/build/pico_tn160/`,不再让用户去 `D_deliverables/` 找。 |
| UVC-17 | 测试固件 endpoint 770 能 `DQBUF bytesused=38400`,但 10s 只出 1 帧 | **已更新唯一测试固件**:第一帧完整说明 descriptor/YUYV/header/EOF 基本合法;后续无帧不是应用侧 `uvc_tx_busy` 门控,因为直接每 100ms 调 `tud_video_n_frame_xfer()` 仍只出 1 帧。当前判断: TinyUSB video 类内部可能出现 `stm->offset >= stm->bufsize` 但 `stm->buffer` 未清的 stale completed frame,导致后续 `tud_video_n_frame_xfer()` 被 `stm->buffer` 拦截。已在 `tools/pico-sdk-2.2.0/lib/tinyusb/src/class/video/video_device.c` 给 `tud_video_n_frame_xfer()` 增加 stale completed frame cleanup。唯一测试文件已更新为 `/home/slam/build/pico_tn160/pico_tn160_UVC_TEST_SMPTE_EP770.uf2`。 |
| UVC-18 | 用户明确要求不要沿着历史失败方案继续,目标是在 Linux 环境实现 UVC 功能 | **成功**:已切 Linux/WSL 优先 Bulk UVC。第一版 payload/EP 都设为 64 时,用户复测 `VIDIOC_STREAMON returned -1 (Cannot allocate memory)`。按 TinyUSB video bulk example 修正为 UVC max payload / TinyUSB epbuf=256,但 Bulk endpoint descriptor `wMaxPacketSize=64` 后,用户 WSL 复测通过:5 个连续 `DQBUF seq 0..4 bytesused=38400`,间隔约 93ms,`/tmp/tn160.yuyv=192000` bytes。用户随后确认 Windows 也能看到,Linux 直接抓取 video 的软件显示 10fps;`guvcview` 能持续出画面但像 1fps 慢刷。当前成功固件 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。 |
| UVC-19 | guvcview 只能选择 `1/1 fps` | **成功**:当前 frame descriptor 使用 continuous interval,之前参数是 default=10fps,min=10fps,max=1fps,step=10fps;guvcview 只显示/选择 `1/1 fps` 符合它取了 max interval。已把 ISO 和 Bulk 两处 YUYV frame interval max 都改为 10fps,形成 min=max=default=10fps,但仍保持 continuous descriptor,不恢复 discrete descriptor 或严格 commit 校验。构建通过,输出 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`;用户复测确认现在可以。 |
| SDK-clean | 用户发现 `/home/slam` 根目录有多个 pico-sdk 副本 | **已清理**:`/home/slam/pico-sdk` 和 `/home/slam/pico-sdk-2.2.0` 已删除。当前 `pico_tn160` 实际 SDK 为 `tools/pico-sdk-2.2.0`,构建缓存 `PICO_SDK_PATH=/home/slam/Sipeed/pico_tn160/tools/pico-sdk-2.2.0` 已确认。旧 `th160_release260414A1` 的历史 build cache 曾引用 `/home/slam/pico-sdk-2.2.0`;若以后要重构建旧工程,需重新设置 `PICO_SDK_PATH`。 |

## 下一步计划（3-5条actionable)

1. 先提交当前 checkpoint:主固件仓库和 `tools/pico_th160_thermocam_gui/` GUI 仓库分别提交,作为后续诊断修改的回滚点。
2. 做一版只加诊断、不改变控制行为的固件/GUI:暴露 `boot_ntc`、`boot_delta`、`warmup_curve_comp`、`normal_ntc_comp`、`total_comp`、`eff_anchor`、`ram_temp_anchor`、NUC `decision/count/dg/dv/dn`。
3. 用诊断版做 3-5 次 0-90s 冷启动对照,至少覆盖一次有漂和一次不漂;判断 `boot_ntc/boot_delta/warmup_comp` 是否随机导致 0-50s 温漂差异。
4. 长跑 10-15 分钟复现 -50°C 灾难掉温;掉温时重点看是否刚发生 `NUC ACC#...`、`ram_temp_anchor/eff_anchor` 是否突变、`NTC/ΔNTC` 是否突跳、`dg` 是否异常。
5. 不要修改 UVC descriptor/payload/commit,不要把测试模式 `MAIN_FUNC_MODE_UVC_MAX` 留在正式源码里;下一轮只根据诊断反馈选择收紧 NUC guard、稳定 `boot_ntc`,或限制 NTC 补偿异常输入。

## 关键文件路径（相对路径，一行一个）

```
src/main.c
src/sdk.h
src/tusb_config.h
src/usb_descriptors.h
src/usb_descriptors.c
tools/pico-sdk-2.2.0/lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
tools/pico-sdk-2.2.0/lib/tinyusb/src/class/video/video_device.c
tools/frame/
build.sh
tools/pico_th160_thermocam_gui/dist/
tools/pico_th160_thermocam_gui/monitor_tab.py
tools/pico_th160_thermocam_gui/protocol.py
docs/bin/raw16.py
docs/logs.txt
```

main.c 关键定位锚:
- `K_ANCHOR_COMP_STATIC` / `K_NTC_COMP_STATIC` / `K_COMP_MAX_ADU` 定义
- `binary_calib_frame()` 内 `eff_anchor =` 的 K 补偿块
- `binary_calib_frame()` 诊断尾部:`PX_CNT+19..20 = ffc_last_ntc`,`PX_CNT+21..22 = cur_ntc`,`PX_CNT+23..30 = NUC decision/count/dg/dv/dn`
- `ffc_run()` 内 `candidate_temp_anchor`、jump guard、`ffc_record_event()`
- `ffc_should_trigger()` 内 FFC 调度(30s + 150 ADU dV + 2s min lockout)
- FFC sanity check(`FFC_REJECT_DG_ADU=2500` / `FFC_REJECT_DV_ADU=1000`)

构建:`bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5` → 输出 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`

## 当前验证状态

- 已确认当前正式固件产物存在:`/home/slam/build/pico_tn160/pico_tn160.elf.uf2`
- 已确认 SHA256:`25ec59279ef4bfd7b1aba27ef68d9eb7f28fabfdbf014426129b842e13b61d3e`
- 已清理项目目录内 `.uf2` 副本;项目根、`docs/`、`D_deliverables/` 下不再保留固件副本。
- 已确认源码模式仍是 `MAIN_FUNC_MODE_SPI_INIT_40MHZ_4`;UVC 仍是 Bulk 10fps 基线。
- 本轮尚未重新构建;当前目标是先提交现有相对稳定 checkpoint,再做诊断修改。
- 未上板验证新诊断项;后续需要用户烧录 build 目录固件并用源码版 GUI 记录冷启动和长跑 trend。

## 还没搞清楚的问题

1. ~~**NTC ADC2 仍读 4095**~~ —— **已解决**:RP2350 AVDD 引脚漏接电源,补焊即恢复正常。
2. **0-50s 冷启动温漂的随机性来源** —— 真实冷启动热漂确定存在;当前优先怀疑 `boot_ntc`/早期 NTC EMA/warmup curve 起点随机,也可能是 MaixCam2 改变热路径导致 NTC 不能代表 shutter/lens。
3. **7-8 分钟后偶发 -50°C 且短时间不恢复** —— warmup curve 已在 320s 后退出,所以这不是冷启动补偿本身;优先怀疑某次运行期 NUC commit 污染 `ram_temp_anchor`/`ram_calibg`,或 NTC/ADC 有效范围内突跳导致补偿异常。
4. **掉温是否和 `NUC ACC#...` 同步** —— 需要诊断版记录掉温瞬间的 `decision/count/dg/dv/dn`、`ram_temp_anchor`、`eff_anchor`、`NTC/ΔNTC`。
5. **ram_mean_diff 实际值和标定环境** —— K 系数和温度跳变量级依赖 `mean_diff`;若 flash 低温参考 ambient 与当前 ambient 不一致,会产生常量偏置,属于标定问题不是运行期算法问题。
6. **热源/结构影响** —— MaixCam2 接入会影响温漂,需要区分供电/热源/气流/机械安装导致的真实热漂和固件补偿误差。

## 用户偏好备注
- 中文回复(`feedback_language_chinese.md` 已记忆)
- 喜欢"删代码"胜过"加代码"。任何新机制要给出物理依据
- 喜欢一次出结果,不喜欢来回反复。给方案前先想清楚失效模式
- 编译指令必须写 `bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5`(已记忆)
