# Read-only device snapshot used for acceptance runs and regression checks.
# Every command here only reads: no probe is started with --apply.
param([string]$OutputPath = "")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root "work\native-write"
if (-not (Test-Path (Join-Path $tools "hid_inventory.exe"))) {
    throw "Build the probes first: .\scripts\build.ps1 -NoGui -BuildDirectory `"$root\work\native-write`""
}
if (-not $OutputPath) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $dir = Join-Path $root "work\verification\device-snapshots"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $OutputPath = Join-Path $dir "$stamp.log"
}
$lines = New-Object System.Collections.Generic.List[string]
function Add-Output([string]$title, [scriptblock]$body) {
    $lines.Add("### $title")
    $lines.Add('$ ' + $title)
    try {
        $text = & $body 2>&1 | Out-String
        foreach ($line in ($text -split "`r?`n")) { if ($line.Trim()) { $lines.Add($line) } }
    } catch {
        $lines.Add("ERROR " + $_.Exception.Message)
    }
    $lines.Add("")
}
function Add-Inventory([string]$vid) {
    $lines.Add("### hid_inventory $vid")
    try {
        $text = & (Join-Path $tools "hid_inventory.exe") --vid $vid 2>&1 | Out-String
        foreach ($line in ($text -split "`r?`n")) { if ($line.Trim()) { $lines.Add($line) } }
    } catch {
        $lines.Add("ERROR " + $_.Exception.Message)
    }
    $lines.Add("")
}

$lines.Add("# Device snapshot $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') (Asia/Shanghai)")
$lines.Add("Read-only: no write command is issued by this script.")
$lines.Add("")
Add-Inventory "3554"
Add-Inventory "373B"
Add-Inventory "1A2C"

$lines.Add("### vgn_dpi_write_probe (read-only)")
try {
    $text = & (Join-Path $tools "vgn_dpi_write_probe.exe") --stage 3 --x 3200 --y 3200 2>&1 | Out-String
    foreach ($line in ($text -split "`r?`n")) { if ($line.Trim()) { $lines.Add($line) } }
} catch { $lines.Add("ERROR " + $_.Exception.Message) }
$lines.Add("")

$lines.Add("### n9_peq_write_probe (read-only)")
try {
    $text = & (Join-Path $tools "n9_peq_write_probe.exe") --point 0 --hz 64 --gain -2.0 --q 0.9 --type 1 2>&1 | Out-String
    foreach ($line in ($text -split "`r?`n")) { if ($line.Trim()) { $lines.Add($line) } }
} catch { $lines.Add("ERROR " + $_.Exception.Message) }
$lines.Add("")

$lines.Add("### live app data directory")
$dataDir = Join-Path $root "work\build-vs\data"
if (Test-Path $dataDir) {
    Get-ChildItem $dataDir -File | ForEach-Object {
        $lines.Add(("{0}  {1} bytes  {2}" -f $_.Name, $_.Length, $_.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")))
    }
} else {
    $lines.Add("no data directory at $dataDir")
}

$lines -join "`r`n" | Set-Content -Path $OutputPath -Encoding UTF8
Write-Output $OutputPath
