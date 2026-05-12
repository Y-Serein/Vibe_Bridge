# pico_tn160

`pico_tn160` 是一个基于 RP2350、TinyUSB 和 PIO/DMA 的热成像采集项目。当前主线围绕 `160x120` 热成像数据采集、双核缓冲处理、UART 输出、USB UVC 输出、双点校准、自动 FFC、温度范围估算和 Flash 校准数据持久化展开。

## 项目目标

- 采集 `160x120` 热成像原始数据
- 通过双核结构降低采集与输出之间的互相阻塞
- 支持通过 UART 输出原始或归一化图像数据
- 支持通过 USB UVC 输出电脑可识别的视频流
- 支持双点校准，生成背景图、增益表、坏点表
- 支持运行时坏点修复
- 支持自动 FFC，刷新背景偏置并补偿 FPA 温漂
- 支持基于双点标定的温度范围估算
- 将校准结果写入 Flash，并在后续启动时加载

## 当前主线与默认业务逻辑

默认模式来自 [src/sdk.h](src/sdk.h)：

- `MAIN_FUNC_MODE = MAIN_FUNC_MODE_SPI_INIT_40MHZ_4`

对应入口：

- `main() -> spi_init_40MHz4()`

当前主线流程可以概括为：

- 传感器输出原始热成像数据
- Core0 通过底层采集链路接收原始包并写入 `rxring`
- Core0 根据包头/包尾规则将原始包组装成完整帧并写入 `rawbuf`
- Core1 从 `rawbuf` 读取 `RAWBUF_READY` 帧
- Core1 将帧处理成：
  - `norm_buf`，用于 UART 输出
  - `uvc_buf`，用于 UVC 输出
- Core1 在正常输出过程中执行：
  - 校准背景扣除
  - 增益校正
  - 坏点修复
  - 百分位平滑
  - 温度范围映射
  - 自动 FFC 判断与执行
- 如果处于校准模式：
  - Core1 采集两张参考图
  - 调用 `start_cali()`
  - 计算并写入校准数据

### Core0 负责

- 初始化采集环境
- 从底层链路接收原始包
- 将原始包写入 `rxring`
- 根据包头/包尾规则组装成完整帧
- 将完整帧写入 `rawbuf`
- 在帧尾进行：
  - ADC 收尾
  - 快门状态切换
  - 当前帧标记为 `RAWBUF_READY`
  - 申请下一帧缓冲
  - 采集同步重启

### Core1 负责

- 初始化 UART、DMA、TinyUSB、按键扫描
- 轮询串口命令并切换运行模式
- 从 `rawbuf` 读取 `RAWBUF_READY` 帧
- 将图像处理为 `norm_buf`
- 根据当前模式执行：
  - UART DMA 输出
  - UVC 帧准备与 USB 输出
  - 自动 FFC
  - 校准流程
  - Flash 校准数据导出

### 数据链路

- `传感器 -> rxring -> rawbuf -> norm_buf / uvc_buf -> UART / UVC`

注意：

- Core0 和 Core1 之间走共享缓冲区和状态位
- 不走 Core 间串口通信

## 软件结构

### 主线文件

- [src/main.c](src/main.c)
  - 主入口
  - 模式分发
  - 当前默认业务逻辑
- [src/init.c](src/init.c)
  - SPI/PIO/DMA 初始化
  - 原始包接收与环形缓冲
- [src/sdk.h](src/sdk.h)
  - GPIO 定义
  - 模式开关
  - Flash 布局
  - 全局结构体
- [src/sdk.c](src/sdk.c)
  - 公共状态
  - 错误处理
  - 按键扫描
  - Flash 指针映射
- [src/usb_descriptors.c](src/usb_descriptors.c)
  - USB/UVC 描述符
- [src/utils.c](src/utils.c)
  - 行处理
  - 14bit 到 8bit 映射辅助逻辑
- [src/dutx.c](src/dutx.c)
  - 调试日志输出

### 相关目录

- [src](src)
  - 固件主线源码
- [docs](docs)
  - 当前整理后的开发文档、接口文档和 TODO
- [tools/backup](tools/backup)
  - 历史脚本、抓包、分析 notebook、参考数据

如果只是继续当前主线开发，通常优先关注：

1. [README.md](README.md)
2. [docs](docs)
3. [src/sdk.h](src/sdk.h)
4. [src/main.c](src/main.c)
5. [src/init.c](src/init.c)

## 文档入口

