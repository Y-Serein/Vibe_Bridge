# Sipeed USB NCM 热成像统一 HTTP 协议与实现指南

版本：1.0 Candidate  
状态：供产品研发实现与联调；完成跨型号互操作测试后冻结为 1.0  
日期：2026-08-20  
适用范围：包括但不限于原生分辨率 160×120、256×192、384×256、640×512 的 Sipeed USB 热成像产品

## 1. 目的与设计原则

本规范定义一套跨传感器、跨主控和跨分辨率的 USB NCM 热成像设备协议，使同一个网页、SDK 和上位机程序不需要根据产品型号硬编码接口。

设计原则：

1. 手机以单设备、免配置使用为主；电脑允许用户预先设置不同网段后同时连接多台设备。
2. 设备只传输灰度或热数据，14 种伪彩统一由浏览器或客户端实现。
3. 所有未压缩图像使用相同的固定 120-byte 帧尾 metadata。
4. JPEG 连续流保持标准 multipart MJPEG；同一份 120-byte metadata 以 Base64 放入每帧的 HTTP 扩展头。
5. 图像、温度参数和帧号必须能够在同一帧内关联，不依赖另一个异步 JSON 请求。
6. 各产品只实现自身支持的格式，并通过设备信息接口声明；客户端不得根据型号猜测功能。
7. 拍照和录像由浏览器保存到手机或电脑，不要求设备相册和设备文件系统。
8. 采集实时性优先。网络、JPEG、网页请求和慢客户端不得阻塞传感器采集链路。

本规范中的“必须（MUST）”“禁止（MUST NOT）”“应该（SHOULD）”“可以（MAY）”用于区分强制要求和建议实现。

## 2. 协议分层

```text
USB NCM
  └─ IPv4 / DHCP / DNS
      └─ HTTP/1.1，TCP 80
          ├─ 设备、网络、测温、校准和 OTA API
          ├─ /frame.xxx 单帧接口
          ├─ /stream.xxx 连续图传接口
          └─ 设备内置网页
```

基础实现不要求 HTTPS、mDNS、WebUSB、设备相册或云服务。设备与主机之间是直接 USB 网络，HTTP 服务监听 TCP 80。

### 2.1 术语

| 术语 | 含义 |
| --- | --- |
| ADC/ADU | 传感器模数转换结果/数字量 |
| DPC | Defective Pixel Correction，坏点检测与替换 |
| NUC | Non-Uniformity Correction，非均匀性校正 |
| FFC | Flat-Field Correction，平场/快门校正 |
| SR | Super Resolution，超分辨率 |
| H | High，原生有效画面的最高温点 |
| L | Low，原生有效画面的最低温点 |
| C | Center，中心ROI的温度和中心坐标 |
| 原生方向 | 传感器输出矩阵的行列方向，未应用网页旋转或镜像 |
| record | 一帧payload及其同帧metadata组成的协议记录 |

## 3. 网络与设备访问

### 3.1 默认网络参数

交付用户的已标定设备统一使用：

| 项目 | 默认值 |
| --- | --- |
| 设备地址 | `192.168.17.1` |
| IPv4 前缀 | `/24`（掩码 `255.255.255.0`） |
| DHCP 地址池 | 建议 `192.168.17.2`～`192.168.17.20` |
| DHCP Router | `192.168.17.1` |
| DNS | `192.168.17.1` |
| 用户入口 | `http://ir.sipeed.com/` |
| IP 兜底入口 | `http://192.168.17.1/` |

完全空白、没有有效网段配置且没有有效标定数据的生产设备使用
`192.168.10.1/24`，只用于 Scope 首次识别和工位分配。Scope 将标定中的设备
分配到 `192.168.11.1`～`192.168.16.1`；标定、写入和回读全部通过后再切换为
用户地址 `192.168.17.1` 并提示拔出。临时工位地址不得作为标定阶段或完成状态的
唯一证据。

设备的本地 DNS 必须将 `ir.sipeed.com` 解析到设备当前地址。不再为每个型号定义 `t160.cam`、`t256s.cam` 等产品专属域名。

DHCP必须通过 Router（Option 3）下发设备当前地址，例如默认的 `192.168.17.1`。这是手机易用性要求：Android获得默认路由后才会把该NCM网络纳入联网验证，继而发起DNS和Captive Portal HTTP探测；设备返回探测响应后，系统才可能自动弹出认证页面。

DHCP server 必须同时处理两类合法的 DHCPREQUEST：SELECTING/INIT-REBOOT
状态从 Option 50 读取 requested IP；BOUND/RENEWING/REBINDING 状态在不含
Option 50 时从 `ciaddr` 读取当前地址。地址属于本设备地址池时返回 DHCPACK；旧
子网地址、越界地址或已分配给其他客户端的地址必须立即返回 DHCPNAK，不能静默
等待主机租约超时。`ciaddr` 续租的 DHCPACK 应直接发往该客户端地址；尚未配置
地址的请求继续使用广播回复。

这里的Router只用于主机侧网络选择和Captive Portal检测，不表示设备可以转发公网：设备自身的上游gateway应保持未配置，也不实现NAT或IP forwarding。Wi-Fi/蜂窝网络并存时的最终选路和是否弹窗由手机系统决定；部分OEM系统可能识别出Captive Portal却仍把普通浏览器绑定到Wi-Fi。产品必须验证目标Android/iOS机型，并始终保留字面IP访问及必要的使用提示。

`ir.sipeed.com` 主要面向单设备使用。系统同时存在 Wi-Fi、蜂窝网络或多块 USB 网卡时，操作系统可能选择其他 DNS；此时字面 IP 是确定性的兜底入口。

### 3.2 USB 身份

每台设备必须根据芯片 UID 或出厂序列号生成稳定且不同的：

- USB Serial Number；
- NCM 设备 MAC 地址；
- DHCP 服务器标识（如协议栈支持）。

禁止所有设备烧录相同 MAC 地址。

### 3.3 电脑多设备

普通浏览器无法枚举 NCM 网卡，也无法把一个 HTTP 请求绑定到指定网卡。因此多台设备不能同时保留相同子网。

网页只让用户设置一个“设备网段编号”，不要求用户理解完整 IP 和子网掩码：

```text
待分配设备：网段 10 → 192.168.10.1/24
工位 1～6：网段 11～16 → 192.168.11.1/24～192.168.16.1/24
完成设备：网段 17 → 192.168.17.1/24
```

设备内部同步修改自身 IP、DHCP 地址池、网关、DNS 和 Captive Portal 跳转地址。电脑用户随后使用预设置的字面 IP 访问各设备。

手机主要支持一次连接一台设备，不要求提供多设备发现。

### 3.4 网络配置接口

#### `GET /api/v1/network`

```json
{
  "mode": "fixed_private_subnet",
  "segment": 17,
  "device_ip": "192.168.17.1",
  "prefix_length": 24,
  "dhcp_first": "192.168.17.2",
  "dhcp_last": "192.168.17.20",
  "default_segment": 10,
  "product_segment": 17
}
```

#### `PUT /api/v1/network`

请求：

```http
Content-Type: application/json
X-Sipeed-Request: 1
```

```json
{"segment": 18}
```

规则：

- `segment` 必须在 `1..254` 内；设备可以额外屏蔽本产品占用的保留值。
- 成功返回 `202 Accepted`，应先完整发出响应，再应用新地址或重启网络。
- 配置必须持久化，但不得写入或覆盖传感器校准区。
- 产品必须提供恢复生产待分配网段 10 的兜底方法；物理按键方式可以因硬件而异。
- 有效标定存在但网络设置损坏时，设备必须回退到用户地址 17。

响应示例：

```json
{
  "accepted": true,
  "new_ip": "192.168.18.1",
  "new_url": "http://192.168.18.1/",
  "restarting": true
}
```

### 3.5 Captive Portal

设备应该兼容常见系统探测路径，例如：

- `/hotspot-detect.html`
- `/library/test/success.html`
- `/success.html`
- `/generate_204`
- `/generate204`
- `/gen_204`
- `/connecttest.txt`
- `/ncsi.txt`

探测请求可以返回 `302` 到 `http://ir.sipeed.com/`。Captive Portal 是否自动弹出由手机系统决定，不能作为唯一入口；内置页面必须同时显示域名和字面 IP 的访问方法。

未知普通网页路径可以跳转首页，但未知 `/api/*` 必须返回明确的 JSON `404`，禁止返回首页 HTML。

## 4. 公共 HTTP API

### 4.1 必选接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/v1/device` | 设备、传感器、能力、格式和资源 URL |
| `GET` | `/api/v1/network` | 当前网络配置 |
| `PUT` | `/api/v1/network` | 修改 `192.168.xxx.1` 中的 `xxx` |
| `GET` | `/` | 设备内置网页 |
| `GET`/`HEAD` | `/frame.<format>` | 返回一个完整帧 |
| `GET`/`HEAD` | `/stream.<format>` | 连续返回帧；仅限设备声明支持的格式 |

