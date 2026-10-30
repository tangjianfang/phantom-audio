// See sim_process.h.
#include "http_client.h" // winsock2 must come before windows.h
#include "sim_process.h"
#include <windows.h>
#include <filesystem>

namespace uacstudio {

namespace {

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

SimProcessDeps WithDefaults(SimProcessDeps d) {
    if (!d.http)
        d.http = [](std::uint16_t port, const std::string& method, const std::string& path) {
            return http_req(port, method.c_str(), path, "");
        };
    if (!d.spawn)
        d.spawn = [](const std::string& exe, const std::string& args, std::string* error) -> void* {
            std::wstring cmd = L"\"" + ToWide(exe) + L"\" " + ToWide(args);
            const std::wstring dir = std::filesystem::path(ToWide(exe)).parent_path().wstring();
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            // No window: the simulator is a background service of the GUI. Its working directory is its own folder so that
            // the default "profiles" lookup finds the profiles next to it.
            if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
                if (error) *error = "CreateProcess failed, error " + std::to_string(GetLastError());
                return nullptr;
            }
            CloseHandle(pi.hThread);
            return pi.hProcess;
        };
    if (!d.waitExit)
        d.waitExit = [](void* p, int ms) { return WaitForSingleObject(static_cast<HANDLE>(p), static_cast<DWORD>(ms)) == WAIT_OBJECT_0; };
    if (!d.terminate) d.terminate = [](void* p) { TerminateProcess(static_cast<HANDLE>(p), 1); };
    if (!d.closeHandle) d.closeHandle = [](void* p) { CloseHandle(static_cast<HANDLE>(p)); };
    if (!d.fileExists)
        d.fileExists = [](const std::string& path) {
            std::error_code ec;
            return std::filesystem::is_regular_file(std::filesystem::path(ToWide(path)), ec);
        };
    if (!d.sleep) d.sleep = [](int ms) { Sleep(static_cast<DWORD>(ms)); };
    return d;
}

constexpr int kPollMs = 100;

} // namespace

SimProcess::SimProcess(SimProcessDeps deps) : d_(WithDefaults(std::move(deps))) {}
SimProcess::~SimProcess() {
    std::lock_guard lk(mtx_);
    ForgetOwned(); // only the handle; the process ends through --parent-pid
}

void SimProcess::ForgetOwned() {
    if (owned_) d_.closeHandle(owned_);
    owned_ = nullptr;
}

bool SimProcess::Owns() {
    std::lock_guard lk(mtx_);
    return owned_ != nullptr && !d_.waitExit(owned_, 0);
}

SimResult SimProcess::Start(const SimStartOptions& opt, int readyMs) {
    const std::string where = "127.0.0.1:" + std::to_string(opt.httpPort);
    if (!d_.http(opt.httpPort, "GET", "/status").empty()) return {true, "a simulator is already answering on " + where + " - nothing started"};

    const std::string exe = opt.exeDir + "\\uacsimd.exe";
    if (!d_.fileExists(exe)) return {false, "uacsimd.exe was not found next to uacstudio.exe (looked for " + exe + ")"};

    std::string args = "--http-port " + std::to_string(opt.httpPort) + " --usbip-port " + std::to_string(opt.usbipPort);
    if (!opt.profilesDir.empty()) args += " --profiles \"" + opt.profilesDir + "\"";
    args += " --parent-pid " + std::to_string(opt.parentPid != 0 ? opt.parentPid : GetCurrentProcessId());

    std::string error;
    void* h = d_.spawn(exe, args, &error);
    if (!h) return {false, "could not start uacsimd.exe: " + error};

    const int attempts = readyMs / kPollMs + 1;
    for (int i = 0; i < attempts; ++i) {
        if (!d_.http(opt.httpPort, "GET", "/status").empty()) {
            std::lock_guard lk(mtx_);
            ForgetOwned(); // an earlier (finished) one
            owned_ = h;
            return {true, "started uacsimd.exe, answering on " + where};
        }
        if (d_.waitExit(h, 0)) {
            d_.closeHandle(h);
            return {false, "uacsimd.exe exited right after it was started - another simulator probably uses ports " +
                               std::to_string(opt.httpPort) + " / " + std::to_string(opt.usbipPort)};
        }
        d_.sleep(kPollMs);
    }
    d_.terminate(h); // ours, and it never became usable
    d_.waitExit(h, 2000);
    d_.closeHandle(h);
    return {false, "uacsimd.exe did not answer on " + where + " within " + std::to_string(readyMs / 1000) + " s; it was stopped again"};
}

SimResult SimProcess::Stop(std::uint16_t httpPort, int gracefulMs) {
    const std::string where = "127.0.0.1:" + std::to_string(httpPort);
    void* h = nullptr;
    {
        std::lock_guard lk(mtx_);
        h = owned_;
    }
    const bool answered = !d_.http(httpPort, "POST", "/shutdown").empty();
    if (!answered) {
        // Not answering: either it is gone, or - if it is ours - it hangs.
        std::lock_guard lk(mtx_);
        if (owned_ && !d_.waitExit(owned_, 0)) {
            d_.terminate(owned_);
            d_.waitExit(owned_, 2000);
            ForgetOwned();
            return {true, "the simulator did not answer on " + where + ", so the process this program started was terminated"};
        }
        ForgetOwned();
        return {true, "no simulator is running on " + where};
    }

    // Accepted: wait for it to go away (the process, if we own it; otherwise until the port stops answering)
    bool gone = false;
    if (h) {
        gone = d_.waitExit(h, gracefulMs);
    } else {
        for (int i = 0; i <= gracefulMs / kPollMs && !gone; ++i) {
            gone = d_.http(httpPort, "GET", "/status").empty();
            if (!gone) d_.sleep(kPollMs);
        }
    }
    if (gone) {
        std::lock_guard lk(mtx_);
        ForgetOwned();
        return {true, "the simulator stopped"};
    }
    if (h) {
        std::lock_guard lk(mtx_);
        d_.terminate(h);
        d_.waitExit(h, 2000);
        ForgetOwned();
        return {true, "the simulator did not stop within " + std::to_string(gracefulMs / 1000) + " s and was terminated"};
    }
    // Somebody else's process: it was asked politely and that is as far as this program goes.
    return {false, "the simulator on " + where + " accepted the shutdown request but is still running; it was not started by "
                   "this program, so it was not killed"};
}

SimProcess& TheSimulator() {
    static SimProcess sim;
    return sim;
}

} // namespace uacstudio
