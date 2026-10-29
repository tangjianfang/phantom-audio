# make-seed-iso.ps1 —— 用系统自带 IMAPI2 生成 NoCloud seed ISO（卷标 CIDATA）
# 注意：COM 互操作需要 Windows PowerShell 5.1（powershell.exe），不要用 pwsh 7 调用。
param(
    [Parameter(Mandatory)] [string]$UserDataPath,
    [string]$MetaDataPath,
    [Parameter(Mandatory)] [string]$OutIso
)

$ErrorActionPreference = 'Stop'

# meta-data 默认极简内容
if (-not $MetaDataPath) {
    $MetaDataPath = Join-Path $env:TEMP 'm0-meta-data'
    Set-Content -Path $MetaDataPath -Value "instance-id: m0-vm-001`nlocal-hostname: uac-m0"
}

$fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage.1
$fsi.VolumeName = 'CIDATA'
$fsi.FileSystemsToCreate = 3  # ISO9660 + Joliet
$fsi.MediaSize = 0
$dir = $fsi.Root
$dir.AddFile('user-data', $UserDataPath)
$dir.AddFile('meta-data', $MetaDataPath)
$image = $fsi.CreateResultImage()

$isoStream = New-Object -ComObject ADODB.Stream
$isoStream.Type = 1 # binary
$isoStream.Open()
$isoStream.Write($image.ImageStream)
$isoStream.SaveToFile($OutIso, 2)
$isoStream.Close()

Write-Host "seed ISO -> $OutIso ($((Get-Item $OutIso).Length) bytes)"
