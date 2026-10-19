[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$QtRoot = 'F:\qt\6.7.0\msvc2019_64',
    [string]$VsRoot = 'D:\c++\software\vs',
    [string]$CMakePath = '',
    [string]$BuildDirectory = '',
    [switch]$NoGui,
    [switch]$RunTests
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'BuildInventory.ps1')
$sourceBefore = @(Get-HubSourceInventory -ProjectRoot $projectRoot)
$sourceDigest = Get-HubInventoryDigest -Inventory $sourceBefore
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot 'work\build'
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$QtRoot = [System.IO.Path]::GetFullPath($QtRoot)
$VsRoot = [System.IO.Path]::GetFullPath($VsRoot)
if ([string]::IsNullOrWhiteSpace($CMakePath)) {
    $vsCMake = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $vsCMake -PathType Leaf) {
        $CMakePath = $vsCMake
    } else {
        $CMakePath = 'F:\qt\Tools\CMake_64\bin\cmake.exe'
    }
}
$CMakePath = [System.IO.Path]::GetFullPath($CMakePath)
$vcvarsPath = Join-Path $VsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$ctestPath = Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe'

$requiredTools = @($vcvarsPath, $CMakePath)
if (-not $NoGui) { $requiredTools += Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake' }
foreach ($requiredPath in $requiredTools) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required tool or Qt configuration was not found: $requiredPath"
    }
}
if ($RunTests -and -not (Test-Path -LiteralPath $ctestPath -PathType Leaf)) {
    throw "CTest was not found: $ctestPath"
}
# The driver is interpreted by cmd.exe; reject metacharacters rather than trying
# to reinterpret arbitrary paths as shell syntax. Spaces and Chinese are valid.
foreach ($shellPath in @($projectRoot, $BuildDirectory, $QtRoot, $vcvarsPath, $CMakePath, $ctestPath)) {
    if ($shellPath -match '["%!&|<>^\r\n]') {
        throw "This script does not support cmd.exe metacharacters in paths: $shellPath"
    }
}

New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
$buildManifestPath = Join-Path $BuildDirectory 'build-manifest.json'
if (Test-Path -LiteralPath $buildManifestPath -PathType Leaf) {
    Remove-Item -LiteralPath $buildManifestPath -Force
}
$driverPath = Join-Path $BuildDirectory 'build-local.cmd'
$buildApp = if ($NoGui) { 'OFF' } else { 'ON' }
$driverLines = @(
    '@echo off',
    'chcp 65001 >nul',
    ('call "{0}"' -f $vcvarsPath),
    'if errorlevel 1 exit /b %errorlevel%',
    # Compiler probes still compile and link an executable. Release probes avoid
    # the old CMake/MSVC PDB path encoding failure in non-ASCII workspaces.
    ('"{0}" -S "{1}" -B "{2}" -G "NMake Makefiles" "-DCMAKE_PREFIX_PATH={3}" "-DCMAKE_BUILD_TYPE={4}" -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DBUILD_TESTING=ON -DPERIPHERAL_BUILD_APP={5}' -f $CMakePath, $projectRoot, $BuildDirectory, $QtRoot, $Configuration, $buildApp),
    'if errorlevel 1 exit /b %errorlevel%',
    ('"{0}" --build "{1}"' -f $CMakePath, $BuildDirectory),
    'if errorlevel 1 exit /b %errorlevel%'
)
if ($RunTests) {
    $driverLines += @(
        ('"{0}" --test-dir "{1}" --output-on-failure' -f $ctestPath, $BuildDirectory),
        'if errorlevel 1 exit /b %errorlevel%'
    )
}
$driverLines += 'exit /b 0'
[System.IO.File]::WriteAllText($driverPath, (($driverLines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c ('"{0}"' -f $driverPath)
if ($LASTEXITCODE -ne 0) {
    throw "Build or tests failed with exit code $LASTEXITCODE. See the output above."
}
$sourceAfter = @(Get-HubSourceInventory -ProjectRoot $projectRoot)
if ((Get-HubInventoryDigest -Inventory $sourceAfter) -ne $sourceDigest) {
    throw 'Build inputs changed while building/testing. Build again before packaging.'
}
if (-not $NoGui) {
    $builtExe = Get-Item -LiteralPath (Join-Path $BuildDirectory 'PeripheralHub.exe')
    [ordered]@{
        schemaVersion = 1
        application = 'PeripheralHub'
        version = $builtExe.VersionInfo.ProductVersion
        configuration = $Configuration
        builtAtUtc = [DateTime]::UtcNow.ToString('o')
        testsPassed = [bool]$RunTests
        sourceDigest = $sourceDigest
        binarySha256 = (Get-FileHash -LiteralPath $builtExe.FullName -Algorithm SHA256).Hash
        inputs = $sourceAfter
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $buildManifestPath -Encoding UTF8
}
if ($NoGui) { Write-Host "Native libraries ready: $BuildDirectory (peripheral_core.lib, peripheral_windows.lib)" }
else { Write-Host "Build complete: $(Join-Path $BuildDirectory 'PeripheralHub.exe')" }
