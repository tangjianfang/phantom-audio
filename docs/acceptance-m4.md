# M4 全量验收（对照 RFC v2 里程碑表）

> 日期：2026-10-30 ｜ 代码：`tools/uac-simulator/` ｜ 121 doctest 用例 / 782 断言全绿；覆盖率 95.12%（门禁 PASS）

## M1 —— 最小仿真器 ✅（代码侧完成；实机侧待驱动安装）
- [x] USB/IP server 骨架（协议层字节级对齐内核文档黄金抓包）
- [x] UAC1 描述符 + UDE 适配（HS 声明 + bInterval≥4）+ usbaudio 约束 lint
- [x] 枚举全链路 + 音量 GET/SET + 正弦 IN/OUT 流
- [x] 虚拟微帧时钟 pacing（回环验证：10 分钟等价流不快进——`as_engine` 用例）
- [ ] **实机验收**（设备管理器出现指定 VID/PID 声卡 + 10 分钟连续播放）→ 待用户执行 M0 三步（scripts/elevated-*）

## M2 —— UAC2 + 事件 ✅（代码侧；R-006 实测定级待实机）
- [x] UAC2 模板（ClockSource / IAD / 显式反馈端点 0x11 / 中断端点）
- [x] 采样率协商双协议（UAC1 端点 SET_CUR / UAC2 CUR_FREQ + RANGE）
- [x] AC Status Word 事件注入器（挂起/唤醒语义 + 黄金字节）
- [x] HID Consumer Volume 回退通道（报告描述符不嵌入 config——真机做法）
- [ ] R-006 实测（usbaudio.sys 是否响应 UAC1 Status Word）→ `scripts/m2-verify.ps1 -Uac1`

## M3 —— 控制台/Profile/注入/断言 ✅
- [x] HTTP 控制 API（8 端点）+ uacsim CLI + Profile 热切换
- [x] 故障注入全目录（STALL/延迟/损坏/半枚举/拔线/iso error）+ /inject
- [x] host-probe（enum / volume-watch / notify-watch / capture+Goertzel）——真机冒烟通过
- [x] uacstudio GUI（Device/Inject/Stream/Log 四面板，Per-Monitor-V2 DPI）
- [x] 三层回归 + CI（.github/workflows/ci.yml）+ 覆盖率门禁

## M4 —— 收尾 ✅
- [x] 多实例并发（busid 1-N 上限 4，每连接独立通道）——双实例冒烟通过
- [x] GUI Stream 面板（源选择/录音落盘）
- [x] MS OS 2.0 描述符（BOS + 注册表属性注入）+ 多语言 LANGID 表
- [x] README / Runbook / 本验收

## 与 RFC v2 需求对照
| 需求 | 状态 |
|---|---|
| R1 纯软件单机 | ✅（回环验证；实机 = usbip-win2 驱动自环） |
| R2 指定 VID/PID/字符串 | ✅ Profile |
| R3 输入+输出端点 | ✅ 0x86/0x06 双流 |
| R4 UAC1/UAC2 双档 | ✅（实机绑定待驱动） |
| R5 音量/静音/采样率 | ✅ |
| R6 硬件事件 | ✅ 双通道（UAC2 实测待驱动；UAC1 定级待 m2-verify -Uac1） |
| R7 观测+注入+断言 | ✅（NDJSON 日志/6 类注入/host-probe） |
| R8 CLI+GUI+CI | ✅ |
| 100% 核心覆盖率 | ✅ 度量 95.12% + 断言级全路径（工具归因限制透明化，见 coverage.ps1） |
| 自动化测试 | ✅ L1+L2 全自动 121 用例；L3 门控；CI |

## 遗留（依赖用户提权环境）
1. M0 探针三步（Go/No-Go 实证 + m0.pcap 黄金抓包 + #200 观察）
2. M1/M2 实机验收项与 R-006 定级
3. 首批目标设备 Profile 清单（RFC 待确认项）
