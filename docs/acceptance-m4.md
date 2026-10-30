# M4 全量验收（对照 RFC v2 里程碑表）

> 日期：2026-10-30（评审修复后更新） ｜ 代码：`tools/uac-simulator/` ｜ **172 用例 / 1468 断言全绿**（Debug 与 Release 均已跑）；
> GUI 非视觉自检通过（27 个控件）；`/WX` 下零警告。覆盖率见文末"度量口径"。
>
> **约束**：开发机上不做真机测试；宿主侧（真实 Windows 音频栈）验证在 Windows Sandbox / VM 里做，见 [sandbox.md](./sandbox.md)。
> 标记含义：✅ 已在开发机验证 ｜ 🟡 代码就绪、**待 Sandbox 首次运行** ｜ ⬜ 需要人工确认。

## M1 —— 最小仿真器（代码侧完成；宿主侧待 Sandbox）
- [x] USB/IP server 骨架（协议层字节级对齐内核文档黄金抓包）
- [x] UAC1 描述符 + UDE 适配（HS 声明 + bInterval≥4）+ usbaudio 约束 lint
- [x] 枚举全链路 + 音量 GET/SET + 正弦 IN/OUT 流
- [x] 虚拟微帧时钟 pacing（回环验证：10 分钟等价流不快进——`as_engine` 用例）
- 🟡 设备管理器出现指定 VID/PID 声卡、绑定内置类驱动 → `host.attach_enum`、`host.driver_binding`
- 🟡 流可被 Windows 采集 → `host.capture_tone`（3 秒；含"3 kHz 必须检不出"的对照）
- ⬜ 10 分钟连续播放不漂移 —— 自动用例只流几秒；人工确认

## M2 —— UAC2 + 事件（代码侧完成；R-006 待 Sandbox）
- [x] UAC2 模板（ClockSource / IAD / 显式反馈端点 0x84 / 中断端点）；反馈值 Q16.16（High-Speed）
- [x] 采样率协商双协议（UAC1 端点 SET_CUR / UAC2 CUR_FREQ + RANGE）
- [x] AC Status Word 事件注入器（挂起/唤醒语义 + 黄金字节）
- [x] HID Consumer Volume 回退通道（报告描述符不嵌入 config——真机做法）
- 🟡 UAC2 Status Word 到达 Windows 音量回调 → `host.volume_event`（uac2-48k，含"不注入则无回调"的对照）
- 🟡 **R-006**：usbaudio.sys 是否响应 UAC1 Status Word → 同一用例在 `uac1-cmedia-sim` 那份结果里的结论。失败不一定是 bug，可能就是不响应

## M3 —— 控制台/Profile/注入/断言
- [x] HTTP 控制 API（8 端点）+ uacsim CLI + Profile 热切换
- [x] 故障注入全目录（STALL/延迟/损坏/半枚举/拔线/iso error）+ /inject
- [x] host-probe（enum / volume-watch / notify-watch / capture+Goertzel）。*原记录称"真机冒烟通过"；本轮未复验*
- [x] uacstudio GUI（Device/Inject/Stream/Log 面板，Per-Monitor-V2 DPI）；按钮表驱动，请求体由控制层构建并有单测
- [x] 三层回归 + CI（`.github/workflows/ci.yml`）+ 覆盖率门禁

## M4 —— 收尾
- [x] 多实例并发（busid 1-N 上限 4，每连接独立通道）。*原记录称"双实例冒烟通过"；单测覆盖实例上限与 busid 分配*
- [x] GUI Stream 面板（源选择/录音落盘）
- [x] MS OS 2.0 描述符（BOS + 注册表属性注入）+ 多语言 LANGID 表
- [x] README / Runbook / 本验收 / Sandbox 文档

## 评审修复（2026-10-30）

对照评审报告逐项处理。"证据"列是验证方式——能做变异验证的，都临时改回缺陷版本确认新测试会红，再恢复。

