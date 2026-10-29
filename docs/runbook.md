# 运行手册（Runbook）

## 驱动安装（一次性，管理员）
1. 从 github.com/vadimgrn/usbip-win2/releases 下载 **v0.9.7.8 x64** 安装包到 `tools/uac-simulator/third_party/usbip-win2/`
2. 管理员 PowerShell：`powershell -File tools/uac-simulator/scripts/elevated-install-driver.ps1`
   （自动建还原点；安装会短暂重启所有 USB3 集线器；锁版本，升级前先跑 L3 回归）

## 常见问题

| 症状 | 处置 |
|---|---|
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
- 日常：`powershell -File tools/uac-simulator/scripts/regression.ps1`
- 驱动环境：`... regression.ps1 -WithDriver`
- 覆盖率：`... coverage.ps1`
