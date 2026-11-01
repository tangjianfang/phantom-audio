<#
.SYNOPSIS
    把"在 Windows Sandbox 里跑起来所需的全部文件"打成一个 zip（build.bat 构建成功后自动调用）。
    产物：dist\uacsim-sandbox-<release|debug>.zip，拷进 Sandbox 解压后双击 run.cmd 即可。

.DESCRIPTION
    zip 内容（顶层目录 uacsim-sandbox\）：
      run.cmd                     双击入口（需要时自动提权）
      run-in-sandbox.ps1          Sandbox 内的总流程：校验 -> 守卫 -> 装驱动 -> 跑用例表 -> 总结
      uacsimd.exe uacstudio.exe host_probe.exe uacsim.exe
      profiles\*.json
      scripts\                    disposable.ps1 / elevated-install-driver.ps1 / run-host-cases.ps1
      collect-usbip-payload.bat   在 Sandbox 里装完官方安装包后运行：一键收集精简驱动载荷（约 3 MB）
      drivers\                    usbip-win2 安装包 + 其 BSD-2-Clause 许可文本（仅当本机 third_party\usbip-win2 里有）
      SHA256SUMS.txt VERSION.txt README.txt

    也被 sandbox\launch-sandbox.ps1 复用来暂存目录（-StageDir ... -NoZip -NoDriver），保证两条路径用同一份清单。

.PARAMETER BinDir
    构建产物目录，默认 build\release。目录名（release/debug）进入 zip 文件名。
.PARAMETER StageDir
    给定则直接暂存到该目录（先清空），不使用临时目录。
.PARAMETER NoZip
    只暂存，不压缩。
.PARAMETER NoDriver
    不放任何 usbip-win2 内容（launch-sandbox.ps1 通过目录映射提供驱动）。
.PARAMETER IncludeInstaller
    即使已有精简载荷（third_party\usbip-win2\usbip-payload\，约 3 MB），也把 25 MB 的官方安装包一并放进去。
    默认有载荷就只放载荷；载荷还没收集时退回放安装包（供在 Sandbox 里用 collect-usbip-payload.bat 收集）。
#>
param(
    [string]$BinDir,
    [string]$OutDir,
    [string]$DriverDir,
    [string]$StageDir,
    [switch]$NoZip,
    [switch]$NoDriver,
    [switch]$IncludeInstaller
)
$ErrorActionPreference = 'Stop'

# 用 .NET 直接算 SHA256，不用 Get-FileHash：装了 PowerShell 7 的机器上，系统 PSModulePath 常把 PS7 的
# 模块目录排在 Windows PowerShell 之前，5.1 加载 Get-FileHash 所在模块时兼容性检查失败并静默放弃，
# 于是连普通 5.1 里也"找不到命令"。（run-in-sandbox.ps1 里是同一个写法，保证两边哈希格式一致。）
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [System.IO.File]::OpenRead($path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($fs))).Replace('-', '') }
    finally { $fs.Dispose(); $sha.Dispose() }
}

$root = Split-Path -Parent $PSScriptRoot            # tools\uac-simulator
if (-not $BinDir) { $BinDir = Join-Path $root 'build\release' }
if (-not (Test-Path $BinDir)) { throw "找不到构建产物目录: $BinDir（先构建）" }
$BinDir = (Resolve-Path $BinDir).Path
$config = Split-Path -Leaf $BinDir                  # release / debug
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
if (-not $DriverDir) { $DriverDir = Join-Path $root 'third_party\usbip-win2' }

$exes = 'uacsimd.exe', 'uacstudio.exe', 'host_probe.exe', 'uacsim.exe'
foreach ($e in $exes) {
    if (-not (Test-Path (Join-Path $BinDir $e))) { throw "缺少 $BinDir\$e（构建不完整？）" }
}

# ---- 暂存目录 ----
$tempRoot = $null
if ($StageDir) {
    $pkg = $StageDir
    if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
} else {
    $tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('uacsim-pkg-' + [guid]::NewGuid().ToString('N'))
    $pkg = Join-Path $tempRoot 'uacsim-sandbox'     # zip 的顶层目录名
}
New-Item -ItemType Directory -Force $pkg, (Join-Path $pkg 'scripts'), (Join-Path $pkg 'profiles') | Out-Null