设备至少必须声明并实现一种图传格式。它不需要实现本文列出的全部格式。

### 4.2 条件必选和可选接口

| 方法 | 路径 | 条件/能力 |
| --- | --- | --- |
| `GET` | `/api/v1/thermal` | 设备端已经计算 H/L/C 时建议提供 |
| `GET` | `/api/v1/measurement` | 支持测温或 RAW16 浏览器测温时提供 |
| `PUT` | `/api/v1/measurement` | 设备端允许修改测温参数时提供 |
| `GET` | `/api/v1/calibration/manifest` | RAW16 可在客户端恢复温度时必须提供 |
| `GET` | `/api/v1/calibration/data/{id}` | 获取校准表二进制数据 |
| `POST` | `/api/v1/calibration/v1` | TN160 工厂双点标定包写入；仅设备明确声明该能力时提供 |
| `GET` | `/api/v1/calibration/v1/manifest` | TN160 工厂标定的只读状态、CRC 和数据 URL |
| `POST` | `/api/v1/ota` | 支持网页 OTA 时提供 |
| `GET` | `/api/v1/ota/status` | 支持网页 OTA 时提供 |

基础规范不定义 `/api/capture`、`/api/gallery` 或设备相册。拍照、录像由客户端完成。

### 4.3 通用响应要求

- 实时数据和状态接口返回 `Cache-Control: no-store`。
- JSON 使用 UTF-8 和 `Content-Type: application/json; charset=utf-8`。
- 未出图时，`/frame.*` 返回 `503 Service Unavailable`，不得返回半帧或全零伪帧。
- 不支持的格式返回 JSON `404` 或 `501`，并且不得在 `/api/v1/device` 中声明该 URL。
- `HEAD /frame.*` 和 `HEAD /stream.*` 返回与 `GET` 相同的格式描述头，但没有响应 body。

统一错误格式：

```json
{
  "ok": false,
  "error": {
    "code": "frame_not_ready",
    "message": "Thermal frame is not ready"
  }
}
```

建议状态码：

| 状态码 | 用途 |
| ---: | --- |
| `400` | 参数或 JSON 错误 |
| `404` | 接口/格式不存在 |
| `409` | 当前设备状态冲突 |
| `413` | OTA 文件过大 |
| `415` | Content-Type 或固件格式错误 |
| `422` | 文件有效但型号/硬件不兼容 |
| `503` | 帧未就绪或资源暂时繁忙 |

## 5. 设备信息查询

### 5.1 `GET /api/v1/device`

该接口是所有客户端的入口。客户端必须先读取能力和 URL，不得硬编码分辨率或根据型号选择接口。

示例：

```json
{
  "api": {
    "name": "sipeed-thermal-http",
    "major": 1,
    "minor": 0
  },
  "device": {
    "model": "T256S",
    "serial": "0123456789abcdef",
    "hardware": "1.0",
    "firmware": "26.40.0"
  },
  "sensor": {
    "native_width": 256,
    "native_height": 192,
    "recommended_rotation": 1
  },
  "network": {
    "segment": 17,
    "device_ip": "192.168.17.1",
    "configurable": true,
    "config_url": "/api/v1/network"
  },
  "streaming": {
    "max_stream_clients": 1,
    "simultaneous_formats": false
  },
  "measurement": {
    "supported": true,
    "center_roi": {"width": 5, "height": 5},
    "config_url": "/api/v1/measurement"
  },
  "super_resolution": {
    "device": true,
    "client_recommended": true,
    "client_max_native_width": 256,
    "client_max_native_height": 192
  },
  "formats": [
    {
      "id": "temp8",
      "width": 256,
      "height": 192,
      "stride_bytes": 256,
      "bits_per_pixel": 8,
      "temperature_encoding": "linear",
      "metadata": "sipeed-thermal-meta-v1",
      "frame": "/frame.temp8",
      "stream": "/stream.temp8"
    },
    {
      "id": "jpg",
      "source": "device_sr",
      "source_width": 256,
      "source_height": 192,
      "width": 768,
      "height": 576,
      "temperature_encoding": "none",
      "metadata": "sipeed-thermal-meta-v1-base64",
      "frame": "/frame.jpg?source=sr",
      "stream": "/stream.jpg?source=sr"
    }
  ],
  "ota": {
    "supported": true,
    "format": "product-signed-bin",
    "max_bytes": 1572864,
    "rollback": true,
    "upload": "/api/v1/ota",
    "status": "/api/v1/ota/status"
  }
}
```

### 5.2 能力声明规则

- `formats[]` 是权威列表；未列出的格式视为不支持。
- `frame` 声明该格式的单帧 URL；`stream` 仅在设备实现连续流时出现，未声明时客户端不得推测或请求对应 `/stream.*`。
- 每项必须提供实际输出尺寸。设备端 SR 的输出尺寸不得写入 `sensor.native_width/height`。
- `recommended_rotation` 只指导显示，不能改变协议坐标或未压缩 payload 排列。
- `simultaneous_formats=false` 时，客户端不得同时保持两个连续流；SR 对比应改用交替 `/frame.*`。
- OTA 文件格式、最大大小和回滚能力必须由设备声明。
- JSON 可以增加新字段；客户端必须忽略不认识的字段。

## 6. 图像处理阶段与格式

### 6.1 统一处理链

```text
传感器 ADC
  │
  ├─ adc16：未经空间 K、RMCOVER、DPC 等校正
  │
  └─ 基础成像校正
       ├─ raw16：校正后的 16-bit 成像值
       │    ├─ raw8：归一化为 8-bit，仅显示
       │    └─ 测温算法
       │         ├─ temp16：16-bit 温度图
       │         └─ temp8：温度线性量化 8-bit
       │                └─ jpgt：高质量灰度 JPEG，近似恢复温度
       │
       └─ 显示/SR处理
              └─ jpg：显示用灰度 JPEG，不恢复逐像素温度
```

### 6.2 格式总表

| 格式 | 数据类型 | 处理程度 | 逐像素温度 | metadata |
| --- | --- | --- | --- | --- |
| `adc16` | little-endian `uint16` | 最原始 ADC，未做空间一致性、RMCOVER、DPC | 默认不能 | 固定 120 bytes |
| `raw16` | little-endian `uint16` | 已做基础成像和坏点校正，可直接成像 | 通过校准表在客户端计算 | 固定 120 bytes |
| `raw8` | `uint8` | RAW16 经 AGC/范围归一化 | 不能 | 固定 120 bytes |
| `temp16` | little-endian `uint16` | 设备完成测温 | 精确线性换算 | 固定 120 bytes |
| `temp8` | `uint8` | 温度图线性量化 | 有轻微量化损失，可线性换算 | 固定 120 bytes |
| `jpgt` | 8-bit 灰度 JPEG | TEMP8 经过 JPEG，质量必须 ≥95 | 近似换算 | multipart 扩展头 Base64 metadata |
| `jpg` | 8-bit 灰度 JPEG | 显示或设备端 SR | 不能；H/L/C 可有效 | multipart 扩展头 Base64 metadata |

### 6.3 `adc16`

- 仅为未来研发、传感器分析和产线用途预留，普通产品可以不实现。
- payload 不得包含传感器行头、包头或非像素附加字节。
- 有效 ADC 位必须右对齐到 `uint16`，实际位数写入 metadata 的 `effective_bits`。
- 如需暴露参考像元或行尾数据，应定义独立接口，禁止混入图像矩阵。
- 产测用低频单帧需求可以只实现 `/frame.adc16`，此时 `/api/v1/device` 中该格式只声明 `frame`，不声明 `stream`。

T160 产测实现固定为：

- `GET /frame.adc16` 读取请求到达后的下一张完整帧；`HEAD /frame.adc16` 只返回格式头。
- 不实现 `/stream.adc16`，消费者网页也不显示 ADC16。
- payload 为 `160×120` 个 little-endian `uint16`，共 38,400 bytes；随后追加 120-byte `SITHMETA`，整条记录共 38,520 bytes。
- `effective_bits=14`。传感器线上的 14-bit Gray code 使用与旧产测串口相同的解码转为右对齐二进制值，再按统一协议改为 little-endian。
- 仅包含 160×120 有效成像像元；旧串口的 `0xFFFF` 帧头、每行 2 个参考值和帧尾辅助值均不进入 payload。
- ADC16 在空间一致性、RMCOVER、DPC 及测温处理之前抓取，因此不置 `CALIBRATED`、`TEMP_TRANSFORM_VALID` 和 H/L/C 有效位。

