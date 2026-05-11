## 2026-05-11｜guvcview UVC 兼容性判断

### 原始问题
Windows已经出画面了，现在就是guvcview，这个功能的实现应该不难吧，只是一些设备描述符，发送格式之类的吧，

### 答案核心
- 方向对，但不能说“只是改描述符”这么简单；UVC 需要 descriptor、probe/commit 协商、payload header、FID/EOF、端点包长、实际发送节奏同时自洽。
- 当前 Windows 已出画面，说明旧 UVC 基线可用，后续不能用会破坏 Windows 的 BULK/5fps/descriptor 试改。
- 当前最可疑点是 full-speed ISO 带宽：160x120 YUY2 30fps 约 1.152MB/s，超过 1023KB/s 上限；Linux/v4l2 可能比 Windows 更严格，因此把 buffer 标记为 corrupted。
- 下一步应先抓 Linux 侧证据：`dmesg -w`、`v4l2-ctl --list-formats-ext`、`v4l2-ctl --stream-mmap --stream-count=30 --stream-to=/tmp/tn160.yuyv`。
- 修复原则是最小改动并保持 Windows 可出画面，优先考虑合法带宽/帧率协商和 TinyUSB payload 分片一致性。

### 相关引用
- `src/usb_descriptors.h`
- `src/usb_descriptors.c`
- `src/tusb_config.h`
- `src/main.c`
- `tools/pico-sdk-2.2.0/lib/tinyusb/src/class/video/video_device.c`

### 备注
- 未上 Linux 主机复测；guvcview 根因置信度：中。
