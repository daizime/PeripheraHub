# 本机开发打包与后续分发

## 当前包的用途

`scripts/package.ps1` 生成本机开发包，按明确白名单动态部署 Qt DLL，并记录实际复制的 EXE/DLL 文件版本。它不代表项目已完成对外分发许可审计，也不代表程序已在第二台未安装开发环境的电脑上完成验证。

项目原创程序代码现按 AGPL-3.0-or-later 提供，完整文本和说明见根目录 `LICENSE`、`COPYING.md`。第三方组件继续受各自许可证约束；不能据此使用没有授权的上游实现。

打包现在要求 `scripts/build.ps1 -Configuration Release -RunTests` 成功后生成的 `build-manifest.json`。源码、构建脚本或测试输入改变，EXE 被替换，都会拒绝沿用旧构建。清单记录相对路径、输入摘要、二进制 SHA-256 与版本，不写入开发工具的绝对安装路径。`scripts/verify-package.ps1` 复查实际文件、版本、长度和哈希；这些校验不能替代设备、系统兼容性和许可验收。

用于审查的干净候选目录可这样生成；目标必须为空，脚本不会删除已有目录：

```powershell
.\scripts\package.ps1 -BuildDirectory "$PWD\work\release-validation" -OutputDirectory "$PWD\work\release-stage" -RequireEmptyOutput -IncludeApplicationSources
.\scripts\verify-package.ps1 -PackageDirectory "$PWD\work\release-stage" -CheckCleanArchive
```

应用源码归档仅包含明确的源码、资源、测试、构建脚本和许可目录，排除 `data/`、`outputs/`、`work/` 及研究下载；归档中编译与测试输入逐项对照已测试构建。源码压缩包和运行时可以独立交付，开发诊断记录不应进入公开包。该候选仍保留 Demo 标识；未附或未核对 Qt 对应源码时不能据此宣称完成所有分发义务。

## 精简部署清单

当前 Widgets 光栅界面仅部署 `Qt6Core.dll`、`Qt6Gui.dll`、`Qt6Widgets.dll`、`platforms/qwindows.dll`、`styles/qmodernwindowsstyle.dll` 和 `qt.conf`。MSVC CRT 以应用本地 DLL 方式，从 VS 的 x64 可再分发目录复制；不从 Windows 系统目录抽取 DLL，不附 `vc_redist` 安装器。

本轮不再使用 `windeployqt` 自动收集，包内不需要旧部署带入的 OpenGL 软件回退、DXC、Network/TLS/SVG 等模块。未来功能依赖这些模块时，必须重新评估清单，而不能依赖开发机 PATH 补齐。

可通过 `-OutputDirectory` 指定干净的输出目录，通过 `-RuntimeDirectory` 指定可再分发 CRT 目录；原有 Qt、VS、构建目录与源码参数仍可使用。重复打包只覆盖同名文件，不移除旧插件，精简包应生成到新目录。实际体积取决于 DLL 版本、许可材料和是否附源码，不能仅看 EXE 大小。

最小包已在本机清除 Qt/开发工具 PATH 及 Qt 插件环境路径后，使用 Windows 平台正常运行，退出码为 0 并检查截图。该验证支持当前依赖清单，但不等于已完成第二台无开发环境机器或所有显卡、系统版本的兼容验收。

## Qt 对应源码安排

本机已安装的 Qt 源码路径默认是 `F:\qt\6.7.0\Src`。默认包只附许可与说明，不附完整源码。需要导出本机源码副本时运行：

```powershell
.\scripts\package.ps1 -BuildDirectory "$PWD\work\build-vs" -IncludeQtSources
```

这会把所选 `-QtSourceRoot` 完整复制到包内 `source/qt-installed-source`，可能占用较多空间和时间。脚本不将上游网页链接当成完整对应源码交付，也不编造具有法律效力的书面源码要约。

在选择实际对外分发方案之前，仍需核对：

1. 实际部署的 Qt 版本、模块与源码版本一致，包含所需构建脚本、补丁和对应构建说明；安装目录标为同一版本不自动证明二进制完全对应。
2. 按实际采用的 LGPLv3 / GPLv3 适用条款安排源码交付、许可通知、修改说明和用户替换或重新链接的权利；如果选择源码要约，需要确保发布主体有能力在规定期限内履行。
3. 审计 Qt 模块内的第三方代码和实际部署的 Windows 平台、样式插件，不能只保留 Qt 根目录的一份 LGPL 文本。精简文件数量不等于已满足商用或闭源分发条件。
4. 核定 MSVC 运行库的再分发权和部署方式，并在未安装 Qt/Visual Studio 的 Windows 11 环境测试。
5. 根据程序最终许可决定是否需要附应用源码、重链接材料及其他说明。动态链接本身不等于自动完成全部义务。

源码全量复制选项只是保留材料的工具，不能替代这些核对。

## 发布材料建议

正式发布时保留固定源代码版本、构建命令、编译器和 SDK 版本、依赖清单、源码归档校验值、许可文本及测试记录。不要将开发机绝对路径、个人配置或诊断设备信息不加检查地放入公开包。

本地打包不会自动删除旧文件，正式包请从干净工作副本导出。建议在通过实机和干净环境验证后，再考虑安装包、签名和自动更新。

官方参考：[Qt LGPL obligations](https://www.qt.io/development/open-source-lgpl-obligations)、[Qt Windows deployment](https://doc.qt.io/qt-6/windows-deployment.html)、[Microsoft C++ redistribution](https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files)。
