// Starting and stopping the uacsimd process from the GUI, so that nothing is left running by accident.
//
// Rules, in the order they matter:
//  - Start passes --parent-pid <this process>: if the GUI crashes or is killed, uacsimd notices and exits by itself.
//  - Stop is polite first: POST /shutdown (the simulator stops its services and exits). Only a process that THIS program
//    started is ever terminated, and only when it ignored the request; a simulator somebody else started is never killed -
//    if it does not stop, that is reported.
//  - Nothing here blocks the UI thread by design: the GUI calls it from its worker thread.
// Every system action is an injectable seam; the tests drive the whole logic with a fake process and then once with the real
// uacsimd.exe.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace uacstudio {

struct SimStartOptions {
    std::string exeDir;                 // folder that contains uacsimd.exe
    std::uint16_t httpPort = 3241;      // control API
    std::uint16_t usbipPort = 3240;
    std::string profilesDir;            // empty = uacsimd finds its own
    unsigned parentPid = 0;             // 0 = this process
};

struct SimResult {
    bool ok = false;
    std::string detail;
};

struct SimProcessDeps {
    // HTTP to the control API: response body, "" when it cannot be reached. Default: http_req.
    std::function<std::string(std::uint16_t port, const std::string& method, const std::string& path)> http;
    // Start `exe args` without a window; returns an opaque process handle, or nullptr and an error text.
    std::function<void*(const std::string& exe, const std::string& args, std::string* error)> spawn;
    std::function<bool(void* process, int waitMs)> waitExit; // true = the process has ended (within waitMs)
    std::function<void(void* process)> terminate;
    std::function<void(void* process)> closeHandle;
    std::function<bool(const std::string& path)> fileExists;
    std::function<void(int ms)> sleep;
};

class SimProcess {
public:
    explicit SimProcess(SimProcessDeps deps = {});
    ~SimProcess(); // does not kill the simulator: --parent-pid ends it when this process ends

    // Already answering on the port -> ok, nothing started. Waits (polling) up to readyMs for the new process to answer.
    SimResult Start(const SimStartOptions& opt, int readyMs = 6000);
    // Asks the simulator on `httpPort` to shut down and waits up to gracefulMs. Escalates to TerminateProcess only for the
    // process this object started.
    SimResult Stop(std::uint16_t httpPort, int gracefulMs = 4000);
    // This object started a simulator and it has not exited yet.
    bool Owns();

private:
    SimProcessDeps d_;
    std::mutex mtx_;
    void* owned_ = nullptr;
    void ForgetOwned(); // closes the handle; call with mtx_ held
};

// The simulator the GUI manages (one per program).
SimProcess& TheSimulator();

} // namespace uacstudio
