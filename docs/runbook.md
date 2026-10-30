# 运行手册（Runbook）

## 驱动安装与宿主侧验证：只在 Sandbox / VM 里做

**开发机上不装 usbip-win2 驱动、不 `usbip attach`、不做真机测试。** 驱动安装脚本和宿主侧用例脚本都有
一次性机器守卫，在开发机上会直接拒绝。完整流程见 [sandbox.md](./sandbox.md)：

1. 从 github.com/vadimgrn/usbip-win2/releases 下载 **v0.9.7.8 x64** 安装包到 `tools/uac-simulator/third_party/usbip-win2/`（GPLv3，不随仓库分发；项目锁定并测试的是这个版本，换版本前先在 Sandbox 里跑一遍用例表，结果按 `VERSION.txt` 里记录的版本归因）
2. **手动拷贝（推荐）：** `build.bat` 构建成功后自动生成 `tools/uac-simulator/dist/uacsim-sandbox-release.zip` → 拷进 Sandbox → 解压 → 双击 `run.cmd`。自动装驱动、起 uacsimd、对 UAC2 / UAC1 各跑一遍用例表，结果在解压目录的 `results\`，拷回来即可
3. **目录映射：** `powershell -File tools/uac-simulator/sandbox/launch-sandbox.ps1` 一条命令启动 Sandbox，结果回到 `sandbox/results/`

在一台一次性 Hyper-V VM 里手动做：把 `sandbox/stage` 拷进去，管理员运行 `run-in-sandbox.ps1`。

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
