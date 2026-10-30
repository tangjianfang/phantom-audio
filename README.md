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
# 构建（Visual Studio 2022 的 C++ 工具集 + CMake + Ninja；scripts\msvc-env.cmd 用 vswhere 定位 VS，本机与 CI 同一入口）
cd tools/uac-simulator
./scripts/msvc-env.cmd cmake --preset win-x64-debug
./scripts/msvc-env.cmd cmake --build --preset win-x64-debug
./scripts/msvc-env.cmd ctest --preset win-x64-debug     # 172 用例 / 1468 断言（无需驱动）
powershell -File scripts/smoke.ps1                      # GUI 非视觉自检（不判断外观）

# 日常使用：仿真器服务 + GUI 控制台（开发机上只到这里：不装驱动、不挂设备）
build/debug/uacsimd.exe
build/debug/uacstudio.exe
```

**需要真实 Windows 音频栈的验证（装驱动、`usbip attach`、WASAPI、`usbaudio2.sys` 绑定）一律在
Windows Sandbox / VM 里做，开发机上不做。** 最省事的做法：

```
build.bat        # 构建 Release，成功后自动打包 -> tools\uac-simulator\dist\uacsim-sandbox-release.zip
```

把这个 zip 拷进 Sandbox，解压，双击 `run.cmd`。它自包含（exe、profiles、脚本；本机 `third_party\usbip-win2\` 里有驱动安装包的话也一并打入），
带 `SHA256SUMS.txt` 校验拷贝是否损坏。也可以用 `tools/uac-simulator/sandbox/launch-sandbox.ps1` 一条命令启动 Sandbox（目录映射方式）。
详见 [docs/sandbox.md](./docs/sandbox.md)。

详见 `tools/uac-simulator/README.md`、`docs/runbook.md`、`docs/sandbox.md`；给 Claude Code 会话的约定见 [CLAUDE.md](./CLAUDE.md)。

## 许可

- **个人 / 研究 / 教育 / 非商业组织**：依据 [PolyForm Noncommercial 1.0.0](./LICENSE) 免费使用、修改与分发。
- **商业用途**：须事先取得商业授权——请提 GitHub Issue 联系作者。

usbip-win2（若安装）是独立的第三方项目，许可为 **BSD-2-Clause**（上游仓库 `LICENSE.txt`；v.0.9.7.8 与 v.0.9.8.1 标签均核对过），
与本仓库代码无链接关系（本项目只通过 TCP 说 USB/IP 协议）。随包分发它的二进制时需附带其 `LICENSE.txt`——`build.bat` 生成的 zip 会自动带上。
（早期 RFC/设计文档里写的 "GPLv3" 有误，见 [docs/sandbox.md](./docs/sandbox.md) 的"许可（更正）"。）
