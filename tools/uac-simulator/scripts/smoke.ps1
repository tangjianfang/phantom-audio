<#
.SYNOPSIS
    uacstudio GUI 冒烟：不提权启动 uacstudio.exe --selftest，它会逐控件做离屏像素统计（深色主题是否生效）、
    几何审计（有没有被盖住/跑出窗口/重叠，含最小窗口尺寸）、并跑控制层自检，然后自己退出。
    程序自己退出且退出码为 0 即通过。

    【它不判断外观】：布局是否好看、文字是否被截断、配色、DPI 观感、交互手感都没有人看过——
    交付时必须把这些列入"需要人工确认"。

.PARAMETER Exe
    uacstudio.exe 路径，默认 build\debug\uacstudio.exe。

.NOTES
    程序退出码：0 通过；3 控件没套上深色主题/什么都没画；4 控制层逻辑；5 几何（被盖住/越界/重叠）；
    其它值多半是崩溃码。
#>
param(
    [string]$Exe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\debug\uacstudio.exe'),
    [string]$ReportName = 'uacstudio-selftest.txt',   # 程序写到 %TEMP% 的报告名
    [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $Exe)) { throw "找不到: $Exe（先构建）" }

# 带管理员清单的程序也能不提权启动；uacstudio 本身不需要管理员
$env:__COMPAT_LAYER = 'RunAsInvoker'
$report = Join-Path $env:TEMP $ReportName
Remove-Item $report -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Exe -ArgumentList '--selftest' -PassThru
$null = $p.Handle   # 保持句柄，退出后才读得到 ExitCode

if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    Write-Host "失败：$TimeoutSeconds 秒后仍在运行（自检没有自己退出）" -ForegroundColor Red
    $p.Kill()
    exit 1
}
$code = $p.ExitCode
Get-Content $report -Encoding UTF8 -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ }
if ($code -ne 0) {
    $meaning = switch ($code) {
        3 { '有控件保持了系统外观或什么都没画（深色主题没生效）' }
        4 { '控制层自检失败（按钮表/请求体构建/控件缺失）' }
        5 { '有控件被盖住、跑出窗口、尺寸为 0 或互相重叠' }
        default { '崩溃或异常退出' }
    }
    Write-Host ("失败：退出码 {0} (0x{1:X8})：{2}" -f $code, $code, $meaning) -ForegroundColor Red
    exit 1
}
Write-Host 'ok：每个控件都做了深色/几何检查，控制层自检通过，程序自己以退出码 0 关闭。'

# ---------------------------------------------------------------------------------------------
# 第二阶段：界面线程不得被阻塞。
# 起因：主窗口的定时器在 UI 线程上同步发 HTTP，而 Windows 上阻塞式 connect() 连一个没人监听的回环端口要等约 2 秒
# （RST 之后协议栈还会重发 SYN）。Sandbox 里没有 uacsimd，于是 UI 线程几乎一直在阻塞，窗口拖不动、点不了。
# 做法：真的启动 GUI（不带 --selftest），端口指向一个必然关闭的端口，外部进程持续用 SendMessageTimeout 探测
# UI 线程是否在 400 ms 内处理消息。再用 --env-check 覆盖"首次运行弹出的模态环境自检窗口"这条路径。
# ---------------------------------------------------------------------------------------------
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class UiProbe {
    [DllImport("user32.dll")]
    static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    // WM_NULL，SMTO_BLOCK | SMTO_ABORTIFHUNG：窗口所属线程在 timeoutMs 内没处理这条消息就返回 false
    public static bool Responds(IntPtr h, uint timeoutMs) {
        IntPtr r;
        return SendMessageTimeout(h, 0, IntPtr.Zero, IntPtr.Zero, 3, timeoutMs, out r) != IntPtr.Zero;
    }
}
"@

