## 2026-05-12｜guvcview 低帧率是否说明设备描述符问题

### 原始问题
现在Linux和Windows上都是可以看到的，Linux用直接抓取video的软件看是10帧，但是用guvcview看是1帧1帧出，但是也会一直出了！，好像是不是设备描述符的问题，我也不确定

### 答案核心
- Windows 和 Linux 都能看到画面,且 `v4l2-ctl` 能连续抓 5 帧,说明 UVC descriptor、YUYV 帧大小、payload header、Bulk 传输链路基本合法。
- guvcview 能持续出画面但像 1fps,更像是 guvcview 选择了较慢的 frame interval、预览/转换路径慢,或 GUI 显示节流,不再是“设备完全不兼容”。
- 当前 frame interval descriptor 是 continuous 范围:10fps 到 1fps;如果 guvcview 默认选最大 interval,就会表现成 1fps。
- 先不要改坏当前成功基线;优先在 guvcview 里手动选 `YUYV 160x120 10fps`,或确认 guvcview 实际协商到的 fps。
- 若确认 guvcview 默认总选 1fps,再考虑把 descriptor 的 frame interval 范围收窄到 10fps 附近,但不要恢复之前会破坏 Windows 的严格 commit/discrete 方案。

### 相关引用
- `src/usb_descriptors.h`
- `src/tusb_config.h`
- `src/usb_descriptors.c`
- `HANDOFF.md`

### 备注
- 置信度:中。没有看到 guvcview 的实际协商日志,所以不能断言一定是 descriptor 默认 interval 选择。

## 2026-05-11｜WSL 测试 UVC/guvcview 的适用性

### 原始问题
我能把这个挂载到wsl上测试吗，然后wsl能有和guvcview一样的工具吗

### 答案核心
- 可以用 `usbipd-win` 把 USB 设备 attach 到 WSL2,但 WSL 不是原生 USB 主机,而是 USB/IP 转发。
- WSL2 可以运行 Linux GUI 应用,所以理论上可以安装 `guvcview`、`v4l-utils`、`ffmpeg` 等工具。
- UVC 摄像头依赖 `uvcvideo`/V4L2 和 isochronous 传输;WSL/USB-IP 可能引入额外问题,因此不适合作为最终 guvcview 兼容性判定。
- 更推荐用原生 Linux 或 Linux Live USB 做最终验证;WSL 只能作为快速 smoke test。
- 如果 WSL 里只有 `lsusb` 能看到设备但没有 `/dev/video*`,通常说明内核/模块/V4L2 路径没起来,不是固件一定错。

### 相关引用
- Microsoft Learn: Connect USB devices to WSL
- Microsoft Learn: Run Linux GUI apps with WSL
- usbipd-win GitHub

### 备注
- 置信度:中。WSL/usbipd 版本和 WSL kernel 版本会影响结果。

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
