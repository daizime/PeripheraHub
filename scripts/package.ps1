[CmdletBinding()]
param(
    [string]$QtRoot = 'F:\qt\6.7.0\msvc2019_64',
    [string]$VsRoot = 'D:\c++\software\vs',
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = '',
    [string]$RuntimeDirectory = '',
    [string]$QtSourceRoot = 'F:\qt\6.7.0\Src',
    [switch]$IncludeQtSources,
    [switch]$RequireEmptyOutput,
    [switch]$IncludeApplicationSources
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'BuildInventory.ps1')
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $projectRoot 'work\build'
}
$BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
$QtRoot = [System.IO.Path]::GetFullPath($QtRoot)
$VsRoot = [System.IO.Path]::GetFullPath($VsRoot)
$QtSourceRoot = [System.IO.Path]::GetFullPath($QtSourceRoot)
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot 'outputs\PeripheralHub-0.0'
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
$binaryPath = Join-Path $BuildDirectory 'PeripheralHub.exe'
$buildManifestPath = Join-Path $BuildDirectory 'build-manifest.json'
if (-not (Test-Path -LiteralPath $buildManifestPath -PathType Leaf)) {
    throw 'No verified build manifest. Run scripts/build.ps1 -RunTests before packaging.'
}
$buildManifest = Get-Content -LiteralPath $buildManifestPath -Raw | ConvertFrom-Json
if ($buildManifest.schemaVersion -ne 1 -or -not $buildManifest.testsPassed -or $buildManifest.configuration -ne 'Release') {
    throw 'Packaging requires a Release build with successful tests.'
}
$sourceInventory = @(Get-HubSourceInventory -ProjectRoot $projectRoot)
if ((Get-HubInventoryDigest -Inventory $sourceInventory) -ne $buildManifest.sourceDigest) {
    throw 'Source inputs differ from the tested build. Rebuild and test before packaging.'
}
if ((Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash -ne $buildManifest.binarySha256) {
    throw 'Executable differs from the tested build manifest.'
}
if ($RequireEmptyOutput -and (Test-Path -LiteralPath $outputPath)) {
    if (-not (Test-Path -LiteralPath $outputPath -PathType Container) -or
        @(Get-ChildItem -LiteralPath $outputPath -Force).Count -gt 0) {
        throw 'Fresh staging requires an empty output directory; existing files are never deleted.'
    }
}
# Widgets 界面使用光栅绘制：只部署实际使用的 Qt 模块和 Windows 插件。
# 保持动态链接，不加入 OpenGL 软件回退、网络/TLS、SVG 或完整运行库安装器。
$qtFiles = @{
    'bin\Qt6Core.dll' = 'Qt6Core.dll'
    'bin\Qt6Gui.dll' = 'Qt6Gui.dll'
    'bin\Qt6Widgets.dll' = 'Qt6Widgets.dll'
    'plugins\platforms\qwindows.dll' = 'platforms\qwindows.dll'
    'plugins\styles\qmodernwindowsstyle.dll' = 'styles\qmodernwindowsstyle.dll'
}
if ([string]::IsNullOrWhiteSpace($RuntimeDirectory)) {
    $versions = Get-ChildItem -LiteralPath (Join-Path $VsRoot 'VC\Redist\MSVC') -Directory |
        Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
        Sort-Object { [version]$_.Name } -Descending
    foreach ($version in $versions) {
        $candidates = @(Get-ChildItem -LiteralPath (Join-Path $version.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT')
        if ($candidates.Count -eq 1) { $RuntimeDirectory = $candidates[0].FullName; break }
    }
}
if ([string]::IsNullOrWhiteSpace($RuntimeDirectory)) { throw 'MSVC x64 CRT was not found; set -RuntimeDirectory.' }
$RuntimeDirectory = [System.IO.Path]::GetFullPath($RuntimeDirectory)
$requiredFiles = @($binaryPath, (Join-Path $QtSourceRoot 'LICENSES\LGPL-3.0-only.txt'), (Join-Path $QtSourceRoot 'LICENSES\GPL-3.0-only.txt'))
$requiredFiles += $qtFiles.Keys | ForEach-Object { Join-Path $QtRoot $_ }
$requiredFiles += @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') | ForEach-Object { Join-Path $RuntimeDirectory $_ }
foreach ($requiredPath in $requiredFiles) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required file was not found: $requiredPath"
    }
}
# Existing output files are not deleted. Use a clean output checkout for releases.
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $outputPath 'PeripheralHub.exe') -Force
foreach ($entry in $qtFiles.GetEnumerator()) {
    $destination = Join-Path $outputPath $entry.Value
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $QtRoot $entry.Key) -Destination $destination -Force
}
# 来自 Visual Studio 的可再分发 CRT 目录，使用应用本地 DLL；不复制 Windows 系统 DLL。
Get-ChildItem -LiteralPath $RuntimeDirectory -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $outputPath -Force
}
@'
[Paths]
Plugins=.
Translations=translations
'@ | Set-Content -LiteralPath (Join-Path $outputPath 'qt.conf') -Encoding ASCII