$script:probeNotes = @()
function Test-UiResponsive([string[]]$ArgList, [string]$Label, [int]$Seconds = 7) {
    $p = Start-Process -FilePath $Exe -ArgumentList $ArgList -PassThru
    try {
        $h = [IntPtr]::Zero
        $deadline = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $deadline) {
            $p.Refresh()
            if ($p.HasExited) { return "$Label : 程序提前退出（退出码 $($p.ExitCode)）" }
            $h = $p.MainWindowHandle
            if ($h -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 100
        }
        if ($h -eq [IntPtr]::Zero) { return "$Label : 20 秒内没有出现窗口" }
        Start-Sleep -Seconds 1   # 让启动阶段的一次性工作先过去
        $bad = 0; $total = 0; $worst = 0
        $end = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $end) {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $ok = [UiProbe]::Responds($h, 400)
            $sw.Stop()
            $total++
            if (-not $ok) { $bad++ }
            if ($sw.ElapsedMilliseconds -gt $worst) { $worst = $sw.ElapsedMilliseconds }
            Start-Sleep -Milliseconds 50
        }
        if ($bad -gt 0) { return "$Label : 界面线程不响应 - $total 次探测里有 $bad 次在 400 ms 内没被处理（最慢 $worst ms）" }
        $script:probeNotes += ('{0}: {1} 次探测全部及时响应（最慢 {2} ms）' -f $Label, $total, $worst)
        return $null
    }
    finally { if (-not $p.HasExited) { $p.Kill() } }
}

$deadPort = '39991'   # 没人监听：阻塞式 connect 会白等约 2 秒
$problems = @()
foreach ($case in @(
        @{ Label = '主窗口（没有 uacsimd）'; Args = @('--port', $deadPort, '--no-env-check') },
        @{ Label = '环境自检模态窗口（没有 uacsimd）'; Args = @('--port', $deadPort, '--env-check') })) {
    $r = Test-UiResponsive $case.Args $case.Label
    if ($r) { $problems += $r }
}
$script:probeNotes | ForEach-Object { Write-Host "    $_" }
if ($problems.Count -gt 0) {
    $problems | ForEach-Object { Write-Host "失败：$_" -ForegroundColor Red }
    exit 1
}
Write-Host 'ok：没有 uacsimd 时界面线程仍然及时响应（主窗口与模态环境自检窗口都测了）。'

# ---------------------------------------------------------------------------------------------
# 第三阶段：模拟器进程的生命周期——只有"启动"没有"退出和清理"会让 uacsimd 一直占着端口。
#   A) GUI 用 --start-simulator 启动了 uacsimd，正常关窗后 uacsimd 必须随之消失（并且端口不再应答）；
#   B) GUI 被强杀（崩溃、任务管理器）后 uacsimd 靠 --parent-pid 自己退出；
#   C) 反例：别人启动的 uacsimd，GUI 退出时不能动它（只停自己启动的）。
# 每一段先确认"确实在运行"，之后的"已消失"才有意义。开发机上不会碰 usbip（守卫拒绝，只停模拟器进程）。
# ---------------------------------------------------------------------------------------------
$simExe = Join-Path (Split-Path -Parent $Exe) 'uacsimd.exe'
if (-not (Test-Path $simExe)) { throw "找不到: $simExe（先构建）" }

function Get-FreePort {
    $l = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $l.Start(); $port = $l.LocalEndpoint.Port; $l.Stop()
    return $port
}
function Test-SimUp([int]$Port) {
    try { $null = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/status" -TimeoutSec 2; return $true } catch { return $false }
}
function Wait-Until([scriptblock]$Cond, [int]$Seconds) {
    $end = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $end) { if (& $Cond) { return $true }; Start-Sleep -Milliseconds 150 }
    return [bool](& $Cond)
}
function Get-SimChild([int]$ParentPid) {
    return @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$ParentPid AND Name='uacsimd.exe'")
}

