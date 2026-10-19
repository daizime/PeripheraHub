# 通用显示器识别与内置功能

## 识别和选择

程序通过 Windows DisplayConfig 读取当前活动显示器的 EDID 友好名称，通过显示器接口路径保存控制目标，不按品牌名称寻找写入对象。单屏自动选中；多屏保留用户明确选择，不自动选择列表第一项。系统没有提供真实型号时，名称也可能只有通用描述，不能据此声称已识别具体型号。

收到显示拓扑或设备变化通知后延迟 750 ms 合并刷新；显示器轮询作为后备。选中目标后自动读取能力表，展开详情时也可重新读取。更换目标会取消未开始的旧查询，迟到的旧结果不能覆盖新目标；每次写入重新打开当前物理句柄并复核身份与拓扑。程序不能替显示器开放固件没有暴露的 OSD 功能。

## 当前开放的控制

| 功能 | VCP | 条件 |
|---|---|---|
| 亮度 | 0x10 | 沿用已验证的亮度范围和实时滑块 |
| 对比度 | 0x12 | 能力表声明，当前值与最大值读取有效；输入显示原始范围 |
| 输入源 | 0x60 | 能力表有明确枚举、当前值可读；选择后点击切换并确认 |
| 情景 / 显示模式 | 0xDC | 能力表有明确枚举、当前值可读；名称未核定时显示设备模式代码 |
| 色彩预设 | 0x14 | 与情景模式分开；只显示设备声明的预设代码 |

`advertised` 表示设备自述，`readable` 表示本次读取成功，`writable` 仅表示可尝试白名单范围内的用户请求，不等于写入已经验收。非连续控制的 maximum 不能用于生成下拉选项。MCCS 3.x 输入源表控制暂不支持。没有列入白名单的电源、复位、厂商私有码不会写入。

点击输入源下拉不会发送命令，必须点击“切换”并确认；屏幕可能无信号，需要使用显示器按键切回。请求只发送一次，不自动重写或切回。写后单项回读区分“回读确认”和“请求已发出但未确认”，不能把驱动 API 返回成功当作模式已生效。

上述色彩预设是显示器声明的预设代码，不等于独立的饱和度控制。当前白名单没有饱和度写入，不将其他 VCP 值解释为饱和度。

## 诊断日志

高级写入会以 info 级别写入 `peripheral-hub.log`（行内类型 `4`）：

```
monitor feature sent <接口ID> code <VCP> value <值> queue_ms <N> validation_ms <N> write_ms <N> capabilities_cached <bool>
monitor feature readback ...
monitor feature failed <接口ID> code <VCP> queue_ms <N> validation_ms -1 write_ms -1 attempt_ms <N>
```

亮度/音量的连续写入保持 debug 级别，不会因拖动滑块刷满日志。H24T7 花屏排查的证据要求见 `work/verification/ddc-flicker-20261019/README.md`：关键判据是切换后是否还有 `readback`。若只有 `sent` 而没有 `readback`，说明新输入源上 DDC 不可达，自动回切的前提不成立，输入源应继续只读。

