# disposable.ps1 - disposable-machine guard (dot-sourced by the other scripts).
# The decision matches uacstudio::IsDisposableMachine in C++ (apps/uacstudio/auto_test.cpp):
#   Windows Sandbox (user WDAGUtilityAccount), Hyper-V guest, VMware/VirtualBox/QEMU/Xen/Parallels.
# Scripts that install a driver or attach a virtual USB sound card must pass this guard first: no real-device tests on the development machine.
# Compatible with Windows PowerShell 5.1 (the one that ships inside Sandbox); no PS7 syntax.

function Get-DisposableReason {
    if ($env:USERNAME -eq 'WDAGUtilityAccount') { return 'Windows Sandbox (user WDAGUtilityAccount)' }
    if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Virtual Machine\Guest\Parameters') { return 'Hyper-V guest' }
    $bios = Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS' -ErrorAction SilentlyContinue
    $manu = "$($bios.SystemManufacturer)"
    $prod = "$($bios.SystemProductName)"
    foreach ($tag in 'VMware', 'innotek', 'QEMU', 'Xen', 'Parallels') {
        if ($manu -like "*$tag*" -or $prod -like "*$tag*") { return "virtual machine ($tag)" }
    }
    if ($manu -like '*Microsoft*' -and $prod -like '*Virtual Machine*') { return 'Hyper-V virtual machine' }
    return $null
}

# Retries only a failed START (an exception), never a process that ran to completion with a non-zero exit code,
# so a command with side effects is not executed twice.
# Why: an exe that was just extracted and is started with Start-Process immediately is sometimes held briefly by the
# antivirus real-time scan and fails with "Access is denied"; the same call succeeds a few seconds later (observed).
function Invoke-Retry {
    param([scriptblock]$Action, [int]$Attempts = 4, [int]$DelaySec = 2)
    for ($i = 1; ; $i++) {
        try { return (& $Action) }
        catch {
            if ($i -ge $Attempts) { throw }
            Write-Host "  (start failed: $($_.Exception.Message) - retrying in $DelaySec s, attempt $i/$($Attempts - 1))" -ForegroundColor Yellow
            Start-Sleep -Seconds $DelaySec
        }
    }
}

# Not a disposable machine -> throw (unless -Allow is given). $What goes into the message and says who is refusing.
function Assert-DisposableMachine {
    param([switch]$Allow, [string]$What = 'this script')
    $reason = Get-DisposableReason
    if ($reason) { Write-Host "[guard] disposable machine: $reason"; return }
    if ($Allow) {
        Write-Warning "[guard] NOT a recognised disposable machine - continuing only because -AllowNonDisposable was given."
        return
    }
    $bios = Get-ItemProperty 'HKLM:\HARDWARE\DESCRIPTION\System\BIOS' -ErrorAction SilentlyContinue
    throw ("$What refuses to run: this does not look like Windows Sandbox or a VM " +
           "(BIOS manufacturer '$($bios.SystemManufacturer)'). It installs a kernel driver and attaches a virtual USB " +
           "sound card. Use sandbox\launch-sandbox.ps1, or pass -AllowNonDisposable on a machine you can throw away.")
}
