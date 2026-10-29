# m2-verify.ps1 - M2 live-machine verification (requires usbip-win2 driver installed)
# Flow: start uacsimd (UAC2) -> attach -> assert usbaudio2.sys binding -> sample-rate
#       switching -> Status Word events (host-probe or manual) -> UAC1 interrupt check (R-006)
param(
    [string]$SimDir = "$PSScriptRoot\..\build\release",
    [switch]$Uac1   # verify UAC1 profile (R-006: does usbaudio.sys respond to Status Word)
)
$ErrorActionPreference = 'Continue'
function Section($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

# Precondition: driver installed
if (-not (Get-Service usbip2_ude -ErrorAction SilentlyContinue)) {
    Write-Host "usbip2_ude not installed - run scripts/elevated-install-driver.ps1 first" -ForegroundColor Red
    exit 1
}

$profile = 'uac2-48k'
$expect = 'usbaudio2'
if ($Uac1) { $profile = 'uac1-cmedia-sim'; $expect = 'usbaudio' }

Section "Start simulator ($profile)"
$exe = Join-Path $SimDir 'uacsimd.exe'
if (-not (Test-Path $exe)) { Write-Host "missing $exe - run build.bat first"; exit 1 }
$sim = Start-Process $exe -ArgumentList '--port','3240' -PassThru -WindowStyle Minimized
Start-Sleep 1

Section "attach"
$usbip = (Get-Command usbip -ErrorAction SilentlyContinue).Source
if (-not $usbip) { $usbip = 'C:\Program Files\USBip\usbip.exe' }
& $usbip attach -r 127.0.0.1 -b 1-1
Start-Sleep 3

Section "Driver binding assertion (expect $expect)"
$devs = Get-PnpDevice -Class MEDIA -Status OK | Where-Object { $_.FriendlyName -match 'UAC|Phantom|Simulator' }
$found = $false
foreach ($d in $devs) {
    $svc = (Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue).Data
    Write-Host "$($d.FriendlyName) -> $svc"
    if ($svc -like "$expect*") { $found = $true }
}
if ($found) { Write-Host "[PASS] class driver binding correct" -ForegroundColor Green }
else { Write-Host "[FAIL] $expect binding not found" -ForegroundColor Red }

Section "Sample rate / events (host-probe automates once wired; manual checklist below)"
Write-Host @"
Manual checklist (Sound settings -> device properties):
 1. Switch default format 44.1/48/96 kHz - simulator log should show CUR_FREQ SET
 2. Inject events: uacsim event volume_up - watch Windows volume UI change
 3. UAC1 run (-Uac1): if volume bar does not move = usbaudio.sys lacks Status Word
    support (R-006) -> enable the HID channel instead
"@

Section "Cleanup"
& $usbip detach -p 1 2>$null
Stop-Process -Id $sim.Id -Force -ErrorAction SilentlyContinue
Write-Host "Write conclusions to docs/m2-report.md"
