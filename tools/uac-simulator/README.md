# UAC Simulator —— Windows 纯软件虚拟 USB 音频设备测试台

单机仿真指定 VID/PID 的 UAC1/UAC2 USB 音频设备：被 Windows 内置类驱动（usbaudio.sys / usbaudio2.sys）按真设备绑定，用于主机侧音频软件的自动化测试。方案与风险详见根目录 `rfc-windows-virtual-uac-device.md`（v2）。

## 架构

```
被测软件 ─WASAPI→ Windows 音频栈+类驱动 ─URB→ usbip-win2(UDE,已签名)
                                                    │ USB/IP v1.1.1 (127.0.0.1:3240)
                                              uacsimd.exe（本目录）
   uacsim CLI / uacstudio GUI / CI ──HTTP JSON:3241──┘
   host_probe.exe（主机侧断言：枚举/音量回调/Goertzel 采集校验）
```

- **协议层**：USB/IP 全大端（内核文档黄金字节对齐）；URB 通道独立线程，多设备并发
- **描述符引擎**：UAC1/UAC2 模板 + UDE 适配（High-Speed + iso bInterval≥4，issue #35 法则）+ usbaudio2.sys 约束 lint
- **流引擎**：虚拟微帧时钟 pacing（RET_SUBMIT 按包数延迟，防音频快进）+ 正弦/文件/静音源 + WAV 录音 + 显式反馈端点
- **R6 事件**：UAC2 AC 中断 Status Word（官方支持）+ HID Consumer Volume 回退通道
- **故障注入**：STALL / 延迟 / 描述符损坏 / 半枚举 / 拔线 / iso error

## 快速开始

```powershell
# 构建（bash）
./scripts/msvc-env.cmd cmake --preset win-x64-debug
./scripts/msvc-env.cmd cmake --build --preset win-x64-debug

# 单测 + 回环场景（121 用例，无需驱动）
./scripts/msvc-env.cmd ctest --preset win-x64-debug

# 覆盖率门禁（需 OpenCppCoverage：winget install OpenCppCoverage.OpenCppCoverage）
./scripts/msvc-env.cmd cmake --preset win-x64-coverage
./scripts/msvc-env.cmd cmake --build --preset win-x64-coverage
powershell -File scripts/coverage.ps1          # 95.12% PASS（阈值 95）

# 实机（需先装 usbip-win2 v0.9.7.8，见 scripts/elevated-install-driver.ps1）
uacsimd.exe &                                  # 仿真器服务（3240/3241）
usbip attach -r 127.0.0.1 -b 1-1               # 出现虚拟声卡
uacsim.exe status / event volume_up / inject '{"kind":"stall"}'
host_probe.exe enum / capture --freq 1000 --loopback --seconds 3
uacstudio.exe                                  # Win32 GUI 控制台
```

## Profile

`profiles/*.json`：`protocol/usb/audio/hid/rawConfigOverride`。`attach <name>` 热切换（模拟换设备）；多实例上限 4（busid 1-1..1-4）。

## 测试分层

| 层 | 内容 | 门槛 |
|---|---|---|
| L1 | doctest 单测（协议/描述符/引擎/注入） | ctest 全绿（121 用例/782 断言） |
| L2 | 回环场景（fake-vhci 全握手/流/事件/注入剧本） | 同上 |
| L3 | 实机（驱动装好后 `-WithDriver`） | regression.ps1 门控 |

覆盖率：OpenCppCoverage 95.12%（lambda/子线程归因限制以 `uac-cov:thread` 标记，见 scripts/coverage.ps1 注释）。
