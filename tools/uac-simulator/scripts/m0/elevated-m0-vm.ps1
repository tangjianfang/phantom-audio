# elevated-m0-vm.ps1 —— M0 探针 VM 一键创建（需管理员 PowerShell）
# 产出：Internal 交换机 UAC-M0 + NAT 192.168.77.0/24 + Gen2 VM uac-m0（Ubuntu 24.04 自动安装）
# 之后运行 scripts/m0/m0-probe.ps1（普通权限）执行探针。
param(
    [string]$IsoDir = "$PSScriptRoot\..\..\third_party\usbip-win2",
    [switch]$SkipDownload
)
$ErrorActionPreference = 'Stop'

# ---- 1) 权限 / 功能检查 ----
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "请用管理员 PowerShell 运行本脚本" }
Import-Module Hyper-V -ErrorAction Stop

# ---- 2) 交换机 + NAT ----
if (-not (Get-VMSwitch -Name UAC-M0 -ErrorAction SilentlyContinue)) {
    $sw = New-VMSwitch -SwitchName UAC-M0 -SwitchType Internal
    New-NetIPAddress -IPAddress 192.168.77.1 -PrefixLength 24 `
        -InterfaceAlias "vEthernet ($($sw.Name))" | Out-Null
    if (-not (Get-NetNat -Name UAC-M0-NAT -ErrorAction SilentlyContinue)) {
        New-NetNat -Name UAC-M0-NAT -InternalIPInterfaceAddressPrefix 192.168.77.0/24 | Out-Null
    }
    Write-Host "[ok] 交换机 UAC-M0 + NAT 192.168.77.0/24"
}

# ---- 3) Ubuntu ISO ----
$iso = Join-Path $IsoDir 'ubuntu-24.04-live-server-amd64.iso'
if (-not (Test-Path $iso)) {
    if ($SkipDownload) { throw "ISO 不存在：$iso（-SkipDownload 时需手动放置）" }
    Write-Host "[..] 下载 Ubuntu Server 24.04 ISO（约 2.6GB，进度条请耐心）"
    $url = 'https://releases.ubuntu.com/24.04/ubuntu-24.04-live-server-amd64.iso'
    Invoke-WebRequest -Uri $url -OutFile $iso -UseBasicParsing
}
Write-Host "[ok] ISO: $iso"

# ---- 4) seed ISO（cloud-init autoinstall）----
$seed = Join-Path $PSScriptRoot 'm0-seed.iso'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'make-seed-iso.ps1') `
    -UserDataPath (Join-Path $PSScriptRoot 'm0-user-data.yaml') -OutIso $seed
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $seed)) { throw "seed ISO 生成失败" }

# ---- 5) VM ----
$vmName = 'uac-m0'
$vhdPath = Join-Path $IsoDir 'uac-m0.vhdx'
if (-not (Get-VM -Name $vmName -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path $vhdPath)) {
        New-VHD -Path $vhdPath -SizeBytes 10GB -Dynamic | Out-Null
    }
    New-VM -Name $vmName -Generation 2 -MemoryStartupBytes 2GB `
        -VHDPath $vhdPath -SwitchName UAC-M0 | Out-Null
    Set-VMProcessor -VMName $vmName -Count 2
    Add-VMDvdDrive -VMName $vmName -Path $iso
    Add-VMDvdDrive -VMName $vmName -Path $seed
    $dvd = Get-VMDvdDrive -VMName $vmName
    Set-VMFirmware -VMName $vmName -FirstBootDevice $dvd[0] -EnableSecureBoot Off
    Write-Host "[ok] VM $vmName 创建（2C/2GB/10GB 动态盘）"
}

Start-VM -Name $vmName
Write-Host ""
Write-Host "=== 下一步 ===" -ForegroundColor Green
Write-Host "1. 若 VM 控制台出现 'Confirm destructive action'，按 Enter 确认（autoinstall 其余无人值守，约 5-10 分钟）"
Write-Host "2. 安装完成自动重启进入系统并启动 m0-gadget 服务（导出 f_uac2）"
Write-Host "3. 用普通权限 PowerShell 运行 tools/uac-simulator/scripts/m0/m0-probe.ps1"
