# phantom-audio 项目约定

Windows 纯软件虚拟 USB 音频设备（UAC1/UAC2）仿真测试台。源码在 `tools/uac-simulator/`（C++20，MSVC）。
面向 Claude Code 会话：下面是容易再踩的坑和必须遵守的做法。

## 构建与测试

所有构建命令都经 `tools/uac-simulator/scripts/msvc-env.cmd`（用 vswhere 找 VS 并初始化 MSVC 环境；本机与 CI 同一入口）：

```
cd tools/uac-simulator
./scripts/msvc-env.cmd cmake --preset win-x64-debug
./scripts/msvc-env.cmd cmake --build --preset win-x64-debug
./scripts/msvc-env.cmd ctest --preset win-x64-debug --output-on-failure
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/smoke.ps1      # GUI 非视觉自检
```

- 警告当错误（`/WX`）：窄化转换、未使用参数、**C4129 无效转义**都会让构建失败。别用 `UAC_WARNINGS_AS_ERRORS=OFF` 绕过。
- 没有 MSVC 环境时 CMake 会选 MinGW g++（配置"成功"、编译才失败）；`CMakeLists.txt` 现在对非 MSVC 直接报错。
- `.cmd`/`.bat`：**纯 ASCII + CRLF**（cmd 按 OEM 代码页解析，UTF-8 注释会被拆成乱码命令）。`.ps1`：UTF-8 **带 BOM**（PowerShell 5.1 把无 BOM 的 UTF-8 当 ANSI），且只用 5.1 语法（Sandbox 里自带的是它）。见 `.gitattributes`。
- 含反斜杠的文本（Windows 路径、UNC、JSON 转义）**不要用 Bash heredoc 或命令行手敲**：转义会被吃掉一层。用写文件工具落盘；测试里用 `char bs = 0x5C` 拼接，不写 `"\\"` 字面量。
- Windows 头文件宏会吃标识符：不要把变量命名为 `small`（`rpcndr.h`: `#define small char`）、`min`/`max`（已有 `NOMINMAX`）。
- **PowerShell 里别用 `Get-FileHash`：** 装了 PS7 的机器上系统 `PSModulePath` 常把 PS7 的模块目录排在 Windows PowerShell 前面，5.1 里会"找不到命令"。哈希用 .NET（`[Security.Cryptography.SHA256]::Create()`，见 `package-sandbox.ps1`）。
- **zip 条目名必须用正斜杠：** `Compress-Archive` 和 `ZipFile.CreateFromDirectory` 在 `powershell.exe`（5.1，运行时按 .NET 4.5 兼容）里都写反斜杠。逐个用 `ZipArchive.CreateEntryFromFile` 写入并显式 `Replace('\','/')`。
- **从 bash 调批处理要给绝对 Windows 路径：** `cmd //c "$(cygpath -w "$PWD/x.bat")"`；`cmd //c x.bat` 会报 "not recognized"。另外 `cd` 进一个目录后别再删它（`rm` 会因 "busy" 失败，`&&` 链随之中断，后面的命令根本没跑，而你可能没察觉）。
- **驱动安装（`apps/uacstudio/driver_installer.cpp`）：** 复刻官方 `setup.iss` 的核心步骤（pnputil filter → devnode ude），载荷是 `third_party/usbip-win2/usbip-payload/`（约 3 MB，整个 `third_party/usbip-win2/` 被 `.gitignore`，**别提交**）。所有碰系统的动作都在 `InstallDeps` 里做成可注入的缝；改这个文件必须保持测试里"守卫拒绝 / 非管理员 / 载荷损坏时零系统命令、安装目录不被创建"和"已装过则什么都不做"（`devnode install` 每次都新建设备节点）。**不要在开发机运行 `--install-driver`**（守卫会拒绝，退出码 2）。usbip-win2 许可是 **BSD-2-Clause**（不是 GPLv3，核对过两个标签），随包分发其二进制要带 `LICENSE-usbip-win2.txt`。
- **PowerShell 里刚解压的 exe 立刻 `Start-Process` 可能偶发 "Access is denied"**（杀毒实时扫描占用，实测几秒后同样调用即成功）：用 `scripts/disposable.ps1` 里的 `Invoke-Retry`（只对"启动失败"重试，不对"跑完但退出码非 0"重试）。
- **临时脚本也要遵守编码规则：** 带中文的 `.ps1` 没有 BOM，PowerShell 5.1 按 ANSI 读，多字节字符会吞掉换行导致莫名其妙的解析错误；一次性脚本用纯 ASCII 注释。
- **UAC 描述符必须按规范表格逐字节核对（`lint_uac2` 会拦一部分）：** `usbaudio2.sys` 对不合规的描述符**不报错**，只是不启动音频功能——症状是设备枚举成功、`SET_CONFIGURATION` 之后设备侧日志（`uacsimd --log-file`）里**没有任何类请求**，Windows 里没有端点。第一次 Sandbox 运行就是这样：UAC1 设备被认领并探测了全部控件，UAC2 设备毫无反应，原因是 CS AS General 少 1 字节（规范 16）、CS 端点描述符类型写成 0x24（应为 0x25）。新增/改描述符时：长度对 ADC 表格（UAC2 AS General 16、Input Terminal 17、Output Terminal 12、Clock Source 8、CS 端点 8），CS 端点类型 0x25 且只跟在等时数据端点后（反馈端点没有），字符串索引 0 返回 LANGID；并给 lint 补"违规 → 报错 / 去掉违规 → 通过"两侧。
- **打包：** `build.bat`（release/debug）成功后自动调用 `scripts/package-sandbox.ps1` 生成 `dist/uacsim-sandbox-<配置>.zip`；`UAC_NO_PACKAGE=1` 跳过。清单只此一处，`launch-sandbox.ps1` 复用它。

