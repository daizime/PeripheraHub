[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageDirectory,
    [switch]$CheckCleanArchive
)
$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath($PackageDirectory).TrimEnd('\', '/')
$manifest = Get-Content -LiteralPath (Join-Path $root 'dependency-manifest.json') -Raw | ConvertFrom-Json
$build = Get-Content -LiteralPath (Join-Path $root 'build-manifest.json') -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 2 -or $build.schemaVersion -ne 1 -or -not $build.testsPassed) {
    throw 'Missing supported manifest or successful build/test evidence.'
}
if ($manifest.version -ne $build.version -or $manifest.sourceDigest -ne $build.sourceDigest) {
    throw 'Package version/source identity differs from the tested build.'
}
if ($CheckCleanArchive -and (Test-Path -LiteralPath (Join-Path $root 'data'))) {
    throw 'Distribution staging must not contain a local data directory.'
}
$declared = @{}
foreach ($entry in $manifest.files) {
    $relative = [string]$entry.path
    if ([System.IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)' -or
        $relative -match '[:\r\n]' -or $declared.ContainsKey($relative)) {
        throw "Invalid or duplicate relative manifest path: $relative"
    }
    $absolute = [System.IO.Path]::GetFullPath((Join-Path $root $relative))
    if (-not $absolute.StartsWith($root + [System.IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Manifest path escaped package directory: $relative"
    }
    $file = Get-Item -LiteralPath $absolute
    if ($file.PSIsContainer -or $file.Length -ne $entry.length -or
        (Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash -ne $entry.sha256 -or
        $file.VersionInfo.FileVersion -ne $entry.fileVersion) {
        throw "Packaged file differs from manifest: $relative"
    }
    $declared[$relative] = $true
}
$actual = @(Get-ChildItem -LiteralPath $root -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') })
foreach ($file in $actual) {
    $relative = $file.FullName.Substring($root.Length + 1).Replace('\', '/')
    if (-not $declared.ContainsKey($relative)) { throw "Undeclared executable dependency: $relative" }
}
$exe = Get-Item -LiteralPath (Join-Path $root 'PeripheralHub.exe')
if ($actual.Count -ne $declared.Count -or $exe.VersionInfo.ProductVersion -ne $manifest.version -or
    (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash -ne $build.binarySha256) {
    throw 'Packaged executable does not match the tested binary/version.'
}
foreach ($required in @('qt.conf', 'LICENSE', 'COPYING.md', 'THIRD_PARTY_NOTICES.md',
        'licenses/FluentSystemIcons/LICENSE', 'licenses/Qt/LICENSES/LGPL-3.0-only.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $required) -PathType Leaf)) {
        throw "Required configuration or notice is missing: $required"
    }
}
if ($manifest.applicationSourceIncluded) {
    $sourcePath = Join-Path $root 'PeripheralHub-source.zip'
    if ((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash -ne $manifest.applicationSourceSha256) {
        throw 'Application source archive differs from the package manifest.'
    }
    Add-Type -AssemblyName System.IO.Compression
    $stream = [System.IO.File]::OpenRead($sourcePath)
    $archive = [System.IO.Compression.ZipArchive]::new($stream, [System.IO.Compression.ZipArchiveMode]::Read)
    try {
        $names = @($archive.Entries | ForEach-Object FullName)
        if ($names -match '^PeripheralHub/(?:work|outputs|data|\.git)/' -or
            $names -notcontains 'PeripheralHub/LICENSE' -or
            $names -notcontains 'PeripheralHub/scripts/build.ps1' -or
            $names -notcontains 'PeripheralHub/src/app/main.cpp') {
            throw 'Source archive contains private build/data roots or lacks required build inputs.'
        }
        # Verify the archive contains exactly the tested compiler/test inputs.
        foreach ($inputFile in $build.inputs) {
            $entry = $archive.GetEntry('PeripheralHub/' + $inputFile.path)
            if (-not $entry) { throw "Source archive lacks tested input: $($inputFile.path)" }
            $inputStream = $entry.Open()
            $hash = [System.Security.Cryptography.SHA256]::Create()
            try { $digest = ([BitConverter]::ToString($hash.ComputeHash($inputStream))).Replace('-', '') }
            finally { $hash.Dispose(); $inputStream.Dispose() }
            if ($digest -ne $inputFile.sha256) { throw "Source archive input differs: $($inputFile.path)" }
        }
    } finally { $archive.Dispose(); $stream.Dispose() }
} elseif ($CheckCleanArchive) {
    throw 'Clean distribution staging requires an application source archive.'
}
Write-Host "Package integrity verified: $($manifest.version), $($actual.Count) binary files. Release acceptance remains separate."
