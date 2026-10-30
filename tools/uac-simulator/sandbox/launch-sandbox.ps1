<#
.SYNOPSIS
    在宿主机上准备并启动 Windows Sandbox，让它从零装驱动、跑宿主侧用例（host.*）。
    这是开发机上做"真机"验证的唯一入口：一切发生在一次性的 Sandbox 里，宿主机不装驱动、不挂设备。

.DESCRIPTION
    做三件事：
      1. 把构建产物（uacsimd / uacstudio / host_probe / uacsim + profiles + 脚本）暂存到 sandbox\stage
      2. 生成 sandbox\uac-sim.generated.wsb（.wsb 要求绝对路径，所以每次生成；已被 .gitignore）
      3. 启动 Sandbox（-NoStart 则只生成）
    结果回到宿主机的 sandbox\results（Sandbox 里 C:\results 的可写映射）。

    Sandbox 配置：禁用网络、禁用 vGPU/音视频输入/剪贴板/打印机重定向；只有 results 目录可写。

.PARAMETER BinDir
    构建产物目录。默认优先 build\release，其次 build\debug。
.PARAMETER DriverDir
    usbip-win2 v0.9.7.8 x64 安装包所在目录（默认 third_party\usbip-win2，用户自行放入；第三方二进制，BSD-2-Clause，不随仓库提交）。
    没有安装包也能跑：env.driver 与 host.* 会 SKIP，api.* 照常。
.PARAMETER NoStart
    只准备与生成 .wsb，不启动。
#>
param(
    [string]$BinDir,
    [string]$DriverDir,
    [switch]$NoStart
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot            # tools\uac-simulator
if (-not $BinDir) {
    $BinDir = @('build\release', 'build\debug') | ForEach-Object { Join-Path $root $_ } |
        Where-Object { Test-Path (Join-Path $_ 'uacstudio.exe') } | Select-Object -First 1
}
if (-not $BinDir -or -not (Test-Path $BinDir)) { throw "找不到构建产物。先构建：scripts\msvc-env.cmd cmake --build --preset win-x64-release" }
if (-not $DriverDir) { $DriverDir = Join-Path $root 'third_party\usbip-win2' }

$sandboxExe = Join-Path $env:SystemRoot 'System32\WindowsSandbox.exe'
if (-not $NoStart -and -not (Test-Path $sandboxExe)) {
    throw "未启用 Windows Sandbox。以管理员运行：Enable-WindowsOptionalFeature -Online -FeatureName Containers-DisposableClientVM -All，然后重启。"
}

# ---- 1. 暂存 ----
# 与 build.bat 自动生成的 zip 包用同一份清单（scripts\package-sandbox.ps1）：只是这里暂存成目录、
# 不压缩，驱动不进目录（由 .wsb 单独映射 C:\drivers）。想手动拷贝文件进 Sandbox 的话，用 build.bat 生成的 zip 更省事。
$stage = Join-Path $PSScriptRoot 'stage'
$results = Join-Path $PSScriptRoot 'results'
New-Item -ItemType Directory -Force $results, $DriverDir | Out-Null
& (Join-Path $root 'scripts\package-sandbox.ps1') -BinDir $BinDir -StageDir $stage -NoZip -NoDriver
if (-not $?) { throw "暂存失败（package-sandbox.ps1）" }

$installer = Get-ChildItem $DriverDir -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.msi', '.exe' } | Select-Object -First 1
Write-Host "构建产物 : $BinDir"
Write-Host "暂存目录 : $stage"
Write-Host "结果目录 : $results"
if ($installer) { Write-Host "驱动安装包: $($installer.FullName)" }
else { Write-Warning "$DriverDir 里没有 usbip-win2 安装包：Sandbox 里 host.* 用例会 SKIP，只跑 api.*。" }

# ---- 2. 生成 .wsb ----
function X($s) { [System.Security.SecurityElement]::Escape($s) }
$wsb = Join-Path $PSScriptRoot 'uac-sim.generated.wsb'
@"
<Configuration>
  <VGpu>Disable</VGpu>
  <Networking>Disable</Networking>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <PrinterRedirection>Disable</PrinterRedirection>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <MemoryInMB>4096</MemoryInMB>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$(X $stage)</HostFolder>
      <SandboxFolder>C:\uacsim</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$(X $DriverDir)</HostFolder>
      <SandboxFolder>C:\drivers</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$(X $results)</HostFolder>
      <SandboxFolder>C:\results</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\uacsim\run-in-sandbox.ps1</Command>
  </LogonCommand>
</Configuration>
"@ | Out-File $wsb -Encoding utf8
Write-Host "已生成   : $wsb"

# ---- 3. 启动 ----
if ($NoStart) { Write-Host "（-NoStart）未启动。手动：WindowsSandbox.exe `"$wsb`""; exit 0 }
Write-Host "启动 Windows Sandbox …（结果会出现在 $results）"
Start-Process $sandboxExe -ArgumentList "`"$wsb`""
