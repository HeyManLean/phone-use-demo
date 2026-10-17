# 绝对指针手势排查记录

ESP32-S3 N16R8 + iPhone 17 / iOS 27，`esp32-iphone-agent` 的「点了没反应、反复点同一处」问题。

## 已验证的事实（有证据）

| 结论 | 证据 |
|---|---|
| 投屏链路本身没坏 | 实时状态 `ap=streaming fps=4-5 aus` 持续增长 |
| 抓到的「冻结画面」是测试方法错误 | GOP 从投屏开始累积，`-frames:v 1` 取的是**第一帧**；取最后一帧即为主屏 |
| 绝对 HID 能驱动光标与点击 | `tap 8500 10000` 后画面变化 239 kbps |
| 固件按钮报告实现正确 | `USBHIDMouse.cpp:53-59` 走虚函数，绝对实现 `:89-93` 会带上 `_buttons` |
| 回滚到 Anan 可行 | 备份 16 MB SHA256 `D8057B46…` 未变；esptool 5.4.0 校验 `Checksum/Validation hash` 均 valid |
| 模型坐标是 0–1000 归一化 | `AgentLoop.swift:391` `(a["x"] ?? 0) / 1000`，`Pointer.swift` 再乘 `absMax` |

## 已修复的代码缺陷

| 提交 | 缺陷 | 影响 |
|---|---|---|
| `599a2fc` | 绝对指针仍加载相对时代的校准文件，`dx/dy` 套用历史 `seen` 修正 | 模型目标与实际发送坐标不一致，可**静默点偏** |
| `599a2fc` | 两点拖拽按下后直接跳到终点，仅 8 ms | iOS 可能不认作拖拽 |
| `599a2fc` | 超过容量或带非法后缀的 `drag` 被静默截断并回 `ok` | 轨迹被截短但仍报成功 |
| `599a2fc` | 校准直接返回成功，不测任何东西 | 掩盖真实偏差 |
| `f791fd1` | 提示词仍写「precise 会定位纠偏」「滑动无惯性，偏好短滑」 | 模型按相对鼠标的旧规则行动 |
| `f791fd1` | 提示词 `path` 上限 20 点，固件与圆形轨迹需更多 | 轨迹被截断 |
| `48b2cfa` | `CMSampleBufferCreateReady` 未给时序条目 | 帧可能被挂起到下一帧才输出，而点击后屏幕恰好静止 |
| `48b2cfa` | 批内动作间隔 120 ms | iOS 可能合并相邻点击 |
| `ae49a3c` | `drag` 参数校验排在 USB 检查之后 | 非法命令仍可能移动光标或留下按住状态 |

## 验证状态

| 项目 | 状态 |
|---|---|
| 固件编译 `devkitc1_n16r8_debug` | ✅ RAM 29.3% / Flash 22.4% |
| 拖拽/参数主机回归（读真实固件代码） | ✅ `tools/test-absolute-drag.py` |
| Swift 语法检查 | ✅ |
| iOS 类型检查 / XCTest | ❌ 需 Mac |
| 真机是否点中目标 | ❌ **未验证** |
| 死循环是否消失 | ❌ **未验证** |

## 仍需真机确认

1. 校准输出 `absolute probe: target …, landed …, error …`：误差 < 1.5% 说明坐标链路正确
2. 单步日志 `absolute tap: model …,… (0-1000) -> HID …,…`：确认目标与发送一致
3. 步骤回放的 `changed` 字段：确认点击后是否仍被判定为「屏幕未变化」

## 回滚

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\restore-flash.ps1
```
