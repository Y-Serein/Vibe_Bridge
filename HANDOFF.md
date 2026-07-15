# HANDOFF.md

最后更新：2026-07-15 11:00 CST
当前分支：test-2026-06-09
当前任务：把 Android 从“手动选择 `160×120` 才出画”收敛为免配置自动出画，同时保持 Windows/Linux full-speed Bulk UVC 基线不回归。

## 当前在做什么

- 最终目标：同一正式固件在 Windows、Linux、Android 上免配置、不闪退、持续出画。
- 已确认 Android UVC 数据链路可用：修复后的诊断 App 能稳定启动、进入 `MainActivity`、识别 `359f:4202`；用户手动把分辨率改为 `160×120` 后确认能看到画面。
- 用户最新实测：通过华为应用渠道找到的 `Next Camera` 上电后即可自动出画，无需手动选择格式；这是当前 Android 免配置成功案例。
- 用户确认本次 `Next Camera` 测试使用的就是新 `OFF` UF2，SHA256 `0a9cf317db0b5e61c446ab8769f499c244cda807e560d47451d43640224ceeb0`；应用具体版本/包名仍未记录。
- 用户继续实测：`Next Camera` 已连续约 5 分钟稳定出画，无需手动选择格式；Android 免配置成功案例通过。
- 用户反馈：同一固件 Windows 已测试通过；原 Tony App 的客户端缺陷不纳入本次产品验收。
- 早期诊断 App 的手动 `160×120` 结果只证明 YUY2 数据链路可用；当前产品路径已由新 `OFF` UF2 + 华为渠道 `Next Camera` 免配置稳定出画验证。
- 失败前 App 默认选择的是 `640×480 MJPEG`；选择 `160×120` 后成功，当前最强证据是“主机默认格式/分辨率选择与固件真实稳定输出不一致”，不是 FFC 前 3 秒无数据、UVC 版本或 VC status endpoint。
- 源码和 CMake Cache 当前均为 `UVC_MJPEG640_STATIC_TEST=OFF`；已按当前工作区重新构建默认 UF2，不再是之前的静态 MJPEG 测试产物。
- 当前新默认 UF2 SHA256 为 `0a9cf317db0b5e61c446ab8769f499c244cda807e560d47451d43640224ceeb0`，与静态 MJPEG 测试 UF2 `1eb002b871c392f9e1964d327b8f073f5b31eb59fbda9247490df82b69ec4dcd` 不同。
- 新默认 UF2 与正式备份 `80dea1dc42a251b2f80aefd43eafa55de8562794f091103e1e9dfb0054e849a1` 也不同；当前工作区包含温漂/诊断及 UVC 相关未提交改动，不能据此宣称与历史正式版完全等价。
- Android 测试前的正式备份为 `/home/slam/build/pico_tn160/pico_tn160_FORMAL_BEFORE_MJPEG_TEST_80dea1dc.uf2`，SHA256 `80dea1dc42a251b2f80aefd43eafa55de8562794f091103e1e9dfb0054e849a1`。
- 当前 WSL PATH 没有 `adb`，固定 Windows `adb.exe` 路径调用仍报 `UtilBindVsockAnyPort: socket failed 1`，无法自动读取设备和固件哈希；后续以用户上板反馈为主，不继续猜测或改描述符。
- 温漂/运行期 NUC 改动仍在工作区，未完成本轮上板验证；不要因为 UVC 收尾而回滚或混入重构。

## 已经试过的方案和结果（含失败的）