## 响应时间与操作反馈
DDC/CI 使用同步驱动调用，程序将它们放在同一个工作线程串行执行。[微软 SetVCP 文档](https://learn.microsoft.com/en-us/windows/win32/api/lowlevelmonitorconfigurationapi/nf-lowlevelmonitorconfigurationapi-setvcpfeature)给出单次设置约 50 ms 的参考时间，[GetVCP 文档](https://learn.microsoft.com/en-us/windows/win32/api/lowlevelmonitorconfigurationapi/nf-lowlevelmonitorconfigurationapi-getvcpfeatureandvcpfeaturereply)给出单次读取约 40 ms；它们不是本机性能保证，也不是画面变化时间。

此前高级设置每次写前读取全部四项，写后在工作线程等待 250 ms，再重读全部能力和四项值。能力字符串可能需要多次传输，完整确认时间与实际发出设置的时间应分别记录。性能验收只会读取已声明功能，或将当前有效对比度原值写回；没有对未知预设和输入源自动试写。

当前服务复用本连接成功读取的能力声明，使用单调时钟限制为 5 分钟，并在真正开始工作时再次检查有效性。目标改变、强制刷新（含设备/显示变化通知）、预设/输入源写入和失败会清除缓存；无有效声明则重新检测。每次写入仍重开物理句柄，核对唯一身份和拓扑，重新解析声明，实际读取目标当前值与范围，最后只写一次。原生默认设置 API 保持完整能力验证；快速 API 只允许服务提供本连接的临时声明，不接受档案中的历史能力。

写入返回后立即显示发送反馈。对比度不再固定等待 250 ms；预设/输入源的 250 ms 确认等待改用 Qt 定时器。随后只回读本项；首次回读失败或值未匹配时，再延后 250 ms 补读一次，仍只发一次设置。然后后台补读其余高级状态；状态文案保留确认结果，并说明后台补读。当前驱动调用无法中途取消，物理显示器的视觉响应也不能由 API 成功推断。若其他软件或实体 OSD 改变能力声明而系统没有通知，缓存可能在下次完整查询前仍是旧声明；提供手动“读取显示器功能”重新检测。

2026-10-18 21:28:42（Asia/Shanghai），退出程序以避免并发 DDC 访问后，对唯一 H24T7 连续做三组只读计时：完整能力及四项值查询分别为 1246、1248、1250 ms；单项原始值回读为 62–63 ms。原始结果在 `work/verification/monitor-latency/timings.log`，源码为同目录 `read-only.cpp`。单项接口不返回能力授权（`advertised/writable=false`），只供既有请求验证；这些数字不代表用户点按到画面变化的实测延迟。

21:54:14，最终真实界面将对比度原值 50 写回 50：排队 1 ms、身份/范围校验 59 ms、Set 61 ms，复用当前连接声明。第一次 62 ms 回读返回 Windows 错误 3223725449；定时等待后，第二次 58 ms 回读为 50，总计 495 ms 确认成功，未重复写入。设置约 0.12 秒发出，约 0.50 秒确认；画面参数没有改变，因此没有量测“新画面生效”的视觉延迟。原始证据 `work/verification/scroll-latency-native/final.log`。

## H24T7 实机只读记录

2026-10-18 19:17:57–19:17:58（UTC+08），当前 Type-C 转 DP 连接：

| 功能 | 能力表枚举 | 当前原始值 | 结论 |
|---|---|---|---|
| 亮度 | 连续 | 47 / 100 | 本次可读；既有改变/恢复验证见 VALIDATION.md |
| 对比度 | 连续 | 50 / 100 | 本次可读，未实机改动 |
| 色彩预设 | 05、08、0B、0C | 01 | 当前值不在设备自述列表中，名称与写入未核定 |
| 输入源 | 11、12、0F | 00 | 当前值不在列表中，不能把 00 解释为 DP 或 HDMI |
| 情景模式 | 00、02、03、05 | 01 | 当前值不在列表中，不能猜成“电影/FPS”等菜单项 |

设备报告 MCCS 2.1。19:22:38–19:22:39 通过新增后端重复只读查询，结果一致。原始证据在 `work/verification/ddc-capabilities/RESULT.md`、`result-20261018/probe.log` 和 `backend-query-20261018.log`。这些是当次读数；本轮没有实际切换输入源、模式、色彩预设或对比度，也没有接入第二台真实显示器。

能力表与读取值不一致时，界面保留原始值并提示，不自动按下标解释。输入源选项使用标准接口名并附协议代码；情景与色彩名称需对照实体 OSD 或厂商协议进一步核定。能力表可能随固件或连接变化，不能将本次结果推广到其他型号。

## 实现入口与证据

- `src/core/ddc_capabilities.*`：独立实现的有界能力字符串解析器；无效数据拒绝生成枚举。
- `src/platform/WindowsMonitor.cpp`：原生 Dxva2 Get/Set VCP、白名单和身份复核。
- `src/app/DeviceService.*`：能力声明缓存、异步读取、目标取消、一次设置与单项回读。
- `src/app/widgets/MonitorControls.*`：能力驱动的编辑界面，无硬件调用。

[KTC H24T7 官方支持页](https://us.ktcplay.com/pages/h24t7-gaming-monitor-support)确认 OSD 有 HDMI 1、HDMI 2、DP、画面预设、亮度与对比度，但未提供这些菜单项对应的型号专用 VCP 表。

[微软低层显示器配置流程](https://learn.microsoft.com/en-us/windows/win32/monitor/using-the-low-level-monitor-configuration-functions)说明能力查询与 VCP 操作方式；[GetVCP 文档](https://learn.microsoft.com/en-us/windows/win32/api/lowlevelmonitorconfigurationapi/nf-lowlevelmonitorconfigurationapi-getvcpfeatureandvcpfeaturereply)明确非连续功能的最大值未定义；[SetVCP 文档](https://learn.microsoft.com/en-us/windows/win32/api/lowlevelmonitorconfigurationapi/nf-lowlevelmonitorconfigurationapi-setvcpfeature)要求实际验证显示器实现，并不支持表控制。

[DDCutil FAQ](https://www.ddcutil.com/faq/)解释能力字符串可能错漏；[SetVCP 说明](https://www.ddcutil.com/command_setvcp/)解释驱动发送成功与显示器实际执行的区别。仅参考协议资料，未复制该 GPL 项目代码，没有增加第三方库或运行 DLL。
