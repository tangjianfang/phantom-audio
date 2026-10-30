# elevated-install-driver.ps1 - install usbip-win2 v0.9.7.8 from the official installer (needs administrator).
# Steps: [restore point] -> silent install -> verify the service. Note: installing briefly restarts every USB3 hub.
#
# Only run it on a disposable machine (Windows Sandbox / a VM): it installs a kernel driver and we do no real-device tests on the development machine.
# Inside Sandbox no restore point is needed (the whole environment is thrown away, and Sandbox does not support restore points), so it is skipped.
# The GUI / CLI path (uacstudio --install-driver, slim payload) does not use this script; this is the fallback for the official installer.
param(
    [string]$InstallerDir = "$PSScriptRoot\..\third_party\usbip-win2",
    [switch]$NoRestorePoint,
    [switch]$AllowNonDisposable   # explicit switch for when you know the machine can be thrown away
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\disposable.ps1"

# Guard first: on a development machine the most useful message is "this must not run here", not "run as administrator"
# (being refused after elevation is wasted effort)
Assert-DisposableMachine -Allow:$AllowNonDisposable -What 'elevated-install-driver.ps1'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { throw "Run this from an administrator PowerShell" }

# A restore point is pointless on a disposable machine (and Sandbox does not support it); only a non-disposable machine that was explicitly allowed gets one
$disposable = [bool](Get-DisposableReason)
if (-not $NoRestorePoint -and -not $disposable) {
    Write-Host "[1/3] Creating a system restore point (about 30-60 s) ..."
    Enable-ComputerRestore -Drive "$env:SystemDrive\"
    Checkpoint-Computer -Description "before-usbip-win2-install" -RestorePointType MODIFY_SETTINGS
} else {
    Write-Host "[1/3] Skipping the restore point (disposable machine / -NoRestorePoint)"
}

Write-Host "[2/3] Installing usbip-win2 ..."
# Top level only: the same folder may also hold SendboxUSBip\USBip\ copied out of a Sandbox (its usbip.exe/devnode.exe are not an installer) and the source tree
$pkg = Get-ChildItem $InstallerDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.msi', '.exe' -and $_.Name -match '0?9\.7\.8|USBip' } | Select-Object -First 1
if (-not $pkg) {
    Write-Host "Installer not found. Go to https://github.com/vadimgrn/usbip-win2/releases" -ForegroundColor Yellow
    Write-Host "and download the x64 installer (.msi/.exe; the version pinned by the project docs is v0.9.7.8) into: $InstallerDir"
    Write-Host "(usbip-win2 is a separate project under the BSD-2-Clause license; when you ship its binaries, include its LICENSE.txt)"
    exit 1
}
if ($pkg.Extension -eq '.msi') {
    $p = Start-Process msiexec.exe -ArgumentList "/i", "`"$($pkg.FullName)`"", "/qn" -Wait -PassThru
} else {
    $p = Start-Process $pkg.FullName -ArgumentList "/S" -Wait -PassThru
}
Write-Host "Installer exit code: $($p.ExitCode) (3010 = reboot required)"

Write-Host "[3/3] Verifying ..."
$svc = Get-Service usbip2_ude -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "[ok] service usbip2_ude = $($svc.Status)" -ForegroundColor Green
    $usbip = Get-Command usbip -ErrorAction SilentlyContinue
    if (-not $usbip -and (Test-Path "$env:ProgramFiles\USBip\usbip.exe")) { $usbip = Get-Command "$env:ProgramFiles\USBip\usbip.exe" }
    if ($usbip) { & $usbip.Source --version }
    exit 0
}
Write-Host "The service did not appear - check the installer type/signature by hand; if the exit code $($p.ExitCode) is 3010, reboot and check again" -ForegroundColor Red
exit 2
