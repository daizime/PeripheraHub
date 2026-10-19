# 程序设置模块

侧栏与系统托盘菜单均提供“设置”。页面沿用当前毛玻璃主题、Fluent 图标和共享下拉框；修改后立即保存，无需重启。

| 选项 | 默认值 | 行为 |
|---|---|---|
| 关闭窗口时 | 收起到系统托盘 | 也可选择退出程序；系统托盘不可用时直接退出。Ctrl+Q 与托盘“退出”始终完全退出 |
| 重连自动恢复 | 自动恢复已确认参数 | 从后续连接会话生效，可改为只读取设备当前参数；已开始的恢复任务继续完成。恢复白名单和输入源规则不变 |
| 后台刷新间隔 | 3 秒 | 可选 3、5、10、30 秒；连接通知、手动刷新、滑块派发及设备缓存策略继续独立运行 |
| 低电量提醒 | 开启 | 鼠标或耳机电量低于 10% 时通过系统托盘通知提示；只对首次低电量读数提醒，电量恢复、开始充电或设备移除后重新布防。关闭后清除待提醒状态 |
| 打开数据目录 / 日志 | 当前 data 目录 | 由系统默认应用打开本地位置，日志尚未生成或打开失败时显示原因 |
| 实验性设备写入 | 关闭 | 开启后，鼠标详情出现写入控件：VGN F2 Master+ 的档位 DPI、回报率、当前档位。关闭时不出现任何写入控件，服务层同时拒绝写入请求 |

“恢复默认程序设置”只重置以上程序选项；`devices.json`、鼠标连接关联、设备选择和保存树展开状态继续保留。

## 模块接口

- `AppPreferences`：五个程序选项的值类型，默认值兼容旧版。
- `AppSettings::load/save`：只读写 `settings.ini` 的 `application/closeToTray`、`application/restoreOnReconnect`、`application/refreshIntervalSeconds`、`application/lowBatteryNotification`、`application/experimentalDeviceWrites`。未知或无效值按项回退并提示；读取损坏文件失败时保留原文件。保存失败会恢复共享配置缓存的旧值，界面和运行时保持原选项。
- `DeviceService::setExperimentalWritesEnabled`：所有私有写入的统一闸门。未开启或以 `--passive` 启动时，写入方法在查找设备之前就拒绝，并回报"未发送任何写入"。
- `widgets/SettingsPage`：纯界面，发出偏好变更与打开本地位置的信号；不访问设备或配置文件。所有选择框复用 `createThemedComboBox()`，滚轮只滚动内容。
- `MainWindow::changePreferences/applyPreferences`：保存成功后同步运行时及界面；保存失败显示原因并撤销界面改动。
- `DeviceService::setRefreshIntervalSeconds`：只更新后台轮询定时器，拒绝小于 3 秒或大于 60 秒的直接调用；设置页使用上述四个档位。
- `ProfileTracker::setRestoreOnReconnect`：决定后续新连接会话是否提交恢复任务；开关本身不向设备写参数。

当前没有开机启动、账号、云同步或独立硬件屏幕功能。关于设备恢复范围见 `DEVICE_PROFILES.md`。

## 本轮验证

2026-10-19：Release 与 CTest 6/6 通过，新增配置检查 46 项；程序界面/服务模拟集成 788 项通过、0 模拟硬件写入；档案跟踪专项 68 项通过，覆盖关闭恢复、读取当前值、开启后不重放当前会话、下一次连接再恢复。记录分别为 `work/verification/settings-module-build.log`、`settings-module-integration.log`、`settings-module-tracker.log`。

集成回归发现超屏音频名称被 Qt 菜单委托压缩后无法横向滚动，现通过共享下拉列表的显式内容宽度修正；初次失败日志保留在 `settings-module-integration-before-popup-fix.log`。模拟截图在 `work/verification/dynamic-cards/settings-minimum-fixture.png` 和 `settings-wide-fixture.png`，不能据此宣称真实设备协议完成适配。

实际交付包界面已核对侧栏、选项全文和版本。刷新间隔 3→5 秒后直接读取 INI 确认已落盘，再点击恢复默认回到 3 秒；保留更新前的 17 份设备档案，未手动调节硬件参数。截图 `outputs/settings-live-five-seconds.png`、`outputs/settings-module.png`，记录 `work/verification/settings-module-native-ui.txt`。

## 0.0 当前选项补充

当前还提供低电量提醒（默认开启，低于 10% 提醒）和实验性设备写入（默认关闭）。后者仅控制 VGN F2 Master+ 的档位 DPI、回报率与当前档位，关闭或被动模式下服务拒绝这些写入请求。应用不会自动恢复私有参数。历史回归数量属于当次开发记录；本版实际结果以 [RELEASE_0.0.md](RELEASE_0.0.md) 为准。