更详细的接手文档已经整理到 [docs](docs)：

- [docs/README.md](docs/README.md)
- [docs/development.md](docs/development.md)
- [docs/interfaces.md](docs/interfaces.md)
- [docs/TODO.md](docs/TODO.md)

建议第一次接手按下面顺序看：

1. [README.md](README.md)
2. [docs/development.md](docs/development.md)
3. [docs/interfaces.md](docs/interfaces.md)
4. [src/sdk.h](src/sdk.h)
5. [src/main.c](src/main.c)
6. [src/init.c](src/init.c)

## 当前支持的输出模式

### UART

支持两类内部输出：

- `UOM_CALI_U8`
  - 8bit 灰度帧
- `UOM_RAW_U16`
  - 原始 16bit 数据流，含附加信息

当前 `UOM_CALI_U8` 输出不仅包含 8bit 灰度帧，还会在帧尾附带温度范围信息：

- `[PX_CNT+3..PX_CNT+4]`
  - 当前帧低温估计，单位 `0.1°C`
- `[PX_CNT+5..PX_CNT+6]`
  - 当前帧高温估计，单位 `0.1°C`

当温度映射尚未建立时，这两个字段返回 `INT16_MAX` 作为 `N/A`。

运行时可通过串口命令切换波特率、输出格式、校准模式和 Flash dump 模式。更详细的协议说明见 [docs/interfaces.md](docs/interfaces.md)。

### UVC

UVC 当前成功基线如下：

- 分辨率：`160x120`
- 帧率：`10fps`
- 像素格式：`YUY2`
- USB 传输：full-speed Bulk UVC
- Bulk endpoint：`wMaxPacketSize=64`
- UVC payload / TinyUSB epbuf：`256`
- 已验证：Windows 可见，Linux/V4L2 可连续抓取 10fps，guvcview 可用

当前流程是：

- 先将内部灰度帧通过色图 LUT 转换
- 再编码为 `YUY2`
- 最后通过 TinyUSB 作为 UVC 设备输出

### CDC

仓库中保留了 `cdc_func()` 测试模式，用于 USB CDC 串口验证，但当前主线默认不走这一模式。

## 校准、FFC 与温度

当前主线支持双点校准、Flash 持久化加载、自动 FFC 和基于双点标定的温度范围估算。

### 双点校准

校准流程大致为：

1. 进入校准模式
2. 采集两张参考图
3. Core1 调用 `start_cali()`
4. 计算：
   - 背景图
   - 增益表
   - 坏点表
   - `mean_diff`
5. 写入 Flash
6. 通过 UART 回传校准结果
7. 重启

当前双点标定会把以下关键量写入 Flash：

- 低温参考背景图
- 每像素增益表
- 坏点表
- `mean_diff`
  - 高低参考帧的平均响应差
- `t_ref_low`
  - 低温参考点温度

当前代码中的高温参考点固定为：

- `T_REF_HIGH = 100.0°C`

也就是说，当前温度映射使用：

- 低温参考点：`t_ref_low`
- 高温参考点：`100°C`

### 运行时图像校正

正常输出时，主线对每帧执行：

1. 原始 `14bit` 数据 Gray 转普通二进制
2. 背景扣除
3. 增益校正
4. 坏点修复
5. 百分位范围统计
6. 低/高范围平滑
7. 线性映射到 `8bit`
8. 估算当前帧温度范围

其中坏点修复已经接入主线，不再只是“识别和写入”。

### 自动 FFC

当前代码已经实现运行时 FFC，不需要每次都重新做完整双点校准。

FFC 的作用是：

- 在快门闭合时采集一帧均匀背景
- 刷新运行时背景图 `ram_calibg`
- 重新计算全局背景均值
- 重置显示百分位平滑器
- 更新温度锚点 `ram_temp_anchor`

这样可以在不改变原始增益表和 `mean_diff` 的前提下，补偿自上次完整标定以来的 FPA 漂移。

当前 FFC 触发条件有两类：

- 首次启动后的短延时触发
- 正常运行阶段按时间或 FPA ADC 漂移量触发

当前阈值为：

- 首次 FFC：`3000 ms`
- 稳态周期 FFC：`120000 ms`
- FPA ADC 漂移阈值：`150`

### 温度映射与温漂补偿

当前代码已经把温度映射从“仅有双点标定数据”推进到“带 FFC 温漂补偿”的状态。

当前温度映射核心依赖：

