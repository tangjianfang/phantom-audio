# phantom-audio 使用说明

> 类型：userguide ｜ 模式：Reverse ｜ 代码基线：`936376c` ｜ 最后核对：2026-11-01 ｜ 范围：全部产物 ｜ lang: zh

面向**使用者**：构建、启动、把虚拟声卡挂进 Windows、跑一键测试。排障见 `runbook.md`；架构见 `architecture.md`。

## Introduction

一台"插在虚拟 USB 主控上的可编程声卡"：被测软件当真 USB 设备对待，全部控制经三客户端（GUI / CLI / CI）与本地 HTTP API 完成。本文档覆盖构建、挂载、日常操作与已知边界。

## Prerequisites

| 项 | 要求 |
|---|---|---|
| 操作系统 | Windows 10/11（真机挂载需管理员；Sandbox/VM 免提权差异见 runbook） |
| 工具链 | Visual Studio 2022+ 的 C++ x64 工具集、CMake、Ninja（`scripts\msvc-env.cmd` 用 vswhere 自动定位） |
| 驱动（仅真机挂载） | usbip-win2（项目锁定 v0.9.8.1；安装与恢复见 runbook） |

## 安装（构建）

```powershell
git clone https://github.com/tangjianfang/phantom-audio.git
cd phantom-audio
.\build.bat                 # 默认 Release：构建 + 单测 + 打包 dist\uacsim-sandbox-release.zip
.\build.bat debug           # Debug：构建 + 单测 + GUI 非视觉自检 + 打包
```

- 构建产物：`tools\uac-simulator\build\release\uacsimd.exe / uacsim.exe / uacstudio.exe / host_probe.exe`
- **测试不过不打包**；链接失败若因旧进程占用，会询问是否结束并重建（30 秒默认否）
- 换驱动版本的节奏：卸载 → **重启** → 安装（热装卸会楔死内核栈）

## Getting Started

把虚拟声卡挂进 Windows。

双击 `uacstudio.exe` 后按序操作（按钮全灰时先做第 1 步）：

1. （仅按钮全灰时）Environment 窗口 → **Restart as administrator**，一次 UAC；重启后宿主按钮点亮
2. Environment → **Start simulator**；LED 变 Listening
3. 主窗口选 profile → **Attach**；日志出现 `{"attached":…}`，LED 显示 profile 与设备音量
4. Environment → **Attach to Windows**；声音设置出现"扬声器/Microphone (UAC2 Simulator 48k)"


日志面板一行一条（`[时间] [动作] 结果`）；Attach 后自动附一句 Windows 侧实况（已在/仅模拟器/VHCI 缺失+修法）。

## How-To

### 切换设备型号（Profile）

主窗口下拉选目标（`uac2-48k` / `uac1-cmedia-sim` / `uac2-hid-headset` / `uac2-sim` / `uac1-razer-clio`）→ **Attach**。换 profile = 先 Detach 再挂新设备（Windows 侧表现为拔出再插入）。

### 模拟音量按键

**Vol + / Vol - / Mute**：设备侧按键（改设备音量，LED 即时显示 `vol -6.0 dB (muted)`）。**HID Vol+/HID Mute/HID Release**：HID 回退通道。注意：Windows 混音器是否跟随取决于平台（见"已知边界"）。

### 注入故障

| 按钮 | 效果 |
|---|---|---|
| STALL | 下一条 bRequest=0x81 的请求 STALL |
| Delay | 控制请求延迟 200 ms |
| Unplug | 模拟拔线（断开） |
| Clear faults | 清除全部注入 |

### 麦克风源与扬声器录音

- Hz 框输入频率（1-24000）→ **Sine**；**Silence** 停
- 文件名框 → **Record**：Windows 播到虚拟扬声器的音频落盘为 WAV（**Stop rec** 结束）
- `/monitor` 开关（默认开）：虚拟扬声器收到的声音同步从**真实声卡**播出（像真实设备出声）

### 跑一键自动测试

主窗口 **Auto test**：20 用例（环境 3 + API 11 + 宿主 5 + 清理 1）。提权 + 宿主开启时真跑挂载/绑定/采集；不满足则该 5 例带原因 SKIP。结果：日志末行 `AUTO TEST RESULT: N passed…`，完整报告在桌面 `uacstudio-autotest-*.txt`。

### 用 CLI（脚本/CI）

```powershell
# 仿真器（可加 --log-file 记录设备侧全量 NDJSON）
uacsimd.exe --usbip-port 3240 --http-port 3241

# 客户端
uacsim.exe status                    # 等价 GET /status
uacsim.exe attach --profile uac2-48k
uacsim.exe event volume_up
uacsim.exe shutdown

# 无头一键测试（退出码 0 = 无失败）
uacstudio.exe --autotest --allow-host --port 3241 --out <目录> --exe-dir <目录>
```

## 配置速查

| 项 | 默认 | 说明 |
|---|---|---|---|
| HTTP 控制端口 | 3241 | `--http-port` / GUI `--port` |
| USB/IP 端口 | 3240 | `--usbip-port` |
| profiles 目录 | exe 旁 `profiles\` | `--profiles`；CMake 已自动部署 |
| 宿主用例开关 | 关 | 提权 GUI 的 Restart 按钮，或 env `UACSIM_FORCE_HOST_TESTS=1`，或 `--allow-host` |
| 扬声器监听 | 开 | `POST /monitor {"on":false}` 关 |
| 打包 | 产物 zip | `UAC_NO_PACKAGE=1` 跳过 |

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| 设备不出现 / 按钮全灰 | VHCI 根设备缺失或守卫拒绝 | `runbook.md`"驱动栈恢复" + Environment 红项修复步骤 |
| 采集 0 样本 / power=0 | usbip-win2/UDE 等时 IN 平台缺陷 | `runbook.md`"iso IN" 判定签名 |
| 构建失败 | 工具链/环境变量 | `runbook.md`"常见问题"；`scripts\envcheck.ps1` |
| Sandbox 全流程 | 需要一次性环境 | `sandbox.md` |

## 已知边界（如实）

- **麦克风流无声**：usbip-win2/UDE 等时数据载荷在本平台双向不通（端点会出现、控制正常）。扬声器监听因此在修复前听不到；证据链与后续选项见 runbook "iso IN"
- Windows 混音器不跟随设备侧音量事件（同根因）；GUI LED 提供设备侧反馈

## 术语

| 术语 | 含义 |
|---|---|---|
| Profile | 一台虚拟设备的描述符/VID/PID/能力集合（`profiles\*.json`） |
| Attach（主窗口） | profile 载入**模拟器**（软件侧） |
| Attach to Windows | `usbip attach` 把设备导入 Windows（需管理员） |
| VHCI | usbip-win2 提供的虚拟 USB 主控（根设备 `ROOT\USBIP_WIN2\UDE`） |
| 守卫 | 一次性机器/提权/环境变量三重判定，防止误在开发机做系统级操作 |
