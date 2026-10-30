# UAC Simulator —— Windows 纯软件虚拟 USB 音频设备测试台

单机仿真指定 VID/PID 的 UAC1/UAC2 USB 音频设备：被 Windows 内置类驱动（usbaudio.sys / usbaudio2.sys）按真设备绑定，用于主机侧音频软件的自动化测试。方案与风险详见根目录 `rfc-windows-virtual-uac-device.md`（v2）。

## 架构

```
被测软件 ─WASAPI→ Windows 音频栈+类驱动 ─URB→ usbip-win2(UDE,已签名)
                                                    │ USB/IP v1.1.1 (127.0.0.1:3240)
                                              uacsimd.exe（本目录）
   uacsim CLI / uacstudio GUI / CI ──HTTP JSON:3241──┘
   host_probe.exe（主机侧断言：枚举/音量回调/Goertzel 采集校验）
```

- **协议层**：USB/IP 全大端（内核文档黄金字节对齐）；URB 通道独立线程，多设备并发
- **描述符引擎**：UAC1/UAC2 模板 + UDE 适配（High-Speed + iso bInterval≥4，issue #35 法则）+ usbaudio2.sys 约束 lint
- **流引擎**：虚拟微帧时钟 pacing（RET_SUBMIT 按包数延迟，防音频快进）+ 正弦/文件/静音源 + WAV 录音 + 显式反馈端点
- **R6 事件**：UAC2 AC 中断 Status Word（官方支持）+ HID Consumer Volume 回退通道
- **故障注入**：STALL / 延迟 / 描述符损坏 / 半枚举 / 拔线 / iso error

## 目录

| 路径 | 内容 |
|---|---|
| `src/` `include/uac/` | 核心库 `uac_core`（协议、描述符、引擎、控制 API、路径/名字白名单 `path_policy`） |
| `apps/uacsimd/` | 仿真器服务；`sim_host.*` 是 HTTP→设备模型的编排层（编成静态库 `uac_host`，单测与 exe 共用） |
| `apps/uacstudio/` | Win32 GUI。`controller.*`（按钮表/请求体，无窗口依赖）+ `auto_test.*`（一键自动测试用例表与运行器）编成 `uac_studio_core`；`main.cpp` 窗口；`gui_selfcheck.*` 非视觉自检 |
| `apps/uacsim/` `apps/host_probe/` | 命令行客户端 / 主机侧断言工具 |
| `scripts/` | `msvc-env.cmd`（构建入口）、`smoke.ps1`、`regression.ps1`、`coverage.ps1`、驱动/宿主侧脚本（带一次性机器守卫） |
| `sandbox/` | Windows Sandbox 一键验证：`launch-sandbox.ps1`（宿主）、`run-in-sandbox.ps1`（内部） |
| `tests/` | doctest 单测与回环场景 |

## 快速开始

```powershell
# 构建（bash）。所有 cmake/ctest 都经 msvc-env.cmd：它用 vswhere 找 VS，不依赖 VS 版本/安装位置
./scripts/msvc-env.cmd cmake --preset win-x64-debug
./scripts/msvc-env.cmd cmake --build --preset win-x64-debug

# 单测 + 回环场景（172 用例 / 1468 断言，无需驱动）
./scripts/msvc-env.cmd ctest --preset win-x64-debug

# GUI 非视觉自检：深色主题 / 几何 / 控制层。不判断外观
powershell -File scripts/smoke.ps1

# 覆盖率门禁（需 OpenCppCoverage：winget install OpenCppCoverage.OpenCppCoverage）
./scripts/msvc-env.cmd cmake --preset win-x64-coverage
./scripts/msvc-env.cmd cmake --build --preset win-x64-coverage
powershell -File scripts/coverage.ps1          # 阈值 95%；统计范围见脚本开头注释

# 仿真器 + 客户端（开发机上只到这里：不装驱动、不挂设备）
uacsimd.exe &                                  # 仿真器服务（3240/3241）
uacsim.exe status / event volume_up / inject '{"kind":"stall"}'
uacstudio.exe                                  # Win32 GUI 控制台
uacstudio.exe --autotest                       # 无窗口一键自动测试（api.* 用例；host.* 在开发机上会 SKIP）

# 宿主侧验证（usbip attach、WASAPI、usbaudio2.sys 绑定……）：只在 Sandbox / VM 里做
../../build.bat                                # 构建后自动打包 dist/uacsim-sandbox-release.zip：拷进 Sandbox，解压，双击 run.cmd
powershell -File sandbox/launch-sandbox.ps1    # 或：一条命令启动 Sandbox（目录映射）。见 ../../docs/sandbox.md
```

