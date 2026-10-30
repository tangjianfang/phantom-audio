// Operations that touch the Windows side of the simulator, and the rules for which GUI buttons may be pressed.
//
// The simulator (uacsimd) only *exports* the virtual device on the USB/IP port. To make Windows see it, the usbip client has
// to attach it (`usbip attach`), which needs the usbip-win2 driver; to leave the machine clean again the ports have to be
// detached and the driver removed. These functions are what the GUI buttons call (and what the tests drive with fakes), so
// the GUI only reads controls, calls here and shows the result.
//
// Everything here goes through the same guard as the driver installer (HostCasesAllowed): on a machine that is not a
// Sandbox / VM nothing is executed unless UACSIM_FORCE_HOST_TESTS=1 was set on purpose. Every system action is an injectable
// seam; the tests run the whole flow against fakes. The real thing only ever runs in a Sandbox / VM / a test machine the
// user chose.
#pragma once
#include "driver_installer.h"
#include "sim_process.h"
#include <cstdint>
#include <set>

namespace uacstudio {

// ---- parsing `usbip port` ----
// Every "Port NN:" in the output (the auto test uses this to tell which ports it created).
std::set<std::string> ParseUsbipPorts(const std::string& usbipPortOutput);
// Only the ports whose remote is OUR simulator (usbip://127.0.0.1:<usbipPort>/...). We never detach anything else: on a real
// PC the user may have other usbip devices attached that are none of our business.
std::vector<std::string> ParseOurUsbipPorts(const std::string& usbipPortOutput, unsigned usbipPort = 3240);

// ---- what the operations need from the system ----
struct HostOpsDeps {
    InstallDeps sys;                                   // run / allowed / isAdmin / serviceInstalled / infDir / log
    std::function<std::string()> findUsbip;            // default: usbip.exe in the install dir, else on PATH
    // HTTP to the simulator's control API; returns the response body, "" when it cannot be reached. Default: http_req.
    std::function<std::string(std::uint16_t port, const std::string& method, const std::string& path)> sim;
    // Stops the simulator process on the control port (polite first; see SimProcess::Stop). Default: TheSimulator().Stop.
    std::function<SimResult(std::uint16_t port)> stopSimulator;
};

struct HostOpResult {
    std::string title;
    bool ok = false;
    bool refused = false;        // the guard said no: nothing was changed
    bool rebootRequired = false;
    std::vector<InstallStep> steps;
    std::string Summary() const; // multi-line, for people
};
HostOpResult FromInstall(const std::string& title, const InstallResult& r);
HostOpResult FromSim(const std::string& title, const std::string& step, const SimResult& r);

// `usbip attach -r 127.0.0.1 -b <busid>`: makes Windows see the exported device. Checks first that the simulator is
// reachable and has a profile attached (otherwise usbip would only say "connection refused"). `busid` must look like "1-1".
HostOpResult AttachToWindows(std::uint16_t controlPort, const std::string& busid, const InstallOptions& opt,
                             const HostOpsDeps& deps = {});

// Detaches the Windows ports that belong to this simulator (see ParseOurUsbipPorts), one `usbip detach -p N` each, then checks.
HostOpResult DetachFromWindows(unsigned usbipPort, const InstallOptions& opt, const HostOpsDeps& deps = {});

// Stops the simulator process. If the host actions are allowed on this machine, the simulator's devices are detached from
// Windows first (so no usbip port is left pointing at a server that is gone); if not, usbip is not touched at all and only
// the simulator is stopped. Used by the Stop simulator button and by the GUI when it exits.
HostOpResult StopSimulatorOp(std::uint16_t controlPort, unsigned usbipPort, const InstallOptions& opt, const HostOpsDeps& deps = {});

// Everything, in the order that is safe: Windows ports -> simulator /detach -> stop the simulator -> uninstall the driver.
// Best effort: a failing step does not stop the next one, the result lists every step.
HostOpResult CleanUpAll(std::uint16_t controlPort, unsigned usbipPort, const InstallOptions& opt, const HostOpsDeps& deps = {});

// ---- which buttons may be pressed ----
struct HostState {
    bool allowed = false;          // the disposable-machine guard
    std::string allowedWhy;
    bool admin = false;
    bool simulatorReachable = false; // something answers on the control port
    bool driverInstalled = false;  // service usbip2_ude
    bool usbipPresent = false;
    std::string usbipPath;
    bool payloadOk = false;        // slim payload present and verified (only checked while the driver is not installed)
    std::string payloadWhy;
    std::vector<std::string> ourPorts; // Windows ports currently holding this simulator's device
};
// Blocks (registry, SHA-256 of the payload, `usbip port`): call it off the UI thread.
HostState QueryHostState(std::uint16_t controlPort, const InstallOptions& opt, unsigned usbipPort = 3240,
                         const HostOpsDeps& deps = {});

// StartSimulator / StopSimulator touch only the simulator process (no driver, no system change), so the disposable-machine
// guard does not apply to them; every other action does.
enum class HostAction { StartSimulator, StopSimulator, InstallDriver, UninstallDriver, AttachToWindows, DetachFromWindows, CleanUpAll };
struct ActionView {
    HostAction action;
    bool enabled = false;
    std::string why; // when disabled: the reason, in English, for the status line
};
// Pure function of the state: one entry per action, in the order of the enum.
std::vector<ActionView> PlanHostActions(const HostState& s);

} // namespace uacstudio
