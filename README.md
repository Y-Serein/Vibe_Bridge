# pico_tn160

`pico_tn160` 是基于 RP2350、TinyUSB、PIO/DMA 的 `160×120` 热成像固件。当前产品基线仅使用 USB CDC-NCM 和设备内 HTTP 服务；不再把 UART、USB CDC 或 UVC 作为产品数据、标定或诊断入口。

完整的跨产品协议见 [Sipeed Thermal USB/NCM/HTTP Protocol v1](docs/SIPEED_THERMAL_USB_NCM_HTTP_PROTOCOL_V1.md)。

## 当前产品接口

- USB class：CDC-NCM（`CFG_TUD_NCM=1`）
- UVC / USB CDC：已关闭（`CFG_TUD_VIDEO=0`、`CFG_TUD_CDC=0`）
- 默认设备地址：`http://192.168.17.1/`
- 设备能力：`GET /api/v1/device`
- 已处理预览：`GET /frame.temp8`、`GET /stream.temp8`
- 产测原始帧：`GET /frame.adc16`（160×120、14-bit 有效 ADC、little-endian）
- 网络设置：`GET` / `PUT /api/v1/network`
- 固件升级：`POST /api/v1/ota`，状态为 `GET /api/v1/ota/status`

设备内置网页、DHCP、DNS captive portal 与 HTTP 服务均运行在同一条 USB NCM 链路上。Windows 使用 Microsoft OS 2.0 `WINNCM` 绑定；构建时还会对 TinyUSB 0.18.0 应用可复现的 NCM 协商补丁。

## 数据链路

```text
传感器 -> PIO/DMA -> rxring -> rawbuf -> Core1 校正/FFC -> NCM HTTP
```

- Core0 接收并组装原始帧，维护采集同步。
- Core1 完成背景扣除、增益校正、坏点修复、显示范围映射、自动 FFC 和 NCM/TCP 服务。
- `temp8` 帧为 `160×120 uint8 + 120-byte SITHMETA`；`adc16` 用于产测读取校正前的原始 ADC。

源代码仍保留少量旧 UART/UVC 测试和兼容代码，避免影响传感器/FFC 逻辑；它们不在当前产品入口和 USB 描述符中，也不构成支持接口。

## 标定与 Flash

运行时仍使用既有的双点标定数据、坏点表与自动 FFC。2 MiB Flash 固定布局如下：

| 地址范围 | 大小 | 用途 |
| --- | ---: | --- |
| `0x000000-0x0BFFFF` | 768 KiB | 当前运行固件 |
| `0x0C0000-0x17FFFF` | 768 KiB | OTA 暂存镜像 |
| `0x180000-0x1FDFFF` | 504 KiB | 传感器标定数据 |
| `0x1FE000-0x1FEFFF` | 4 KiB | OTA 事务头 |
| `0x1FF000-0x1FFFFF` | 4 KiB | 产品设置（当前为网络段号） |

链接脚本会拒绝超过 768 KiB 的活动镜像，保证可构建镜像能放入等大的 OTA 槽。

### 重要边界

`/frame.adc16` 用于产测主机读取校正前 RAW；`POST /api/v1/calibration/v1`
及配套只读接口已实现固定长度 TNC1 的双槽事务写入、CRC 回读和掉电保护。TNC1
保留区中的 `TN`、`TC`、`RR` 继续支持读取诊断；当前默认运行控制是三层模型：基础 V1、
四台设备均值 NTC 锚点保底，以及可选的逐机 B 状态残差。基础 V1 写入、重启和全量回读
成功后，只要 V1 保存了有效基准 NTC，均值层立即启用（`K_Q8=290`，即
`1.1328125 ADU/NTC`）。逐机 64-byte `T160DM2` 只有 CRC、Flash 记录 CRC 和
`calibration_id` 全部匹配当前 V1 时才参与测温；没有逐机包或 B 写入失败时仍保留
V1+均值，NTC/V1 参考无效时退回纯 V1。Scope 提供“清除B补偿”操作；它只写入经 CRC
保护的清除记录，使设备回退到 V1+均值，不擦除基础 V1。模型不复制像素表，也不改变
NCM 帧格式。
`TN`、`TC`、`RR`、启动时间和 VTEMP 均不参与默认测温；单条 50°C 轨迹不能作为全量程模型。

当前 OTA 端点只完成长度、CRC 和 RP2350 向量范围检查，设备能力会声明 `"rollback": false`；没有签名、型号/硬件匹配或掉电回滚。它仅可用于非生产板联调，不能作为量产升级机制。

## 构建

增量构建：

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --build --preset pico2-release --parallel "$(nproc)"
```

配置并构建：

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --preset pico2-release && cmake --build --preset pico2-release --parallel "$(nproc)"
```

彻底刷新：

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --fresh --preset pico2-release && cmake --build --preset pico2-release --parallel "$(nproc)"
```

不要把临时下载的依赖或构建产物提交到仓库。构建产物为：

```text
/home/slam/dev/pico_tn160/build/pico_tn160.elf.uf2
```

## 验证

静态协议校验脚本需要已连接的 NCM 设备：

```bash
python3 scripts/validate_thermal_protocol_v1.py http://192.168.17.1
```

本仓库不能用构建结果代替上板验证。发布前至少验证：Windows、Linux 与 Android/iOS 的 NCM 枚举；DHCP 后可访问设备页；`/frame.temp8`、`/frame.adc16`、OTA 及反复拔插均按协议工作。

## 主要文件

- [src/main.c](src/main.c)：采集、校正、FFC、Core0/Core1 协作和 NCM 帧发布
- [src/thermal_ncm.c](src/thermal_ncm.c)：NCM、DHCP/DNS、HTTP 与帧传输
- [src/thermal_ota.c](src/thermal_ota.c)：OTA 与 Flash 事务
- [src/usb_descriptors.c](src/usb_descriptors.c)：NCM USB 描述符和 Windows 绑定
- [src/tusb_config.h](src/tusb_config.h)：TinyUSB class 与 NCM 缓冲配置
- [docs/SIPEED_THERMAL_USB_NCM_HTTP_PROTOCOL_V1.md](docs/SIPEED_THERMAL_USB_NCM_HTTP_PROTOCOL_V1.md)：公开协议

`docs/README/` 中的 UART/UVC 内容是历史资料，不应作为当前产品或产测契约。