### 6.4 `raw16`

`raw16` 至少已经完成该产品需要的：

- 逐像素一致性/空间 K 校正；
- RMCOVER 或等价成像校正；
- DPC 坏点检测和替换；
- 产品定义的基础去噪或成像校正。

它必须可以直接归一化显示，但通常不能通过单一 `scale/offset` 恢复温度。

如果声明 `temperature_encoding="calibration_tables"`，设备必须提供校准 manifest、KT、BT、NUC-T、距离修正等算法要求的数据，并在每帧 metadata 中提供当前增益档、探测器温度、校准 ID 和 NUC 状态。

发射率、距离和反射温度由浏览器采用默认值或用户设置，在客户端完成计算。此时 raw16 帧中的温度 `scale/offset` 和 H/L/C 可以无效，浏览器计算后自行得到温度图与 H/L/C。

### 6.5 `raw8`

- 是 `raw16` 的 `uint8` 归一化结果，不是 `int8`。
- 只用于显示，禁止声称可以恢复温度。
- metadata 中 `source_low/source_high` 表示量化前 RAW16 范围。
- `range_method` 声明 min/max、百分位、设备自动 AGC 或用户固定范围。

### 6.6 `temp16` 和 `temp8`

统一温度换算公式：

```text
temperature_celsius = pixel_value × temperature_scale
                    + temperature_offset
```

系数使用 IEEE-754 float32。

`temp16` 使用 little-endian `uint16` 像素。负温度通过负的 offset 表示，不使用 `int16` 像素。

`temp8` 使用 `uint8` 像素。`quant_max` 是最大有效码值：

- 当前 T160 旧链路因为历史串口使用 `0xFF` 作为包头，填 254；
- 新实现可以使用完整范围并填 255；
- 客户端不得写死 254 或 255；大于 `quant_max` 的码值视为保留/无效。

### 6.7 `jpgt`

- 输入必须来自 temperature-linear 的 `temp8` 灰度图。
- 必须使用 8-bit 单分量灰度 JPEG。
- JPEG quality 必须不低于 95，并在 part header 中报告。
- JPEG 有损压缩会改变灰度值，逐像素温度只能近似恢复。
- metadata 中由设备计算的 H/L/C 温度不受 JPEG 压缩影响，仍可视为精确设备结果。
- 解码值超出 `0..quant_max` 时，客户端先钳位再应用温度公式。

### 6.8 `jpg`

- 仅用于显示，不提供逐像素温度恢复。
- 可以是原生分辨率显示图，也可以是设备端 SR 输出。
- 统一协议下应传输灰度 JPEG，不应用设备端伪彩。
- metadata 的 H/L/C 可以有效，由网页在显示层绘制十字温标，禁止直接烧录进 JPEG 像素。
- 设备端 SR 只通过 `jpg` 输出，不要求为 SR 图建立温度图。

## 7. 图传接口

### 7.1 路径

已声明格式按下列规则命名。`frame` 是单帧入口，`stream` 是可选的连续流入口；实际支持情况以 `/api/v1/device` 为准：

```text
/frame.adc16       /stream.adc16
/frame.raw16       /stream.raw16
/frame.raw8        /stream.raw8
/frame.temp16      /stream.temp16
/frame.temp8       /stream.temp8
/frame.jpgt        /stream.jpgt
/frame.jpg         /stream.jpg
```

设备端 SR 使用查询参数区分来源：

```text
/frame.jpg?source=sr
/stream.jpg?source=sr
```

原生显示 JPEG 可以使用 `source=native`。`/api/v1/device` 中给出的 URL 是权威入口。

### 7.2 `/frame.<format>`

- 返回恰好一个完整帧。
- 未压缩格式的 `Content-Length = payload_bytes + 120`。
- JPEG 的 `Content-Length` 只计算 JPEG body，不包括 HTTP header。
- 请求时没有可用帧，返回 JSON `503`。
- 适用于老浏览器轮询、抓取、自动化测试和低频读取。

### 7.3 `/stream.<format>`

- 一个长 HTTP 响应持续输出完整帧。
- 不得依赖 TCP read、pbuf、USB transfer 或 Fetch chunk 的边界。
- 未压缩格式直接拼接固定长度 record。
- JPEG 使用标准 multipart MJPEG。
- 客户端断开或发送速度不足时，设备应该丢弃旧帧并发送最新帧，禁止反向阻塞采集 DMA。
- 若实现只允许一个连续流，第二个请求返回 `409` 或 `503`，并在设备信息中声明 `max_stream_clients`。

### 7.4 未压缩帧的 HTTP 头

单帧和连续流都应该提供：

```http
X-Sipeed-Protocol: 1.0
X-Sipeed-Format: temp8
X-Sipeed-Width: 160
X-Sipeed-Height: 120
X-Sipeed-Stride-Bytes: 160
X-Sipeed-Payload-Bytes: 19200
X-Sipeed-Meta-Bytes: 120
X-Sipeed-Record-Bytes: 19320
```

建议 Content-Type：

```http
application/vnd.sipeed.thermal-frame; version=1; format=temp8
application/vnd.sipeed.thermal-stream; version=1; format=temp8
```

连续流不设置 `Content-Length`，可以采用“连接关闭表示 body 结束”的 HTTP/1.1 响应；实现应设置 `Connection: close` 并保持连接直到流停止。

### 7.5 未压缩 record 排列

```text
record 0: [pixel payload][120-byte metadata]
record 1: [pixel payload][120-byte metadata]
record 2: [pixel payload][120-byte metadata]
...
```

一次连接内的格式、尺寸、stride 和 record 长度必须保持不变。切换格式或尺寸必须重连。

未压缩 payload 统一采用原生方向、左上角起点、逐行从左到右的 row-major 排列：

```text
payload_bytes = stride_bytes × height
row_y         = payload + y × stride_bytes
```

8-bit像素每点1字节，16-bit像素每点2字节且little-endian。`stride_bytes`不得小于一行有效像素字节数；如有行尾padding，发送端写0、接收端忽略，但payload长度和payload CRC仍包含padding。

TCP 保证字节不丢失、不乱序。若 CRC/magic 校验失败，客户端应关闭并重新建立流；禁止在任意像素中搜索 magic 猜测边界。新连接的第一个字节必须是新 record 的 payload 起点。

## 8. Sipeed Thermal Metadata v1

### 8.1 总体规则

- 固定长度：120 bytes。
- 未压缩格式：位于像素 payload 之后。
- JPEG：相同 120 bytes 经过标准 Base64 后放入对应 HTTP part 的 `X-Sipeed-Meta`。
- 多字节整数：little-endian。
- 浮点数：IEEE-754 binary32，little-endian。
- 结构体不得依赖编译器自然对齐；必须 packed + offset static assertion，或逐字段序列化。

### 8.2 固定布局

