# 在 Windows Sandbox 里做宿主侧验证

> 约束：**开发机上不做真机测试**（不装驱动、不挂虚拟 USB 声卡）。凡是需要真实 Windows 音频栈的验证，
> 都在一次性的 Windows Sandbox（或 VM）里跑。开发机上的脚本有守卫，会拒绝在非一次性机器上运行。

## 两种用法

### A) zip 包：手动拷进 Sandbox（`build.bat` 自动生成）

```
build.bat                      # 构建 Release，成功后自动打包
                               #   -> tools\uac-simulator\dist\uacsim-sandbox-release.zip
build.bat debug                # 构建 Debug + 跑单测，测试通过才打包 -> dist\uacsim-sandbox-debug.zip
set UAC_NO_PACKAGE=1           # 想跳过打包时
```

1. 把 `dist\uacsim-sandbox-release.zip` 拷进 Windows Sandbox（宿主机与 Sandbox 之间可直接复制粘贴文件）。
2. 在 Sandbox 里右键"全部解压缩"，**双击 `run.cmd`**（需要管理员时会自动提权）。
3. 跑完后把解压目录里的 `results\` 整个拷回宿主机；摘要也在 Sandbox 桌面的 `uacsim-sandbox-summary.txt`。

zip 内容（顶层目录 `uacsim-sandbox\`，路径用正斜杠，符合 zip 规范）：

| 文件 | 作用 |
|---|---|
| `run.cmd` | 双击入口；退出码非 0（含被守卫拒绝）时如实打印 FAILED，不会假装"Finished" |
| `run-in-sandbox.ps1` | 总流程：校验 → 守卫 → 提权 → 装驱动 → 跑用例表 → 总结。`-VerifyOnly` 只核对哈希，任何机器都能跑 |
| `uacsimd.exe` `uacstudio.exe` `host_probe.exe` `uacsim.exe` | 全部运行文件（静态 CRT，Sandbox 里无需装运行库） |
| `profiles\*.json` `scripts\*.ps1` | 设备描述与脚本 |
| `usbip-payload\` | **精简驱动载荷**（约 3 MB，`bin\` `driver\` `runtime\` `MANIFEST.txt` + `LICENSE-usbip-win2.txt`）。本机 `third_party\usbip-win2\usbip-payload\` 存在时就放它。`run.cmd` → `uacstudio --install-driver` 用它安装（见下面"安装驱动"） |
| `drivers\` | 官方安装包（约 25 MB）。**默认只在没有精简载荷时才放**（供在 Sandbox 里用收集脚本收集载荷）；有载荷时想一并带上用 `package-sandbox.ps1 -IncludeInstaller`。同样附 `LICENSE-usbip-win2.txt`：usbip-win2 是 **BSD-2-Clause**，二进制再分发要求随附版权声明与许可条款 |
| `collect-usbip-payload.bat` | 在 Sandbox 里装完官方安装包后运行，一键把精简载荷收进一个文件夹（见下一节） |
| `SHA256SUMS.txt` | 每个文件的哈希。拷贝过程中损坏/缺失会在第一步被点名并停下 |
| `VERSION.txt` | 版本、配置、git 提交（及是否有未提交改动）、打包时间、**驱动安装包文件名与哈希**（结果要能归因到具体驱动版本） |
| `README.txt` | 上述用法 |

> **驱动版本：** 项目文档锁定并测试过的是 usbip-win2 **v0.9.7.8**。放进 `third_party\usbip-win2\` 的是什么就打包什么——
> 版本不同时打包会给出警告。换版本之前，先在 Sandbox 里跑一遍用例表；结果按 `VERSION.txt` 里的版本归因。

### 精简驱动载荷（不带 25 MB 的官方安装包）

官方安装包里 24.8 MB 的大头是内嵌的 `vc_redist.x64.exe`。真正要用的只有下面这些，合计约 **3 MB**：

| 载荷 | 来源（在装过官方包的 Sandbox 里） | 大小 | 说明 |
|---|---|---|---|
| `devnode.exe` `usbip.exe` `libusbip.dll` `resources.dll` | `C:\Program Files\USBip\` | 2.2 MB | 创建虚拟主控设备节点 / attach·detach 命令行 / 其库与消息资源 |
| 驱动包 `usbip2_ude`、`usbip2_filter`（各 `.inf` `.sys` `.cat`） | `C:\Windows\System32\DriverStore\FileRepository\usbip2_ude.inf_amd64_bcfb5e9b79f800fa\` 与 `…\usbip2_filter.inf_amd64_2af67c4689565fb6\` | 0.25 MB | 微软签名，**`.cat` 不能动**。内核驱动本身不需要任何运行库 |
| `vcruntime140.dll` `vcruntime140_1.dll` `msvcp140.dll` `msvcp140_atomic_wait.dll` | `C:\Windows\System32\`（官方安装包的 VC redist 任务装进去的） | 约 0.8 MB | 见下 |

**不需要：** 各种 `.pdb`（约 17 MB）、`wusbip.exe`（GUI）、`include\` 与 `lib\`（SDK）、`unins000.*`（卸载器）。

**运行库为什么不能用本机 VS 的：** 静态读取官方二进制的导入表（`dumpbin -dependents`，没执行任何文件）：`usbip.exe` / `devnode.exe` / `libusbip.dll`
都动态链接 `VCRUNTIME140` / `VCRUNTIME140_1` / `MSVCP140`（`usbip.exe` 另有 `MSVCP140_ATOMIC_WAIT`）；`api-ms-win-crt-*` 是 Windows 10+ 自带的通用 CRT，不用带。
这几个官方二进制的链接器版本是 **14.51**（VS 2026），而本机 VS 2022 的 app-local 运行库是 **14.44**——**老运行库配新工具链编译的程序可能缺入口点**，所以要取装官方包之后 `System32` 里的那一套（并确认版本 ≥ 14.51）。

**一键收集（在 Sandbox 里）：** 先运行 `drivers\` 里的官方安装包一次（**勾选** "Install Microsoft Visual C++ Redistributable"），
再双击 zip 里的 **`collect-usbip-payload.bat`**。它把上表三类载荷收进桌面上的 `usbip-payload\` 文件夹（`bin\` `driver\` `runtime\` + `MANIFEST.txt`），
你把这个文件夹拷回宿主机即可。脚本从不删除任何东西（输出目录已存在就拒绝）；缺什么会逐项列出 `MISSING`，并以非 0 退出码结束。
注意：Sandbox 关闭后里面的东西就没了，所以趁它还开着收集。

（脚本里驱动包的位置是 `C:\Windows\System32\DriverStore\FileRepository\usbip2_{ude,filter}.inf_amd64_*\`，文件夹名来自 Windows 的 `.ini` 登记文件里的
`Id=`；那两个 `.ini` 不是驱动本身，但记录了每个驱动包应有的文件、大小和 **SHA1**。）

**核对已拷出的驱动包（2026-10-30 的那一份）：** 6 个文件的大小与登记一致；`.cat`、`.inf` 的 SHA1 与登记**完全一致**；`.sys` 的整文件 SHA1 与登记不同——
因为带签名的 PE 文件登记的是 **Authenticode 摘要**（排除 CheckSum 字段、证书目录项和证书表）。我按 PE 规范自己算了一遍，两个 `.sys` 都与登记值**完全匹配**，所以它们就是安装进系统的那一份。
INF 是自包含的：UDE 只拷 `usbip2_ude.sys`，硬件 ID `ROOT\USBIP_WIN2\UDE`；filter 是挂在 `USB\ROOT_HUB30` 上的 Extension INF（这也是官方说明里"安装会重启所有 USB 3.0 集线器"的原因）。

**关于 "amd64"（以及 Intel 能不能用）：** 驱动包文件夹名 `…_amd64_…` 和 INF 里的 `NTamd64` 指的是 **x64 指令集架构**（x86-64，最早由 AMD 设计，所以 Windows 一直这么叫），
**Intel 和 AMD 的 64 位 CPU 都适用**，与 CPU 品牌无关。例如 `PROCESSOR_ARCHITECTURE` 在 Intel 机器上也是 `AMD64`。我读了 `.sys`/`.exe`/`.dll` 的 PE 头，目标架构全是 `8664 (x64)`。
真正**不支持**的是 32 位 x86；ARM64 机器需要另一份 arm64 构建（官方发布里有 `USBip-*-arm64.exe`）。官方安装脚本也只允许 64 位 Windows 10 1903+。

**备份位置：** `C:\SendboxUSBip` 的整份拷贝备份在 `tools\uac-simulator\third_party\usbip-win2\SendboxUSBip\`（已逐字节核对一致）；官方源码 zip 解压在同级的 `src\`（用于核对，见下）。
`third_party\usbip-win2\` **整个目录被 `.gitignore` 忽略**：这些是第三方二进制和源码，不随仓库提交；只忽略 `*.exe` 的话 `.dll/.pdb/.sys` 会作为未跟踪文件冒出来、容易被误提交。

**许可（更正）：** usbip-win2 是 **BSD-2-Clause**，不是 GPLv3——GitHub 仓库登记如此，`v.0.9.7.8` 与 `v.0.9.8.1` 两个标签上的 `LICENSE.txt` 也都是 BSD 2-Clause，
源码树里没有 GPL/SPDX 声明。（项目早期的 RFC / 设计文档里写的 "GPLv3" 是不对的，已在两份文档里更正并加了顶部勘误；`rfc-windows-virtual-uac-device.html`
是**没有随 v2 重新生成的 v1 渲染**，我只更正了其中的许可文字，其余内容仍是 v1——要让它与 md 一致需要重新生成。）实际影响：随包分发其二进制时，
需要在文档或随附材料里**复现它的版权声明与许可条款**（`package-sandbox.ps1` 会把 `LICENSE.txt` 打进 `drivers\`），**没有**源码提供义务。
`usb.ids` 数据文件和 CLI11 的许可我没有逐个核实。这不是法律意见。

**源码版本：** 下载的 `usbip-win2-master.zip` 是 **0.9.8.2**，比你装的 **0.9.8.1** 新。我之前对着 GitHub HEAD 核对的 `usbip` 命令行选项（`attach --once`、`-X` 等）
因此不一定适用于你的 0.9.8.1 二进制；要精确对应，下载标签 `v.0.9.8.1`（注意标签名带个点）的源码。
`devnode` 的行为已对着源码确认：需要管理员权限；`install <inf> <hwid>` 用 `UpdateDriverForPlugAndPlayDevices`；**仅当它自己判定需要重启时**才打印
`Reboot is required to finish setup.`——官方安装包的 `AlwaysRestart` 是一刀切的保守做法，我们的安装器可以靠这句话如实报告是否需要重启。

### 安装驱动（精简载荷，取代官方安装包）

三个入口共用同一份实现（`apps/uacstudio/driver_installer.cpp`）：

| 入口 | 用法 |
|---|---|
| 命令行 | `uacstudio.exe --install-driver [--payload DIR] [--install-dir DIR]`、`--uninstall-driver`。默认载荷 `<exe 目录>\usbip-payload`，默认安装目录 `%ProgramFiles%\USBip`（与官方包一致，`host.*` 用例的 `usbip.exe` 查找能命中） |
| 脚本 | `run-in-sandbox.ps1` 的第 3 步：有 `usbip-payload\` 就调上面的命令行，没有则退回官方安装包 |
| GUI | 主窗口 **Environment** 按钮打开环境检查窗口，其中 "usbip-win2 driver" 一项旁的 **Install driver** 按钮（仅当载荷通过校验**且**本机是一次性机器时出现；点击先弹确认框，后台线程安装，完成后弹结果并重新检测）。开发机上该项只显示 "not disposable ... install it in Windows Sandbox / a VM" |

**步骤**（复刻官方 `setup.iss` 里真正起作用的部分，不含 VC redist / GUI / SDK / PDB）：
守卫（必须是一次性机器）→ 管理员 → **已装过则什么都不做** → 校验载荷（`MANIFEST.txt` 逐文件 SHA256；缺 `.cat`、重复驱动包、哈希不符都算错）→ 把 `bin\` 与 `runtime\` 拷到安装目录（app-local）
→ `pnputil /add-driver usbip2_filter.inf /install` → `devnode install usbip2_ude.inf ROOT\USBIP_WIN2\UDE` → 校验服务 `usbip2_ude` 出现。第一个失败即停。

**要点：**
- **幂等**：`devnode install` 每次都会新建一个设备节点，重复执行会得到第二个虚拟主控——所以服务已存在时直接返回（Sandbox 重启后 LogonCommand 会把脚本再跑一遍）。要重装先 `--uninstall-driver`。
- **重启如实报告**：官方包写死 `AlwaysRestart`，而 `devnode` 只在自己判定需要时才打印 `Reboot is required to finish setup.`（源码核对过）；`pnputil` 退出码 3010 也算。需要时退出码为 3，GUI 弹窗标题注明。`pnputil` 退出码 259（已进驱动库、暂无匹配设备）按成功处理。
- **卸载**（`--uninstall-driver`）：`devnode remove ROOT\USBIP_WIN2\UDE root` → 扫描 `%WINDIR%\INF\oem*.inf`（兼容 UTF-16），对属于 `usbip2_*` 的逐个 `pnputil /delete-driver … /uninstall /force` → **只删我们自己拷的文件**（按名字），不动官方安装器留下的 `unins000.*`，目录非空就保留。尽力而为：每步都做、汇总结果。
- **退出码**：0 成功；1 失败；2 被守卫拒绝（什么都没动）；3 成功但需要重启。
- **守卫**：与 `host.*` 用例同一个（`HostCasesAllowed`）：Sandbox / Hyper-V 来宾 / 常见虚拟机，或显式 `UACSIM_FORCE_HOST_TESTS=1`。

**验证状态：**
- 已验证（开发机）：16 个单测用例，对**真实载荷**的 `MANIFEST` 校验通过；用假的执行器验证了步骤顺序与参数、失败即停（pnputil 失败后 devnode 不会被调用）、重启判定、幂等、守卫/非管理员/载荷损坏时**一条系统命令都不执行且安装目录不会被创建**、卸载只删我们的文件。
  变异验证过三道防线（哈希不符不报错 / 守卫不拦截 / 命令失败不停止），对应测试都会变红。包里的 `uacstudio --install-driver` 在开发机上被守卫拒绝（退出码 2），实测没有创建安装目录、服务或驱动包。
- **已在 Sandbox 验证（2026-10-30 第一次运行，走的是 `run-in-sandbox.ps1` → `uacstudio --install-driver`）：** `pnputil` 与 `devnode` 成功，服务 `usbip2_ude` 出现并处于运行状态，Windows 没有报告待重启，
  随后 `usbip attach` / `detach` 可用。
- **没有验证：** `devnode install` 的原始输出与退出码没有被保留下来（transcript 里这一步没有输出）；GUI 的 **Install driver** 按钮（环境检查窗口不在 `--selftest` 的几何/主题覆盖范围内，且没人看过它的外观；它的"不卡 UI"由 `smoke.ps1` 的响应性探测覆盖，但按钮点击本身没有被自动点过）。

### B) `.wsb` 目录映射：一条命令启动 Sandbox

```powershell
# 前置：1) 已构建（build.bat 或 scripts\msvc-env.cmd cmake --build --preset win-x64-release）
#       2) 已启用 Windows Sandbox（Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All，需重启）
#       3) usbip-win2 安装包放进 tools\uac-simulator\third_party\usbip-win2\
powershell -File tools\uac-simulator\sandbox\launch-sandbox.ps1
```

没有安装包也能跑：`env.driver` 与 `host.*` 会 SKIP 并说明原因，`api.*` 照常。`-NoStart` 只生成配置不启动。
结果回到宿主机的 `tools\uac-simulator\sandbox\results\`（Sandbox 里 `C:\results` 的可写映射）。
两种用法共用同一份打包清单（`scripts\package-sandbox.ps1`）和同一个 `run-in-sandbox.ps1`。

## 里面发生了什么

```
（B 才有）launch-sandbox.ps1（宿主）
  1. 暂存（复用 package-sandbox.ps1，不压缩、不含驱动）→ sandbox\stage
  2. 生成 uac-sim.generated.wsb（绝对路径，已 .gitignore）：网络禁用、vGPU/音视频输入/剪贴板/打印机禁用，
     stage 与驱动目录只读，只有 results 可写
  3. 启动 WindowsSandbox.exe
