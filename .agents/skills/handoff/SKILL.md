---
name: handoff
description: Write or update ./HANDOFF.md so the user or next AI session can resume the project in 30 seconds after several days.
---

# HANDOFF Skill

你要把当前 session 状态写入或更新到 `./HANDOFF.md`。

目标：让用户或下一轮 AI 在 3 天后打开文件，30 秒内接上工作。

默认按工程控制论闭环记录：目标 -> 状态 -> 误差 -> 控制动作 -> 反馈 -> 修正 -> 验证 -> 沉淀。HANDOFF 必须能说明当前目标、系统状态、目标与现状误差、已控制的变量、真实反馈、未收敛偏差和下一步验证方式。

## 触发条件

只有以下情况才使用本 skill：

- 用户明确要求“写 HANDOFF / 更新 HANDOFF / 交接 / 保存当前状态”。
- 长任务暂停、结束或准备切换 session。
- 多步排查后产生关键上下文、失败尝试或未解决问题。
- 当前上下文复杂，继续对话可能丢失关键线索。

小修小改、简单问答、一次性命令，不要更新 HANDOFF。

## 写入位置

`./HANDOFF.md`

## 固定结构

必须使用以下结构，标题不要改名：

```markdown
# HANDOFF.md

最后更新：YYYY-MM-DD HH:mm
当前分支：
当前任务：

## 当前在做什么

## 已经试过的方案和结果（含失败的）

## 下一步计划（3-5条actionable)

## 关键文件路径（相对路径，一行一个）

## 当前验证状态

## 还没搞清楚的问题
```



## 写入要求

- 只写当前真实状态，不写套话。
- 失败方案必须写清楚结果和线索。
- 下一步计划必须是 3-5 条可执行 action。
- 关键文件路径必须使用相对路径，一行一个。
- 当前验证状态必须写清：
  - 已运行什么命令。
  - 结果是什么。
  - 哪些没有验证。
  - 建议下一步怎么验证。
- 如果反馈与预期不一致，必须写清偏差来源、已排除项和下一轮修正动作。
- 不写 secrets、tokens、API key、Cookie、私密配置。
- 不要把完整长日志、大段报错、私密配置原样塞进 HANDOFF。
- 如果已有 HANDOFF，更新它，不要无脑追加重复内容。

---