- `ram_mean_diff`
- `ram_t_ref_low`
- `ram_temp_anchor`

运行时不再直接使用当前背景均值作为温度零点，而是使用：

- 低温参考帧均值 `ram_l_ref_mean`
- 配合 FFC 后更新的 `ram_temp_anchor`

这样可以避免“FFC 后快门温度变化导致绝对温度整体偏移”的问题。代码里已经显式修正了这类常数偏移误差。

当前 Flash 目标区域为最后 `512KB`，详细偏移见 [docs/development.md](docs/development.md)。

已知限制：

- 当前输出的是“当前帧温度范围”，不是完整逐像素温度矩阵
- 低温参考点仍由标定数据决定，当前默认产测流程里写入的是 `0.0°C`
- 高温参考点当前写死为 `100°C`，如果产线标定点变化，需要同步修改固件常量
- FFC 主要补偿偏置漂移，不会替代完整双点标定

## 构建环境

- `CMake + Ninja`
- `pico-sdk`
- `TinyUSB`
- `Arm GNU Toolchain`

项目自带脚本 [cdk](cdk) 依赖：

- `PICO_SDK_PATH`
- `PICO_TOOLCHAIN_PATH`

- Arm GNU Toolchain Downloads: <https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads>
- 常见 WSL x86_64 环境使用：
  - `arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi.tar.xz`

### `cdk` 常用命令

- `./cdk b`
  - 格式化并构建
- `./cdk r`
  - 清理后重构建
- `./cdk c`
  - 删除 `build`
- `./cdk f`
  - 格式化代码
- `./cdk a`
  - 构建并复制 UF2 到设备

注意：

- `cdk` 中有本机路径和挂载设备的写死依赖
- 接手前应先本地化

## 固件烧录

1. 下载固件和烧录程序
   - 固件 `sha256`:
     - `be941b84b7265ced7d715bae93dbf48c3704d0fa722575c79824bbf4c5927131`
   - 烧录程序：
     - 构建于 `https://git.corp.sipeed.com/iawak9lkm/th160flash`
2. 双击打开烧录程序
3. 点击 `Select Fireware`，选择固件
4. 点击 `Start Monitoring`
5. 按住 `Th160` 的 `BOOT` 按钮上电，等待自动检测并烧录
6. 显示绿色 `SUCCESS` 后拔掉设备

## 产测 / 双点校准流程

以下内容保留自旧文档，作为当前可参考的产测流程草稿。

### 环境准备

1. 下载产测软件（Windows）
   - 构建于 `https://git.corp.sipeed.com/iawak9lkm/th160pt`
2. 安装 `PotPlayerSetup64`
3. 准备两条 Type-C 线：
   - 一条接产测电脑
   - 一条接电源适配器

术语约定：

- 连接电脑的线：数据线
- 连接电源适配器的线：电源线
- `USB 转串口模块`：串口模块
- `Thermal160`：`th160`

### 当前产测步骤

注意：以下流程不包含烧录固件，仅为双点校准。

1. 打开产测软件 `th160pt_xxx.exe`
2. 将数据线接到串口模块，再将串口模块连接到 `th160`，注：串口模块要求2M-5M的波特率
3. 将电源线接到 `th160`
4. 连续按两次 `RST`
5. 在产测软件中刷新串口并确认设备探测成功
6. 点击开始校准
7. 对准低温平面，稳定后按下 `BOOT`，观察 LED
8. 对准高温平面，稳定后再次按下 `BOOT`
9. 等待校准结果
10. 连续按两次 `RST`，打开串口预览，观察图像是否清晰
11. 再切换到 UVC 预览，观察图像是否清晰、是否正常出图

### 常见不良项

- 无法探测设备
- 校准灯不亮
- 串口模糊
- UVC 模糊
- UVC 不出图

## 当前状态

已经可以从源码确认的内容：

- 默认主流程是 `spi_init_40MHz4()`
- Core0/Core1 双核架构已打通
- UART 输出链路存在
- UVC 输出链路存在
- 双点校准链路存在
- Flash 校准数据写入和读回校验存在
- 运行时坏点修复已经接入主线
- 自动 FFC 已接入主线
- FFC 后温度锚点更新和温漂补偿已接入主线
- 当前帧温度范围估算已接入 UART 输出
- 板级 ID / Flash UID / ADC / 快门 / GPIO / CDC 等测试模式存在

还没有完全闭环的内容：

- 有时FFC的快门会留下一个残影在屏幕上