1. UVC 1.1、34-byte Probe/Commit、VC interrupt status endpoint `0x82`：解决了旧的 `ctrl_if.bEndpointAddress is null`，但原 Tony App 仍在预览启动前关闭，因此这些不是本轮剩余根因。
2. 原 Tony App 日志/反编译：`onOpen()` 无条件发送 `4386`，会停止相机并重建 `640×480` handler；延迟回调未检查 handler，最终产生空指针闪退。App 在数毫秒级关闭，不能归因于固件 FFC 前 3 秒无帧。
3. 为隔离 App 竞态制作共存诊断包：禁用了 `4386` 重启路径和不安全延迟回调，原 Tony App 未覆盖、未卸载。
4. 第一版诊断包失败：直接把 DEX 内 `com.tony...` 等长替换为 `com.diag...`，破坏 DEX `string_ids` 字典序；`dexdump` 明确报告 `Out-of-order string_ids`，Android 因而无法加载 `classes2.dex` 中的 `com.serenegiant.usbcamera.App`，表现为打开即关闭。
5. 修复诊断包：Manifest 保持独立包名，DEX 内硬编码包名恢复原值，重新计算 DEX SHA-1/Adler32、zipalign、v1/v2/v3 签名；官方 `dexdump`、APK 完整性和签名验证均通过。
6. 修复包通过 `adb install --no-streaming -r` 覆盖安装成功；冷启动后进程持续存活，前台进入 `HomeActivity`，插入设备后进入 `MainActivity` 并收到 `USB_DEVICE_ATTACHED`，识别 VID/PID 十进制 `13727/16898`（`359f:4202`）。
7. 用户在诊断 App 中把分辨率设置为 `160×120` 并确认出画。由此排除“Android 完全不支持当前 Bulk UVC”和“无首帧一定由 FFC 导致”。
8. 当前测试固件额外暴露静态 `640×480 MJPEG`，App 日志显示它自动选择该模式；这个测试 profile 反而改变了主机默认选择，不能留在正式兼容结论里。
9. 历史成功基线：full-speed Bulk、`160×120`、YUY2、10fps、Bulk EP `0x81`/64 bytes、TinyUSB payload/epbuf 256；Windows 可出画，Linux/V4L2 连续 `DQBUF`，guvcview 可用。不要回到 ISO descriptor/payload 猜测路线。

## 下一步计划（3-5条actionable)

1. 记录 `Next Camera` 的华为应用页、版本/包名，保留当前新 `OFF` 固件不变。
2. 保留当前固件和 `Next Camera` 组合，记录应用版本/包名；Android 免配置稳定性已通过。
3. Linux/WSL 用上次的 `v4l2-ctl` 命令确认连续 `DQBUF`、`bytesused=38400`、5 帧文件 `192000` bytes，并复测 guvcview。
4. 保留 Windows 已通过和 Android 已通过证据，完成 Linux/guvcview 后形成三平台结论。
5. 三平台通过后再决定是否需要真实 MJPEG 兼容模式；静态 `640×480 MJPEG` 测试 profile 不得作为产品方案。

## 关键文件路径（相对路径，一行一个）

```text
AGENTS.md
CMakeLists.txt
C_context/MEMORY.md
src/main.c
src/sdk.h
src/tusb_config.h
src/usb_descriptors.c
src/usb_descriptors.h
src/uvc_mjpeg_test_frame.h
docs/logs.txt
docs/usb_camera_base.apk
out/android/tony_uvc_diag.apk
out/android/tony_uvc_diag_fixed.apk
.claude/skills/pico-tn160-android-uvc/SKILL.md
.claude/skills/pico-tn160-android-uvc/references/case-study.md
build.sh
```

构建命令：

```bash
bash /home/slam/Sipeed/pico_tn160/build.sh 2>&1 | tail -5
```

关键产物：

- 修复诊断 APK：`out/android/tony_uvc_diag_fixed.apk`
- 修复诊断 APK SHA256：`ca86a63df8381a0195d96e0d88115e86a8c9efe719ed696a2908d98ab52727fd`
- 初始无效诊断 APK SHA256：`faab593797c0a9ec9197d028819980003a1bc4134dedb3ceac9f42a6d56e2d3f`
- 正式备份 UF2：`/home/slam/build/pico_tn160/pico_tn160_FORMAL_BEFORE_MJPEG_TEST_80dea1dc.uf2`
- 当前静态 MJPEG 测试 UF2：`/home/slam/build/pico_tn160/pico_tn160_UVC_TEST_MJPEG640_STATIC.uf2`
- 温漂/UVC 测试前回滚 UF2：`/home/slam/build/pico_tn160/pico_tn160_ROLLBACK_BEFORE_PHONE_TEST_1fe43a9d.uf2`