| Offset | Size | 类型 | 字段 | 含义 |
| ---: | ---: | --- | --- | --- |
| 0 | 8 | char[8] | `magic` | ASCII `SITHMETA` |
| 8 | 1 | u8 | `version_major` | v1 为 1 |
| 9 | 1 | u8 | `version_minor` | 初始为 0 |
| 10 | 2 | u16 | `meta_bytes` | 固定 120 |
| 12 | 4 | u32 | `flags` | 字段有效性和帧状态 |
| 16 | 4 | u32 | `frame_number` | 当前输出格式的 record 序号，允许回绕 |
| 20 | 4 | u32 | `source_frame_number` | 本输出来源的原生传感器帧号；非 SR 通常等于 `frame_number` |
| 24 | 4 | u32 | `timestamp_ms` | 原生帧捕获时间，设备本次启动后的毫秒数 |
| 28 | 4 | u32 | `payload_bytes` | metadata 之前的 payload 长度；JPEG 时是 JPEG body 长度 |
| 32 | 4 | u32 | `payload_crc32` | IEEE CRC-32，仅覆盖 payload |
| 36 | 2 | u16 | `width` | payload 图像宽度 |
| 38 | 2 | u16 | `height` | payload 图像高度 |
| 40 | 2 | u16 | `stride_bytes` | 未压缩每行字节数；JPEG 填 0 |
| 42 | 1 | u8 | `pixel_format` | 见格式枚举 |
| 43 | 1 | u8 | `effective_bits` | ADC/RAW 有效位；8-bit/JPEG 填 8 |
| 44 | 1 | u8 | `display_rotation` | 建议顺时针旋转次数 `0..3`，不改变 payload |
| 45 | 1 | u8 | `gain_mode` | 当前增益档索引；`255` 表示未知 |
| 46 | 1 | u8 | `range_method` | 归一化范围算法 |
| 47 | 1 | u8 | `quant_max` | 8-bit 最大有效码值；不适用填 0 |
| 48 | 4 | f32 | `temperature_scale` | 温度线性换算系数 |
| 52 | 4 | f32 | `temperature_offset` | 温度线性换算偏移，单位 ℃ |
| 56 | 4 | f32 | `center_temperature` | 中心 ROI 温度，单位 ℃ |
| 60 | 4 | f32 | `minimum_temperature` | 场景最低温，单位 ℃ |
| 64 | 4 | f32 | `maximum_temperature` | 场景最高温，单位 ℃ |
| 68 | 2 | u16 | `center_x` | 中心 ROI 中心的原生传感器 X |
| 70 | 2 | u16 | `center_y` | 中心 ROI 中心的原生传感器 Y |
| 72 | 2 | u16 | `minimum_x` | 最低温原生传感器 X |
| 74 | 2 | u16 | `minimum_y` | 最低温原生传感器 Y |
| 76 | 2 | u16 | `maximum_x` | 最高温原生传感器 X |
| 78 | 2 | u16 | `maximum_y` | 最高温原生传感器 Y |
| 80 | 4 | f32 | `detector_temperature` | 探测器/FPA 温度，单位 ℃ |
| 84 | 4 | f32 | `source_low` | 量化前低端；语义见 8.6 |
| 88 | 4 | f32 | `source_high` | 量化前高端；语义见 8.6 |
| 92 | 4 | u32 | `calibration_id` | 当前校准数据 ID/版本 |
| 96 | 2 | u16 | `nuc_table_index` | 当前 NUC-T 状态/查表坐标，具体映射由 manifest 定义 |
| 98 | 2 | u16 | `ffc_count` | 本次启动后的 FFC 计数，允许回绕 |
| 100 | 1 | u8 | `ffc_state` | FFC 状态枚举 |
| 101 | 1 | u8 | `shutter_state` | 快门状态枚举 |
| 102 | 2 | u16 | `reserved0` | 写 0 |
| 104 | 4 | u32 | `measurement_config_id` | 设备端测温参数版本；客户端 RAW16 测温时可填 0 |
| 108 | 8 | u8[8] | `reserved` | 写 0，供 v1 minor 扩展 |
| 116 | 4 | u32 | `meta_crc32` | IEEE CRC-32，覆盖 metadata bytes `0..115` |

参考 C 定义：

```c
#include <stdint.h>

struct __attribute__((packed)) sipeed_thermal_meta_v1 {
    char     magic[8];
    uint8_t  version_major;
    uint8_t  version_minor;
    uint16_t meta_bytes;
    uint32_t flags;
    uint32_t frame_number;
    uint32_t source_frame_number;
    uint32_t timestamp_ms;
    uint32_t payload_bytes;
    uint32_t payload_crc32;
    uint16_t width;
    uint16_t height;
    uint16_t stride_bytes;
    uint8_t  pixel_format;
    uint8_t  effective_bits;
    uint8_t  display_rotation;
    uint8_t  gain_mode;
    uint8_t  range_method;
    uint8_t  quant_max;
    float    temperature_scale;
    float    temperature_offset;
    float    center_temperature;
    float    minimum_temperature;
    float    maximum_temperature;
    uint16_t center_x;
    uint16_t center_y;
    uint16_t minimum_x;
    uint16_t minimum_y;
    uint16_t maximum_x;
    uint16_t maximum_y;
    float    detector_temperature;
    float    source_low;
    float    source_high;
    uint32_t calibration_id;
    uint16_t nuc_table_index;
    uint16_t ffc_count;
    uint8_t  ffc_state;
    uint8_t  shutter_state;
    uint16_t reserved0;
    uint32_t measurement_config_id;
    uint8_t  reserved[8];
    uint32_t meta_crc32;
};

_Static_assert(sizeof(struct sipeed_thermal_meta_v1) == 120,
               "sipeed thermal metadata ABI mismatch");
```

上述定义只适用于 little-endian、IEEE-754 float32 的目标；跨端序实现必须逐字段编码。即使使用 packed struct，也必须对关键 `offsetof` 建立静态断言。

### 8.3 `pixel_format` 枚举

| 值 | 格式 |
| ---: | --- |
| 1 | `adc16` |
| 2 | `raw16` |
| 3 | `raw8` |
| 4 | `temp16` |
| 5 | `temp8` |
| 6 | `jpgt` |
| 7 | `jpg` |

其他值保留。客户端遇到未知格式必须拒绝按已知格式解释。

### 8.4 flags

| Bit | 名称 | 含义 |
| ---: | --- | --- |
| 0 | `IMAGE_VALID` | payload 是可显示的有效图像 |
| 1 | `CALIBRATED` | 已应用该格式所需的设备校准 |
| 2 | `TEMP_TRANSFORM_VALID` | `temperature_scale/offset` 有效 |
| 3 | `HLC_TEMPERATURES_VALID` | H/L/C 三个温度有效 |
| 4 | `HLC_COORDINATES_VALID` | H/L/C 坐标有效 |
| 5 | `PAYLOAD_CRC_VALID` | `payload_crc32` 有效 |
| 6 | `DETECTOR_TEMPERATURE_VALID` | 探测器温度有效 |
| 7 | `SOURCE_RANGE_VALID` | `source_low/high` 有效 |
| 8 | `SOURCE_FRAME_VALID` | `source_frame_number` 有效 |
| 9 | `NUC_INDEX_VALID` | `nuc_table_index` 有效 |
| 10 | `MEASUREMENT_CONFIG_VALID` | `measurement_config_id` 有效 |
| 11 | `FFC_ACTIVE` | 该帧产生时正在 FFC |
| 12 | `SHUTTER_CLOSED` | 该帧产生时快门关闭 |
| 13 | `CALIBRATION_ID_VALID` | `calibration_id` 有效 |
| 14..31 | reserved | 写 0 |

`ffc_state`：

| 值 | 含义 |
| ---: | --- |
| 0 | idle |
| 1 | pending/requested |
| 2 | acquiring shutter/reference frame |
| 3 | applying correction |
| 4 | failed |
| 255 | unknown/not supported |

`shutter_state`：

| 值 | 含义 |
| ---: | --- |
| 0 | open |
| 1 | closed |
| 2 | moving |
| 3 | not present |
| 255 | unknown |

`gain_mode` 是校准 manifest 中的稳定索引，不在公共协议中强制“高增益=0”或“低增益=1”。产品必须在 manifest 中给每个索引声明名称和适用温区；不支持增益切换的设备使用索引 0。

### 8.5 无效字段

- 无效 float32 推荐写 canonical quiet NaN，bit pattern `0x7FC00000`。
- 无效坐标写 `0xFFFF`。
- 无效索引写其类型最大值；同时必须清除对应 valid flag。
- 客户端以 flags 为准，禁止只根据数值猜有效性。

### 8.6 `source_low/high` 和范围算法

`range_method`：

| 值 | 含义 |
| ---: | --- |
| 0 | unknown / not applicable |
| 1 | scene min/max |
| 2 | percentile |
| 3 | device automatic AGC |
| 4 | user fixed range |

字段语义：

- `raw8`：`source_low/high` 是量化前 RAW16/ADU 值。
- `temp8` 和 `jpgt`：`source_low/high` 是量化前温度范围，单位 ℃；通常分别等于 offset 和 `offset + quant_max × scale`。
- `adc16`、`raw16`、`temp16`、`jpg`：没有范围量化时可以无效。

### 8.7 H/L/C 坐标

- 坐标始终基于传感器原生方向，左上角为 `(0,0)`。
- 坐标不随网页旋转、显示缩放、JPEG输出尺寸或 SR 尺寸改变。
- 设备信息中的 `sensor.native_width/height` 定义坐标空间。
- SR 页面把原生坐标按比例映射到显示图。
- H/L 是整个原生有效画面的极值。
- C 是设备信息 `measurement.center_roi` 声明的中心 ROI 平均值；1×1 表示中心像素。
- 温标由前端叠加，禁止污染原始像素或 JPEG。

### 8.8 CRC

CRC 使用 IEEE CRC-32：反射多项式 `0xEDB88320`，初始值 `0xFFFFFFFF`，最终异或 `0xFFFFFFFF`。

标准检查值：ASCII `123456789` 的结果必须为 `0xCBF43926`。

- `meta_crc32` 必须实现并始终校验。
- `payload_crc32` 可以因低端 MCU 性能限制而不实现；未实现时清除 flag 并填 0。
- JPEG metadata 中的 `payload_crc32` 覆盖纯 JPEG body。
- `frame_number` 和 `timestamp_ms` 都按无符号 32-bit 模运算比较；客户端必须允许约 49.7 天时 `timestamp_ms` 回绕。

两个 CRC 的覆盖范围严格分开：