$licensePath = Join-Path $outputPath 'licenses\Qt'
New-Item -ItemType Directory -Path $licensePath -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $QtSourceRoot 'LICENSES') -Destination $licensePath -Recurse -Force
foreach ($module in @('qtbase')) {
    $moduleLicenses = Join-Path $QtSourceRoot "$module\LICENSES"
    if (Test-Path -LiteralPath $moduleLicenses -PathType Container) {
        $moduleDestination = Join-Path $licensePath $module
        New-Item -ItemType Directory -Path $moduleDestination -Force | Out-Null
        Copy-Item -LiteralPath $moduleLicenses -Destination $moduleDestination -Recurse -Force
    }
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $outputPath -Force
foreach ($applicationLicense in @('LICENSE', 'COPYING.md')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $applicationLicense) -Destination $outputPath -Force
}
Copy-Item -LiteralPath $buildManifestPath -Destination $outputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md') -Destination $outputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'CHANGELOG.md') -Destination $outputPath -Force
$docsOutputPath = Join-Path $outputPath 'docs'
New-Item -ItemType Directory -Path $docsOutputPath -Force | Out-Null
$fluentLicensePath = Join-Path $outputPath 'licenses\FluentSystemIcons'
New-Item -ItemType Directory -Path $fluentLicensePath -Force | Out-Null
foreach ($fluentNotice in @('LICENSE', 'manifest.json')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot "third_party\fluent-system-icons\$fluentNotice") -Destination $fluentLicensePath -Force
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\DISTRIBUTION.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\VALIDATION.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\ARCHITECTURE.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\MONITOR_CONTROLS.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\DEVICE_PROFILES.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\APP_SETTINGS.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\UI_ICONS.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\UI_GLASS.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\N9_V2_PROTOCOL.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\VGN_F2_PROTOCOL.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\RELEASE_READINESS_2026-10-19.md') -Destination $docsOutputPath -Force
# 被上面两份协议文档引用，必须一起分发，否则包内出现失效引用。
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\PRIVATE_WRITE_CANDIDATES_2026-10-19.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\ACCEPTANCE_CHECKLIST.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\FEATURES.md') -Destination $docsOutputPath -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\RELEASE_0.0.md') -Destination $docsOutputPath -Force
@'
# 集成式外设管理 · 0.0 初始预览版

双击同目录 PeripheralHub.exe 启动，保留同目录 DLL、platforms、styles 和 qt.conf。

系统托盘可用时，关闭窗口会收起到托盘；完全退出可按 Ctrl+Q，或右键托盘图标选择“退出”。配置和日志默认保存在同目录 data 文件夹。请保留整个程序目录，不要只移动 EXE。