## 当前验证状态

- 已运行项目 preflight；脚本仍硬编码 `/home/rv_nano/Sipeed`，在当前 `/home/slam/Sipeed` 环境报告路径缺失。实际项目文件按 `/home/slam/Sipeed` 读取完成。
- 已用 Android SDK `dexdump` 验证初始诊断 DEX：失败，明确为 `Out-of-order string_ids`。
- 已用 `dexdump` 验证修复 DEX：通过；SHA-1 和 Adler32 校验一致。
- 已验证修复 APK 压缩完整性：通过。
- 已用官方 `apksigner` 验证修复 APK：v1/v2/v3 均通过。
- 已安装修复 APK：`adb install --no-streaming -r ...` 返回 `Success`。
- 已验证修复 App 冷启动和 5 秒后进程存活；无新的 `APP CRASH(EXCEPTION)`。
- 已验证插入设备后进入诊断 App `MainActivity`，并收到 `USB_DEVICE_ATTACHED`、识别 `359f:4202`。
- 用户实测反馈：手动设置 `160×120` 后诊断 App 能看到画面。
- 用户实测反馈：华为渠道的 `Next Camera` 在新 `OFF` 固件 `0a9cf317...` 上电即自动出画；具体版本、包名未记录。
- 用户实测反馈：同一新默认 UF2 在 Windows 已测试通过。
- 已运行固定构建命令：构建通过，输出 `/home/slam/build/pico_tn160/pico_tn160.elf.uf2`。
- 已验证新默认 UF2 SHA256 为 `0a9cf317...`，且与静态 MJPEG 测试 UF2 `1eb002b8...` 不同；CMake Cache/编译宏仍为 `OFF/0`，`MAIN_FUNC_MODE` 为 `SPI_INIT_40MHZ_4`。
- 当前 WSL PATH 没有 `adb`；固定 Windows `adb.exe` 调用因 WSL socket 错误失败，未执行清除诊断 App 设置。
- 已验证：`Next Camera` 在新 `OFF` UF2 上免手动设置并连续约 5 分钟稳定出画。
- 未验证：清除诊断 App 设置后能否免配置自动选择 `160×120`。
- 原 Tony App：已知客户端生命周期/空指针问题，不纳入本次产品验收，未继续复测。
- 已验证：新默认 UF2 在 Android/`Next Camera` 上完成免配置、约 5 分钟稳定出画；Windows 用户反馈同一固件通过。
- 未验证：同一新默认 UF2 在 Linux/V4L2 和 guvcview 上是否保持基线。
- 未验证：首帧延迟和 FFC 3 秒窗口；现有证据只排除了它是“App 数毫秒级闪退”的原因。
- 未上板验证温漂/NUC 当前工作区改动。

## 还没搞清楚的问题

1. `Next Camera` 超过 5 分钟的更长时间运行是否持续稳定；当前 5 分钟已通过。
2. 原 Tony App 的 `4386` 强制重建/空指针属于已知客户端问题；本次不以该 App 作为验收条件。
3. 是否需要为了更多 Android 客户端实现真实 MJPEG；如果需要，目标分辨率、帧率、RP2350 编码预算和 full-speed 带宽尚未评估。
4. FFC 前约 3 秒无有效热图是否会影响“已成功进入 Probe/Commit 后”的首帧超时，需要在稳定 App 上单独测量，不能与 App 启动崩溃混为一谈。
5. 新默认 UF2 与历史正式备份哈希不同；需要区分当前工作区温漂/诊断/UVC 改动的影响，不能仅凭构建通过宣称完全一致。
6. 温漂主线仍有冷启动 0-120 秒偏移和运行期灾难掉温的未完成验证；UVC 三平台收敛前暂不继续扩大温漂改动。
