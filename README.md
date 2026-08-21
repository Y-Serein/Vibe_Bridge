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

`/frame.adc16` 已使产测主机能够通过 USB 获取原始帧；但“把新的双点标定结果写入 Flash”的 HTTP 请求格式、鉴权和掉电恢复语义尚未定义，也尚未实现。旧串口标定命令不应再用于产品流程。实现 USB 标定写入前，必须先冻结该协议契约并在真机上验证 Flash 事务安全性。

当前 OTA 端点只完成长度、CRC 和 RP2350 向量范围检查，设备能力会声明 `"rollback": false`；没有签名、型号/硬件匹配或掉电回滚。它仅可用于非生产板联调，不能作为量产升级机制。

## 构建

项目使用 `build.sh`：

```bash
bash /home/slam/Sipeed/pico_tn160/build.sh
```

NCM 依赖 Pico SDK 2.2.0 的 lwIP submodule。SDK 副本的 `lib/lwip` 若未初始化，先提供一个完整的 lwIP 根目录：

```bash
PICO_LWIP_PATH=/path/to/lwip bash /home/slam/Sipeed/pico_tn160/build.sh
```

不要把临时下载的依赖或构建产物提交到仓库。构建产物为：

```text
/home/slam/build/pico_tn160/pico_tn160.elf.uf2
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
