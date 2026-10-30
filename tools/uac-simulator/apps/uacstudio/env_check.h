// 环境自检：首次启动展示本机状态与缺失依赖的修复引导（官网/命令/重启步骤）
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace uacstudio {

struct EnvItem {
    std::wstring name;     // 项目名
    int state = 2;         // 2=就绪 1=可选/警告 0=缺失
    std::wstring detail;   // 状态说明
    std::wstring fixNote;  // 修复步骤说明（含重启提示）
    std::wstring fixCmd;   // 可复制的安装命令（可空）
    const wchar_t* url = nullptr; // 官方下载页（可空）
    // 精简载荷已就位、通过校验，且本机是一次性机器（见 driver_installer.h）。只是这一行的说明；
    // 窗口底部各动作按钮能否点击由 host_ops.h 的 PlanHostActions 决定。
    bool canInstallDriver = false;
};

// 每开始一项检查就调用一次，参数是"正在做什么"（英文，给用户看）。可能在后台线程里调用。
using EnvProgress = std::function<void(const std::wstring&)>;

// 执行全部检查（port = uacsimd 控制端口）。会阻塞（网络、注册表、对载荷做 SHA-256），
// 所以窗口里绝不在 UI 线程上调用它——见 env_check.cpp 里的后台检查线程。
std::vector<EnvItem> RunEnvChecks(std::uint16_t port, const EnvProgress& progress = {});

// 弹出环境检查窗口（模态）；返回前允许"重新检测"
void ShowEnvCheck(HWND owner, std::uint16_t port);

// 首次启动判定（HKCU\Software\PhantomAudio\uacstudio）；mark=true 写入已检标记
bool IsFirstRun();
void MarkFirstRunDone();

} // namespace uacstudio