run-in-sandbox.ps1（Sandbox 内；A 由 run.cmd 调用，B 由 LogonCommand 调用）
  0. 守卫：必须是 Sandbox/VM（在一切动作之前；失败就整体停止，不是"记录后继续"）→ 必要时自动提权
  1. 校验 SHA256SUMS         2. 工作目录：Root 可写则就地运行，只读（B 的映射）则拷到 C:\uacsim-run
  3. 装 usbip-win2 驱动（跳过还原点）  4. run-host-cases.ps1：启动 uacsimd，
     对 uac2-48k 与 uac1-cmedia-sim 各跑一遍用例表（uacstudio --autotest --allow-host --profile X）
  5. 总结：每步 ok/FAIL、是否需要重启、结果文件位置
```

步骤失败只记录、继续跑，一次运行看全貌。每个 profile 一份结果文件，第一行是机器可读的
`RESULT: N passed, M failed, K skipped`，其后每用例一行（PASS/FAIL/SKIP + 用时 + 原因），
然后是"需要人工确认"清单，最后是完整日志。

## 用例表

三个入口共用同一张表（`apps/uacstudio/auto_test.cpp` 的 `Cases()`）：GUI 的 **Auto test** 按钮、
`uacstudio.exe --autotest [--allow-host] [--profile X] [--out DIR]`、本文的 Sandbox 脚本。

| id | 范围 | 验证什么 |
|---|---|---|
| `env.api` | 任何机器 | uacsimd 控制 API 可达 |
| `env.machine` | 任何机器 | 是不是一次性机器；host.* 是否启用 |
| `env.driver` | 仅 Sandbox/VM | usbip-win2 驱动（服务 `usbip2_ude`）已安装 |
| `api.reset` … `api.security` | 任何机器 | detach/attach、音量/静音/HID 事件、日志、音源、录音、故障注入、多实例、安全回归（坏 profile 名 / 跨域 / UNC 路径被拒） |
| `host.attach_enum` | 仅 Sandbox/VM | `usbip attach` 后 Windows 出现新的 render + capture 端点 |
| `host.driver_binding` | 仅 Sandbox/VM | 绑定的是内置类驱动：UAC2→`usbaudio2.sys`，UAC1→`usbaudio.sys` |
| `host.volume_event` | 仅 Sandbox/VM | 设备侧 Status Word 以音量/静音回调的形式到达 Windows（先测"不注入则无回调"作对照） |
| `host.capture_tone` | 仅 Sandbox/VM | 模拟麦克风的 1 kHz 正弦被采到；同一路流里找 3 kHz 必须检不出（对照） |
| `host.gui_ops` | 仅 Sandbox/VM | GUI 的 **Attach to Windows** / **Detach from Windows** 按钮背后的同一份代码在目标系统上往返一次，并用 `usbip port` 独立核对 |
| `cleanup.devices` | 跑过任何用例就跑 | 只摘自己 attach 的端口（**不再**有 `detach -a` 兜底）；仿真器归零；Windows 端点回到基线 |

**R-006**（usbaudio.sys 是否响应 UAC1 的 Status Word）的实测答案 = `uac1-cmedia-sim` 那份结果里
`host.volume_event` 的结论。它失败**不一定是 bug**，可能就是 `usbaudio.sys` 不响应——那就用 HID 通道。

## 界面：英文文案、线程模型与状态提示

**界面文字一律英文。** Sandbox 镜像没有 CJK 字体，中文在窗口里显示成方块。范围：GUI、`uacsimd`/`uacsim` 的输出与错误信息、
Sandbox 里执行的脚本（`run-in-sandbox.ps1`、`disposable.ps1`、`run-host-cases.ps1`、`elevated-install-driver.ps1`）以及 zip 里的
`README.txt`。源码注释和只在开发机上跑的脚本（`smoke.ps1`、`coverage.ps1`、`launch-sandbox.ps1`、`package-sandbox.ps1` 等）可以保留中文。
`tests/test_env_check.cpp` 里有一条源码扫描用例：`apps/`、`src/`、`include/` 下任何字符串字面量含非 ASCII 字符（包括 `…`、`·` 这类也会乱码的符号）就失败。

**UI 线程永远不等。** 起因：实测 Windows 上阻塞式 `connect()` 连没人监听的回环端口要等约 **2.0 秒**（2017–2117 ms）。
旧代码在 UI 线程的定时器里同步发请求、环境检查窗口也同步跑检查，没有 `uacsimd` 时界面被钉死（Sandbox 里实际发生过：启动即"卡死"）。
现在的做法：

| 流程 | 以前 | 现在 |
|---|---|---|
| 周期性取状态 / 日志 | UI 线程定时器里同步 HTTP | 轮询线程；结果用 `PostMessage` 投回（堆上载荷，接收方释放） |
| 点按钮发请求（音量、静音、注入、Attach 等） | `WM_COMMAND` 里同步 HTTP | 请求队列线程；忙时队列显示在状态栏里 |
| 环境检查（连 `uacsimd`、对载荷做 SHA-256、查服务/注册表） | 弹窗前同步跑完 | 检查线程；窗口先出来，逐步提示 `Checking: ...`，完成后才填列表 |
| 安装驱动 | 已是后台线程 | 同上，并且安装期间关窗被推迟到线程结束 |
| 自动测试 | 已是后台线程 | 同上，并且关窗会设置取消标志、等当前用例收尾 |
| 打开网页 / `ShellExecute` | UI 线程 | 短生命周期线程（`CoInitializeEx`） |
| HTTP 客户端 | 阻塞 `connect`，无超时 | 非阻塞 connect + `select` 300 ms 上限，读写 2 s 超时（`apps/uacstudio/http_client.h`） |

**状态提示：** 主窗口底部有一行状态栏：左边是"在干什么"（`Ready` / `Sending: ...` / `Auto test running ...` / 关闭等待提示），
右边是网络状态（`uacsimd: connecting to ...` / `connected` / `NOT reachable ... (retrying)`；连接中为琥珀色、不可达为红色）。
环境检查窗口的提示行同样显示 `Checking: <正在做的步骤>`、`Installing the driver ...`。

**参数：** `uacstudio.exe --port N`（控制端口，默认 3241）、`--env-check`（启动后立即打开环境检查窗口，供冒烟脚本使用）。

**验证状态：**
- 已验证（开发机）：`scripts/smoke.ps1` 的**响应性探测**——外部进程对 GUI 窗口反复发 `SendMessageTimeout(WM_NULL, SMTO_BLOCK|SMTO_ABORTIFHUNG, 400 ms)`，
  GUI 带 `--port <关闭的端口>` 启动（等价于"没有 uacsimd"），分别探测主窗口和 `--env-check` 模态窗口 7 秒。
  **旧代码：17 次探测里 14 次在 400 ms 内没被处理（两个窗口都是）；新代码：Debug 主窗口 112/112、环境检查窗口 113/113 全部及时响应（最慢 5 ms）；Release 109/109 与 111/111（最慢 58 ms）。**
  `tests/test_http_client.cpp`（关闭端口 <1200 ms、有服务仍可达、接受连接但不应答被 I/O 超时限住）、`tests/test_env_check.cpp`（每一步都有进度、文案全英文、关闭端口不久等、服务在线时同一调用转绿）。
  变异验证过：进度回调丢失、uacsimd 恒判就绪，对应测试都会变红。
- **没有验证：** 状态栏与环境检查窗口的外观（颜色、文字是否被截断、英文按钮标题是否放得下）没人看过；Install driver 按钮没有被自动点过；
  响应性探测只证明"消息泵没被阻塞"，不证明"显示内容正确"；真实 Sandbox 里的表现要等第一次运行。

## 模拟器、驱动、Windows 端口的完整生命周期：GUI 与命令行各有什么

四样东西各有"启动"和"退出/清理"，缺一头就会留下东西：**模拟器进程**（`uacsimd.exe`，占 3240/3241 端口）、**Windows 侧 usbip 端口**（设备是否挂在 Windows 上）、
**驱动**（`usbip2_ude` 服务和虚拟主控）、**模拟的设备**（uacsimd 里的 profile 实例）。

| 操作 | GUI | 命令行 |
|---|---|---|
| 切换模拟的设备（profile） | 主窗口：下拉框选 profile → **Attach**。Attach 现在是**替换**（先 detach 再 attach，实例数恒为 1）；状态灯文字显示当前 profile 和实例数 | `uacsim detach` + `uacsim attach <名字>`（命令行仍是**叠加**，最多 4 个实例，用于多设备测试） |
| 摘掉模拟的设备 | 主窗口 **Detach** | `uacsim detach` |
| 启动模拟器进程 | **Environment** 窗口 **Start simulator**；或 `uacstudio --start-simulator`（窗口启动时自动起） | `uacsimd.exe [--parent-pid <pid>] ...` |
| 停止模拟器进程 | **Environment** 窗口 **Stop simulator**；**关闭主窗口**时自动停（只停 GUI 自己启动的）；**Clean up all** 也会停 | `uacsim shutdown`（`POST /shutdown`）；Ctrl+C / 关控制台窗口 |
| 让 Windows 看到设备 | **Environment** 窗口 **Attach to Windows**（`usbip attach -r 127.0.0.1 -b 1-1`；先检查模拟器在线且已挂 profile） | `usbip attach -r 127.0.0.1 -b 1-1`；`host.*` 用例会自动做 |
| 释放 Windows 侧端口 | **Detach from Windows**（只摘本模拟器的端口，不动别人的 usbip 设备） | `usbip detach -p <端口>`；**不要用 `detach -a`**（会摘掉别人的） |
| 安装驱动 | **Install driver**（载荷校验通过 + 本机允许 + 驱动未装） | `uacstudio --install-driver [--payload DIR] [--install-dir DIR]` |
| 卸载驱动 | **Uninstall driver** | `uacstudio --uninstall-driver [--payload DIR] [--install-dir DIR]` |
| 全部清理 | **Clean up all**：Windows 端口 → 模拟器 detach → **停模拟器** → 卸载驱动（尽力而为，一步失败不跳过后面的） | 依次执行上面几条 |

**哪些按钮什么时候能点**（`host_ops.h` 的 `PlanHostActions`，有单测）：Start / Stop simulator 在任何机器上都可用（只动我们自己的进程）；
其余按钮要求本机通过一次性机器守卫。按钮灰掉的原因写在窗口下方的状态文字里。

**模拟器不会无人看管地留下来：**
- `uacsimd --parent-pid <pid>`：指定进程一结束就自己正常退出。GUI 启动它时传自己的 pid，所以 GUI 崩溃、被任务管理器强杀也不会留下孤儿进程；`run-host-cases.ps1` 也这样启动。
- `POST /shutdown`：优雅停止（回应发出后主循环收尾）。和其它端点同一套 Origin / Host 策略，网页碰不到。
- GUI **只停自己启动的那个**：先礼貌地请求，对方没停且是自己启动的才结束进程；别人启动的模拟器只请求、不强杀，没停就如实报告。
- 关闭主窗口时，如果模拟器是 GUI 启动的：先（本机允许时）摘 Windows 端口，再停模拟器，然后才关窗；整个过程在后台线程，状态栏显示在做什么。

卸载的步骤（`devnode remove` → 删除属于 `usbip2_*` 的 `oem*.inf` 驱动包 → 只删我们自己拷的文件）和 **Clean up all** 只在开发机上用**假执行器**的单测验证过，**从未在 Sandbox 或真机上真正跑过**。
Start / Stop simulator 的逻辑用假进程和**真实的 `uacsimd.exe`**（`/shutdown` 结束进程、`--parent-pid` 随父进程退出）测过，`smoke.ps1` 第三阶段验证了"正常关窗 / 被强杀 / 别人启动的不被动"，**但这些按钮本身没有被点过**。
如果驱动是用官方安装包装的，用官方的卸载器（`unins000.exe`）。

**用户自己的另一台测试机（真机）：** 守卫会拒绝——`--install-driver` 退出码 2，驱动和 Windows 相关的按钮灰掉，`host.*` 用例 SKIP（Start / Stop simulator 仍可用）。
这台机器是否可以放行由使用者决定：启动 uacstudio 前设置环境变量 `UACSIM_FORCE_HOST_TESTS=1` 才能越过检测；它会装内核驱动、短暂重启 USB3 集线器，安装路径**不会**创建还原点。
机器上已经装了 usbip-win2（官方安装包）时不需要再装：**Start simulator** → 主窗口 **Attach** → **Attach to Windows**。

## 守卫

| 位置 | 守卫 |
|---|---|
| `scripts/elevated-install-driver.ps1`、`scripts/run-host-cases.ps1`、`sandbox/run-in-sandbox.ps1` | `Assert-DisposableMachine`（`scripts/disposable.ps1`）：非 Sandbox/VM 抛错；`-AllowNonDisposable` 显式放行 |
| `uacstudio --autotest` 的 `host.*` | 必须同时 `--allow-host` **且**机器是一次性的；环境变量 `UACSIM_FORCE_HOST_TESTS=1` 才能越过检测 |
| GUI **Auto test** 按钮 | 仅在检测为一次性机器时启用 `host.*`，否则 SKIP |

判定为"一次性"：用户 `WDAGUtilityAccount`（Sandbox）、Hyper-V 来宾、VMware/VirtualBox/QEMU/Xen/Parallels。
PowerShell 与 C++ 两处判定逻辑保持一致。

## 验证状态（如实）

**已在开发机上验证：**
- 脚本语法解析通过；`launch-sandbox.ps1 -NoStart` 生成合法的 `.wsb`（网络禁用、仅 results 可写），暂存内容齐全。
- `build.bat` 端到端：构建后自动生成 zip（现在默认带精简载荷：32 个条目，约 2.3 MB）；条目路径是正斜杠；
  解压到全新目录后 `run-in-sandbox.ps1 -VerifyOnly` 通过（31 个文件全部匹配），**改坏一个字节 / 删掉一个文件都被点名并以退出码 1 结束**（早先验证过）。
  包里所有 `.ps1`/`.cmd`/`.bat`/`README.txt` 都是纯 ASCII 英文。
- 在开发机上运行解压后的 `run.cmd`：被守卫拒绝，如实打印 FAILED，没有创建 results 目录、没有进程、没有服务、桌面无摘要文件。
- `run-host-cases.ps1`、`elevated-install-driver.ps1` 在开发机上**拒绝运行**，且无副作用（没有进程、没有服务）。
- `api.*` 用例：单测里对着进程内 SimHost 全部通过；`uacstudio --autotest` 对着真实 `uacsimd.exe` 进程（开发机，不带 `--allow-host`）19 个用例：14 通过 / 0 失败 / 5 跳过（`env.driver` 与 4 个 `host.*`，原因均写明）。
- 用例运行器：超时判 FAIL 且后续跳过、清理照跑、异常映射、取消；`RunTool` 的退出码/stderr 合并/超时杀进程树。
- `host.driver_binding` 里用的 PowerShell 查询在开发机上跑过（只读），输出格式 `InstanceId|服务|状态` 与解析对得上。

**已在 Sandbox 里验证（两次真实运行，2026-10-30，Windows 10.0.26100，见下一节）：** zip 完整性校验；守卫识别 Sandbox 并启用 `host.*`；
精简载荷安装（`pnputil` + `devnode`，服务 `usbip2_ude` 运行，装完 Windows 没有报告待重启）；`usbip attach` 成功、`usbip detach -p` 成功（没有留下自动重试项）；
全部 `api.*` 用例（两个 profile 各一遍）；`cleanup.devices`；
**Windows 内置类驱动认领模拟设备**（`host.driver_binding`）：UAC1 → `usbaudio.sys`、UAC2 → `usbaudio2.sys`，PnP 状态 OK、problem 0。这是方案成立的前提（RFC 1.4），第二次运行才首次通过。

**没有验证：**
- **音频端点是否会出现。** `host.attach_enum` 在两次运行里都失败（两个 profile 都是），所以 `host.volume_event`、`host.capture_tone` **从未在任何机器上通过过**。
  Sandbox 是远程会话，只暴露"Remote Audio"端点（证据见下一节）；要在控制台会话里重做一遍才能知道端点会不会出现。
- `usbip` 命令行：已对着 usbip-win2 仓库 **HEAD 源码**核对过选项；`attach`/`detach -p`/`port` 在 0.9.8.1 上的实际行为已被运行证实可用，
  `detach -a` 与 `port` 输出格式的解析（`Port\s+(\d+)`）也已被 `cleanup.devices` 的 PASS 间接证实。
- `volume-watch` / `capture` 对新端点的行为；R-006（`usbaudio.sys` 是否响应 UAC1 Status Word）——两次运行里设备侧的音量/静音注入都发生在 attach 之前，没有数据。
- 安装程序要求重启（退出码 3010 / `devnode` 打印 `Reboot is required`）时的行为：没有触发过。

## Sandbox 运行记录（2026-10-30）

### 第一次运行（16:21）：`15 passed, 1 failed, 3 skipped`（两个 profile 相同）

失败的唯一一项是 `host.attach_enum`：`usbip attach` 退出码 0（`successfully attached to port 1`），但 30 秒内 Windows 音频栈没有出现新的 render / capture 端点；
另外三个 `host.*` 因此被跳过。设备侧日志（`uacsimd.ndjson`）里 UAC1 和 UAC2 的表现不同：

| | UAC1（`uac1-cmedia-sim`） | UAC2（`uac2-48k`） |
|---|---|---|
| 枚举、`SET_CONFIGURATION` | 完成 | 完成 |
| 之后的类请求 | `usbaudio.sys` 对两个 Feature Unit 的静音/音量各发了 `GET_CUR/MIN/MAX/RES`，然后对两个 AS 接口 `SET_INTERFACE alt 0` | **一个都没有**，也没有 `SET_INTERFACE` |
| 结论 | 内置类驱动认领了设备 | `usbaudio2.sys` 没有启动这个音频功能 |

对着 ADC 规范核对描述符，查明并修复了四处设备侧缺陷（单测覆盖，变异验证过）：
1. UAC2 的 CS AS General 描述符少了末尾的 `iChannelNames`，是 15 字节，ADC 2.0 要求 16 字节。
2. UAC2 的 CS 等时端点描述符 `bDescriptorType` 写成 `0x24`（CS_INTERFACE），应为 `0x25`（CS_ENDPOINT）。UAC1 那边本来就是对的。
3. UAC2 的反馈端点后面多跟了一个 CS 端点描述符；规范里只有等时*数据*端点有。
4. 字符串描述符索引 0（语言 ID 列表）被 STALL，日志里 Windows 一共问了 3 次（UAC2 两次、UAC1 一次）。现在返回 `04 03 09 04`；不存在的其它索引仍然 STALL。

`lint_uac2` 现在按 ADC 2.0 表格核对每个类特定描述符的长度、类型和位置（`uac2-cs-descriptor-length` / `-endpoint-type` / `-endpoint-misplaced`）。
`usbaudio2.sys` 对不合规描述符不报错，只是不启动功能——表现就是"设备在线、没有端点、没有任何类请求"，所以这类问题必须在设备侧用 lint 拦住。
同时给 `host.attach_enum` 加了失败现场（见下），并让 `host.driver_binding` 只依赖"已 attach"。

### 第二次运行（16:42，带上面的修复和诊断）：`16 passed, 1 failed, 2 skipped`（两个 profile 相同）

**修复被证实有效：** `host.driver_binding` 两个 profile 都 PASS（UAC1 → `usbaudio`，UAC2 → `usbaudio2`，`status=OK problem=0`）。UAC2 的配置描述符从 271 变成 265 字节
（271 + 2 − 8，与预期一致），字符串索引 0 返回 4 字节，`SET_CONFIGURATION` 之后出现了完整的类请求流量：对两个 Feature Unit 的 `GET_RANGE`、时钟源采样率 `GET_RANGE`（38 字节 = 2 + 3×12）、
大量 `GET_CUR`，以及 `SET_CUR` 采样率 + `SET_INTERFACE alt 1/0` 的格式探测循环。`usbaudio2.sys` 确实在用这个设备。

**端点仍然没有，现场证据（`host.attach_enum` 失败时自动写出的诊断）：**
- `host_probe session` → `{"sessionId":1,"consoleSessionId":4294967295,"remoteSession":true}`：**这是远程会话，没有控制台会话。**
- `host_probe enum --all`（所有状态）→ 只有一个端点，名字是"远程音频"（Remote Audio），`active`。**没有任何 USB 音频端点，连 `notpresent` / `disabled` 的都没有。**
- uacsimd `/status` → `imported:true`（Windows 真的占用了设备）；`usbip port` → `Port 01: device in use at High Speed`。
- `Audiosrv`、`AudioEndpointBuilder` 都在 Running；`USB\VID_xxxx&PID_xxxx&MI_00` 节点 `service=usbaudio`/`usbaudio2`、`status=OK`、`problem=0`。

结论：驱动认领和启动都成功，音频服务也在跑，没有端点的最合理解释是 **Windows Sandbox 是远程会话，音频栈只给它重定向的"Remote Audio"**。
这是**强烈支持但尚未证明**的推断——决定性的实验是在有控制台会话的环境里重做一遍（Hyper-V 虚拟机的**基本会话**；注意增强会话也是 RDP，同样是远程会话）。
在那之前，`host.attach_enum` 保持失败（不改成 SKIP：改了就会掩盖真正的失败），但在远程会话里失败信息会带上这条说明。

**第二次运行里新发现、已修：** `usbaudio2.sys` 启动时对中断端点 0x81 发 `CLEAR_FEATURE(ENDPOINT_HALT)`，被 STALL（设备侧日志 `bmRequestType:2, bRequest:1, wIndex:129`）。
USB 2.0 §9.4 要求对存在的端点成功。现在对配置描述符里存在的端点，`CLEAR_FEATURE` / `SET_FEATURE(ENDPOINT_HALT)` 返回成功、`GET_STATUS` 返回 0；不存在的端点和其它特性值仍然 STALL。
**没有证据表明它影响了端点创建**，这条修复还没有在 Sandbox 里验证。

**顺带发现、没有改：** 所有 profile 都以 High-Speed（`device_model.cpp` 的 `d.speed = 3`）导出，包括 `bcdUSB 0x0110` 的 UAC1 profile（它的 `bInterval=1` 在高速下是 125 µs 而不是 1 ms）。
枚举和控件探测不受影响；如果之后 UAC1 的采集/播放行为异常，先看这里。

**要验证音频端点（`host.attach_enum` / `host.volume_event` / `host.capture_tone`）：** 换一台一次性 Hyper-V VM，**用基本会话连接**
（`vmconnect` 里 View → 取消勾选 Enhanced session；增强会话走 RDP，同样是远程会话），把 zip 拷进去解压，以管理员运行 `run.cmd`（守卫认得 Hyper-V 来宾）。
其余流程相同。开始前先看 `host_probe session` 的输出：`remoteSession` 应为 `false`、`consoleSessionId` 应是个正常的会话号，否则这一步还是不会有端点。
（Sandbox 里已经验证的部分——安装、attach/detach、类驱动认领——不需要换环境。）

## 加一个用例

在 `auto_test.cpp` 里写 `void CaseXxx(Ctx&)`（用 `c.Expect` / `c.Fail` / `c.Skip` / `c.Note`；做不到就 `Skip` 并说原因，
**不要悄悄通过**），加进 `Cases()`；对称地写一条"去掉前提必须失败"的对照。`tests/test_auto_test.cpp` 的完整性检查会逼你补全。
