// uacsimd —— UAC 设备仿真器服务
// 用法：uacsimd.exe [--usbip-port 3240] [--http-port 3241] [--profiles <dir>]
//                  [--profile <name|path>（启动即 attach）] [--log-file <path>] [--parent-pid <pid>]
//
// 退出（都会先正常停掉服务再退出，不留下占着端口的进程）：
//   - Ctrl+C / Ctrl+Break / 关闭控制台窗口 / 注销 / 关机
//   - 控制 API 的 POST /shutdown（GUI 的 Stop simulator、Clean up all 用它）
//   - --parent-pid：指定的进程一结束就退出。GUI 启动它时传自己的 pid，GUI 崩溃或被强杀也不会留下孤儿进程。
#include "sim_host.h"
#include <uac/version.h>
#include <windows.h>
#include <cstdio>

static std::atomic<bool> g_run{true};

static BOOL WINAPI on_console_event(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        g_run.store(false);
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        // Windows ends the process as soon as this handler returns (after ~5 s at the latest): ask the main loop to stop the
        // services and give it time to do so.
        g_run.store(false);
        Sleep(4000);
        return TRUE;
    default:
        return FALSE;
    }
}

int main(int argc, char** argv) {
    std::uint16_t usbipPort = 3240, httpPort = 3241;
    std::string profilesDir = "profiles", logFile, initialProfile;
    DWORD parentPid = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--usbip-port") usbipPort = static_cast<std::uint16_t>(atoi(next().c_str()));
        else if (a == "--http-port") httpPort = static_cast<std::uint16_t>(atoi(next().c_str()));
        else if (a == "--profiles") profilesDir = next();
        else if (a == "--profile") initialProfile = next();
        else if (a == "--log-file") logFile = next();
        else if (a == "--parent-pid") parentPid = static_cast<DWORD>(strtoul(next().c_str(), nullptr, 10));
    }

    // The handle is opened before anything is started: a parent that is already gone means there is nobody to serve
    HANDLE parent = nullptr;
    if (parentPid != 0) {
        parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
        if (!parent) {
            fprintf(stderr, "uacsimd: --parent-pid %lu is not a running process - exiting\n", static_cast<unsigned long>(parentPid));
            return 3;
        }
    }

    uac::SimHost host(profilesDir);
    if (!host.start(usbipPort, httpPort)) {
        fprintf(stderr, "uacsimd: control api listen %u failed\n", httpPort);
        if (parent) CloseHandle(parent);
        return 1;
    }
    host.set_log_file(logFile); // applied to every model created at attach time
    if (!initialProfile.empty()) {
        const auto r = host.on_attach(initialProfile, /*confineName=*/false); // --profile accepts paths
        printf("attach(%s) -> %s\n", initialProfile.c_str(), r.c_str());
    }
    SetConsoleCtrlHandler(on_console_event, TRUE);
    printf("uacsimd %s | usbip 127.0.0.1:%u | control 127.0.0.1:%u | profiles: %s\n",
           uac::kVersion, usbipPort, httpPort, profilesDir.c_str());
    printf("Press Ctrl+C to stop, or POST /shutdown; uacsim.exe is the command-line client\n");
    fflush(stdout);

    const char* why = "stopped";
    while (g_run.load()) {
        if (host.shutdown_requested()) {
            why = "stopped (shutdown requested over the control API)";
            break;
        }
        if (parent && WaitForSingleObject(parent, 0) == WAIT_OBJECT_0) {
            why = "stopped (the parent process exited)";
            break;
        }
        Sleep(200);
    }
    host.stop();
    if (parent) CloseHandle(parent);
    printf("uacsimd: %s\n", why);
    return 0;
}
