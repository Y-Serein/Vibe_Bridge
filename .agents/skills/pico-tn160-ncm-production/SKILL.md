---
name: pico-tn160-ncm-production
description: 接手和闭环 pico_tn160 + Serein Scope 的 USB CDC-NCM 多设备生产标定，包括空白 .10、临时工位 .11-.16、完成 .17、设备 serial 防串台、未完成拔插重做、最终地址互斥、生产 UI 简化、Windows Release 打包与真机验收。凡用户提到 TN160 NCM、USB 标定、同时连接多台、IP 冲突、生产标定页面、标定完成后拔出、生成 Scope 测试包或“按上次效果一次完成”，都应优先使用本 Skill。
---

# TN160 USB NCM 多设备生产闭环

## 目标

把固件、USB 网络、Scope 工位调度、操作员 UI、Release 包和真机反馈作为一个系统闭环。交付标准不是“代码能编译”，而是：每台空白设备可被唯一分配、独立标定、验证后安全切换到统一用户地址，并且下一台不会产生 IP 冲突。

## 开工顺序

1. 完整读取：
   - 当前仓库 `AGENTS.md`
   - 当前仓库 `HANDOFF.md`
   - `C_context/MEMORY.md`
   - `docs/runbooks/PROJECT_MEMORY.md`
   - `/home/slam/Sipeed/C_context/KNOWN_FAILURES.md`
   - `/home/slam/Project/yserein-scope/AGENTS.md`
   - `/home/slam/Project/yserein-scope/HANDOFF.md`
2. 运行：

   ```bash
   python3 /home/slam/tools/agent_preflight.py --project pico-tn160
   ```

3. 检查两个仓库的 `git status --short`。Scope 通常有大量用户未提交改动，只修改明确相关文件，不 reset、restore、clean、commit 或 push。
4. 第一条状态汇报按：`目标 → 状态 → 误差 → 控制动作 → 反馈 → 修正 → 验证 → 沉淀`。用户要求“先汇报/不要改代码”时，到获得明确修改授权前只做只读检查。

## 产品逻辑授权门

产测步骤和状态机是用户拥有的产品契约。修改以下任一内容前必须先停止编码，给出状态表并获得用户对具体变化的明确确认：

- 步骤顺序或前后依赖。
- 按钮可见、启用、点击结果。
- 自动扫描、连接、重连、断开或清空行为。
- 超时、节拍告警、继续、FAIL 的语义。
- RAW/遥测/FFC/标定数据的接受或拒绝条件。
- 写入、回读、最终地址和拔出状态。

状态表至少包含：触发条件、当前行为、建议行为、按钮状态、点击结果、数据是否接受、日志、下一状态、回退。把未知项交给用户确认，不能用代理的产品判断补齐。

以下表达不构成具体逻辑授权：`修一下`、`优化`、`继续`、一般性同意、质疑现象、认可方向、问句。只有用户明确确认状态表或精确行为差异后才能修改。

永久防回归规则：`120 s 只作节拍告警、不自动 FAIL` 只说明它不是失败出口；它不自动意味着解锁、允许采集、绕过状态 B 门槛或接受覆盖不足数据。第 4/5 步的正确边界当前是未决产品语义，必须重新向用户确认，不能从旧源码、测试、Memory 或本 Skill 推断。

实现获批状态机时，按钮启用、点击 handler、日志/tooltip、数据接受和测试必须消费同一份规则，禁止在 `updateControls()` 和 `capture*()` 维护两套条件。

## 冻结的地址状态机

```text
空白设备：192.168.10.1
临时工位：192.168.11.1 ～ 192.168.16.1
完成设备：192.168.17.1
```

- 有效持久化网络设置优先。刷入新固件不会把已经保存 `.17` 的设备自动改回 `.10`。
- 无有效网络设置时：有有效标定回退 `.17`；无有效标定回退 `.10`。
- 每次只新增一台 `.10`。两台相同 `.10` 同时接入时，IP/HTTP 层不可可靠区分；不要声称软件可以唯一识别。
- `.10 → .11～.16` 后记录设备 serial，并在目标地址重新出现时核对同一 serial；不一致就报告地址冲突，不能绑定工位。
- 未完成时拔出：保留设备临时 IP，但 Scope 清空本轮内存采集，从第 1 步重做。不要自动恢复 `.10`。
- `.17` 是全局互斥资源。每次最终切换前主动扫描 `.17`；占用时锁住完成队列。
- 完成链路必须是：

  ```text
  临时地址写入标定
  → 重启
  → 临时地址 header + 三段 payload 全量回读
  → 获取 .17 独占权
  → PUT .17
  → 同 serial 重连
  → .17 再次全量回读
  → 在线确认
  → 显示“标定完成 · 请拔出”
  → 检测物理移除
  → 放行下一台
  ```

- `.17` 写入成功但后续验证失败时保守锁止；不能假设地址已经释放。

## 操作员 UI 规则