$lifecycle = @()
$cleanup = New-Object System.Collections.Generic.List[int]   # 出错也要收拾干净的进程
try {
    # --- A) 正常关窗
    $port = Get-FreePort
    $gui = Start-Process -FilePath $Exe -ArgumentList @('--port', $port, '--start-simulator', '--no-env-check') -PassThru
    $cleanup.Add($gui.Id)
    if (-not (Wait-Until { Test-SimUp $port } 20)) { $lifecycle += 'A) --start-simulator：20 秒内模拟器没有起来' }
    else {
        $child = @(Get-SimChild $gui.Id)   # @(): a single result would otherwise lose .Count
        if ($child.Count -ne 1) { $lifecycle += "A) 启动后找不到 GUI 的子进程 uacsimd（找到 $($child.Count) 个）" }
        else {
            $cleanup.Add([int]$child[0].ProcessId)
            $null = $gui.Refresh(); [void]$gui.CloseMainWindow()
            if (-not $gui.WaitForExit(20000)) { $lifecycle += 'A) 关窗后 GUI 20 秒内没有退出' }
            # 没有宽限期：GUI 是等模拟器停掉才关窗的，所以 GUI 退出的这一刻它就必须已经不在了。
            # （--parent-pid 兜底会让它稍后也消失，宽限期会把"GUI 没有主动清理"这个缺陷掩盖掉。）
            elseif (Get-Process -Id $child[0].ProcessId -ErrorAction SilentlyContinue) { $lifecycle += 'A) GUI 退出的那一刻，它启动的 uacsimd 还在运行（退出清理没起作用）' }
            elseif (Test-SimUp $port) { $lifecycle += 'A) uacsimd 进程没了，端口却还在应答' }
            else { $lifecycle += $null; Write-Host '    A) 正常关窗：GUI 启动的 uacsimd 随之退出，端口释放' }
        }
    }

    # --- B) GUI 被强杀
    $port = Get-FreePort
    $gui = Start-Process -FilePath $Exe -ArgumentList @('--port', $port, '--start-simulator', '--no-env-check') -PassThru
    $cleanup.Add($gui.Id)
    if (-not (Wait-Until { Test-SimUp $port } 20)) { $lifecycle += 'B) --start-simulator：20 秒内模拟器没有起来' }
    else {
        $child = @(Get-SimChild $gui.Id)   # @(): a single result would otherwise lose .Count
        if ($child.Count -ne 1) { $lifecycle += "B) 启动后找不到 GUI 的子进程 uacsimd" }
        else {
            $simPid = [int]$child[0].ProcessId
            $cleanup.Add($simPid)
            Stop-Process -Id $gui.Id -Force
            if (-not (Wait-Until { -not (Get-Process -Id $simPid -ErrorAction SilentlyContinue) } 8)) { $lifecycle += 'B) GUI 被强杀 8 秒后，uacsimd 仍在运行（--parent-pid 没生效）' }
            else { $lifecycle += $null; Write-Host '    B) GUI 被强杀：uacsimd 靠 --parent-pid 自己退出' }
        }
    }

    # --- C) 别人启动的模拟器，GUI 退出时不能动它
    $port = Get-FreePort
    $foreign = Start-Process -FilePath $simExe -ArgumentList @('--http-port', $port, '--usbip-port', (Get-FreePort), '--parent-pid', $PID) -PassThru -WindowStyle Hidden
    $cleanup.Add($foreign.Id)
    if (-not (Wait-Until { Test-SimUp $port } 20)) { $lifecycle += 'C) 手动启动的 uacsimd 20 秒内没有起来' }
    else {
        $gui = Start-Process -FilePath $Exe -ArgumentList @('--port', $port, '--no-env-check') -PassThru
        $cleanup.Add($gui.Id)
        Start-Sleep -Seconds 2
        $null = $gui.Refresh(); [void]$gui.CloseMainWindow()
        if (-not $gui.WaitForExit(20000)) { $lifecycle += 'C) 关窗后 GUI 20 秒内没有退出' }
        elseif ($foreign.HasExited -or -not (Test-SimUp $port)) { $lifecycle += 'C) GUI 退出时把不是它启动的 uacsimd 也停掉了' }
        else { $lifecycle += $null; Write-Host '    C) 别人启动的 uacsimd：GUI 退出时原样保留' }
    }
}
finally {
    foreach ($id in $cleanup) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
}
$lifecycleProblems = @($lifecycle | Where-Object { $_ })
if ($lifecycleProblems.Count -gt 0) {
    $lifecycleProblems | ForEach-Object { Write-Host "失败：$_" -ForegroundColor Red }
    exit 1
}
Write-Host 'ok：模拟器进程的生命周期：自己启动的会随 GUI 退出（正常关窗 / 被强杀），别人启动的不会被动。'
Write-Host '    这【不】代表界面好看：位置、文字是否被截断、配色、DPI 观感都没有人看过。'
exit 0
