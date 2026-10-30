<#
.SYNOPSIS
    Run everything inside Windows Sandbox (or a disposable VM), from a clean machine to the final report.
    Two ways to use this script:
      A) zip package: copy it into the Sandbox, extract, double-click run.cmd (see scripts\package-sandbox.ps1; build.bat creates the zip)
      B) .wsb folder mapping: sandbox\launch-sandbox.ps1 writes the config, LogonCommand calls this script
    A failing step is only recorded and the run continues, so one run shows the whole picture.

.DESCRIPTION
    Location independent: Root defaults to the folder of this script.
      - Root writable (zip extracted inside the Sandbox): run in place; results go to <Root>\results.
      - Root read-only (C:\uacsim mapped by the .wsb): copy to C:\uacsim-run first; results go to C:\results (mapped back to the host).
    Driver package: <Root>\usbip-payload first (slim payload), then an official installer in <Root>\drivers or C:\drivers.
    If none is found the host.* cases are SKIPped.

    Flow: verify the package -> guard (must be Sandbox/VM) -> elevate -> writable folder -> install driver ->
          run-host-cases.ps1 (the case table once for uac2-48k and once for uac1-cmedia-sim) -> summary

    If the installer asks for a reboot (exit code 3010) this script only tells you; it never reboots for you.

.PARAMETER VerifyOnly
    Only check SHA256SUMS.txt and exit (0 = intact, 1 = missing/corrupt). Works on any machine and does not touch the system.
#>
param(
    [string]$Root = $PSScriptRoot,
    [string]$ResultsDir,
    [switch]$AllowNonDisposable,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Continue'

# Same approach as scripts\package-sandbox.ps1 (no Get-FileHash: when the PS7 module folder comes first in PSModulePath, 5.1 cannot find it)
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') }
    finally { $fs.Dispose(); $sha.Dispose() }
}

# ---- package integrity: files can get damaged or lost while being copied into the Sandbox ----
function Test-PackageHashes([string]$dir) {
    $f = Join-Path $dir 'SHA256SUMS.txt'
    if (-not (Test-Path $f)) {
        Write-Warning "No SHA256SUMS.txt (staging folder of the folder-mapping flow, or an old package): skipping the integrity check"
        return $true
    }
    $ok = $true
    $n = 0
    foreach ($line in Get-Content $f) {
        if ($line -notmatch '^([0-9A-Fa-f]{64}) \*(.+)$') { continue }
        $want = $Matches[1]; $rel = $Matches[2]; $n++
        $p = Join-Path $dir $rel
        if (-not (Test-Path $p)) { Write-Host "  MISSING  $rel" -ForegroundColor Red; $ok = $false; continue }
        if ((Get-Sha256 $p) -ne $want) { Write-Host "  MISMATCH $rel" -ForegroundColor Red; $ok = $false }
    }
    Write-Host ("Integrity check: {0} files, {1}" -f $n, $(if ($ok) { 'all match' } else { 'some missing or corrupt' }))
    return $ok
}

if ($VerifyOnly) {
    if (Test-PackageHashes $Root) { Write-Host 'package OK'; exit 0 }
    Write-Host 'package CORRUPT - copy the zip again' -ForegroundColor Red
    exit 1
}

