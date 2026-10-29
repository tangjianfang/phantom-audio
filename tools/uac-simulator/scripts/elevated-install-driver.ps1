# elevated-install-driver.ps1 —— 安装 usbip-win2 v0.9.7.8（需管理员）
# 步骤：还原点 → 静默安装 → 验证服务。注意：安装会短暂重启所有 USB3 集线器。
param(
    [string]$InstallerDir = "$PSScriptRoot\..\third_party\usbip-win2"
)
$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "请用管理员 PowerShell 运行" }

Write-Host "[1/3] 创建系统还原点（约 30-60s）…"
Enable-ComputerRestore -Drive "$env:SystemDrive\"
Checkpoint-Computer -Description "before-usbip-win2-install" -RestorePointType MODIFY_SETTINGS

Write-Host "[2/3] 安装 usbip-win2 …"
$pkg = Get-ChildItem $InstallerDir -Include *.msi, *.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object Name -match '0?9\.7\.8|USBip' | Select-Object -First 1
if (-not $pkg) {
    Write-Host "未找到安装包。请到 https://github.com/vadimgrn/usbip-win2/releases" -ForegroundColor Yellow
    Write-Host "下载 v0.9.7.8 的 x64 安装包（.msi/.exe）放入：$InstallerDir"
    Write-Host "（仅内部测试用途，遵守 GPLv3）"
    exit 1
}
if ($pkg.Extension -eq '.msi') {
    Start-Process msiexec.exe -ArgumentList "/i", "`"$($pkg.FullName)`"", "/qn" -Wait
} else {
    Start-Process $pkg.FullName -ArgumentList "/S" -Wait
}

Write-Host "[3/3] 验证 …"
$svc = Get-Service usbip2_ude -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "[ok] 服务 usbip2_ude = $($svc.Status)" -ForegroundColor Green
    $usbip = Get-Command usbip -ErrorAction SilentlyContinue
    if ($usbip) { & $usbip.Source --version }
} else {
    Write-Host "服务未出现 —— 手动检查安装包类型/签名" -ForegroundColor Red
}
