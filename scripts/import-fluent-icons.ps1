[CmdletBinding()]
param([string]$Commit = 'f1a33bcbe1b133cf36bbb80b2dc7ca935a1011bf')

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$sourceDirectory = Join-Path $projectRoot 'third_party\fluent-system-icons'
if ($Commit -notmatch '^[a-f0-9]{40}$') { throw 'Use a complete upstream commit SHA.' }

# Only these official assets are imported. Regular and filled share the same 24 px grid.
$icons = [ordered]@{
    apps = 'Apps'
    save_multiple = 'Save Multiple'
    cursor = 'Cursor'
    desktop = 'Desktop'
    keyboard = 'Keyboard'
    headphones = 'Headphones'
    brightness_high = 'Brightness High'
    speaker_2 = 'Speaker 2'
    speaker_mute = 'Speaker Mute'
    mic = 'Mic'
    mic_off = 'Mic Off'
    arrow_sync = 'Arrow Sync'
    text_bullet_list_ltr = 'Text Bullet List LTR'
    link = 'Link'
    more_horizontal = 'More Horizontal'
    chevron_up = 'Chevron Up'
    chevron_down = 'Chevron Down'
    arrow_exit = 'Arrow Exit'
    info = 'Info'
    settings = 'Settings'
}
New-Item -ItemType Directory -Path (Join-Path $sourceDirectory 'svg') -Force | Out-Null
$upstream = "https://raw.githubusercontent.com/microsoft/fluentui-system-icons/$Commit"
Invoke-WebRequest -Uri "$upstream/LICENSE" -OutFile (Join-Path $sourceDirectory 'LICENSE')
$assets = @()
foreach ($entry in $icons.GetEnumerator()) {
    foreach ($variant in @('regular', 'filled')) {
        $fileName = "ic_fluent_$($entry.Key)_24_$variant.svg"
        $upstreamPath = "assets/$($entry.Value)/SVG/$fileName"
        $uri = "$upstream/assets/$([Uri]::EscapeDataString($entry.Value))/SVG/$fileName"
        $destination = Join-Path $sourceDirectory "svg\$fileName"
        Write-Host "Importing $fileName"
        Invoke-WebRequest -Uri $uri -OutFile $destination
        $assets += [ordered]@{
            name = $entry.Key
            variant = $variant
            source = $upstreamPath
            sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}
[ordered]@{
    project = 'https://github.com/microsoft/fluentui-system-icons'
    commit = $Commit
    license = 'MIT'
    copyright = 'Copyright (c) 2020 Microsoft Corporation'
    licenseSha256 = (Get-FileHash -LiteralPath (Join-Path $sourceDirectory 'LICENSE') -Algorithm SHA256).Hash.ToLowerInvariant()
    assets = $assets
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $sourceDirectory 'manifest.json') -Encoding UTF8
Write-Host "Imported $($assets.Count) official SVG assets at $Commit. Render with tools/fluent-icon-renderer."
