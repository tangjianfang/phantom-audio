# regression.ps1 —— 三层回归
#   L1 单测 + L2 场景（ctest，全自动）
#   L3 实机（-WithDriver：需 usbip-win2 驱动 + 管理员装好的环境；普通权限可跑）
# 产出：build/regression-report.txt
param([switch]$WithDriver)
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$report = @()
function Add-Line($t) { $script:report += $t; Write-Host $t }

Add-Line "=== UAC Simulator Regression $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ==="

# ---------- 构建 ----------
Add-Line "[build] win-x64-release"
& "$root\scripts\msvc-env.cmd" cmake --preset win-x64-release 2>&1 | Out-Null
& "$root\scripts\msvc-env.cmd" cmake --build --preset win-x64-release 2>&1 |
    Select-String -Pattern "error|FAILED" | ForEach-Object { Add-Line "  BUILD-ERR: $_" }

# ---------- L1+L2：单测与场景（同一测试二进制） ----------
Add-Line "[L1+L2] ctest（doctest 全套）"
$ctest = & "$root\scripts\msvc-env.cmd" ctest --preset win-x64-debug 2>&1
$l1ok = $LASTEXITCODE -eq 0
$ctest | Select-String -Pattern "tests passed|tests failed|Failed" | ForEach-Object { Add-Line "  $_" }
if (-not $l1ok) { Add-Line "  RESULT: L1+L2 FAIL" } else { Add-Line "  RESULT: L1+L2 PASS" }

# ---------- L3：实机门控 ----------
if ($WithDriver) {
    if (Get-Service usbip2_ude -ErrorAction SilentlyContinue) {
        Add-Line "[L3] 实机（驱动已装）"
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\scripts\m2-verify.ps1" |
            ForEach-Object { Add-Line "  $_" }
    } else {
        Add-Line "[L3] SKIP：usbip2_ude 未安装"
    }
} else {
    Add-Line "[L3] SKIP：未指定 -WithDriver"
}

$out = "$root\build\regression-report.txt"
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$report | Out-File -FilePath $out -Encoding utf8
Write-Host "`nreport -> $out"
if (-not $l1ok) { exit 1 }
exit 0
