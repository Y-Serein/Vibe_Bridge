# pico_tn160

`pico_tn160` 是一个基于 RP2350、TinyUSB 和 PIO/DMA 的热成像采集项目。当前主线围绕 `160x120` 热成像数据采集、双核缓冲处理、UART 输出、USB UVC 输出、双点校准和 Flash 校准数据持久化展开。

## 项目目标

- 采集 `160x120` 热成像原始数据
- 通过双核结构降低采集与输出之间的互相阻塞
- 支持通过 UART 输出原始或归一化图像数据
- 支持通过 USB UVC 输出电脑可识别的视频流
- 支持双点校准，生成背景图、增益表、坏点表
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

运行时可通过串口命令切换波特率、输出格式、校准模式和 Flash dump 模式。更详细的协议说明见 [docs/interfaces.md](docs/interfaces.md)。

### UVC

UVC 参数当前可确认如下：

- 分辨率：`160x120`
- 帧率：`22fps`
- 像素格式：`YUY2`

当前流程是：

- 先将内部灰度帧通过色图 LUT 转换
- 再编码为 `YUY2`
- 最后通过 TinyUSB 作为 UVC 设备输出

### CDC

仓库中保留了 `cdc_func()` 测试模式，用于 USB CDC 串口验证，但当前主线默认不走这一模式。

## 校准与 Flash

当前主线支持双点校准。

校准流程大致为：

1. 进入校准模式
2. 采集两张参考图
3. Core1 调用 `start_cali()`
4. 计算：
   - 背景图
   - 增益表
   - 坏点表
5. 写入 Flash
6. 通过 UART 回传校准结果
7. 重启

当前 Flash 目标区域为最后 `512KB`，详细偏移见 [docs/development.md](docs/development.md)。

已知限制：

- 当前坏点“识别与写入”流程存在
- 运行时坏点修复逻辑仍未真正完成

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
- 板级 ID / Flash UID / ADC / 快门 / GPIO / CDC 等测试模式存在

还没有完全闭环的内容：

- 坏点修复逻辑未完成
- UVC 色图切换预留了入口但未完成
- `uvc.c` 基本为空，是否废弃仍需确认
- `tools/backup` 中哪些仍然有效尚未清理
