# 运行手册（Runbook）

## 驱动安装与宿主侧验证：默认 Sandbox / VM；开发机须显式开启

**默认不在开发机上做宿主侧操作**（守卫会拒绝）。**开发机允许真机测试，但必须显式开启**（2026-11 用户决定）：
进程环境变量 **`UACSIM_FORCE_HOST_TESTS=1`**（解锁 GUI 的 Attach/Detach to Windows 与 `--autotest --allow-host`），
脚本加 **`-AllowNonDisposable`**（`elevated-install-driver.ps1`、`run-host-cases.ps1`）。要点：
`usbip attach` 需要**管理员**；非一次性机器装驱动会先建**系统还原点**（别加 `-NoRestorePoint`）；退出码 3010 = 需重启。
Sandbox 流程不变，见 [sandbox.md](./sandbox.md)：

1. 从 github.com/vadimgrn/usbip-win2/releases 下载 x64 安装包到 `tools/uac-simulator/third_party/usbip-win2/`（BSD-2-Clause，随包分发需附 LICENSE；文档锁定并测试过 v0.9.7.8，实机也验证过 0.9.8.1，换版本前先在 Sandbox 里跑一遍用例表，结果按 `VERSION.txt` 里记录的版本归因）
2. **手动拷贝（推荐）：** `build.bat` 构建成功后自动生成 `tools/uac-simulator/dist/uacsim-sandbox-release.zip` → 拷进 Sandbox → 解压 → 双击 `run.cmd`。自动装驱动、起 uacsimd、对 UAC2 / UAC1 各跑一遍用例表，结果在解压目录的 `results\`，拷回来即可
3. **目录映射：** `powershell -File tools/uac-simulator/sandbox/launch-sandbox.ps1` 一条命令启动 Sandbox，结果回到 `sandbox/results/`

在一台一次性 Hyper-V VM 里手动做：把 `sandbox/stage` 拷进去，管理员运行 `run-in-sandbox.ps1`。

## UAC2 采样率宣告的实测约束（Win11 26200 / usbaudio2.sys）

2026-11-01 开发机实跑定位的三个独立问题与最终因果（均已修复并附测试）：

1. **控制写风暴（真凶：`actual_length` 报 0）**——控制写（SET_CUR 类，OUT 带数据阶段）完成曾恒报
   `actual_length=0`，usbaudio2 当作"没写成"无限重发：音频引擎介入设备（选为默认/被应用占用）时
   ~400 次/秒（16 分钟 28.6 万次），设置页卡顿、设备列表闪烁。修复：完成时上报数据阶段字节数。
   修复后引擎介入下 408 行/秒 → 1 行/秒，WASAPI 采集 1000 Hz 正弦无回归。
2. **单采样率 → 音频功能 code 10 不启动**（独立 bug，与上面无关）：枚举走完、无端点，Windows 只
   反复重问 clock RANGE；RES=0/1、numSubRanges=1/2、bmControls 读写/只读、wMaxPacketSize 全部排除，
   唯一变量是"不同速率数 < 2"。对策：`SimHost::on_attach` 拒绝 <2 个不同速率的 uac2 profile。
3. **GUI 日志面板换行重排**——EDIT 自动换行时每次 `SetWindowTextW` 都全文 ScriptItemize，风暴
   内容 0.5s 一整块把 UI 线程烧满（cdb 栈定位）。对策：日志框 `ES_AUTOHSCROLL|WS_HSCROLL` 不换行。

经验矩阵（每行全新 USB 身份独立验证，可作为压力测试参考）：

| `rates` 宣告 | Windows 音频功能 | 引擎介入时 SET_CUR 行为（actual_length 修复后） |
|---|---|---|
| `[48000]`、`[96000]` | ❌ code 10（问题 2） | — |
| `[48000,44100]` | ✅ | ~1 次/秒 |
| `[48000,96000]` | ✅ | ~1 次/秒 |
| `[48000,44100,96000]` | ✅ | ~1 次/秒（修复前是 400 次/秒交替风暴） |

诊断手段（uacsimd `--log-file`，NDJSON）：
- 控制传输每条一行（含 **OUT 载荷十六进制**，`"data":"80bb0000"` 即 48000）——看 Windows 在设什么采样率；
- 每秒一条 `{"event":"urb_stats",...}` 全端点增量（ep0/ok/stall/int81/hid82/fb84/iso86/iso06/unk/unl + 挂起数）——看风暴在哪个端点、多大速率；空闲秒自动静默。

**ISO 吞吐按全局流时间线 pacing**：usbaudio.sys（UAC1）会同时压 ~6 个在飞 iso IN URB；按"每个 URB
到达时刻+span"排程会以 K 倍速率出数（真机实测 6.09× 超采，usbaudio2 因只有 1 个在飞而测不出）。
引擎现在把 URB 排进全局流槽位 `[max(now, 流末端), +span]`，K 个在飞也是墙钟速率。

## 常见问题

| 症状 | 处置 |
|---|---|
| 构建报 `inappropriate file type or format` / `CMAKE_CXX_COMPILER not set` | PATH 里先命中无扩展名的 `ninja` 包装脚本（如 `depot_tools\ninja`），或没有 MSVC 环境。用 `scripts\msvc-env.cmd cmake ...`（vswhere 定位 VS，并把真正的 `ninja.exe` 排前面）；`scripts/envcheck.ps1` 可体检 |
| 配置阶段报 "需要 MSVC 工具链" | CMake 选中了 MinGW g++（CI runner 与不少开发机 PATH 里都有）。同上，经 `msvc-env.cmd` 运行 |
| HTTP API 返回 403 | 请求带了 `Origin` 头（浏览器跨域）或 `Host` 不是回环地址。CLI/GUI/脚本应发 `Host: 127.0.0.1:<port>`，不要带 `Origin` |
| HTTP API 返回 408 | 5 s 内没发完请求（空闲/慢速连接）。客户端应一次发完请求头与体 |
| `/attach` 返回 `invalid profile name` | profile 名只允许 `[A-Za-z0-9_-]`（可带 `.json`）；要用完整路径请走 `uacsimd --profile <path>` |
| `/capture` `/source` 返回 `invalid ... path` | 路径必须是 `.wav`，且不能是 UNC/设备路径、含 `..`、含盘符外的 `:` |
| attach 后设备管理器无声卡 | `usbip list -r 127.0.0.1` 应显示 1-1；查 uacsimd 控制台；确认描述符过 lint（`uacsim status`） |
| USB 设备在线、MEDIA 接口黄色叹号（code 10） | uac2 profile 单采样率（见上面"采样率宣告"一节）；`/attach` 现在会直接拒绝，检查 profile 的 `rates` |
| 声音设置卡顿/设备列表闪烁、日志里 SET_CUR 刷屏 | rates 含 `{48000,44100}` 且还有第三档 → usbaudio2 采样率风暴；把 rates 收到两档 |
| 声卡出现但驱动不是 usbaudio2.sys | 检查 profile protocol=uac2 且 bcdUSB=0x0200 + IAD；IHV 驱动优先级高于内置时可禁用第三方驱动 |
| 音频快进/暴音 | 微帧 pacing 失效——确认走 3240 且引擎未报 underrun（status.inBytes 增速） |
| 长时间流后漂移 | usbip-win2 #200（QueryBusTime 纪元）：重跑 `scripts/m0/m0-probe.ps1` 观察，必要时降版本重测 |
| 端口 3240/3241 占用 | uacsimd `--usbip-port/--http-port` 可改；CI 并行需错开 |

## WPP 日志采集（usbip-win2 侧）
```
tracelog -start usbip-ude -guid #ed18c9c5-8322-48ae-bf78-d01d898a1562 -f usbip-ude.etl -flag 0xF -level 5
# 复现
tracelog -stop usbip-ude
tracefmt -nosummary -o usbip-ude.txt usbip-ude.etl   # 需装驱动时勾选 PDB
```

## BSOD
启用内核转储后用 WinDbg：`!analyze -v; !wdfkd.wdflogdump usbip2_ude`（usbip-win2 README「Debugging a BSOD」）。

## 回归入口
- 日常：`powershell -File tools/uac-simulator/scripts/regression.ps1`（构建 + ctest + GUI 非视觉自检）
- GUI 自检单跑：`... scripts/smoke.ps1`（不判断外观）
- 宿主侧（真实音频栈）：`sandbox/launch-sandbox.ps1`（开发机）；`regression.ps1 -WithDriver` 只在 Sandbox/VM 里有效，开发机上被守卫拒绝
- 覆盖率：`... coverage.ps1`（统计范围见脚本开头注释）
