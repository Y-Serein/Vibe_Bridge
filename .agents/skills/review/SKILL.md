---
name: review
description: Review the current working tree before finishing a task, focusing on over-engineering, unrelated changes, risks, and missing validation.
---

# Review Skill

对当前工作区做一次交付前 review。

## 目标

像一个对代码洁癖到变态的 Staff Engineer 一样检查本次改动。

重点不是夸奖，而是找问题：

- 是否按目标 -> 状态 -> 误差 -> 控制动作 -> 反馈 -> 修正 -> 验证 -> 沉淀形成闭环。
- 是否清楚说明本轮控制了哪些变量，反馈是否真实支持结论。
- 是否有无关改动。
- 是否有过度设计。
- 是否有可以更简单实现的地方。
- 是否改动范围过大。
- 是否引入不必要依赖。
- 是否破坏公开 API、协议、配置格式或兼容性。
- 是否缺少验证。
- 是否有安全隐私风险。
- 是否有 secrets、tokens、API key、Cookie、私密配置泄露。

## 推荐检查命令

优先运行只读命令：

```bash
git status --short
git diff --stat
git diff
```

如果项目有明确测试命令，再运行最小相关验证。

## 输出格式

```
## Review 结论

- 是否建议交付：是 / 否 / 有条件

## 主要问题

- ...

## 建议简化

- ...

## 验证情况

- 已运行：
- 结果：
- 未验证：

## 风险

- ...
---

```