```text
payload_crc32 = CRC32(payload[0 .. payload_bytes-1])     // 可选
meta_crc32    = CRC32(metadata[0 .. 115])                // 必选
```

`meta_crc32` 不覆盖 payload，也不覆盖自身。客户端需要验证完整 record 时分别检查两者。

这样设计是为了照顾低资源 MCU：

- metadata 在 25fps 时的 CRC 输入量只有 `116 × 25 = 2.9KB/s`，即使使用无查表的逐 bit CRC 也很轻；
- 640×512 RAW16 在 25fps 时的 payload 约为 16.38MB/s，再额外软件扫描一遍可能明显占用总线和 CPU；
- USB 链路本身有 CRC，TCP 也有端到端校验和，因此普通实时预览不强制重复计算 payload CRC；
- 产线、调试、存档或高可靠产品仍可以设置 `PAYLOAD_CRC_VALID` 并提供 payload CRC。

实现可以在生成 metadata 时一次性计算 116 bytes 的 CRC，不需要复制图像。为了节约 Flash，可使用逐 bit 或 16-entry nibble table；不强制使用 1KB 的 256-entry CRC 查表。

## 9. JPEG multipart 传输

### 9.1 `/stream.jpg` 和 `/stream.jpgt`

顶层响应：

```http
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=sipeedframe
Cache-Control: no-store
Access-Control-Allow-Origin: *
Connection: close
```

每帧 part：

```http
--sipeedframe\r\n
Content-Type: image/jpeg\r\n
Content-Length: 42831\r\n
X-Sipeed-Meta: <120-byte metadata 的单行标准 Base64>\r\n
X-Sipeed-JPEG-Quality: 95\r\n
\r\n
<42831 bytes JPEG body>\r\n
```

规则：

- `Content-Length` 只包含 JPEG body。
- `X-Sipeed-Meta` 与紧随其后的 JPEG body 属于同一个 part，因此不会跨帧错位。
- Base64 必须使用标准 alphabet 和 `=` padding，不插入换行。
- 未知 `X-*` part header 会被普通 MJPEG 客户端忽略，JPEG 仍能显示。
- 客户端遇到不完整 part 时丢弃该帧；下一次 boundary 后恢复。
- 可额外提供可读的 `X-Sipeed-Frame-Number`，但它只是调试冗余，二进制 metadata 是权威值。

### 9.2 `/frame.jpg` 和 `/frame.jpgt`

```http
HTTP/1.1 200 OK
Content-Type: image/jpeg
Content-Length: 42831
X-Sipeed-Meta: <Base64>
X-Sipeed-JPEG-Quality: 95
Access-Control-Expose-Headers: X-Sipeed-Meta, X-Sipeed-JPEG-Quality
```

body 是纯 JPEG。禁止在 JPEG EOI 后追加 metadata。

### 9.3 浏览器注意事项

`<img src="/stream.jpg">` 可以直接显示 MJPEG，但 JavaScript 无法通过 `<img>` 取得每个 multipart part 的扩展头。需要同帧 H/L/C 时，官方网页必须用 Fetch 读取并解析 multipart，或同时使用设备声明支持的热数据流。

## 10. 温度摘要与测温配置

### 10.1 `GET /api/v1/thermal`

这是方便应用读取“设备最近一次已计算结果”的可选接口，不替代同帧 metadata，也不保证调用瞬间正好对应屏幕正在解码的 JPEG。

```json
{
  "frame_number": 12345,
  "timestamp_ms": 287430,
  "valid": true,
  "center": {"celsius": 27.62, "x": 128, "y": 96},
  "minimum": {"celsius": 19.20, "x": 13, "y": 91},
  "maximum": {"celsius": 34.18, "x": 217, "y": 22},
  "measurement_config_id": 3
}
```

只输出 RAW16 且温度完全由浏览器计算的设备可以不实现该接口。

### 10.2 `GET /api/v1/measurement`

```json
{
  "processing": "client",
  "revision": 0,
  "defaults": {
    "emissivity": 0.95,
    "distance_m": 1.0,
    "reflected_temperature_c": 25.0
  },
  "limits": {
    "emissivity": [0.01, 1.0],
    "distance_m": [0.1, 100.0],
    "reflected_temperature_c": [-40.0, 550.0]
  }
}
```

规则：

- `processing="client"`：RAW16 温度在浏览器计算；网页使用 defaults 初始化，并把用户设置保存在客户端。设备可以不支持 PUT。
- `processing="device"`：temp16/temp8/jpgt 由设备计算；如果允许高级设置，`PUT /api/v1/measurement` 更新参数并递增 `measurement_config_id`。
- 普通用户界面应该隐藏这些高级参数并使用默认值。

写接口请求必须带 `X-Sipeed-Request: 1`，且不开放任意跨域 CORS。

## 11. RAW16 校准数据接口

### 11.1 原则

如果客户端需要从 RAW16 计算温度，仅给出若干 16-bit 数组不够。设备必须同时说明：

- 算法 ID 和版本；
- 当前校准 ID；
- 增益档及对应表；
- KT、BT、NUC-T、距离修正表的类型、点数和维度；
- 定点 Q 格式或 `stored × scale + offset`；
- 单位、索引轴和插值方式；
- 每张表的字节数和 CRC；
- metadata 中 `detector_temperature`、`nuc_table_index` 等动态量如何参与计算。

不同传感器可以使用不同算法，但必须使用稳定 `algorithm_id`。官方网页/SDK只对其声明支持的算法 ID 执行计算，禁止根据数组长度猜公式。

### 11.2 `GET /api/v1/calibration/manifest`

示例：

```json
{
  "schema": "sipeed-thermal-calibration-v1",
  "calibration_id": 305419896,
  "algorithm_id": "sipeed.kt-bt-nuct-distance.v1",
  "byte_order": "little",
  "gain_modes": [
    {"id": 0, "name": "high_gain"},
    {"id": 1, "name": "low_gain"}
  ],
  "tables": [
    {
      "id": "kt-g0",
      "role": "kt",
      "gain_mode": 0,
      "data_type": "int16",
      "count": 49152,
      "dimensions": [192, 256],
      "scale": 0.000030517578125,
      "offset": 0.0,
      "unit": "algorithm_specific",
      "interpolation": "none",
      "bytes": 98304,
      "crc32": "8a31c9e2",
      "url": "/api/v1/calibration/data/kt-g0"
    },
    {
      "id": "nuct-g0",
      "role": "nuc_t",
      "gain_mode": 0,
      "data_type": "uint16",
      "count": 8192,
      "dimensions": [8192],
      "scale": 0.01,
      "offset": -273.15,
      "unit": "celsius",
      "interpolation": "linear",
      "index_source": "metadata.nuc_table_index",
      "bytes": 16384,
      "crc32": "136f81a4",
      "url": "/api/v1/calibration/data/nuct-g0"
    }
  ]
}
```

KT、BT、NUC-T 和距离修正的实际维度由产品算法决定。NUC-T 常见点数 8192 或 16384，但客户端必须读取 manifest，禁止写死。

### 11.3 校准二进制数据

- 响应 `Content-Type: application/octet-stream`。
- 必须提供 `Content-Length`、`ETag` 或 `X-Sipeed-Calibration-ID`、CRC 信息。
- 数组紧密排列，不包含 C 结构体 padding。
- 16-bit item 使用 little-endian。
- 大表建议支持 HTTP Range，但不是 v1 强制要求。
- 浏览器可以按 `serial + calibration_id + table CRC` 缓存，校准 ID 改变时必须重新下载。
- OTA 不得覆盖校准数据，除非 OTA 包明确包含经过产品签名和硬件匹配校验的校准更新。

### 11.4 RAW16 浏览器计算流程

```text
1. GET /api/v1/device
2. GET /api/v1/calibration/manifest
3. 下载并校验当前 gain mode 所需表
4. 读取 /stream.raw16
5. 根据每帧 gain_mode、calibration_id、detector_temperature、nuc_table_index 选择/插值
6. 使用网页默认或用户输入的发射率、距离、反射温度
7. 计算 float 温度图
8. 计算 H/L/C、归一化灰度、伪彩和显示标记
```

若 metadata 的 `calibration_id` 与缓存 manifest 不一致，客户端必须暂停测温并重新获取校准数据，不能继续套用旧表。

### 11.5 TN160 工厂双点标定写入扩展

本节是 TN160 的生产标定扩展，不替代 11.1～11.4 的通用 RAW16 温度算法表接口。
它只适用于设备信息声明 calibration.supported=true 且
calibration.format=tn160-calibration-v1 的固件。

#### 11.5.1 写入请求

请求必须为 POST /api/v1/calibration/v1，并带以下 HTTP 头：

    Content-Type: application/vnd.sipeed.tn160-calibration-v1
    X-Sipeed-Request: 1
    X-Sipeed-Calibration-Confirm: write-v1
    Content-Length: 115274

