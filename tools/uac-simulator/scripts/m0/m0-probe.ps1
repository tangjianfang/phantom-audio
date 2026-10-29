# m0-probe.ps1 —— M0 探针执行（普通权限）
# 前置：elevated-m0-vm.ps1 已建 VM 并完成安装；usbip-win2 驱动已装（elevated-install-driver.ps1）
# 产出：docs/m0-report.md 素材（控制台输出 + m0.pcap 黄金抓包）
param(
    [string]$Subnet = '192.168.77',
    [string]$Key = "$PSScriptRoot\id_m0"
)
$ErrorActionPreference = 'Continue'

function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

# ---- 1) 找 VM IP（扫 22 端口）----
Section "定位 VM（$Subnet.2-254:22）"
$vmIp = $null
foreach ($i in 2..40) {
    $ip = "$Subnet.$i"
    if (Test-NetConnection -ComputerName $ip -Port 22 -InformationLevel Quiet -WarningAction SilentlyContinue) {
        $vmIp = $ip; break
    }
}
if (-not $vmIp) { Write-Host "未找到 VM。确认 VM 已装完系统（控制台无安装界面）" -ForegroundColor Red; exit 1 }
Write-Host "[ok] VM = $vmIp"

$ssh = @('-i', $Key, '-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=NUL', "uac@$vmIp")
function VmRun($cmd) { & ssh @ssh $cmd 2>$null }

# ---- 2) VM 内状态 ----
Section "VM 内 gadget 状态"
VmRun 'sudo cat /var/log/m0-gadget.log'
VmRun 'sudo dmesg | grep -iE "vudc|uac2|gadget" | tail -20'
VmRun 'ss -tlnp 2>/dev/null | grep 3240 || sudo ss -tlnp | grep 3240'

# ---- 3) Windows 侧 attach ----
Section "usbip attach"
$usbip = (Get-Command usbip -ErrorAction SilentlyContinue).Source
if (-not $usbip) { $usbip = 'C:\Program Files\USBip\usbip.exe' }
& $usbip list -r $vmIp
# 取 vudc 总线号（形如 udc-0 / 1-1）
$listOut = & $usbip list -r $vmIp 2>$null
$busid = ($listOut | Select-String -Pattern '^\s*([-\w]+):').Matches.Groups[1].Value | Select-Object -First 1
if (-not $busid) { Write-Host "未发现可导入设备 —— 检查 VM 内 usbipd" -ForegroundColor Red; exit 1 }
Write-Host "[..] attach $busid"
& $usbip attach -r $vmIp -b $busid

# ---- 4) 枚举观察（60s）----
Section "等待 PnP 枚举（最长 60s）"
$dev = $null
foreach ($n in 1..30) {
    Start-Sleep 2
    $dev = Get-PnpDevice -Class MEDIA -Status OK -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match 'UAC2|Gadget|Linux|Multifunction' }
    if ($dev) { break }
}
if ($dev) {
    Write-Host "[ok] 枚举到音频设备：" -ForegroundColor Green
    $dev | Format-List FriendlyName, InstanceId, Status
    $dev | ForEach-Object {
        $svc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
        Write-Host "    service = $svc   (期望 usbaudio2.sys)"
    }
} else {
    Get-PnpDevice -Class MEDIA -Status OK | Format-Table FriendlyName, InstanceId -AutoSize
    Write-Host "未见 Gadget 设备 —— 检查设备管理器/驱动绑定" -ForegroundColor Yellow
}

# ---- 5) 中断端点观察（R6）----
Section "VM 侧中断端点活动（设备树）"
VmRun 'ls /sys/kernel/debug/usb/devices 2>/dev/null | head -1 && sudo cat /sys/kernel/debug/usb/devices 2>/dev/null | grep -A3 "I:*" | head -30'
VmRun 'sudo dmesg | tail -10'

# ---- 6) 黄金抓包（20s 回环观察）----
Section "tcpdump 抓包 20s"
VmRun 'sudo timeout 20 tcpdump -i any port 3240 -w /tmp/m0.pcap 2>/dev/null; ls -la /tmp/m0.pcap'
& scp -i $Key -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL "uac@${vmIp}:/tmp/m0.pcap" "$PSScriptRoot\m0.pcap" 2>$null
if (Test-Path "$PSScriptRoot\m0.pcap") { Write-Host "[ok] 黄金抓包 -> $PSScriptRoot\m0.pcap" }

Write-Host ""
Write-Host "=== M0 判定输入齐备 ===" -ForegroundColor Green
Write-Host "Go 条件：上面枚举到设备且 service=usbaudio2.sys；播放/录制可在 声音设置 手动验证 10s"
Write-Host "结论请写入 docs/m0-report.md（模板见 scripts/m0/m0-report-template.md）"
