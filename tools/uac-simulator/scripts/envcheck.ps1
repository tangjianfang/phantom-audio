# envcheck.ps1 — 只读环境体检（非管理员可运行）
# 输出构建工具链 / 虚拟化 / 驱动 / 磁盘状态，供 M0 前置判断。
$ErrorActionPreference = 'Continue'

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

Section "工具链"
foreach ($tool in @('cmake', 'ninja', 'git', 'python')) {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if ($cmd) { Write-Host ("{0,-8} {1}" -f $tool, (& $tool --version 2>$null | Select-Object -First 1)) }
    else { Write-Host ("{0,-8} MISSING" -f $tool) -ForegroundColor Yellow }
}
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
Write-Host ("msvc     {0}" -f (Test-Path $vs ? 'vcvars64 OK' : 'NOT FOUND'))

Section "权限与虚拟化"
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host ("IsAdmin            : {0}" -f $isAdmin)
Write-Host ("HypervisorPresent  : {0}" -f (Get-CimInstance Win32_ComputerSystem).HypervisorPresent)
try { $vm = Get-VM -ErrorAction Stop | Select-Object -First 3; Write-Host ("Hyper-V Get-VM     : OK ({0})" -f (($vm | ForEach-Object Name) -join ', ')) }
catch { Write-Host "Hyper-V Get-VM     : 不可用（需管理员或 Hyper-V 未启用）" }

Section "usbip-win2 驱动"
$svc = Get-Service -Name usbip2_ude -ErrorAction SilentlyContinue
if ($svc) { Write-Host ("usbip2_ude 服务    : {0}" -f $svc.Status) }
else { Write-Host "usbip2_ude 服务    : 未安装" }
$usbipExe = Get-Command usbip -ErrorAction SilentlyContinue
if ($usbipExe) { Write-Host ("usbip.exe          : {0}" -f $usbipExe.Source) }

Section "磁盘"
Write-Host ("C: 剩余 {0:N1} GB" -f ((Get-PSDrive C).Free / 1GB))

Section "音频端点"
Write-Host ("渲染端点数         : {0}" -f (@(Get-CimInstance -Namespace root/cimv2 -ClassName Win32_SoundDevice -ErrorAction SilentlyContinue)).Count)
