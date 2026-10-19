# 0.0 验证说明

本项目将自动测试、驱动读回、实际物理效果和发布验收分开记录。当前版本是 0.0 初始预览版；本轮发布检查不执行实际硬件参数写入。

## 自动回归

根目录 `CMakeLists.txt` 与 `tests/qt/CMakeLists.txt` 定义维护中的核心、档案、服务和 Qt 组件测试。构建命令和要求见 [README.md](../README.md)。Qt 测试使用模拟后端或纯组件 fixture，截图中的设备和参数不作为真实兼容性证据。

```powershell
.\scripts\verify-all.ps1 -BuildDirectory "$PWD\work\build-release"
```

原生库还可独立于 Qt 构建。CI 验证原生构建与核心测试；完整 Qt 回归在本机运行。本轮测试数量和退出状态以 [RELEASE_0.0.md](RELEASE_0.0.md) 的实际记录为准。

## 已有实机记录

- H24T7：亮度曾完成小幅改变并恢复；输入源和模式的当前值与能力枚举矛盾。曾报告软件切源后花屏，当前不判为已适配切源。
- VGN F2 Master+：限定 2.4G 接口已读取电量、DPI/档位、回报率、按键和性能字段；三项参数有探针写入/回读/改回记录，物理效果和断电保存仍待验收。
- N9 V2 ULTRA：限定 2.4G 接口已读取电量、状态和 PEQ。开发探针曾改动 Custom 曲线一点而未恢复原值；产品 EQ 写入未开放。
- L98：只有系统接口发现，不宣称电量、RGB 或改键完成。

协议、方法和边界见 [VGN_F2_PROTOCOL.md](VGN_F2_PROTOCOL.md)、[N9_V2_PROTOCOL.md](N9_V2_PROTOCOL.md)、[PRIVATE_WRITE_CANDIDATES_2026-10-19.md](PRIVATE_WRITE_CANDIDATES_2026-10-19.md)。记录中的读数属于当次采样，不代表当前状态或所有同型号固件。

## 尚待验收

真实多屏、重复拔插、充电及有线/无线切换、休眠唤醒、持续运行和第二台无开发环境 Windows 11 电脑。完整步骤见 [ACCEPTANCE_CHECKLIST.md](ACCEPTANCE_CHECKLIST.md)。输入源风险、未确认协议和实验性写入应单独留证，不以 API 返回成功判定物理效果。

历史诊断日志和截图保存在开发机的忽略目录，不作为源码发布附件。公开仓库保留必要测试源码和去身份样本；需要复验时按上述脚本生成自己的记录。