try {
    foreach ($e in $exes) { Copy-Item (Join-Path $BinDir $e) $pkg }
    Copy-Item (Join-Path $root 'profiles\*.json') (Join-Path $pkg 'profiles')
    foreach ($s in 'disposable.ps1', 'elevated-install-driver.ps1', 'run-host-cases.ps1') {
        Copy-Item (Join-Path $root "scripts\$s") (Join-Path $pkg 'scripts')
    }
    Copy-Item (Join-Path $root 'sandbox\run-in-sandbox.ps1') $pkg
    # 在 Sandbox 里装完官方安装包后运行它，一键把精简载荷收进一个文件夹（见 docs\sandbox.md）
    Copy-Item (Join-Path $root 'sandbox\collect-usbip-payload.bat') $pkg

    # ---- 精简驱动载荷（约 3 MB；有它就不需要 25 MB 的官方安装包） ----
    # 布局由 sandbox\collect-usbip-payload.bat 产出：bin\ driver\ runtime\ MANIFEST.txt。
    # Sandbox 里 run.cmd -> uacstudio --install-driver 会先逐文件核对 MANIFEST 的 SHA256 再安装。
    $payloadIncluded = $false
    $payloadDesc = ''
    $payloadSrc = Join-Path $DriverDir 'usbip-payload'
    if (-not $NoDriver -and (Test-Path (Join-Path $payloadSrc 'bin\usbip.exe'))) {
        $payloadDst = Join-Path $pkg 'usbip-payload'
        Copy-Item $payloadSrc $payloadDst -Recurse
        # BSD-2-Clause：二进制再分发要求随附版权声明与许可条款（见下面安装包分支的同一段说明）
        $lic = @(Get-ChildItem (Join-Path $DriverDir 'src') -Filter 'LICENSE.txt' -Recurse -Depth 2 -ErrorAction SilentlyContinue) |
            Select-Object -First 1
        if ($lic) { Copy-Item $lic.FullName (Join-Path $payloadDst 'LICENSE-usbip-win2.txt') }
        else { Write-Warning "没找到 usbip-win2 的 LICENSE.txt（期望在 $DriverDir\src\ 下）：随包分发其二进制时需要附带它的 BSD-2-Clause 许可文本。" }
        # 结果要能归因到具体驱动版本：usbip.exe 的文件版本 + UDE 驱动 INF 的 DriverVer
        $ude = Get-ChildItem (Join-Path $payloadDst 'driver') -Filter 'usbip2_ude.inf' -Recurse | Select-Object -First 1
        $drvVer = '?'
        if ($ude -and ((Get-Content $ude.FullName -Raw) -match '(?m)^\s*DriverVer\s*=\s*(.+?)\s*$')) { $drvVer = $Matches[1] }
        $exeVer = (Get-Item (Join-Path $payloadDst 'bin\usbip.exe')).VersionInfo.FileVersion
        $nFiles = @(Get-ChildItem $payloadDst -Recurse -File).Count
        $payloadDesc = "usbip-payload ($nFiles files): usbip.exe $exeVer, usbip2_ude DriverVer $drvVer"
        $payloadIncluded = $true
    }

    # ---- 驱动安装包（可选；默认仅在没有精简载荷时才放） ----
    $driverIncluded = $false
    $driverDesc = ''
    if (-not $NoDriver -and ($IncludeInstaller -or -not $payloadIncluded)) {
        # 只看 $DriverDir 顶层的 .msi/.exe。不能 -Recurse：该目录下还放着从 Sandbox 拷出的 SendboxUSBip\USBip\
        # （devnode.exe / usbip.exe / wusbip.exe / 卸载器 ...）和源码树，递归会把它们全当成"安装包"打进 zip。
        $installers = @(Get-ChildItem $DriverDir -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Extension -in '.msi', '.exe' })
        if ($installers.Count -gt 0) {
            $drv = Join-Path $pkg 'drivers'
            New-Item -ItemType Directory -Force $drv | Out-Null
            $installers | ForEach-Object { Copy-Item $_.FullName $drv }
            # 结果要能归因到具体驱动版本（驱动行为随版本变，如 usbip-win2 #200）：文件名 + 哈希写进 VERSION.txt
            $driverDesc = ($installers | ForEach-Object { '{0} (sha256 {1})' -f $_.Name, (Get-Sha256 $_.FullName) }) -join '; '
            $pinned = '0.9.8.1'   # 2026-11-01 起锁定：真机（Win11 26200）全链路验证过的版本；0.9.8.0 实测行为一致
            $installers | Where-Object { $_.Name -notmatch [regex]::Escape($pinned) } | ForEach-Object {
                Write-Warning "安装包 $($_.Name) 不是项目文档锁定的 v$pinned：结果请按这个版本归因（VERSION.txt 已记录）；升级驱动前应先在 Sandbox 里跑一遍用例表。"
            }
            # 上游许可文本一并带上：usbip-win2 是 BSD-2-Clause（仓库 LICENSE.txt 与 GitHub 登记均如此，v.0.9.7.8 / v.0.9.8.1
            # 两个标签核对过），二进制形式再分发要求"在文档或随附材料里复现版权声明与许可条款"。
            $licenseSrc = @(Get-ChildItem (Join-Path $DriverDir 'src') -Filter 'LICENSE.txt' -Recurse -Depth 2 -ErrorAction SilentlyContinue) |
                Select-Object -First 1
            $licenseNote = 'LICENSE-usbip-win2.txt'
            if ($licenseSrc) { Copy-Item $licenseSrc.FullName (Join-Path $drv $licenseNote) }
            else { Write-Warning "没找到 usbip-win2 的 LICENSE.txt（期望在 $DriverDir\src\ 下）：随包分发其二进制时需要附带它的 BSD-2-Clause 许可文本。" }
            $note = @(
                'usbip-win2 (https://github.com/vadimgrn/usbip-win2) - a separate project, not part of phantom-audio.',
                $(if ($licenseSrc) { "License: BSD 2-Clause, see $licenseNote (keep it with these files)." } else { 'License: BSD 2-Clause (LICENSE.txt was not available when this package was made).' }),
                'Copied here from this machine''s third_party\usbip-win2 for Sandbox testing.'
            ) -join "`r`n"
            [IO.File]::WriteAllText((Join-Path $drv 'README.txt'), $note + "`r`n")
            $driverIncluded = $true
        }
    }

    # ---- 版本信息 ----
    $verText = Get-Content (Join-Path $root 'include\uac\version.h') -Raw
    $version = if ($verText -match 'kVersion\[\]\s*=\s*"([^"]+)"') { $Matches[1] } else { 'unknown' }
    $commit = 'unknown'; $dirty = 'unknown'
    try {
        $commit = (& git -C $root rev-parse --short HEAD 2>$null)
        $dirty = if ((& git -C $root status --porcelain 2>$null)) { 'yes (uncommitted changes)' } else { 'no' }
    } catch { }
    $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    $ver = @(
        "uac-simulator $version",
        "config   : $config",
        "git      : $commit (dirty: $dirty)",
        "packaged : $stamp",
        "driver   : $(if ($payloadIncluded -or $driverIncluded) { (@($payloadDesc, $(if ($driverIncluded) { "installer $driverDesc" })) | Where-Object { $_ }) -join '; ' } else { 'NOT included - host.* cases will SKIP (no usbip-payload\ and no installer)' })"
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $pkg 'VERSION.txt'), $ver + "`r`n")

    # ---- run.cmd：纯 ASCII + CRLF（cmd 按 OEM 代码页解析批处理，UTF-8 会乱码） ----
    $runCmd = @(
        '@echo off',
        'rem Double-click entry for the Windows Sandbox package. Self-elevates when needed.',
        'cd /d "%~dp0"',
        'powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-in-sandbox.ps1" %*',
        'set RC=%ERRORLEVEL%',
        'echo.',
        'if not "%RC%"=="0" goto failed',
        'echo Finished. Results: %~dp0results   (summary also on the Desktop)',
        'goto done',
        ':failed',
        'echo FAILED or refused (exit code %RC%). See the messages above.',
        ':done',
        'pause',
        'exit /b %RC%'
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $pkg 'run.cmd'), $runCmd + "`r`n", [Text.Encoding]::ASCII)

    # ---- README.txt: English only (the Sandbox image has no CJK fonts; Chinese showed up as boxes) ----
    $readme = @(
        'UAC simulator test bench - Windows Sandbox package',
        '',
        'How to use:',
        '  1. Copy this zip into Windows Sandbox and use "Extract All" from the right-click menu.',
        '  2. Double-click run.cmd (it elevates itself when needed; inside Sandbox you are an administrator by default).',
        '  3. Wait for it to finish: results\ holds one result file per profile, the first line reads',
        '     "RESULT: N passed, M failed, K skipped"; a summary is on the Desktop as uacsim-sandbox-summary.txt.',
        '  4. Copy the whole results folder back to the host (files can be copied and pasted between Sandbox and the host).',
        '',
        'Notes:',
        '  - Driver: when usbip-payload\ (the slim payload, about 3 MB) is present, run.cmd first checks every file against the',
        '    SHA-256 in its MANIFEST.txt and then installs from it (pnputil + devnode, the core steps of the official installer).',
        '    Without it the official installer in drivers\ is used; with neither, the host.* cases SKIP and only api.* runs.',
        '    An already installed driver is not installed again (devnode install creates a new device node every time).',
        '  - Collecting the slim payload (when there is no usbip-payload\ yet): run the official installer from drivers\ once',
        '    (tick "Install Microsoft Visual C++ Redistributable"), then double-click collect-usbip-payload.bat and copy the',
        '    folder it produces back to the host.',
        '  - Integrity: copying can damage files; run  powershell -File run-in-sandbox.ps1 -VerifyOnly  to check SHA256SUMS.txt first.',
        '  - The scripts only run inside Windows Sandbox / a VM; on an ordinary development machine they refuse (guard) and',
        '    install no driver and attach no device.',
        '  - Version and build information: VERSION.txt.',
        '  - To try things by hand: uacstudio.exe (GUI), uacsimd.exe + uacsim.exe.',
        '    uacstudio opens an environment check first; it lists what is missing and, inside Sandbox, can install the driver.'
    ) -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $pkg 'README.txt'), $readme + "`r`n", (New-Object Text.UTF8Encoding($true)))

    # ---- SHA256SUMS.txt（最后写：覆盖前面所有文件，自己除外） ----
    $pkgFull = (Resolve-Path $pkg).Path.TrimEnd('\')
    $lines = Get-ChildItem $pkg -Recurse -File | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($pkgFull.Length + 1)
        '{0} *{1}' -f (Get-Sha256 $_.FullName), $rel
    }
    [IO.File]::WriteAllText((Join-Path $pkg 'SHA256SUMS.txt'), ($lines -join "`r`n") + "`r`n", [Text.Encoding]::ASCII)

    # ---- 压缩 ----
    if ($NoZip) {
        Write-Host "[package] 已暂存到 $pkg（-NoZip）"
    } else {
        New-Item -ItemType Directory -Force $OutDir | Out-Null
        $zip = Join-Path (Resolve-Path $OutDir).Path "uacsim-sandbox-$config.zip"
        if (Test-Path $zip) { Remove-Item $zip -Force }
        # 逐个写入条目、显式用正斜杠。Compress-Archive 和 ZipFile.CreateFromDirectory 在 Windows PowerShell 5.1
        # 里都会写反斜杠条目名（powershell.exe 的配置把运行时声明为 .NET 4.5，沿用旧行为）；
        # 反斜杠不符合 zip 规范，部分解压工具会把目录结构弄平。
        Add-Type -AssemblyName System.IO.Compression
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $top = Split-Path $pkg -Leaf
        $zfs = [IO.File]::Open($zip, [IO.FileMode]::Create)
        $za = New-Object IO.Compression.ZipArchive($zfs, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($f in (Get-ChildItem $pkg -Recurse -File | Sort-Object FullName)) {
                $name = $top + '/' + $f.FullName.Substring($pkgFull.Length + 1).Replace('\', '/')
                $null = [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($za, $f.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
            }
        }
        finally { $za.Dispose(); $zfs.Dispose() }
        $zi = Get-Item $zip
        $count = (Get-ChildItem $pkg -Recurse -File).Count
        Write-Host ''
        Write-Host '[package] Windows Sandbox 包已生成' -ForegroundColor Green
        Write-Host "  文件   : $zip"
        Write-Host ("  大小   : {0:N1} MB，{1} 个文件，配置 {2}，版本 {3}，git {4}" -f ($zi.Length / 1MB), $count, $config, $version, $commit)
        Write-Host "  SHA256 : $(Get-Sha256 $zip)"
        if ($payloadIncluded) {
            Write-Host "  驱动   : 已包含精简载荷 $payloadDesc（BSD-2-Clause，许可文本已随附）" -ForegroundColor Yellow
        }
        if ($driverIncluded) {
            Write-Host '  驱动   : 已包含 usbip-win2 官方安装包（BSD-2-Clause，许可文本已随附）' -ForegroundColor Yellow
        }
        if (-not $payloadIncluded -and -not $driverIncluded) {
            Write-Host "  驱动   : 未包含（$DriverDir 里既没有 usbip-payload\ 也没有 .msi/.exe）。Sandbox 里 host.* 用例会 SKIP；" -ForegroundColor Yellow
            Write-Host '           放入精简载荷或官方安装包后重新打包，或在 Sandbox 里拷进 drivers\。' -ForegroundColor Yellow
        }
        Write-Host '  使用   : 拷进 Sandbox -> 解压 -> 双击 run.cmd'
    }
}
finally {
    if ($tempRoot -and (Test-Path $tempRoot)) { Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue }
}
