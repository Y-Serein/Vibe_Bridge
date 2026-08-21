# HANDOFF.md

最后更新：2026-08-21 11:38 CST
当前分支：`test-2026-06-09`
当前任务：将产品基线切换为 USB NCM/HTTP-only；构建完成，等待真实设备与 USB 标定写入协议闭环。

## 当前在做什么

- 目标：产品只保留 CDC-NCM + HTTP。预览、原始 ADC16 产测、网络设置和 OTA 均走 USB；不再使用 UART、USB CDC 或 UVC 作为产品入口。
- 状态：当前源码采用 `origin/feature/unified-thermal-protocol-v1` 的 NCM 基线（远端提交 `c8c1779`），当前分支此前可回退 checkpoint 为 `6c1c418`。
- 误差：源码和构建证明 NCM-only 配置成立，但本次 UF2 尚未重新上板验证 Windows、Linux、Android/iOS 枚举、DHCP、HTTP、OTA 或反复拔插；不能把远端分支的历史实测直接写成本次实测。
- 控制动作：启用 `CFG_TUD_NCM=1`、关闭 Video/CDC，默认 `dataomode=DOM_NCM`；引入 NCM 描述符、HTTP v1、DHCP/DNS、网页、ADC16/temp8 帧与 OTA；Flash 改为 2 MiB、活动镜像/OTA 镜像各 768 KiB。
- 反馈：标准 `build.sh` 生成 `pico_tn160.elf.uf2`，SHA-256 为 `dade5cd83465ff17fbe4ac7a037a22fe3517bd7ff804bc246a9c4366208783d7`，代码段 284316 B，低于 768 KiB 槽限制。
- 修正：本地 Pico SDK 的 `lib/lwip` 为空，`build.sh` 现要求完整 lwIP，或由 `PICO_LWIP_PATH` 显式提供；缺失时退出 2，不自动联网下载。SDK 挂载盘把 TinyUSB 源文件标为可执行时，CMake 现在只在 build-local overlay 中恢复普通源文件权限，再应用 NCM 补丁。

## 已经试过的方案和结果（含失败的）

1. 将 `origin/feature/unified-thermal-protocol-v1` 解压到 `/tmp` 后构建：初次失败，原因是本地 SDK 的 `lib/lwip` 空目录，缺少 `lwip/err.h`；不是 NCM 源码编译错误。
2. 使用 Pico SDK 2.2.0 固定的 lwIP 提交 `77dcd25a72509eb83f72b033d219b1d40cd8eb95` 作为临时构建依赖：隔离和标准 `build.sh` 构建都成功。
3. 检查 USB 配置：`CFG_TUD_VIDEO=0`、`CFG_TUD_CDC=0`、`CFG_TUD_NCM=1`；USB PID 为 `0x4203`，描述符为 CDC-NCM，默认 Core1 数据输出为 `DOM_NCM`。
4. 检查旧主机串口路径：源码仍有历史 UART/UVC 测试和兼容函数，但 NCM 正式入口没有初始化或轮询主机 UART，TinyUSB 也不暴露 CDC/UVC class。
5. 检查标定接口：`GET /frame.adc16` 已可通过 USB 读取校正前 ADC；当前分支没有“通过 HTTP 写入新的双点标定数据到 Flash”的 endpoint。协议文档中的 calibration manifest/data 是只读契约，不能误报为标定写入已完成。

## 下一步计划（3-5条actionable)

1. 在可回退测试板上烧录当前 UF2，确认 Windows、Linux、Android/iOS 都枚举为 NCM，并通过 DHCP 访问 `http://192.168.17.1/`。
2. 在已连接设备上运行 `python3 scripts/validate_thermal_protocol_v1.py http://192.168.17.1`，核对 `/api/v1/device`、`/frame.temp8` 和 `/frame.adc16` 的 metadata/长度/CRC。
3. 在非生产板验证网络段设置、OTA 上传、校准区域 CRC 保持和多次拔插恢复；OTA/Flash 布局为高风险，未验证前不得用于量产烧录。
4. 冻结 USB 标定写入协议（输入形式、双点温度、身份/防误写、CRC、掉电事务、回读验证）后，再实现 HTTP 标定写入；不要恢复旧串口命令作为替代。
5. NCM 多平台实测全部通过后，再决定是否删除遗留的 UART/UVC 测试代码；当前不为清理而重构。

## 关键文件路径（相对路径，一行一个）

```text
CMakeLists.txt
build.sh
pico_flash_region_custom.ld
src/main.c
src/sdk.h
src/tusb_config.h
src/usb_descriptors.c
src/thermal_ncm.c
src/thermal_ncm.h
src/thermal_ota.c
src/thermal_ota.h
src/lwipopts.h
src/web/index.html
cmake/tinyusb-0.18-ncm-ios26.patch
scripts/validate_thermal_protocol_v1.py
docs/SIPEED_THERMAL_USB_NCM_HTTP_PROTOCOL_V1.md
docs/PRODUCT_ITERATION_LOG.md
README.md
```

## 当前验证状态

- 已运行：隔离 CMake 构建，`PICO_LWIP_PATH` 指向 lwIP `77dcd25...`；结果通过。
- 已运行：`PICO_LWIP_PATH=/tmp/pico_tn160_lwip_2_2_1 bash /home/slam/Sipeed/pico_tn160/build.sh`；结果通过，生成 ELF 与 UF2。
- 已运行：`python3 -m py_compile scripts/validate_thermal_protocol_v1.py` 和 `--help`；结果通过。
- 已运行：不设 `PICO_LWIP_PATH` 的 `build.sh`；结果按预期以退出码 2 报告缺少 lwIP，不产生新构建。
- 未上板验证：当前 UF2 未连接真实 TN160；未实测 NCM 枚举、HTTP 流、OTA、Flash 迁移、网络设置或任何主机平台。

## 还没搞清楚的问题

1. 当前硬件上实际 Flash 容量是否全部为 2 MiB，以及已有校准数据在新布局下是否保持可读，必须在可回退板验证。
2. NCM 远端分支虽称已完成多平台测试，但相关具体设备、操作系统版本和回归记录尚未在本 session 重放；本次仅完成构建验证。
3. USB 标定写入的公开协议尚未定义：不能在没有身份、CRC、掉电恢复和回读语义的情况下直接开放 Flash 写入。
4. 临时 lwIP 路径只用于本 session 构建；正式开发环境应初始化 Pico SDK 的 lwIP submodule 或设置持久的 `PICO_LWIP_PATH`。
5. 当前 OTA 仅校验长度、CRC 和 RP2350 向量范围，`/api/v1/device` 明确声明 `rollback:false`；尚未实现签名、型号/硬件匹配或掉电回滚，不能用于量产升级。
