# envcheck.ps1 - 只读环境体检（非管理员可运行）
# 输出构建工具链 / 虚拟化 / 驱动 / 磁盘状态。兼容 Windows PowerShell 5.1（不用 PS7 语法）。
$ErrorActionPreference = 'Continue'

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

Section "工具链"
foreach ($tool in @('cmake', 'ninja', 'git', 'python')) {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if ($cmd) { Write-Host ("{0,-8} {1}" -f $tool, (& $tool --version 2>$null | Select-Object -First 1)) }
    else { Write-Host ("{0,-8} MISSING" -f $tool) -ForegroundColor Yellow }
}
# 与 scripts\msvc-env.cmd 同一方法：vswhere 定位，不写死 VS 年份/版本
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = $null
if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if ($vsRoot) { Write-Host ("msvc     {0}" -f $vsRoot) } else { Write-Host "msvc     NOT FOUND (需要带 C++ x64 工具集的 Visual Studio)" -ForegroundColor Yellow }

# CMake 会把 PATH 里先命中的无扩展名 ninja 包装脚本（如 depot_tools\ninja）当成可执行文件。
$ninjaHits = @(& where.exe ninja 2>$null)
if ($ninjaHits.Count -gt 0 -and $ninjaHits[0] -notmatch '\.exe$') {
    Write-Host ("ninja    PATH 首个命中是无扩展名脚本: {0}（scripts\msvc-env.cmd 会把真正的 ninja.exe 排到前面）" -f $ninjaHits[0]) -ForegroundColor Yellow
}

Section "权限与虚拟化"
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host ("IsAdmin            : {0}" -f $isAdmin)
Write-Host ("HypervisorPresent  : {0}" -f (Get-CimInstance Win32_ComputerSystem).HypervisorPresent)
. "$PSScriptRoot\disposable.ps1"
$d = Get-DisposableReason
if ($d) { Write-Host ("一次性机器         : 是（{0}）—— 可以跑 host.* 用例" -f $d) }
else { Write-Host "一次性机器         : 否 —— 宿主侧用例/装驱动脚本会拒绝在这里运行（用 sandbox\launch-sandbox.ps1）" }
$sandbox = Test-Path (Join-Path $env:SystemRoot 'System32\WindowsSandbox.exe')
Write-Host ("Windows Sandbox    : {0}" -f $(if ($sandbox) { '已启用' } else { '未启用（Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All）' }))

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
