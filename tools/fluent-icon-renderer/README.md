# Fluent 图标预渲染工具

仅在更新图标时使用。应用本身不依赖 Qt Svg。上游来源和许可见 `third_party/fluent-system-icons/manifest.json` 及 `LICENSE`。

在已加载 MSVC x64 环境的终端中，工作目录为项目根目录：

```powershell
# 仅在重新获取源文件时联网；脚本默认固定到本项目已核定的提交。
.\scripts\import-fluent-icons.ps1
cmake -S tools/fluent-icon-renderer -B work/fluent-icon-renderer -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TRY_COMPILE_CONFIGURATION=Release -DCMAKE_PREFIX_PATH=F:/qt/6.7.0/msvc2019_64
cmake --build work/fluent-icon-renderer
$env:PATH = 'F:\qt\6.7.0\msvc2019_64\bin;' + $env:PATH
.\work\fluent-icon-renderer\fluent_icon_renderer.exe third_party/fluent-system-icons/svg src/app/widgets/icons/fluent
```

正常构建直接使用已提交到项目的 PNG，不执行联网或图标转换。扩展图标时同步更新导入列表、`UiIcon` 映射和 `UiTheme.qrc`。现有 18 种图形各有 regular / filled 两种资源，统一采用官方 24px 版本并输出 96×96 透明 PNG；运行时按主题着色。
