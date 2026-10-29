# m2-verify.ps1 —— M2 实机验证（需 usbip-win2 驱动已装；普通权限）
# 流程：起 uacsimd(UAC2) → attach → 断言 usbaudio2.sys 绑定 → 采样率切换 →
#       Status Word 事件（host-probe volume-watch 或手验）→ UAC1 中断定级（R-006）
param(
    [string]$SimDir = "$PSScriptRoot\..\build\release",
    [switch]$Uac1   # 验证 UAC1 档（R-006：usbaudio.sys 是否响应 Status Word）
)
$ErrorActionPreference = 'Continue'
function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

# 前置检查：驱动
if (-not (Get-Service usbip2_ude -ErrorAction SilentlyContinue)) {
    Write-Host "usbip2_ude 未安装 —— 先跑 scripts/elevated-install-driver.ps1" -ForegroundColor Red
    exit 1
}

Section "启动仿真器（$($Uac1 ? 'UAC1' : 'UAC2')）"
$exe = Join-Path $SimDir 'uacsimd.exe'
if (-not (Test-Path $exe)) { Write-Host "缺 $exe —— 先 cmake --build --preset win-x64-release"; exit 1 }
$sim = Start-Process $exe -ArgumentList '--port','3240' -PassThru -WindowStyle Minimized
Start-Sleep 1

Section "attach"
$usbip = (Get-Command usbip -ErrorAction SilentlyContinue).Source
if (-not $usbip) { $usbip = 'C:\Program Files\USBip\usbip.exe' }
& $usbip attach -r 127.0.0.1 -b 1-1
Start-Sleep 3

Section "驱动绑定断言"
$devs = Get-PnpDevice -Class MEDIA -Status OK | Where-Object { $_.FriendlyName -match 'UAC|Phantom|Simulator' }
$found = $false
foreach ($d in $devs) {
    $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
    Write-Host "$($d.FriendlyName) -> $svc"
    $expect = $Uac1 ? 'usbaudio' : 'usbaudio2'
    if ($svc -like "$expect*") { $found = $true }
}
if ($found) { Write-Host "[PASS] 类驱动绑定正确" -ForegroundColor Green }
else { Write-Host "[FAIL] 未见 $expect 绑定" -ForegroundColor Red }

Section "采样率/事件（host-probe 就绪后自动化；当前提示手验）"
Write-Host @"
手验清单（声音设置 → 设备属性）：
 1. 高级页切换默认格式 44.1/48/96kHz，仿真器日志应出现 CUR_FREQ SET
 2. 注入事件（等 Task 16 控制台就绪）：uacsim event volume_up —— 观察 Windows 音量条变化
 3. UAC1 档加 -Uac1 重跑：若音量条不变 = usbaudio.sys 不支持 Status Word（R-006 → 启用 HID 档）
"@

Section "清理"
& $usbip detach -p 1 2>$null
Stop-Process -Id $sim.Id -Force -ErrorAction SilentlyContinue
Write-Host "结论写入 docs/m2-report.md"