| # | 问题 | 处理 | 证据 |
|---|---|---|---|
| 1 | CI 连续失败（Configure 通过、Build 失败） | `ci.yml` 所有命令经 `scripts\msvc-env.cmd`（vswhere 定位 VS；`call` + `&&` 串联）；CMake 对非 MSVC 工具链直接报错。**根因未被日志直接证实**（日志当时下载不到）：推断为 runner 无 MSVC 环境、CMake 选了 MinGW g++。同时发现本机 VS 是 *BuildTools*，旧脚本写死的 `...\Community` 路径根本不存在 | ✅ 本机用新入口干净构建 + 测试通过。⬜ **CI 本身尚未重新跑过**（未推送） |
| 2 | HTTP API 无来源校验（跨域 POST / DNS 重绑定） | 拒绝带 `Origin` 的请求；`Host` 必须是回环；请求体 ≤1 MiB；头名大小写不敏感（原先 `Content-Length` 小写会读不到体） | ✅ `test_hardening.cpp`，每条规则带"去掉违规点 → 通过"的对照 |
| 3 | profile 名过滤漏掉反斜杠（`":/\.."` 里 `\.` 被转义成 `.`），UNC 名可穿透到 `fs::exists` | 改白名单 `valid_profile_name`；`/capture` `/source` 路径走 `valid_media_path`（.wav、拒 UNC/`..`/ADS/保留名） | ✅ 实测原缺陷；✅ 变异验证（改回旧黑名单，`test_sim_host` 与 `test_hardening` 里各 1 个用例变红；过程中还发现并修掉了测试自身的漏洞：最初的 UNC 例子带 `.`，旧黑名单恰好会拦住它，测不到反斜杠路径）。⬜ UNC 触发 SMB 外联只是现象一致（缺陷版本下请求卡住），未抓包证实 |
| 4 | 一条空闲连接即可卡死 API（串行 accept + 超时循环无总时限） | 整请求总时限（408）；USB/IP 握手阶段空闲时限 | ✅ 实测原缺陷（4 s 超时）；✅ 变异验证（让两处时限失效，4 个用例变红） |
| 5 | 覆盖率口径与"100%"需求不符 | 统计范围加入 `sim_host.cpp`、`controller.cpp`；`sim_host` 编成 `uac_host` 库进入单测；口径写明 | ⬜ 新范围下百分比**未重测**（无 OpenCppCoverage） |
| 6 | 核心风险点从未在真机验证 | 不在开发机做；建 Sandbox 一键流程 + 4 个 `host.*` 用例（及 `env.*`/`cleanup.*`）+ 一次性机器守卫；GUI 主题检查也做了变异验证（改成系统白底，三个控件被点名、冒烟失败） | ✅ 脚本语法、守卫拒绝、`.wsb` 生成、`api.*` 全绿、`RunTool`；🟡 **Sandbox 流程本身从未真正运行** |
| 7 | 健壮性/卫生 | `cancelled` 集合有上限并用完即删；通道线程回收；去掉可能返回空指针的 `shared_from`；`inject_volume/mute` 日志值取自锁内；5 个编译警告清零并开 `/WX`；`LastCoverageResults.log` 取消跟踪、`.gitignore` 路径修正；过期的测试名/注释（Q12.13→Q16.16）；`m2-verify.ps1` 传了无效参数 `--port`（被静默忽略）→ 整体由 `run-host-cases.ps1` 取代 | ✅ 构建/测试；通道线程回收只能间接验证（15 轮 import/断开循环 + `stop` 及时） |

附带修复（自检发现）：窗口窄于约 852px 时标题标签盖住 Profile 下拉框（`--selftest` 的几何审计在最小窗口尺寸下抓到）。

## 与 RFC v2 需求对照
| 需求 | 状态 |
|---|---|
| R1 纯软件单机 | ✅ 回环验证；🟡 实机 = usbip-win2 驱动自环，待 Sandbox |
| R2 指定 VID/PID/字符串 | ✅ Profile |
| R3 输入+输出端点 | ✅ 0x86/0x06 双流 |
| R4 UAC1/UAC2 双档 | ✅ 描述符与协商；🟡 类驱动绑定待 Sandbox（`host.driver_binding`） |
| R5 音量/静音/采样率 | ✅ |
| R6 硬件事件 | ✅ 双通道；🟡 UAC2 回调待 Sandbox；🟡 UAC1 定级（R-006）待 Sandbox |
| R7 观测+注入+断言 | ✅ NDJSON 日志 / 6 类注入 / host-probe |
| R8 CLI+GUI+CI | ✅ CLI+GUI；CI 配置已修但尚未重新跑过 |
| 核心覆盖率 | 见下"度量口径"。**不再声称 100%** |
| 自动化测试 | ✅ L1+L2 全自动；GUI 非视觉自检；`api.*` 一键用例；🟡 `host.*` 待 Sandbox |

## 度量口径
- 行覆盖率 95.12% 是 OpenCppCoverage 在 **`src/` 上**、提交 8c0a544 时测得的数字（lambda/子线程体以 `uac-cov:thread` 排除，共 8 行）。
- 之后统计范围扩到 `sim_host.cpp` 与 `controller.cpp`，**新范围下的百分比未重测**；门禁阈值仍为 95%，重测可能需要补用例。
- **不在统计内**：GUI 窗口代码（`main.cpp` / `env_check.cpp` / `gui_selfcheck.cpp`，靠 `--selftest` 冒烟）、`host_probe`、`uacsim`、
  `auto_test.cpp` 的 `host.*` 用例体（单测里只会 SKIP，验证只有 Sandbox 里的真实运行）。

## 遗留
1. **跑一次 Sandbox**（`sandbox/launch-sandbox.ps1`）：它是 `host.*` 用例与 M1/M2 宿主侧项的第一次真实运行，预计会暴露 `usbip` 命令行细节、端点出现时延等问题（见 sandbox.md 的"没有验证"清单）。
2. 推送后确认 CI 变绿；若仍红，先看 Build 步骤的编译器输出（本轮没能取到失败日志）。
3. 重测覆盖率（装 OpenCppCoverage 后 `scripts/coverage.ps1`）。
4. 首批目标设备 Profile 清单（RFC 待确认项）。
5. 人工确认项：GUI 外观（`--selftest` 不判断）、真对端软件行为、长时间稳定性、125%/150% DPI 观感。