> **ninja 提示：** PATH 里先命中无扩展名的 `ninja` 包装脚本（如 `depot_tools\ninja`）时，CMake 会报
> "inappropriate file type"。`msvc-env.cmd` 已把第一个真正的 `ninja.exe` 排到最前；直接调 cmake 时请自行处理。

## Profile

`profiles/*.json`：`protocol/usb/audio/hid/rawConfigOverride`。`attach <name>` 热切换（模拟换设备）；多实例上限 4（busid 1-1..1-4）。

## 本地控制 API 的安全边界

127.0.0.1:3241，**无认证**，因此靠下面几条防浏览器类攻击（网页对本机端口发跨域 POST、DNS 重绑定）：

| 规则 | 行为 |
|---|---|
| 请求带 `Origin` 头 | 403（浏览器跨域请求必带；CLI/GUI/脚本不带） |
| `Host` 头不是 `127.0.0.1` / `localhost`（可带端口） | 403（防 DNS 重绑定）；缺 `Host` 放行 |
| profile 名 | 白名单 `[A-Za-z0-9_-]{1,64}`（可带 `.json`）；不能表达绝对/相对/UNC 路径 |
| `/capture` `/source` 的文件路径 | 必须 `.wav`；拒绝 UNC/设备路径、`..`、盘符外的 `:`（NTFS 备用数据流）、保留设备名、非法字符 |
| 请求体 | ≤ 1 MiB（413）；不支持分块（501） |
| 整个请求的读取总时限 | 默认 5 s（408）——串行处理的 API 不会被一条空闲连接卡死；USB/IP 握手阶段同理（10 s 空闲时限） |

命令行 `uacsimd --profile <path>` 仍接受完整路径（`confineName=false`，本地信任的入口）。

## 测试分层

| 层 | 内容 | 入口 | 门槛 |
|---|---|---|---|
| L1 | doctest 单测（协议/描述符/引擎/注入/路径白名单/控制层/用例运行器） | `ctest` | 全绿（172 用例 / 1468 断言） |
| L2 | 回环场景（fake-vhci 全握手/流/事件/注入剧本；SimHost + 控制 API 端到端；加固回归） | 同上 | 同上 |
| GUI | `uacstudio --selftest`：逐控件离屏像素统计 + 几何审计（含最小窗口尺寸）+ 控制层逻辑；外加 **UI 线程响应性探测**（无 uacsimd 时主窗口与环境检查窗口仍须及时应答） | `scripts/smoke.ps1` | 退出码 0；**不判断外观** |
| L3 | 一键自动测试：`api.*` 任何机器；`host.*` 真实 Windows 音频栈，**仅 Sandbox/VM** | GUI **Auto test** 按钮 / `--autotest` / `sandbox/launch-sandbox.ps1` | 无 FAIL |
| L4 | 人工确认清单（外观、真对端软件、长时间稳定性） | 每份自动测试结果文件末尾 | 人 |

**覆盖率**：OpenCppCoverage 在 `src/` 上测得 95.12%（提交 8c0a544 时的数字）；之后统计范围扩到 `apps/uacsimd/sim_host.cpp` 与
`apps/uacstudio/controller.cpp`，**新范围下的百分比尚未重测**（本机没装 OpenCppCoverage）。GUI 窗口代码、`host_probe`、`uacsim`、
`auto_test.cpp` 的 `host.*` 用例体不在统计内。lambda/子线程体以 `uac-cov:thread` 标记（见 `scripts/coverage.ps1` 注释）。
