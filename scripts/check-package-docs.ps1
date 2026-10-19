# Verify the documentation set shipped by scripts/package.ps1 is self-contained:
# every shipped document must exist, and every docs/*.md it references must also
# be shipped. English-only output so the file does not depend on a UTF-8 BOM.
param(
    [string]$PackagePath = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$package = if ($PackagePath) { $PackagePath } else { Join-Path $root "scripts\package.ps1" }
if (-not (Test-Path $package)) { Write-Error "package script not found: $package"; exit 2 }

$text = [System.IO.File]::ReadAllText($package, [System.Text.Encoding]::UTF8)
$shipped = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
foreach ($match in [regex]::Matches($text, "docs\\([A-Za-z0-9_.-]+\.md)")) {
    $null = $shipped.Add($match.Groups[1].Value)
}
Write-Output "shipped=$($shipped.Count)"

$problems = New-Object System.Collections.Generic.List[string]
foreach ($name in $shipped) {
    $path = Join-Path $root "docs\$name"
    if (-not (Test-Path -LiteralPath $path)) { $problems.Add("package.ps1 ships a missing file: docs/$name"); continue }
    $body = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
    foreach ($ref in [regex]::Matches($body, "([A-Za-z0-9_.-]+\.md)")) {
        $target = $ref.Groups[1].Value
        if ($target -eq $name) { continue }
        $targetPath = Join-Path $root "docs\$target"
        # Only punish references to files that exist in docs/ but are not shipped.
        if ((Test-Path -LiteralPath $targetPath) -and -not $shipped.Contains($target)) {
            $problems.Add("docs/$name references docs/$target, which the package does not ship")
        }
    }
}

$unique = $problems | Sort-Object -Unique
Write-Output "problems=$($unique.Count)"
foreach ($item in $unique) { Write-Output "PROBLEM $item" }
if ($unique.Count -gt 0) { exit 1 }
