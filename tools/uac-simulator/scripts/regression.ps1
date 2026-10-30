# regression.ps1 - 分层回归
#   L1+L2  单测 + 回环场景（ctest，全自动，不需要驱动）
#   GUI    非视觉自检（smoke.ps1）：深色主题 / 几何 / 控制层。不判断外观。
#   L3     宿主侧（真实 Windows 音频栈：usbip attach、WASAPI、usbaudio*.sys）。
#          只在 Windows Sandbox / VM 里跑：-WithDriver 在开发机上会被守卫拒绝。
#          开发机上做 L3 的入口是 sandbox\launch-sandbox.ps1。
# 产出：build\regression-report.txt
param([switch]$WithDriver)
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$report = @()
function Add-Line($t) { $script:report += $t; Write-Host $t }

Add-Line "=== UAC Simulator Regression $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="

# ---------- 构建（Debug：单测与 GUI 自检都用它） ----------
Add-Line "[build] win-x64-debug"
& "$root\scripts\msvc-env.cmd" cmake --preset win-x64-debug 2>&1 | Out-Null
$buildOut = & "$root\scripts\msvc-env.cmd" cmake --build --preset win-x64-debug 2>&1
$buildOk = $LASTEXITCODE -eq 0
$buildOut | Select-String -Pattern "error|FAILED" | ForEach-Object { Add-Line "  BUILD-ERR: $_" }
Add-Line $(if ($buildOk) { "  RESULT: BUILD PASS" } else { "  RESULT: BUILD FAIL" })

# ---------- L1+L2：单测与场景（同一测试二进制） ----------
$l1ok = $false
if ($buildOk) {
    Add-Line "[L1+L2] ctest（doctest 全套）"
    $ctest = & "$root\scripts\msvc-env.cmd" ctest --preset win-x64-debug 2>&1
    $l1ok = $LASTEXITCODE -eq 0
    $ctest | Select-String -Pattern "tests passed|tests failed|Failed" | ForEach-Object { Add-Line "  $_" }
    Add-Line $(if ($l1ok) { "  RESULT: L1+L2 PASS" } else { "  RESULT: L1+L2 FAIL" })
}

# ---------- GUI 非视觉自检 ----------
$smokeOk = $false
if ($buildOk) {
    Add-Line "[GUI] uacstudio --selftest（非视觉：主题/几何/控制层；不判断外观）"
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\scripts\smoke.ps1" 2>&1 |
        Where-Object { $_ -match 'exit code|measured|PROBLEM|FAIL|^ok' } | ForEach-Object { Add-Line "  $_" }
    $smokeOk = $LASTEXITCODE -eq 0
    Add-Line $(if ($smokeOk) { "  RESULT: GUI-SELFTEST PASS" } else { "  RESULT: GUI-SELFTEST FAIL" })
}

# ---------- L3：宿主侧（Sandbox / VM 专用） ----------
if ($WithDriver) {
    Add-Line "[L3] 宿主侧用例（守卫：仅 Sandbox / VM）"
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\scripts\run-host-cases.ps1" -BinDir "$root\build\debug" 2>&1 |
        ForEach-Object { Add-Line "  $_" }
} else {
    Add-Line "[L3] SKIP：未指定 -WithDriver（开发机请用 sandbox\launch-sandbox.ps1）"
}

$out = "$root\build\regression-report.txt"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$report | Out-File -FilePath $out -Encoding utf8
Write-Host "`nreport -> $out"
if (-not ($buildOk -and $l1ok -and $smokeOk)) { exit 1 }
exit 0