# ---- guard first: on a development machine the most useful message is "this must not run here"; elevate after the guard (being refused after elevation is wasted effort) ----
. (Join-Path $Root 'scripts\disposable.ps1')
Assert-DisposableMachine -Allow:$AllowNonDisposable -What 'run-in-sandbox.ps1'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Not running as administrator: restarting this script elevated ...' -ForegroundColor Yellow
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Root', "`"$Root`"")
    if ($ResultsDir) { $argList += @('-ResultsDir', "`"$ResultsDir`"") }
    if ($AllowNonDisposable) { $argList += '-AllowNonDisposable' }
    $p = Start-Process powershell.exe -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $p.ExitCode
}

# ---- paths ----
function Test-Writable([string]$dir) {
    try {
        $t = Join-Path $dir ('.w' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType File $t -ErrorAction Stop | Out-Null
        Remove-Item $t -Force
        return $true
    } catch { return $false }
}
if (-not $ResultsDir) {
    $ResultsDir = if (Test-Path 'C:\results') { 'C:\results' } else { Join-Path $Root 'results' }
}
New-Item -ItemType Directory -Force $ResultsDir | Out-Null
$log = Join-Path $ResultsDir 'sandbox-run.log'
Start-Transcript -Path (Join-Path $ResultsDir 'sandbox-transcript.txt') -Force | Out-Null

$steps = New-Object System.Collections.Generic.List[string]
function Step($name, [scriptblock]$body) {
    Write-Host "`n##### $name" -ForegroundColor Cyan
    try {
        & $body
        $steps.Add("ok    $name")
    } catch {
        Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
        $steps.Add("FAIL  $name : $($_.Exception.Message)")
    }
}

$run = $Root
Step '1. verify package integrity (SHA256SUMS.txt)' {
    if (-not (Test-PackageHashes $Root)) { throw 'The package has missing or corrupt files - copy the zip again and re-run (results of a damaged package cannot be trusted)' }
}

Step '2. working directory' {
    if (Test-Writable $Root) {
        Write-Host "Running in place: $Root"
    } else {
        $script:run = 'C:\uacsim-run'
        robocopy $Root $script:run /E /NFL /NDL /NJH /NJS /NP | Out-Null   # robocopy exit codes below 8 all mean success
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
        $global:LASTEXITCODE = 0
        Write-Host "Root is read-only, copied to a local writable folder: $script:run"
    }
}

$driverInstalled = $false
Step '3. install usbip-win2 driver' {
    # Prefer the slim payload usbip-payload\ (about 3 MB): uacstudio --install-driver first checks every file against the
    # SHA-256 in MANIFEST, then runs pnputil + devnode (the core steps of the official installer); an already installed
    # driver is not installed again. The guard and the administrator check live in the C++ code.
    $payload = Join-Path $script:run 'usbip-payload'
    if (Test-Path (Join-Path $payload 'bin\usbip.exe')) {
        if ($AllowNonDisposable) { $env:UACSIM_FORCE_HOST_TESTS = '1' }   # explicit override that means the same as -AllowNonDisposable
        # uacstudio is a GUI-subsystem program: calling it with & does not wait; Start-Process -Wait is needed to get the exit code
        $p = Invoke-Retry { Start-Process (Join-Path $script:run 'uacstudio.exe') -ArgumentList '--install-driver', '--payload', "`"$payload`"" -Wait -PassThru -NoNewWindow -ErrorAction Stop }
        switch ($p.ExitCode) {
            0 { }
            3 { Write-Warning 'The driver is installed, but the installer reports that a reboot is needed to finish; the host.* cases may fail because of that.' }
            2 { throw 'uacstudio --install-driver was refused by the guard (this is not a disposable machine)' }
            default { throw "uacstudio --install-driver failed, exit code $($p.ExitCode) (output above / sandbox-transcript.txt)" }
        }
        $script:driverInstalled = $true
        return
    }
    # No slim payload: fall back to the official installer package
    $drvDir = @((Join-Path $Root 'drivers'), 'C:\drivers') | Where-Object { Test-Path $_ } |
        Where-Object { Get-ChildItem $_ -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.msi', '.exe' } } |
        Select-Object -First 1
    if (-not $drvDir) {
        throw "Neither the slim payload ($payload) nor a usbip-win2 installer was found (looked in $Root\drivers and C:\drivers). Put one there and re-run; the host.* cases will be SKIPped."
    }
    & (Join-Path $script:run 'scripts\elevated-install-driver.ps1') -InstallerDir $drvDir -NoRestorePoint -AllowNonDisposable:$AllowNonDisposable
    if ($LASTEXITCODE -ne 0) { throw "elevated-install-driver.ps1 exit $LASTEXITCODE" }
    $script:driverInstalled = $true
}

Step '4. run the case table (uac2-48k, then uac1-cmedia-sim for R-006)' {
    & (Join-Path $script:run 'scripts\run-host-cases.ps1') -BinDir $script:run -OutDir $ResultsDir -AllowNonDisposable:$AllowNonDisposable
    if ($LASTEXITCODE -ne 0) { throw "run-host-cases.ps1 exit $LASTEXITCODE (some cases failed; see the result file in each profile folder)" }
}

# ---- summary ----
$rebootPending = (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') -or
                 (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired')
$ver = Join-Path $Root 'VERSION.txt'
$out = @()
$out += "=== sandbox run summary $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="
if (Test-Path $ver) { $out += (Get-Content $ver) }
$out += ''
$out += $steps
$out += ''
$out += "driver installed this run     : $driverInstalled"
$out += "Windows reports reboot pending: $rebootPending"
$out += 'devices cleaned up           : see cleanup.devices in each result file (cases clean up after themselves)'
$out += ''
$out += "Per-profile result files: $ResultsDir\<profile>\uacstudio-autotest-*.txt (first line is 'RESULT: ...')"
$out += 'NEEDS HUMAN CONFIRMATION: see the section of the same name at the end of each result file.'
$out | Tee-Object -FilePath $log
$out | Out-File (Join-Path ([Environment]::GetFolderPath('Desktop')) 'uacsim-sandbox-summary.txt') -Encoding utf8
Stop-Transcript | Out-Null
Write-Host "`nDone. Copy the whole folder $ResultsDir back to the host (in the .wsb flow it is already mapped to sandbox\results on the host)."
if ($steps -match '^FAIL') { exit 1 }
exit 0
