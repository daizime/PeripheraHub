# Check that paths referenced from Markdown docs exist somewhere in the repository.
# A reference counts as resolved when it exists relative to the repository root,
# relative to the document's own directory, or when its basename exists anywhere
# in the tree (docs habitually cite files such as `WindowsHid.cpp` by name).
# English-only output so the file does not depend on a UTF-8 BOM or code page.
param(
    [string]$DocsPath = "",
    [switch]$IncludeRuntime
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not $DocsPath) { $DocsPath = Join-Path $root "docs" }
if (-not (Test-Path $DocsPath)) { Write-Error "docs directory not found: $DocsPath"; exit 2 }

$pattern = '`([^`\r\n]+?\.(?:md|ps1|cpp|h|hpp|json|jsonl|log|png|txt|qss|in))`|\]\(([^)\s]+)\)'

# Index every file once so basename references resolve without walking per lookup.
$byName = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$byPath = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
Get-ChildItem -Path $root -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
    $null = $byName.Add($_.Name)
    $null = $byPath.Add($_.FullName.Substring($root.Length + 1).Replace('\', '/'))
}

$checked = 0
$missing = New-Object System.Collections.Generic.List[string]
foreach ($doc in Get-ChildItem -Path $DocsPath -Filter *.md -File -Recurse) {
    $text = [System.IO.File]::ReadAllText($doc.FullName, [System.Text.Encoding]::UTF8)
    $docDir = $doc.DirectoryName.Substring($root.Length + 1)
    foreach ($match in [regex]::Matches($text, $pattern)) {
        $target = if ($match.Groups[1].Success) { $match.Groups[1].Value } else { $match.Groups[2].Value }
        $target = $target.Trim()
        if ($target -match '^(https?:|mailto:|#|<)') { continue }
        # Absolute paths point outside the repository (for example a local Qt source tree).
        if ($target -match '^[A-Za-z]:[\\/]') { continue }
        if ($target.Contains('*') -or $target.Contains(' ')) { continue }
        $target = ($target -split '#')[0]
        if (-not $target) { continue }
        $normalized = ($target -replace '\\', '/').TrimStart('.', '/')
        if (-not $IncludeRuntime -and $normalized -match '^(data|outputs|work|build)/') { continue }
        $checked++
        $base = [System.IO.Path]::GetFileName($normalized)
        $hit = $byPath.Contains($normalized) -or $byPath.Contains("$docDir/$normalized") -or
            $byName.Contains($base)
        if (-not $hit) { $missing.Add("$($doc.Name) -> $target") }
    }
}

$unique = $missing | Sort-Object -Unique
Write-Output "checked=$checked missing=$($unique.Count)"
foreach ($item in $unique) { Write-Output "MISSING $item" }
if ($unique.Count -gt 0) { exit 1 }
