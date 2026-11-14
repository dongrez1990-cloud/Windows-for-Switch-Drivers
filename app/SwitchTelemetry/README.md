# Switch 硬件监控

原生 Windows ARM64、无 .NET 依赖；打开窗口查看 CPU/GPU/RAM 频率、CPU/GPU 电压、
SoC 温度及风扇 PWM。后台线程每秒刷新，查询只读，不改变电源档位、显示或风扇设置。

数据优先使用 SwitchFan Power Status 接口，兼容现有驱动的 Parameters 读回记录。
当前已安装的旧版 KMD 没有持续 GPU 遥测，回退值标注为“启动时硬件计数器 · 非实时”。
RAM 显示固件交接的实际 EMC 频率；缺少有效交接时显示“未提供”。风扇显示 PWM%，
现有驱动没有 RPM 转速反馈，不能将占空比冒充 RPM。过期驱动缓存会明确标出。

构建：`sh app/SwitchTelemetry/build.sh <output>`。
输出 `SwitchTelemetry-aarch64.exe` 为实机程序，x86_64 仅用于本地控制验证。
`--snapshot` 输出只读 JSON，完全没有数据时返回3；有数据只说明读到了相应字段，
不证明所有字段实时、GPU稳定或新DVFS已经生效。

本工具独立于正在开发的完整 CPU DFLL / Switchroot 七档及 OC，不作为其验收证据。