## 验证分层（从便宜到贵）

| 层 | 在哪跑 | 内容 | 入口 |
|---|---|---|---|
| ① 单测 + 回环场景 | 开发机 | 协议/描述符/引擎/注入/SimHost/控制 API 加固/控制层/用例运行器 | `ctest` |
| ② GUI 非视觉自检 | 开发机，无需管理员/设备 | 逐控件深色主题 + 几何 + 控制层逻辑 + **UI 线程响应性探测**（无 uacsimd 时，主窗口与 `--env-check` 窗口 7 秒内每次探测须在 400 ms 内被处理） | `scripts/smoke.ps1` |
| ③ 一键自动测试 | `api.*` 任何机器；`host.*` **仅 Sandbox/VM** | 真的 attach、WASAPI 枚举、驱动绑定、音量回调、采集音调 | GUI **Auto test** 按钮 / `uacstudio --autotest` / `sandbox/launch-sandbox.ps1` |
| ④ 人工确认 | 人 | ①–③ 判断不了的（外观、真对端软件、长时间稳定性） | 每份自动测试结果文件末尾的清单 |

## 硬规则

1. **开发机允许真机测试，但必须显式开启（2026-11-01 用户决定，usbip-win2 0.9.8.1 已装并实跑验证）。** 默认仍被一次性机器守卫拒绝（`scripts/disposable.ps1`、`IsDisposableMachine()`）；旁路只有两个正规开关：进程环境变量 **`UACSIM_FORCE_HOST_TESTS=1`**（解锁 GUI 的 Attach/Detach to Windows 与 `--autotest --allow-host` 的 host.* 用例，`auto_test.cpp` 的 `HostCasesAllowed`）和脚本的 **`-AllowNonDisposable`**（`elevated-install-driver.ps1`、`run-host-cases.ps1`）。要点：`usbip attach` 需要**管理员**（非提权进程必败，`host_ops.cpp` 会提示）；提权跑 `elevated-install-driver.ps1` 在非一次性机器上会**先建系统还原点**（别加 `-NoRestorePoint`）；装完可能要求重启（退出码 3010）。宿主侧验证仍可走 `build.bat` 的 zip + Sandbox（不装驱动的机器）。**守卫必须放在"步骤失败只记录、继续跑"的包装之外**：早期的 `run-in-sandbox.ps1` 把守卫写成了一个可失败的步骤，守卫失败后脚本照样往下跑。测试这类脚本时，确认你运行的是**刚生成的**副本，而不是上一轮遗留的暂存目录。
2. **只能在目标系统验证的功能必须带一键自动测试。** 新功能 = 往 `apps/uacstudio/auto_test.cpp` 的 `Cases()` 加用例（不要另起按钮/脚本），并保持 `tests/test_auto_test.cpp` 的完整性检查通过（id 唯一、有说明、清理在最后、host.* 都 SKIP 守卫）。做不到自动验证的写进 `ManualChecklist()`，不要悄悄省略。
3. **检查要有牙齿。** 凡是"设了某个值让检查通过"的功能，要有对称的反例：去掉它检查必须失败。新增安全/超时/过滤规则时，写"违规 → 拒绝"和"同一请求去掉违规点 → 通过"两侧，并至少做一次变异验证（临时改回缺陷版本，确认新测试会红，再恢复）。
4. **不做视觉审查。** UI 改动：编译通过 + `smoke.ps1` 通过即停，并明确列出"哪些界面没人看过"。不要为外观自行截图。
5. **如实写验证状态。** 提交信息/文档/交付说明里分开写"验证过的（在哪验证）"和"没验证的"。`host.*` 用例在 Sandbox 里实跑之前一律是"未验证"；读源码得到的原因不要写成已证实。
6. **本地 HTTP 控制 API 是安全边界。** 回环 + 无认证，靠这几条防浏览器攻击：拒绝带 `Origin` 的请求、`Host` 必须是回环、profile 名走白名单（`uac::policy::valid_profile_name`）、媒体路径走 `valid_media_path`、请求体 ≤1 MiB、整请求总时限。新增端点必须经过同样的策略；客户端（`http_client.h`、`uacsim`）发真实的 `Host: 127.0.0.1:<port>`。`POST /shutdown`（停掉进程）是最敏感的一个，有专门的"网页碰不到"测试（`test_hardening.cpp`）。
7. **UI 线程永远不等。** 窗口过程里不做任何可能阻塞的事：网络（Windows 上阻塞式 `connect()` 连没人监听的回环端口实测要等 **约 2.0 秒**）、磁盘/SHA-256、起进程、查服务、`ShellExecute`。统一做法：后台线程做事，用 `PostMessage` 把**堆上分配的载荷**投回（接收方释放；投递失败要自己 `delete`），窗口当时就显示"正在干什么"（状态栏 / `Checking: ...`）。线程忙时关窗要**推迟**到它回报之后（线程不得访问已销毁的窗口）。`http_client.h` 已带 300 ms 连接上限 + 2 s 读写超时，别绕过；新增窗口/流程必须被 `smoke.ps1` 的响应性探测覆盖（`uacstudio --port <关闭的端口> [--env-check]`）。
8. **界面文字一律英文（ASCII）。** Sandbox 镜像没有 CJK 字体，中文会显示成方块。范围：GUI 文案、`uacsimd`/`uacsim` 输出与错误信息、Sandbox 内执行的脚本和 zip 里的 `README.txt`。`…`、`·`、`—` 这类非 ASCII 符号也一样会乱码。源码注释和只在开发机运行的脚本可以保留中文。`tests/test_env_check.cpp` 会扫描 `apps/ src/ include/` 的字符串字面量，含非 ASCII 就失败。
9. **起了进程、挂了设备、装了东西，就必须有对应的"退出和清理"。** 只有启动没有停止，`uacsimd` 会一直占着 3240/3241 端口和虚拟设备。现状：模拟器可以被 `POST /shutdown` 优雅停止（`uacsim shutdown`、GUI 的 Stop simulator），并带 `--parent-pid`——父进程（GUI / 脚本）一结束它就自己退出，崩溃也不留孤儿；GUI 关窗时会停掉**自己启动的**模拟器（只停自己的：先请求，对方没停且是自己启动的才结束进程，别人启动的绝不强杀）。清理 Windows 侧 usbip 端口时**只摘本模拟器的端口**（`ParseOurUsbipPorts`，认 `usbip://127.0.0.1:3240/`），**不要用 `usbip detach -a`**——在使用者指定的真机上它会连别人的 usbip 设备一起摘掉。新增会起进程/挂设备/装驱动的功能，要同时给出停止路径、父进程消失时的兜底、以及"只动自己创建的东西"的单测（含反例）；GUI 的进程级行为用 `smoke.ps1` 第三阶段那种"启动 → 确认在运行 → 关/杀 → 确认消失"的方式验证。

## 其它

- 提交身份、推送规则见用户级 `~/.claude/CLAUDE.md`（本仓库是 GitHub：禁用全局 git 身份）。
- `docs/superpowers/` 下的 spec/plan 是历史记录，不随实现更新；现状以 `README.md`、`docs/runbook.md`、`docs/sandbox.md`、`docs/acceptance-m4.md` 为准。
