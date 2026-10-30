<#
.SYNOPSIS
    Run the host-side (host.*) cases on a disposable machine: start uacsimd, run
    "uacstudio.exe --autotest --allow-host" once for every profile, and summarise the results.
    Replaces the old m2-verify.ps1 (which attached on any machine).

.DESCRIPTION
    The case table is the same one behind the GUI "Run auto test" button and the command-line --autotest
    (see apps/uacstudio/auto_test.h). Two profiles are run by default:
      uac2-48k         full check of the UAC2 path (enumeration, usbaudio2.sys binding, volume/mute callbacks, captured tone)
      uac1-cmedia-sim  the same table with UAC1: the result of host.volume_event is the measured answer to R-006
                       (does usbaudio.sys react to the Status Word; a failure is not necessarily a bug, it may simply not react)

    Guard: refuses everything that is not Windows Sandbox / a VM (-AllowNonDisposable overrides explicitly). No real-device tests on the development machine.
    The driver must be installed first (sandbox\run-in-sandbox.ps1 does it; or run scripts\elevated-install-driver.ps1 by hand).

.PARAMETER BinDir
    Folder with uacsimd.exe / uacstudio.exe / host_probe.exe; the profiles folder is below it, or two levels up in the source tree.
#>
param(
    [string]$BinDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release'),
    [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\host-cases'),
    [string[]]$Profiles = @('uac2-48k', 'uac1-cmedia-sim'),
    [int]$HttpPort = 3241,
    [int]$UsbipPort = 3240,
    [switch]$AllowNonDisposable
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\disposable.ps1"
Assert-DisposableMachine -Allow:$AllowNonDisposable -What 'run-host-cases.ps1'

foreach ($exe in 'uacsimd.exe', 'uacstudio.exe', 'host_probe.exe') {
    if (-not (Test-Path (Join-Path $BinDir $exe))) { throw "Cannot find $BinDir\$exe (build first, or stage it with sandbox\launch-sandbox.ps1)" }
}
$profilesDir = @((Join-Path $BinDir 'profiles'), (Join-Path (Split-Path -Parent $PSScriptRoot) 'profiles')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $profilesDir) { throw "Cannot find the profiles folder (below $BinDir or in the source tree)" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

if (-not (Get-Service usbip2_ude -ErrorAction SilentlyContinue)) {
    Write-Warning "Service usbip2_ude does not exist: the host.* cases will SKIP (env.driver). Install the driver first: scripts\elevated-install-driver.ps1"
}

Write-Host "Starting uacsimd (usbip $UsbipPort / http $HttpPort, profiles $profilesDir)"
$sim = Invoke-Retry { Start-Process (Join-Path $BinDir 'uacsimd.exe') -PassThru -WindowStyle Hidden -ErrorAction Stop -ArgumentList @(
    '--usbip-port', $UsbipPort, '--http-port', $HttpPort, '--profiles', "`"$profilesDir`"",
    '--log-file', "`"$(Join-Path $OutDir 'uacsimd.ndjson')`"",
    '--parent-pid', $PID) }   # the simulator ends by itself if this script is killed - the finally block below is not the only exit
$failed = 0
$summary = New-Object System.Collections.Generic.List[string]
try {
    $ready = $false
    for ($i = 0; $i -lt 40 -and -not $ready; $i++) {
        try { $null = Invoke-RestMethod "http://127.0.0.1:$HttpPort/status" -TimeoutSec 2; $ready = $true }
        catch { Start-Sleep -Milliseconds 500 }
    }
    if (-not $ready) { throw "uacsimd did not answer http://127.0.0.1:$HttpPort/status within 20 s" }

    foreach ($prof in $Profiles) {
        Write-Host "`n=== autotest: host profile $prof ===" -ForegroundColor Cyan
        $profOut = Join-Path $OutDir $prof
        New-Item -ItemType Directory -Force $profOut | Out-Null
        $p = Invoke-Retry { Start-Process (Join-Path $BinDir 'uacstudio.exe') -Wait -PassThru -ErrorAction Stop -ArgumentList @(
            '--autotest', '--allow-host', '--profile', $prof, '--port', $HttpPort,
            '--out', "`"$profOut`"", '--exe-dir', "`"$BinDir`"") }
        $report = Get-ChildItem $profOut -Filter 'uacstudio-autotest-*.txt' |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $line = if ($report) { (Get-Content $report.FullName -TotalCount 1) } else { 'RESULT: (no report file)' }
        $summary.Add(("{0,-18} exit {1}  {2}" -f $prof, $p.ExitCode, $line))
        if ($report) {
            Get-Content $report.FullName | Where-Object { $_ -match '^(PASS|FAIL|SKIP)  ' } | ForEach-Object { Write-Host "  $_" }
        }
        if ($p.ExitCode -ne 0) { $failed++ }
    }
}
finally {
    if ($sim -and -not $sim.HasExited) { Stop-Process -Id $sim.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host "`n=== Summary ===" -ForegroundColor Cyan
$summary | ForEach-Object { Write-Host $_ }
$summary | Out-File (Join-Path $OutDir 'summary.txt') -Encoding utf8
Write-Host "Results folder: $OutDir"
if ($failed -gt 0) { exit 1 }
exit 0