请求 body 是连续 little-endian TNC1 包：

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | ASCII TNC1 |
| 4 | 2 | version，固定为 1 |
| 6 | 2 | header bytes，固定为 64 |
| 8 | 2 | width，固定为 160 |
| 10 | 2 | height，固定为 120 |
| 12 | 2 | bad point count，固定为 5 |
| 14 | 2 | 保留，必须为零 |
| 16 | 4 | image bytes，固定为 38400 |
| 20 | 4 | gain bytes，固定为 76800 |
| 24 | 4 | bad point bytes，固定为 10 |
| 28 | 4 | mean_diff，IEEE-754 float；必须为有限值且不小于 100 ADU |
| 32 | 4 | t_ref_low，IEEE-754 float，单位 °C |
| 36 | 4 | image CRC32 |
| 40 | 4 | gain CRC32 |
| 44 | 4 | bad point CRC32 |
| 48 | 4 | 三段 payload 串联后的 CRC32 |
| 52 | 2 | 可选模型 tag：基础 V1 为 0；旧 NTC 模型为 `0x4e54`（`TN`）；冷启动模型为 `0x4354`（`TC`） |
| 54 | 2 | `TN`：`ntc_reference_adu`；`TC`：`cold_ntc_threshold_adu`；两者范围均为 `9..4087` |
| 56 | 2 | little-endian int16；`TN`：`ntc_k_q8`，范围 `-8192..8192`；`TC`：`startup_amplitude_adu`，范围 `-600..600`；0 表示该机无需启动幅度修正 |
| 58 | 2 | `TN`：`ntc_limit_adu`，范围 `1..1500`；`TC`：`startup_tau_ds`，0.1 秒单位，范围 `50..600`（5～60 秒） |
| 60 | 4 | bytes 0..59 的 CRC32 |
| 64 | 38400 | img_bg，160×120 个 little-endian uint16 |
| 38464 | 76800 | gain，160×120 个 little-endian IEEE-754 float |
| 115264 | 10 | 五个 {y, x} 坏点，0xffff 为尾部终止项 |

模型扩展必须为全零，或按 tag 满足对应的全部字段约束；不接受半初始化参数。旧
`TN` 参数继续支持写入和回读，但不参与运行，`thermal_ntc_model_active` 固定为
`false`。`TC` 是当前正式的逐机冷启动模型：只有本次启动捕获的 `boot_ntc_adu`
严格大于 `cold_ntc_threshold_adu` 时，运行时才给有效 anchor 加上
`startup_amplitude_adu × exp(-elapsed_ms / (startup_tau_ds × 100))`；幅度是有符号
anchor ADU，经过 `6τ` 后归零。该绝对时间项不会写入 FFC 基础 anchor，避免 FFC
后重复补偿。`/api/v1/diagnostics` 的 `thermal_startup_model_valid/active` 及
`thermal_startup_model_ntc_threshold_adu/amplitude_adu/tau_ds` 用于写后核对。

基础 V1、旧 `TN` 和新 `TC` 均同时保留小幅板载 NTC fallback：相对上次有效 FFC
计算 `-0.20×ΔNTC`，限幅 `±1500 ADU`，并在下一次 FFC 把累计相对修正写入新
anchor。VTEMP 只保留为诊断字段，不参与逐帧补偿、模型门控或 FFC anchor。NTC
无效时相对 fallback 为零；冷启动模型无效或未通过门控时其时间项为零，设备仍继续
出图。

设备必须拒绝长度、magic、固定尺寸、header CRC、段 CRC 或 payload CRC 不正确的包；
还必须拒绝非法扩展模型、超出传感器 ADU 范围、非有限/不合理 gain、越界或
非连续终止的坏点。

成功返回 202 Accepted，并包含 accepted、generation、calibration_id、payload_crc32、
package_header_crc32、flash_header_crc32、state=verified 和 rebooting=true。
calibration_id 是完整 64-byte TNC1 header 的 CRC32，并会写入后续帧 metadata。
客户端必须比较返回的 payload/header CRC 与发送包，不能把 202 当作写入完成。

#### 11.5.2 掉电边界与重启

TN160 使用两个 payload 槽和两个 selector 页。新包先完整擦写到未激活 payload
槽，逐页回读、校验三段 CRC 和语义，再写入带格式标记、记录 CRC 与 payload CRC 的
新 selector；启动时只接受 selector 和其 payload 都校验通过、且 generation 最新的槽。
因而在 selector 写入前断电时，旧标定仍保持有效；擦除中或部分写入的 selector 不会
被当作旧版标定加载；
写入成功响应发出约 1.5 秒后设备重启并切换到新标定。

客户端收到成功响应后必须等待 NCM 链路重新枚举，并读取：

    GET /api/v1/calibration/v1/manifest
    GET /api/v1/calibration/v1/data/header
    GET /api/v1/calibration/v1/data/image
    GET /api/v1/calibration/v1/data/gain
    GET /api/v1/calibration/v1/data/badpoints

完整 64-byte header 和三段数据都必须逐字节与待写入包一致。没有有效 V1 标定时，
这些只读接口返回 404。

#### 11.5.3 访问边界

该接口没有网络级身份认证；X-Sipeed-Request 和
X-Sipeed-Calibration-Confirm 只是防止普通网页误触发的显式确认，不是密钥。
它仅适用于物理直连 USB NCM 的受控工位，禁止暴露到 LAN、Wi-Fi、互联网或
不受信任的 USB 主机。量产工位若需要访问控制，必须由主机侧账户、USB 物理隔离
或后续经过专门设计的设备认证机制提供。

## 12. 伪彩色板

设备公共图传接口只输出灰度，不允许设备色板状态改变 RAW、TEMP、JPGT 或 JPG payload。官方浏览器 v1 以以下 14 种色板作为默认参考基线，按常用程度排列：

| ID | 中文名 | 英文名 | 参考色阶（位置:RGB） |
| --- | --- | --- | --- |
| `white` | 白热 | White Hot | `0:#000000, 1:#FFFFFF` |
| `black` | 黑热 | Black Hot | `0:#FFFFFF, 1:#000000` |
| `iron` | 铁红 | Ironbow | `0:#000008, .2:#23083A, .45:#84194A, .68:#E84E23, .86:#FFBE4A, 1:#FFFFEE` |
| `rainbow` | 彩虹 | Rainbow | `0:#0A0026, .18:#1625B4, .38:#00B2DF, .58:#3AD249, .78:#FFD71E, 1:#F41C12` |
| `rainbow_hc` | 高对比彩虹 | Rainbow HC | `0:#000000, .12:#2A0080, .28:#0040FF, .43:#00E6E6, .57:#14D214, .72:#FFF500, .86:#FF5000, 1:#FFFFFF` |
| `arctic` | 北极 | Arctic | `0:#000416, .28:#122D70, .52:#2D8BB7, .7:#503D73, .84:#F2AB2B, 1:#FFFAD5` |
| `lava` | 熔岩 | Lava | `0:#000000, .28:#2B0036, .5:#840C6F, .7:#EB3733, .88:#FFA428, 1:#FFFBCD` |
| `red` | 红热 | Red Hot | `0:#000000, .5:#460000, .72:#E10A0A, .9:#FFAA12, 1:#FFFFEB` |
| `fusion` | 融合 | Fusion | `0:#030318, .24:#2E1269, .47:#115AAB, .67:#BB2884, .84:#F5692B, 1:#FFED83` |
| `icefire` | 冰火 | Ice & Fire | `0:#000A46, .25:#0069DC, .49:#B7F0FF, .51:#FFF2BE, .75:#EB4A10, 1:#600000` |
| `sepia` | 褐色 | Sepia | `0:#080503, .35:#4B2B19, .7:#B17D4F, 1:#FFF4DA` |
| `amber` | 琥珀 | Amber | `0:#000000, .42:#4F2100, .76:#E16F00, 1:#FFE899` |
| `green` | 绿热 | Green Hot | `0:#000300, .45:#004F17, .78:#24CD50, 1:#E0FFDE` |
| `medical` | 医疗 | Medical | `0:#000014, .18:#221873, .36:#007CDF, .54:#00D38E, .7:#E6E21D, .86:#F04218, 1:#FFFFFF` |

色阶节点之间使用逐通道线性插值，生成 256-entry RGB LUT。UI 应使用带实际热图缩略图的横向选择条，不要求用户只根据名称判断。

色板属于客户端显示资源，不属于设备wire protocol或固件capability ABI：