- 当前已明确的多设备布局目标是一台设备一行：三台显示三行，更多设备纵向滚动；点击哪一行就切换对应 Context/Output。右侧不使用设备选择下拉框，保留扫描设备。
- 中央区域只展示短步骤和当前状态：
  1. `1、对准 0°C`
  2. `2、对准 50°C`
  3. `3、计算`
  4. `4、再次对准 50°C`
  5. `5、回到 0°C`
  6. `6、写入并验证`
- 不向操作员显示 A1/A2/B1/B2、V1、PCB/Sensor、selector、payload 等内部概念。
- 不强制填写现场无法稳定获得的 PCB/Sensor 标签，不保留“新建 Sensor”。
- 导入标定包、导出结果和生产记录属于右侧 Context。
- 0°C、50°C、温差、Offset、Gain、采集质量六个结果属于下方 Output。
- 每个设备行的六步必须在 1100×720 完整显示；设备更多时只纵向滚动，不恢复右侧下拉选择。测试控件时限定目标设备行；多个工位有同名步骤控件。
- `120 s` 不作 FAIL；除此之外的解锁、继续或数据接受语义必须走上面的产品逻辑授权门。

## 改动边界

- 固件默认只改 NCM 网络默认/回退时，不碰 USB descriptor、NTB、PIO、DMA、GPIO、120-byte metadata、TNC1 格式、Flash/OTA 布局。
- Scope 保持插件 ABI 2 和宿主 UI 所有权边界。组件提供 Context/Output 内容，宿主仍拥有 Dock、标题、几何和持久化。
- 只改 NCM 插件且 Core/宿主没变时，只更新组件包。
- 修改 `HttpRequestClient`、`MainWindow` 或其他宿主/Core 后，必须重建完整 Release；只替换插件会留下二进制不一致。
- 不为每次尝试创建 `out/*latest*` 或时间戳副本。复用：
  - 正式包：`/home/slam/Project/yserein-scope/dist/SereinScope.zip`
  - 测试入口：`/home/slam/Project/yserein-scope/out/run-win/SereinScope/SereinScope.exe`
  - 组件目录：`/home/slam/Project/yserein-scope/out/run-win/Components`

## 构建与验证

### 固件

固定入口：

```bash
cd /home/slam/dev/pico_tn160 && source ../env.sh && cmake --preset pico2-release && cmake --build --preset pico2-release --parallel "$(nproc)"
```

构建环境统一由 `../env.sh` 和 `CMakePresets.json` 提供；缺少 lwIP 时必须失败，不要静默下载未知版本。输出仍只有：

```text
/home/slam/dev/pico_tn160/build/pico_tn160.elf.uf2
```

报告字节数和 SHA-256。

### Scope

1. 先在已有 Windows 构建目录做相关测试，避免新建 `out` 目录。
2. 至少验证：
   - NCM Transport：503 可恢复、serial/segment、PUT、连续断连。
   - 插件加载/卸载。
   - 1100×720 当前工位步骤完整显示。
   - Context/Output 语义槽归属。
3. 结束前跑完整 CTest。
4. 用户要求最新包时，说明 `build_win.bat` 会删除并重建 `build-win`、`dist`，获得权限后执行。
5. Release 生命周期 smoke 使用 Windows 原生平台。不要给未部署 offscreen 插件的 Release 包强制 `QT_QPA_PLATFORM=offscreen`。
6. 组件管理器可能把发布名规范化为 ID 文件名；最终按实际目录读回并核对哈希，不凭预期文件名判断丢失。

## 真机验收矩阵

软件通过后仍明确写“未上板验证”，直到至少完成：

1. 一台空白板 `.10` 可达，自动分配 `.11`。
2. 三台依次接入得到 `.11/.12/.13`，serial 不串台。
3. 未完成拔插后保留临时 IP，但从第 1 步重做。
4. 第一台完成到 `.17` 后不拔出，第二台完成被阻止。
5. 拔出第一台后，第二台才获准切换 `.17`。
6. 完成设备在 `.17` 运行协议校验并确认标定 ID/CRC。
7. Windows 多 NCM 网卡 DHCP/路由稳定；最终用户在目标 Android/iOS 只访问 `.17`。

## 失败分类

- HTTP 503/诊断未就绪：可恢复，不累计为物理断连。
- 三次连续网络失败：可判 disconnected，用于最终拔出反馈。
- WSL `UtilBindVsockAnyPort`、PTY 光标查询、Qt 平台插件缺失：先归类环境错误，重试/修正启动方式，不算产品回归。
- 构建、CTest、smoke、哈希只分别证明编译、契约、生命周期和身份；不能替代三设备、黑体、Flash 与拔插真机证据。

## 最终汇报

按以下顺序，保持简短：

1. 改了什么（固件 / Scope / UI / 包）。
2. 唯一可测试入口和 SHA。
3. 已运行的命令与通过数。
4. 未上板风险。
5. 停止并等待用户自行真机测试；不要反复安排额外硬件轮次。只有缺少一项会改变下一次代码决策的证据时，才说明最小所需反馈。

用户要求沉淀或长任务结束时，更新 `HANDOFF.md`、`C_context/MEMORY.md` 和用户可感知的产品迭代日志；不要把原始长日志或私密信息写进去。
