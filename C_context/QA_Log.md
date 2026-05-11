## 2026-05-11｜v4l2/guvcview 排查命令作用

### 原始问题
v4l2-ctl --list-formats-ext -d /dev/videoX
      - v4l2-ctl --stream-mmap --stream-count=30 -d /dev/videoX
      - 同时看 dmesg -w这三个指令功能

### 答案核心
- `v4l2-ctl --list-formats-ext -d /dev/videoX` 查看该 video 节点向 Linux/V4L2 声明的格式、分辨率、帧率,用于确认 descriptor 协商结果是否符合预期。
- `v4l2-ctl --stream-mmap --stream-count=30 -d /dev/videoX` 不经过 guvcview,直接用 V4L2 mmap 拉 30 帧,用于区分问题在 UVC/驱动层还是 guvcview UI/转换层。
- `dmesg -w` 实时看内核日志,用于捕捉 UVC 驱动报错、带宽不足、payload/header/EOF/FID 异常、buffer corrupted 等信息。
- 三个命令配合使用:先看设备宣称什么,再看实际能否拉流,同时看内核为什么失败。

### 相关引用
- 无。

### 备注
- `/dev/videoX` 需要替换成实际节点,例如 `/dev/video0` 或 `/dev/video2`。

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
