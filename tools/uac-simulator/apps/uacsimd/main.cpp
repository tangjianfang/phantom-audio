// uacsimd —— UAC 设备仿真器服务
// 用法：uacsimd.exe [--usbip-port 3240] [--http-port 3241] [--profiles <dir>]
//                  [--profile <name|path>（启动即 attach）] [--log-file <path>]
#include "sim_host.h"
#include <uac/version.h>
#include <csignal>
#include <cstdio>

static std::atomic<bool> g_run{true};
static void on_sigint(int) { g_run.store(false); }

int main(int argc, char** argv) {
    std::uint16_t usbipPort = 3240, httpPort = 3241;
    std::string profilesDir = "profiles", logFile, initialProfile;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--usbip-port") usbipPort = static_cast<std::uint16_t>(atoi(next().c_str()));
        else if (a == "--http-port") httpPort = static_cast<std::uint16_t>(atoi(next().c_str()));
        else if (a == "--profiles") profilesDir = next();
        else if (a == "--profile") initialProfile = next();
        else if (a == "--log-file") logFile = next();
    }

    uac::SimHost host(profilesDir);
    if (!host.start(usbipPort, httpPort)) {
        fprintf(stderr, "uacsimd: control api listen %u failed\n", httpPort);
        return 1;
    }
    if (!logFile.empty() && host.model()) host.model()->log().set_file_sink(logFile);
    if (!initialProfile.empty()) {
        const auto r = host.on_attach(initialProfile);
        printf("attach(%s) -> %s\n", initialProfile.c_str(), r.c_str());
    }
    std::signal(SIGINT, on_sigint);
    printf("uacsimd %s | usbip 127.0.0.1:%u | control 127.0.0.1:%u | profiles: %s\n",
           uac::kVersion, usbipPort, httpPort, profilesDir.c_str());
    printf("Ctrl+C 停止；uacsim.exe 是命令行客户端\n");
    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    host.stop();
    printf("uacsimd: stopped\n");
    return 0;
}