这是初始预览版，设备能力以实际检测为准。VGN F2 Master+ 已实测读取电量、DPI 档位、回报率、按键及性能参数；实验性写入默认关闭，开启后仅对限定型号开放档位 DPI、回报率和当前档位，物理效果及断电保存仍待验收，详见 VGN_F2_PROTOCOL.md。未经验证的零状态字节显示为状态未确认。LANGTU L98 的电量、RGB、改键尚未完成适配。N9 V2 ULTRA 已接入电量与 10 段 PEQ 只读参数，并按 Container ID 关联到音频端点及保存档案，EQ 写入仍未开放。未知数据不会用模拟值代替，详见 N9_V2_PROTOCOL.md。

拖动音量或亮度滑块时会持续调整，松手确保最终值生效。亮度受 DDC/CI 响应速度限制，软件会合并过密请求。音频端点存在不代表耳机本体在线；插线充电不一定改变通信模式。

点击卡片空白处或右上角“更多”，可在卡片内打开详细控制；点击空白处或“收起”返回。各卡片独立切换，长内容在内部滚动。亮度和音量支持精确输入及快捷档位，鼠标的连接关联入口位于详情中。

滚轮用于滚动内容，不改变闭合下拉框、数值框和滑条参数；下拉列表展开后可滚动浏览，再点击选项确认。精确输入、方向键和拖动滑条保持可用。

界面采用微软 Fluent System Icons，默认线框、侧栏选中实心，静音图标随实际读回状态变化。图标已嵌入 EXE，MIT 许可与来源清单位于 licenses/FluentSystemIcons，详见 UI_ICONS.md。

整窗采用浅蓝紫毛玻璃主题与一体式标题栏；系统支持且开启“透明效果”时请求窗口背景模糊，透明颜色随后方内容变化，失败时使用同配色磨砂渐变。数值框统一使用加减按钮，对比度继续保留设备原始范围并在“应用”后提交。详见 UI_GLASS.md。

窗口打开或最小化时，悬停任务栏图标可显示四格简略状态，点击由 Windows 切回主窗口。简略内容不含标题和完整界面入口，未知读数显示“—”；Windows 控制外框、系统标题和缩略图尺寸。关闭到托盘后不保证仍有任务栏窗口预览。

显示器按系统提供的型号自动识别；单屏自动选中，多屏需明确选择。内置功能按 DDC/CI 能力检测开放。H24T7 已报告软件切源后持续花屏、切回 DP 恢复，该功能尚未完成适配验证，目前请保持 DP，不使用软件切源。情景与色彩预设代码未核定时不要猜菜单名称；参见 MONITOR_CONTROLS.md。

无接入设备的类别卡片隐藏，同类设备在下拉列表切换；音频的输出与输入分别选择、调节。“已保存设备”按类别展开，音频再区分输出和输入，保留各设备名称与参数。devices.json 保存档案，重连恢复已确认的亮度、音量和可用情景；输入源匹配规则及未支持功能见 DEVICE_PROFILES.md。请保留 data 文件夹。

单独删除档案：先结束使用并断开设备（2.4G 拔出对应接收器），在“已保存设备”中选择具体设备，点击详情底部“删除设备档案”。确认后会重新扫描，目标仍未接入才删除；其他设备档案保留。重新接入时读取当前参数并建立新档案，不重置设备本身。

侧栏“设置”可调整关闭窗口行为、重连自动恢复和后台刷新间隔，也可打开数据目录、日志并查看版本。修改立即保存。“恢复默认程序设置”保留设备档案、连接关联和设备选择，具体见 APP_SETTINGS.md。

精简包采用 Windows 11 原生绘制，带应用本地 MSVC 运行库 DLL，不附开发工具或运行库安装器。未在没有开发环境的第二台电脑验收。

