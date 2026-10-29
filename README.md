# phantom-audio

> Windows 纯软件虚拟 USB 音频设备（UAC1/UAC2）仿真测试台 —— 无需真实声卡/麦克风，即可对主机侧音频软件做全自动化测试。

[![ci](https://github.com/tangjianfang/phantom-audio/actions/workflows/ci.yml/badge.svg)](./.github/workflows/ci.yml)

## 它是什么

一台"插在虚拟 USB 主控上的可编程声卡"：

- 被测软件经 WASAPI 看到**真 USB 声卡**（Windows 内置 usbaudio.sys / usbaudio2.sys 类驱动绑定，非虚拟声卡驱动）
- **任意 VID/PID 与描述符**（Profile 热切换，模拟多机型；可从真机 dump 克隆）
- **设备侧硬件事件**：音量 +/-/静音按键（UAC2 Status Word 中断 + HID 回退双通道）
- **故障注入**：STALL / 延迟 / 描述符损坏 / 半枚举 / 拔线 / iso 错误
- **双向断言**：设备侧全量请求日志（NDJSON）× 主机侧 host-probe（枚举/音量回调/Goertzel 采集校验）
- 三客户端控制台：**CLI（uacsim）/ Win32 GUI（uacstudio）/ CI**，共享本地 HTTP API

虚拟主控复用已签名的开源 [usbip-win2](https://github.com/vadimgrn/usbip-win2)（UDE 框架），**零内核开发**；设备模型全部在用户态。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `rfc-windows-virtual-uac-device.md` | 方案 RFC（v2，含网络核实依据） |
| `tools/uac-simulator/` | 全部源码（C++20）、Profile、脚本、测试 |
| `docs/` | 设计文档 / 实施计划 / 运行手册 / 验收报告 |
| `LICENSE` | **PolyForm Noncommercial 1.0.0**（个人与非商业免费；商业需授权） |

## 快速开始

```powershell
# 构建（bash / VS2022 + CMake + Ninja）
cd tools/uac-simulator
./scripts/msvc-env.cmd cmake --preset win-x64-debug
./scripts/msvc-env.cmd cmake --build --preset win-x64-debug
./scripts/msvc-env.cmd ctest --preset win-x64-debug     # 121 用例全绿（无需驱动）

# 实机仿真（需先安装 usbip-win2 v0.9.7.8，见 docs/runbook.md）
build/debug/uacsimd.exe        # 仿真器服务
usbip attach -r 127.0.0.1 -b 1-1
build/debug/uacstudio.exe      # GUI 控制台
```

详见 `tools/uac-simulator/README.md` 与 `docs/runbook.md`。

## 许可

- **个人 / 研究 / 教育 / 非商业组织**：依据 [PolyForm Noncommercial 1.0.0](./LICENSE) 免费使用、修改与分发。
- **商业用途**：须事先取得商业授权——请提 GitHub Issue 联系作者。

usbip-win2（若安装）为独立 GPLv3 驱动，与本仓库代码无链接关系，其许可以上游为准。