- 官方前端后续可以新增色板、调整排列或改进参考色阶，无需升级HTTP API或图传协议版本；
- 设备仍然只传灰度，更新色板不得改变任何payload或metadata；
- 已发布的色板ID建议保持稳定，避免浏览器保存的用户偏好失效；
- 第三方客户端不需要完整实现14种色板，也不得根据色板支持情况判断设备兼容性。

## 13. 超分辨率（SR）

### 13.1 浏览器端 SR

- 原生分辨率不高于 256×192 时可以提供浏览器 SR。
- 更高分辨率默认关闭浏览器 SR，因为移动浏览器帧率通常不足，且原始清晰度已经较高。
- 是否启用最终由客户端性能检测决定；设备信息中的上限是产品建议，不是浏览器能力保证。
- 浏览器 SR 输入是灰度图，SR 后再应用伪彩，避免为每个色板重复推理。

### 13.2 设备端 SR

- 通过 `/frame.jpg?source=sr` 和 `/stream.jpg?source=sr` 输出。
- 设备信息必须同时声明原生输入和 SR 输出尺寸。
- SR JPEG 只用于显示，不宣称逐像素温度。
- `source_frame_number` 和 `timestamp_ms` 应指向生成该 SR 图的原生帧。

### 13.3 左右拖动对比

对比功能只是让普通用户肉眼观察 SR 效果，不要求严格同步阻塞：

- 客户端可以选择时间或帧号最接近的一对原图/SR图。
- 用户正常静止观察时允许相邻帧近似配对。
- 协议仍保留 `source_frame_number`，用于评估延迟和调试。
- 不支持同时双流时，客户端交替请求 `/frame.temp8`（或其他原图）和 `/frame.jpg?source=sr`。

## 14. 客户端拍照和录像

基础产品只采用客户端保存：

```text
设备实时传图 → 浏览器渲染伪彩/SR/温标 → Canvas → 拍照或 MediaRecorder → 手机/电脑
```

- 不定义设备相册 API。
- 不占设备 Flash 或 SD 卡。
- 拍照可以保存用户当前看到的伪彩、SR和温标效果。
- 原始热数据导出可以作为客户端高级功能，但不属于 v1 基础拍照按钮。
- 录像编码格式由浏览器支持能力决定，客户端应检测 `MediaRecorder.isTypeSupported()` 并提供失败提示。

## 15. OTA

### 15.1 统一接口

```text
POST /api/v1/ota
GET  /api/v1/ota/status
```

上传请求：

```http
Content-Type: application/octet-stream
Content-Length: ...
X-Sipeed-Request: 1
```

设备信息必须声明 OTA `format`、`max_bytes`、硬件版本和回滚能力。HTTP接口统一不代表不同型号共用同一个固件文件。

成功接收并开始验证：

```http
HTTP/1.1 202 Accepted
Content-Type: application/json
```

```json
{
  "accepted": true,
  "bytes": 235416,
  "state": "verifying",
  "rebooting": false
}
```

### 15.2 状态 JSON

```json
{
  "state": "receiving",
  "received_bytes": 131072,
  "total_bytes": 235416,
  "progress": 0.5568,
  "current_firmware": "1.2.3",
  "last_error": null
}
```

`state` 建议枚举：

```text
idle, receiving, verifying, accepted, installing, rebooting, complete, failed
```

### 15.3 安全和可靠性

设备至少必须校验：

- 文件 magic/容器格式；
- 产品型号；
- 硬件兼容版本；
- 文件长度；
- CRC、hash 或数字签名；
- 写入地址不得覆盖 bootloader、校准区和网络配置区。

支持 A/B 或回滚的产品必须在新固件成功启动后再确认镜像。上传超时、断电或校验失败不得破坏当前可启动固件。

### 15.4 TN160 当前实现边界

本仓库当前 TN160 实现会在设备信息中声明 `"rollback": false`。它只校验镜像长度、CRC 和 RP2350 向量范围，尚未实现签名、产品型号/硬件匹配或掉电回滚。因此它尚不满足本节 15.3 及 20.1 的 OTA 量产验收要求；只能在可回退的非生产板上联调，不能把该端点视为量产升级能力。

## 16. CORS 和写接口保护

### 16.1 只读接口

设备信息、图传、温度读取、校准下载、测温默认值和 OTA 状态等只读接口允许第三方网页访问：

```http
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, HEAD, OPTIONS
Access-Control-Expose-Headers: X-Sipeed-Meta, X-Sipeed-JPEG-Quality, X-Sipeed-Protocol, X-Sipeed-Format, X-Sipeed-Width, X-Sipeed-Height, X-Sipeed-Stride-Bytes, X-Sipeed-Payload-Bytes, X-Sipeed-Meta-Bytes, X-Sipeed-Record-Bytes
```

不使用 Cookie，不返回：

```http
Access-Control-Allow-Credentials: true
```

CORS只解除浏览器同源读取限制。HTTPS网站通过Fetch访问设备的HTTP接口仍可能被浏览器按mixed content拦截；第三方网页集成应使用设备内置页面、HTTP开发页面、原生应用或符合目标浏览器安全策略的本地方案，不得把CORS误当成HTTPS兼容机制。

### 16.2 写接口

网络修改、测温参数修改和 OTA 等写接口：

- 默认不返回 `Access-Control-Allow-Origin: *`；
- 必须要求 `X-Sipeed-Request: 1`；
- 必须严格检查 `Content-Type`；
- 浏览器请求带 `Origin` 时，只接受与当前页面同源的 Origin；
- 没有 Origin 的原生 SDK 仍必须带 `X-Sipeed-Request: 1`。

这样可以阻止普通恶意网页通过简单 HTML form 对已连接设备发起配置或 OTA。设备仍是物理 USB 信任模型，不应对外网暴露 HTTP 服务。

## 17. 官方网页实现要求

### 17.1 启动顺序

```text
1. GET /api/v1/device
2. 根据 formats[] 选择最佳输入
3. 初始化尺寸、旋转、温标坐标空间和色板
4. 首选 /stream.xxx
5. 流式 Fetch 不可用或实测不稳定时回退循环 /frame.xxx
6. 根据能力启用 SR、测温、OTA等 UI
```

建议输入选择顺序并非强制：

```text
temp8
→ raw16 + 已支持的浏览器测温算法
→ raw8
→ jpg
```

网页禁止硬编码 `160×120`、固定 metadata 长度之外的 record 长度、产品型号或 SR 输出尺寸。

### 17.2 未压缩流解析

客户端从响应头取得 `payload_bytes` 和 `record_bytes`。每累计一个完整 record：

```text
payload = record[0 .. payload_bytes-1]
meta    = record[payload_bytes .. payload_bytes+119]
```

依次检查：

1. magic、major version 和 `meta_bytes`；
2. metadata CRC；
3. `meta.payload_bytes` 与响应头一致；
4. 格式、width、height、stride；
5. payload CRC（如果 flag 有效）；
6. 再交给显示或测温流程。

生产网页应使用固定容量 ring buffer 或增量解析，避免每次 chunk 都重新分配并复制整个历史 buffer。

### 17.3 坐标和旋转

先在原生坐标中处理 H/L/C，再映射到显示画布。协议中的 `display_rotation` 仅是显示建议：

```text
0：不旋转
1：顺时针 90°
2：180°
3：顺时针 270°
```

Canvas缩放、CSS布局和设备横竖屏不得反向修改协议坐标。

## 18. 固件实现指南

### 18.1 帧生命周期

推荐生产者/消费者结构：

```text
采集 DMA/IRQ
  → 完成原始帧 token
  → 成像/测温处理
  → 发布不可变 frame view（payload + metadata）
  → HTTP消费者 acquire/release token
```

要求：

- DMA接收重装不得依赖可能被网络阻塞的主循环轮询。
- 发布后的 payload 和 metadata 在所有消费者 release 前不得被覆盖。
- 慢客户端导致缓冲不足时丢弃旧显示帧，不阻塞传感器采集。
- `/frame.*` 在响应发送完成前持有对应帧 token。
- `/stream.*` 每发完一帧立即 release，再 acquire 最新可用帧。
- JPEG编码、Base64和TCP发送不得在高实时采集IRQ中执行。

### 18.2 内存与发送

- 不要求为了追加 metadata 再复制整张图；TCP层可以先发送 payload，再发送独立120-byte metadata。
- metadata建议每个已发布 frame 一份，不使用全局可变结构。
- JPEG part header 可以按帧写入小型固定缓冲，Base64后的metadata固定160字符。
- 大分辨率产品应优先使用 USB HS，并限制同时流数量。

### 18.3 带宽估算

以下为25fps、未计 TCP/NCM/USB 开销的约数：

