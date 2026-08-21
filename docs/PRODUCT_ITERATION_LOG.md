# 产品迭代日志

## 2026-08-21 — 产品链路切换为 USB NCM/HTTP

- 目标/用户现象：产品只保留经多平台验证的 USB NCM；预览、产测原始帧、网络设置和 OTA 均走 USB，不再将 UART、UVC 或 USB CDC 作为产品入口。
- 关键实现：采用 `feature/unified-thermal-protocol-v1` 基线，启用 CDC-NCM 和 HTTP v1，关闭 TinyUSB Video/CDC class；引入 NCM 描述符、网页、DHCP/DNS、`temp8`/`adc16` 帧接口、OTA 和 2 MiB Flash 的双 768 KiB 镜像布局。
- 验证：在 Pico SDK 2.2.0 固定的 lwIP 版本下完成标准 `build.sh` 固件构建；UF2 SHA-256 为 `dade5cd83465ff17fbe4ac7a037a22fe3517bd7ff804bc246a9c4366208783d7`，代码段 284316 B，低于 768 KiB OTA 槽限制。协议校验脚本可解析，但本轮未连接设备执行 HTTP 校验。
- 当前状态：已完成代码迁移，待上板验证。
- 提交/版本：本次 NCM-only 基线提交；源基线为 `origin/feature/unified-thermal-protocol-v1` 的 `c8c1779`。
- 风险/未验证平台：当前 UF2 未重新完成 Windows、Linux、Android/iOS 的 NCM 枚举、DHCP、HTTP 流、OTA 或反复拔插验证。`/frame.adc16` 已提供 USB 原始帧读取，但 USB 写入双点标定 Flash 的协议尚未定义或实现；旧串口标定命令不应继续用于产品流程。OTA 当前没有签名、型号/硬件匹配或掉电回滚，不能用于量产升级。
