# One command that runs every repository check used before handoff:
#   1. Release build with the full CTest suite
#   2. documentation reference check
#   3. packaged-document whitelist check
# English-only output so the file does not depend on a UTF-8 BOM.
param(
    [string]$BuildDirectory = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root "work\build-vs" }
$exe = (Get-Process -Id $PID).Path
$results = New-Object System.Collections.Generic.List[object]

function Run-Check([string]$name, [scriptblock]$body) {
    Write-Output ""
    Write-Output "=== $name ==="
    $code = 0
    try { & $body; $code = $LASTEXITCODE } catch { Write-Output "ERROR $($_.Exception.Message)"; $code = 1 }
    if ($null -eq $code) { $code = 0 }
    $results.Add([pscustomobject]@{ check = $name; exit = $code })
    return $code
}

if (-not $SkipBuild) {
    $null = Run-Check "build and CTest" {
        & $exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1") `
            -Configuration Release -BuildDirectory $BuildDirectory -RunTests
    }
}

$null = Run-Check "document references" {
    & $exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "check-doc-links.ps1")
}

$null = Run-Check "packaged documents" {
    & $exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "check-package-docs.ps1")
}

Write-Output ""
$results | Format-Table -AutoSize | Out-String -Width 80 | Write-Output
$failed = @($results | Where-Object { $_.exit -ne 0 }).Count
if ($failed -gt 0) {
    Write-Output "RESULT failed=$failed"
    exit 1
}
Write-Output "RESULT all checks passed"