默认包未附完整 Qt 对应源码，限本机开发评估。对外分发前请阅读 DISTRIBUTION.md 和 THIRD_PARTY_NOTICES.md。退出程序后可整体移动这个目录，不要只复制 EXE。
'@ | Set-Content -LiteralPath (Join-Path $outputPath 'README-运行说明.md') -Encoding UTF8
$dependencyFiles = Get-ChildItem -LiteralPath $outputPath -Recurse -File |
    Where-Object { $_.Extension -in @('.exe', '.dll') } |
    Select-Object @{Name='path'; Expression={$_.FullName.Substring($outputPath.Length + 1).Replace('\', '/')}}, @{Name='fileVersion'; Expression={$_.VersionInfo.FileVersion}}, Length,
        @{Name='sha256'; Expression={(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}}
$manifest = [ordered]@{
    schemaVersion = 2
    application = 'PeripheralHub'
    version = $buildManifest.version
    releaseStage = 'Initial Preview'
    purpose = 'Local development package; external redistribution review pending'
    qtSourceIncluded = [bool]$IncludeQtSources
    applicationLicense = 'AGPL-3.0-or-later'
    applicationSourceIncluded = [bool]$IncludeApplicationSources
    sourceDigest = $buildManifest.sourceDigest
    deployment = 'Qt Widgets raster; Core/Gui/Widgets; qwindows and modern Windows style; app-local MSVC CRT'
    files = @($dependencyFiles)
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputPath 'dependency-manifest.json') -Encoding UTF8

if ($IncludeQtSources) {
    $sourceDestination = Join-Path $outputPath 'source\qt-installed-source'
    New-Item -ItemType Directory -Path $sourceDestination -Force | Out-Null
    Get-ChildItem -LiteralPath $QtSourceRoot -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $sourceDestination -Recurse -Force
    }
}
if ($IncludeApplicationSources) {
    # Explicit source roots prevent user settings, binaries, logs and research
    # downloads from leaking into a distributable application source archive.
    $sourceFiles = @()
    foreach ($folder in @('src', 'cmake', 'tests', 'scripts', 'docs', 'third_party', 'tools', 'assets', '.github')) {
        $sourceFiles += Get-ChildItem -LiteralPath (Join-Path $projectRoot $folder) -Recurse -File |
            Where-Object { $_.Extension -notin @('.exe', '.dll', '.lib', '.obj', '.pdb') -and
                $_.FullName -notmatch '[\\/](?:build|node_modules|\.git)[\\/]' }
    }
    foreach ($file in @('CMakeLists.txt', 'README.md', 'CHANGELOG.md', 'THIRD_PARTY_NOTICES.md', 'COPYING.md', 'LICENSE', '.clang-format', '.gitignore', '.gitattributes')) {
        $sourceFiles += Get-Item -LiteralPath (Join-Path $projectRoot $file)
    }
    Add-Type -AssemblyName System.IO.Compression
    $archivePath = Join-Path $outputPath 'PeripheralHub-source.zip'
    $archiveStream = [System.IO.File]::Open($archivePath, [System.IO.FileMode]::Create)
    $archive = [System.IO.Compression.ZipArchive]::new($archiveStream, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in ($sourceFiles | Sort-Object FullName -Unique)) {
            $relative = $file.FullName.Substring($projectRoot.Length + 1).Replace('\', '/')
            $entry = $archive.CreateEntry(('PeripheralHub/' + $relative), [System.IO.Compression.CompressionLevel]::Optimal)
            $inputStream = [System.IO.File]::OpenRead($file.FullName)
            $outputStream = $entry.Open()
            try { $inputStream.CopyTo($outputStream) }
            finally { $outputStream.Dispose(); $inputStream.Dispose() }
        }
    } finally { $archive.Dispose(); $archiveStream.Dispose() }
    $manifest.applicationSourceSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputPath 'dependency-manifest.json') -Encoding UTF8
}
@'
INITIAL PREVIEW 0.0 - LOCAL EVALUATION CANDIDATE
This directory is intended for local evaluation. External redistribution review is pending.
Read DISTRIBUTION.md before publishing this directory. The default package includes
license notices but does not include complete corresponding Qt source. Source inclusion
is available with scripts/package.ps1 -IncludeQtSources; source/binary correspondence,
build instructions, additional notices and Microsoft runtime redistribution rights still
require verification before release.
'@ | Set-Content -LiteralPath (Join-Path $outputPath 'LOCAL_DEVELOPMENT_PACKAGE.txt') -Encoding UTF8
Write-Host "Local package ready: $outputPath"
& (Join-Path $PSScriptRoot 'verify-package.ps1') -PackageDirectory $outputPath
