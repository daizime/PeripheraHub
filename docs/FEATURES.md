# 0.0 功能核对

核对日期：2026-10-19。范围为当前源码、现有自动测试和已有实机记录。本次发布检查不执行实际设备参数写入，也不把此前记录当成当前设备状态。

## 功能与实现

| 功能 | 实现和测试入口 | 0.0 状态 |
|---|---|---|
| 动态类别卡片、设备选择、详情和侧栏 | `MainWindow.cpp`、`DeviceGrid.cpp`、`dynamic_device_tests.cpp` | 已实现，模拟回归覆盖 |
| 显示器名称、亮度与 DDC 能力 | `WindowsMonitor.cpp`、`MonitorControls.cpp`、`ddc_tests.cpp`、`device_service_tests.cpp` | 基础控制已实现；型号能力与枚举需验证 |
| 音频输入/输出独立调节及静音 | `WindowsAudio.cpp`、`DeviceService.cpp`、`device_service_tests.cpp` | 已实现，重连不自动写静音 |
| VGN 电量与配置读取 | `vgn_protocol.cpp`、`vgn_config.cpp`、`WindowsHid.cpp`、`vgn_config_tests.cpp` | F2 Master+ 2.4G 已有实机证据 |
| VGN 实验性参数写入 | `DeviceService.cpp`、`MouseDetails.cpp`、`AppSettings.cpp`、`mouse_details_tests.cpp` | 默认关闭；仅 DPI、回报率、当前档位白名单 |
| N9 V2 状态与 PEQ 读取 | `n9_protocol.cpp`、`WindowsN9.cpp`、`HeadsetDetails.cpp`、`n9_protocol_tests.cpp` | 限定接收器已实测；产品不开放 EQ 写入 |
| 键盘系统接口发现 | `hid_discovery.cpp`、`hid_discovery_tests.cpp` | 通用发现；L98 私有功能未完成 |
| 按设备保存、删除与受限重连恢复 | `DeviceProfiles.cpp`、`ProfileTracker.cpp`、`SettingsRestore.cpp` 及对应测试 | 已实现；输入源为只读上下文 |
| 托盘快捷调节与低电量提醒 | `MainWindow.cpp`、`dynamic_device_tests.cpp` | 已实现；阈值低于 10%，支持关闭 |
| 程序偏好与单实例数据保护 | `AppSettings.cpp`、`RuntimeSession.cpp` 及对应测试 | 已实现；数据目录按规范化路径加锁 |
| 唤醒后的延迟整轮重扫 | `MainWindow.cpp`、`dynamic_device_tests.cpp` | 消息链有模拟测试；真实睡眠/唤醒未验收 |
| 全局快捷键、自启、云同步、RGB、固件升级 | 当前没有对应产品功能 | 未提供 |

同名文件的完整路径可在 `src/` 和 `tests/` 中查找。持续参数调节使用合并队列，静态刷新不会被视为用户设置意图。

## 四款目标设备

| 设备 | 已验证的主要范围 | 保留限制 |
|---|---|---|
| KTC H24T7 | 亮度改变/恢复；能力和当前值读取 | 切源曾花屏；矛盾的输入源/情景/色彩枚举保持只读 |
| VGN F2 Master+ | 2.4G 的电量、配置读取；三项探针写入/回读/改回 | 物理效果、断电保存、按键/性能写入、有线和充电切换未完成 |
| LANGTU L98 | 系统接口和用途发现 | 电量、RGB、改键无完整适配 |
| ATK N9 V2 ULTRA | 2.4G 的状态、电量、10 段 PEQ 读取 | 产品不开放 EQ 写入；有线和充电变化仍待验收 |

协议细节见 [VGN_F2_PROTOCOL.md](VGN_F2_PROTOCOL.md)、[N9_V2_PROTOCOL.md](N9_V2_PROTOCOL.md)、[MONITOR_CONTROLS.md](MONITOR_CONTROLS.md)。实机步骤见 [ACCEPTANCE_CHECKLIST.md](ACCEPTANCE_CHECKLIST.md)。

## 版本边界

0.0 是首次公开源码的初始预览版本。实验性开关打开、驱动返回成功、写后回读相同、物理效果、断电保存和发布验收是不同证据，不互相替代。未知电量或参数不以默认数值补全。
