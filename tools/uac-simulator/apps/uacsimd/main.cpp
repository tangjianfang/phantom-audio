// uacsimd —— UAC 设备仿真器服务（M1 版：内置 uac1-cmedia-sim preset）
// 用法：uacsimd.exe [--port 3240] [--log-file <path>] [--preset uac1-cmedia-sim]
#include <uac/device_model.h>
#include <uac/usbip_server.h>
#include <uac/version.h>
#include <atomic>
#include <thread>
#include <csignal>
#include <cstdio>
#include <string>

static std::atomic<bool> g_run{true};
static void on_sigint(int) { g_run.store(false); }

int main(int argc, char** argv) {
    std::uint16_t port = 3240;
    std::string logFile, preset = "uac1-cmedia-sim";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) port = static_cast<std::uint16_t>(atoi(argv[++i]));
        else if (a == "--log-file" && i + 1 < argc) logFile = argv[++i];
        else if (a == "--preset" && i + 1 < argc) preset = argv[++i];
    }

    auto model = std::make_unique<uac::DeviceModel>(uac::make_uac1_default_snapshot());
    if (!logFile.empty()) model->log().set_file_sink(logFile);
    model->engine().set_source_sine(1000.0, 0.5); // IN 默认 1kHz 正弦

    uac::UsbIpServer srv;
    if (!srv.start(port, *model, {model->usbip_device()})) {
        fprintf(stderr, "uacsimd: cannot listen on 127.0.0.1:%u\n", port);
        return 1;
    }
    std::signal(SIGINT, on_sigint);
    printf("uacsimd %s listening 127.0.0.1:%u (busid %s, vid=%04x pid=%04x)\n", uac::kVersion,
           srv.port(), model->usbip_device().busid.c_str(), model->usbip_device().idVendor,
           model->usbip_device().idProduct);
    printf("press Ctrl+C to stop\n");
    while (g_run.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    srv.stop();
    printf("uacsimd: stopped\n");
    return 0;
}