| 原生分辨率 | 8-bit + meta | 16-bit + meta |
| ---: | ---: | ---: |
| 160×120 | 0.48 MB/s | 0.96 MB/s |
| 256×192 | 1.23 MB/s | 2.46 MB/s |
| 384×256 | 2.46 MB/s | 4.92 MB/s |
| 640×512 | 8.19 MB/s | 16.39 MB/s |

计算公式：

```text
bytes_per_second = (stride_bytes × height + 120) × fps
```

640×512 RAW16 等高带宽模式必须使用 USB HS 或降低帧率；不能只依据理论USB线速评估，还要预留 NCM、TCP、协议调度和主机抖动余量。

### 18.4 metadata 序列化

推荐定义公共头文件和跨语言测试向量，但不要直接假设所有编译器结构体布局相同。

C实现至少应：

```c
static_assert(sizeof(struct sipeed_thermal_meta_v1) == 120,
              "thermal metadata ABI mismatch");
```

并对关键 `offsetof` 做静态断言。Rust、Python和JavaScript解析器必须使用明确 little-endian 读取。

### 18.5 2350/低资源 MCU 建议

- 可以只实现 `temp8`、`/frame.temp8`、`/stream.temp8`、设备信息和OTA。
- `payload_crc32`可选，但metadata CRC必须保留。
- 不支持机内JPEG/SR时，通过能力声明关闭，不要提供空接口。
- 网页资源可以压缩/嵌入Flash，但不得占用传感器校准分区。

### 18.6 高性能/高分辨率产品建议

- 提供 `raw16` 或 `temp16` 给专业SDK，提供 `temp8`或`jpg`给普通网页。
- 支持机内SR时只输出灰度`jpg`。
- 校准大表支持HTTP Range和浏览器缓存。
- 明确 USB HS 实际持续吞吐、最高帧率和最大同时客户端数。

### 18.7 型号与驱动命名

- USB产品字符串、网页、`/api/v1/device` 和公开SDK只使用真实产品型号与 `Sipeed Thermal` 系列名称，不暴露开发板样例摄像头名称。
- 从普通摄像头工程复用代码时，不得继续用 `gc0328`、`ov5640` 等具体可见光sensor名称承载通用NCM、HTTP、JPEG或热成像逻辑；通用模块使用 `thermal_sensor`、`thermal_stream`、`thermal_http` 等中性名称。
- 只有确实访问对应芯片寄存器的驱动文件才保留具体sensor型号；更名时不得顺带改动寄存器表、DVP/MIPI时序或DMA行为。
- 不同产品的 `device.model` 必须稳定且精确，例如 `T160`、`T256S`；传感器供应商或芯片料号如需公开，应另加可选字段，不得替代产品型号。

## 19. 版本兼容

### 19.1 API版本

- `/api/v1/*` 的 major version 表示不兼容变更。
- JSON增加字段属于向后兼容；客户端必须忽略未知字段。
- 删除字段、改变含义或改变既有枚举需要新 major version。

### 19.2 metadata版本

- `version_major=1` 和 `meta_bytes=120` 是本规范固定值。
- v1 minor只能使用保留字段增加可忽略信息，不能移动现有字段。
- 不认识 major version 的客户端必须停止解析。
- 新设备不得把产品私有诊断数据占用公共保留字段；诊断应使用独立 `/api/v1/diagnostics`。

### 19.3 旧接口

已经量产并已有第三方依赖的产品可以保留旧接口至少一个完整产品周期，但新接口必须使用本规范的内容，禁止只改路径而沿用误导格式。未量产的内部测试接口应直接删除，避免永久维护两套线格式。

典型旧接口说明：

- T160开发阶段曾使用 `/stream.raw` 和160-byte `T160META`，未量产；统一协议固件直接删除该接口，只提供120-byte `SITHMETA` 的 `/stream.temp8` 和 `/frame.temp8`。
- T256s旧 `/stream.raw.mjpg` 实际是已经归一化、着色并JPEG压缩的图，不是 RAW。
- 旧 `/stream.mjpg` 可以保留；统一接口使用 `/stream.jpg`。
- 旧OTA接口可以映射同一底层handler，但新状态和错误必须返回统一JSON。

新SDK只读取 `/api/v1/device` 提供的新URL，不依赖旧路径。

## 20. 一致性测试与交付检查表

### 20.1 协议自动化测试

每款设备至少通过：

1. `/api/v1/device` JSON可解析，所有声明URL真实存在。
2. 同一解析器无需判断型号即可处理该设备的所有声明格式。
3. 随机把连续流拆成1～4096-byte chunk，仍能正确重组。
4. `/frame.*`每次只返回一个完整帧，长度严格正确。
5. metadata大小、字段offset、magic和CRC正确。
6. payload CRC有效时与实际payload一致。
7. temp8/temp16公式能还原已知测试温度。
8. H/L/C坐标位于原生传感器范围，旋转后显示位置正确。
9. JPEG每个part的metadata与该part JPEG帧号一致。
10. 改变浏览器色板不会改变设备图传payload。
11. 不支持的格式未出现在设备信息中，并返回明确错误。
12. CORS只读接口可从第三方测试页读取；写接口跨域失败。
13. OTA拒绝错误型号、超长、损坏和硬件不匹配的镜像。
14. OTA失败或断电不覆盖校准数据和现有可启动固件。
15. 修改网段后设备IP、DHCP、DNS和网页链接同步改变。

### 20.2 RAW16测温测试

1. manifest的每张表长度、类型、点数、CRC与下载数据一致。
2. 不同gain mode选择正确表。
3. calibration ID变化后客户端拒绝使用旧缓存。
4. 浏览器使用默认参数的结果与设备/产线参考实现一致。
5. 修改发射率、距离、反射温度只影响客户端温度计算，不改变RAW16。
6. 典型温区、高低增益切换、FFC前后均满足产品测温误差要求。

### 20.3 稳定性测试

1. 连续图传至少运行1小时，无帧割裂、纯色半帧或采集链停顿。
2. 慢客户端、暂停读取、反复连接不会拖死采集DMA。
3. 网页拍照、录像、SR、OTA状态轮询不会破坏图像流。
4. Android Chrome、系统WebView、iPhone Safari和桌面Chrome至少覆盖产品要求版本。
5. 不支持流式Fetch的浏览器自动回退 `/frame.*`，且能够恢复连接。
6. USB反复拔插、主机休眠恢复和网络重新枚举后能够重新获得DHCP地址。

### 20.4 研发交付物

每个产品研发团队交付：

- `/api/v1/device` 实际响应样本；
- 支持格式和最大帧率表；
- 120-byte metadata C/Rust结构及字段offset测试；
- 至少3个完整二进制测试帧；
- JPEG multipart抓包样本（如支持）；
- 校准manifest和测试表（如支持RAW16温度恢复）；
- OTA错误用例结果；
- 浏览器兼容和1小时稳定性报告；
- 旧接口兼容清单。

## 21. 最小合规组合

### 21.1 低资源显示/测温设备

```text
NCM + DHCP + DNS
/api/v1/device
/api/v1/network
/frame.temp8
/stream.temp8
120-byte metadata
内置网页、客户端伪彩、拍照录像
可选OTA
```

### 21.2 专业RAW16设备

```text
上述网络和设备信息
/frame.raw16
/stream.raw16
/api/v1/measurement
/api/v1/calibration/manifest
/api/v1/calibration/data/{id}
浏览器/SDK端温度计算
```

### 21.3 机内SR设备

```text
至少一种原生灰度/温度格式
/frame.jpg?source=sr
/stream.jpg?source=sr
JPEG multipart X-Sipeed-Meta
source_frame_number
设备信息声明输入/输出尺寸和并发能力
```

## 22. 研发不得自行改变的公共约定

以下内容必须由所有产品保持一致：

- 空白设备默认 `192.168.10.1/24`、完成设备统一 `192.168.17.1/24`，并支持可配置 `192.168.xxx.1/24`；
- 公共入口 `ir.sipeed.com`；
- `/api/v1/device`、`/frame.<format>`、`/stream.<format>` 路径体系；
- 格式名称和处理阶段含义；
- 120-byte metadata 的字节布局、little-endian和float32；
- H/L/C原生坐标语义；
- temp16/temp8的 `pixel × scale + offset` 公式；
- JPEG multipart中 `X-Sipeed-Meta` 与纯JPEG body的组织；
- 灰度传输和客户端14色板原则；
- 客户端拍照录像原则；
- OTA路径、状态JSON和校准区保护；
- 只读CORS开放、写接口不开放任意跨域。

如果新产品确实无法满足某个强制约定，应先升级本公共规范或增加明确的新major version，禁止在相同版本号下赋予已有字段不同含义。

---

文档维护建议：各产品发现协议缺项时，先提交公共规范变更和跨型号影响分析，再修改产品固件。产品私有诊断字段、实验参数和内部日志不进入公共 metadata ABI。
