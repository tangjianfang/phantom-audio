# coverage.ps1 —— 核心模块行覆盖率（OpenCppCoverage）
# 用法：powershell -File scripts/coverage.ps1 [-MinCoverage 100] [-ModulesCsv "src/usbip_proto.cpp,..."]
param(
    [double]$MinCoverage = 95.0,
    [string]$Exe = "$PSScriptRoot\..\build\coverage\uac_tests.exe"
)
$ErrorActionPreference = 'Stop'

$occ = "C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe"
if (-not (Test-Path $occ)) { Write-Error "OpenCppCoverage 未安装（winget install OpenCppCoverage.OpenCppCoverage）"; exit 2 }

# 覆盖率构建（在 bash 里执行：./scripts/msvc-env.cmd cmake --build --preset win-x64-coverage）
$root = Split-Path $PSScriptRoot -Parent
if (-not (Test-Path $Exe)) {
    Write-Error "缺覆盖率构建 $Exe —— 先运行: ./scripts/msvc-env.cmd cmake --preset win-x64-coverage && ./scripts/msvc-env.cmd cmake --build --preset win-x64-coverage"
    exit 2
}

$outDir = "$root\build\cov-out"
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force $outDir | Out-Null

# 统计范围（--sources）：
#   src\                           核心库
#   apps\uacsimd\sim_host.cpp      HTTP -> 设备模型的编排层（attach/路径校验在这里；此前不在范围内，
#                                  profile 名过滤漏掉反斜杠的 bug 因此没被发现）
#   apps\uacstudio\controller.cpp  GUI 控制层（按钮表/请求体构建）
# 【不在范围内】（门禁的数字不代表它们）：
#   uacstudio 的窗口代码（main.cpp / env_check.cpp / gui_selfcheck.cpp）——靠 --selftest 冒烟，不靠行覆盖；
#   host_probe、uacsim 命令行客户端；
#   apps\uacstudio\auto_test.cpp 的 host.* 用例体——需要驱动与 Windows 音频栈，单测里只会 SKIP，
#   它们的验证只有 Sandbox 里的真实运行。
# 注：OpenCppCoverage 在本环境不记录子线程（accept/channel/pump 线程体），
# 这些代码以行尾标记 uac-cov:thread 注明并从统计中排除（可在源码中审计）
& $occ --quiet --export_type cobertura:$outDir\coverage.xml `
    --excluded_modules ".*doctest.*" --excluded_modules ".*nlohmann.*" `
    --excluded_line_regex "uac-cov:thread" `
    --sources "$root\src" `
    --sources "$root\apps\uacsimd\sim_host.cpp" `
    --sources "$root\apps\uacstudio\controller.cpp" `
    -- "$Exe" | Out-Null

[xml]$x = Get-Content "$outDir\coverage.xml"
$report = @()
foreach ($class in $x.coverage.packages.package.classes.class) {
    foreach ($line in $class.lines.line) {
        if ($line.hits -eq 0 -and $line.branch -eq "False") {
            $report += [pscustomobject]@{ file = $class.filename; line = $line.number }
        }
    }
}

# 汇总行覆盖率（cobertura: line-rate 属性）
$pkg = $x.coverage.packages.package
$rate = [double]$pkg.'line-rate'
$pct = [math]::Round($rate * 100, 2)
Write-Host "核心行覆盖率: $pct% （阈值 $MinCoverage%）"

if ($report.Count -gt 0) {
    Write-Host "未覆盖行数: $($report.Count)（前 40 条）"
    $report | Select-Object -First 40 | ForEach-Object { Write-Host "  $($_.file):$($_.line)" }
}

if ($pct -lt $MinCoverage) {
    Write-Host "FAIL：低于阈值" -ForegroundColor Red
    exit 1
}
Write-Host "PASS（度量说明：lambda 体与子线程体存在 OpenCppCoverage 归因限制，" -ForegroundColor Green
Write-Host "      这些路径由 doctest 断言在主线程等价验证；已标记 uac-cov:thread 可审计。" -ForegroundColor Green
Write-Host "      统计范围见本脚本开头注释——GUI 窗口代码、host_probe、host.* 用例体不在其中）" -ForegroundColor Green
