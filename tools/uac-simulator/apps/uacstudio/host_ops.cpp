// See host_ops.h.
#include "http_client.h" // winsock2 must come before windows.h
#include "host_ops.h"
#include <algorithm>
#include <regex>
#include <sstream>

namespace uacstudio {

namespace {

// Trim and shorten tool output so one step stays on a few lines.
std::string Clip(std::string s, std::size_t max = 300) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    std::size_t at = 0;
    while (at < s.size() && (s[at] == '\r' || s[at] == '\n' || s[at] == ' ')) ++at;
    s.erase(0, at);
    if (s.size() > max) s = s.substr(0, max) + "...";
    return s;
}

struct Rec {
    HostOpResult& r;
    const InstallDeps& d;
    bool add(const std::string& name, bool ok, const std::string& detail) {
        r.steps.push_back({name, ok, detail});
        if (d.log) d.log(std::string(ok ? "[ok]   " : "[FAIL] ") + name + (detail.empty() ? "" : " - " + detail));
        return ok;
    }
};

struct Resolved {
    InstallDeps sys;
    std::function<std::string()> findUsbip;
    std::function<std::string(std::uint16_t, const std::string&, const std::string&)> sim;
    std::function<SimResult(std::uint16_t)> stopSimulator;
};

Resolved ResolveOps(const HostOpsDeps& in, const InstallOptions& opt) {
    Resolved r;
    r.sys = ResolveDeps(in.sys);
    r.findUsbip = in.findUsbip;
    if (!r.findUsbip) {
        const std::string dir = opt.installDir;
        r.findUsbip = [dir] { return DetectDriver(dir).usbipPath; };
    }
    r.sim = in.sim;
    if (!r.sim)
        r.sim = [](std::uint16_t port, const std::string& method, const std::string& path) {
            return http_req(port, method.c_str(), path, "");
        };
    r.stopSimulator = in.stopSimulator;
    if (!r.stopSimulator) r.stopSimulator = [](std::uint16_t port) { return TheSimulator().Stop(port); };
    return r;
}

// The bus id ends up on a command line: only digits and one dash, e.g. "1-1".
bool ValidBusId(const std::string& s) {
    const auto dash = s.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= s.size() || s.size() > 16) return false;
    for (std::size_t i = 0; i < s.size(); ++i)
        if (i != dash && (s[i] < '0' || s[i] > '9')) return false;
    return true;
}

void Finish(HostOpResult& r) {
    r.ok = !r.refused && !r.steps.empty() &&
           std::all_of(r.steps.begin(), r.steps.end(), [](const InstallStep& s) { return s.ok; });
}

// Detach this simulator's Windows ports. Shared by DetachFromWindows and CleanUpAll.
void DetachSteps(Rec& rec, const Resolved& d, unsigned usbipPort) {
    const std::string usbip = d.findUsbip();
    if (usbip.empty()) {
        rec.add("usbip.exe", true, "not found - the driver is not installed, so nothing can be attached to Windows");
        return;
    }
    const auto ours = [&] { return ParseOurUsbipPorts(d.sys.run(usbip, "port", 15000).out, usbipPort); };
    const auto ports = ours();
    if (ports.empty()) {
        rec.add("Windows ports", true, "no device of this simulator is attached to Windows");
        return;
    }
    for (const auto& p : ports) {
        const ToolResult t = d.sys.run(usbip, "detach -p " + p, 20000);
        const bool ok = t.started && !t.timedOut && t.code == 0;
        rec.add("usbip detach -p " + p, ok,
                ok ? Clip(t.out) : (t.timedOut ? "usbip did not finish in 20 s" : "exit code " + std::to_string(t.code) + ": " + Clip(t.out)));
    }
    const auto left = ours();
    std::string joined;
    for (const auto& p : left) joined += (joined.empty() ? "" : ", ") + p;
    rec.add("verify Windows ports", left.empty(), left.empty() ? "all detached" : "still attached: port " + joined);
}

} // namespace

std::set<std::string> ParseUsbipPorts(const std::string& text) {
    std::set<std::string> ports;
    static const std::regex re(R"(Port\s+(\d+))", std::regex::icase);
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) ports.insert((*it)[1].str());
    return ports;
}

std::vector<std::string> ParseOurUsbipPorts(const std::string& text, unsigned usbipPort) {
    // Output of `usbip port`:
    //   Port 01: device in use at High Speed(480Mbps)
    //            C-Media Electronics, Inc. : CM102-A+/102S+ Audio Controller (0d8c:0103)
    //              -> usbip://127.0.0.1:3240/1-1
    const std::string needle = "usbip://127.0.0.1:" + std::to_string(usbipPort) + "/";
    static const std::regex head(R"(^\s*Port\s+(\d+)\s*:)", std::regex::icase);
    std::vector<std::string> ours;
    std::string current;
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        std::smatch m;
        if (std::regex_search(line, m, head)) {
            current = m[1].str();
            continue;
        }
        if (!current.empty() && line.find(needle) != std::string::npos) {
            ours.push_back(current);
            current.clear(); // one entry per port block
        }
    }
    return ours;
}

std::string HostOpResult::Summary() const {
    std::ostringstream o;
    for (const auto& s : steps) o << (s.ok ? "[ok]   " : "[FAIL] ") << s.name << (s.detail.empty() ? "" : " - " + s.detail) << "\n";
    if (refused) o << "REFUSED: nothing was changed on this machine.\n";
    else if (ok) o << "DONE" << (rebootRequired ? " - a REBOOT IS REQUIRED to finish" : "") << "\n";
    else o << "FAILED - see the [FAIL] line(s) above.\n";
    return o.str();
}

HostOpResult FromInstall(const std::string& title, const InstallResult& in) {
    HostOpResult r;
    r.title = title;
    r.ok = in.ok;
    r.refused = in.refused;
    r.rebootRequired = in.rebootRequired;
    r.steps = in.steps;
    return r;
}

HostOpResult FromSim(const std::string& title, const std::string& step, const SimResult& in) {
    HostOpResult r;
    r.title = title;
    r.steps.push_back({step, in.ok, in.detail});
    Finish(r);
    return r;
}

HostOpResult AttachToWindows(std::uint16_t controlPort, const std::string& busid, const InstallOptions& opt,
                             const HostOpsDeps& deps) {
    const Resolved d = ResolveOps(deps, opt);
    HostOpResult r;
    r.title = "Attach to Windows";
    Rec rec{r, d.sys};
    std::string why;
    if (!d.sys.allowed(&why)) {
        r.refused = true;
        rec.add("guard: disposable machine", false, why);
        return r;
    }
    rec.add("guard: disposable machine", true, why);
    if (!ValidBusId(busid)) {
        rec.add("bus id", false, "'" + Clip(busid, 40) + "' is not a bus id like 1-1");
        Finish(r);
        return r;
    }
    const std::string usbip = d.findUsbip();
    if (usbip.empty()) {
        rec.add("usbip.exe", false, "not found - install the driver first");
        Finish(r);
        return r;
    }
    rec.add("usbip.exe", true, usbip);

    const std::string status = d.sim(controlPort, "GET", "/status");
    if (status.empty()) {
        rec.add("simulator", false, "uacsimd is not reachable on 127.0.0.1:" + std::to_string(controlPort) + " - start uacsimd.exe");
        Finish(r);
        return r;
    }
    if (status.find("\"listening\":true") == std::string::npos) {
        rec.add("simulator", false, "no profile is attached in the simulator yet - pick a profile and press Attach in the main window first");
        Finish(r);
        return r;
    }
    rec.add("simulator", true, "a profile is attached and exported as " + busid);

    const ToolResult t = d.sys.run(usbip, "attach -r 127.0.0.1 -b " + busid, 30000);
    const bool ok = t.started && !t.timedOut && t.code == 0;
    std::string detail = ok ? Clip(t.out) : t.timedOut ? "usbip did not finish in 30 s" : "exit code " + std::to_string(t.code) + ": " + Clip(t.out);
    if (!ok && !d.sys.isAdmin()) detail += " (this program is not elevated - usbip needs administrator rights)";
    rec.add("usbip attach -r 127.0.0.1 -b " + busid, ok, detail);
    if (ok) {
        std::string ports;
        for (const auto& p : ParseUsbipPorts(d.sys.run(usbip, "port", 15000).out)) ports += (ports.empty() ? "" : ", ") + p;
        rec.add("Windows ports", true, ports.empty() ? "usbip reports no imported device" : "imported at port " + ports);
    }
    Finish(r);
    return r;
}

HostOpResult DetachFromWindows(unsigned usbipPort, const InstallOptions& opt, const HostOpsDeps& deps) {
    const Resolved d = ResolveOps(deps, opt);
    HostOpResult r;
    r.title = "Detach from Windows";
    Rec rec{r, d.sys};
    std::string why;
    if (!d.sys.allowed(&why)) {
        r.refused = true;
        rec.add("guard: disposable machine", false, why);
        return r;
    }
    rec.add("guard: disposable machine", true, why);
    DetachSteps(rec, d, usbipPort);
    Finish(r);
    return r;
}

HostOpResult StopSimulatorOp(std::uint16_t controlPort, unsigned usbipPort, const InstallOptions& opt, const HostOpsDeps& deps) {
    const Resolved d = ResolveOps(deps, opt);
    HostOpResult r;
    r.title = "Stop simulator";
    Rec rec{r, d.sys};
    std::string why;
    if (d.sys.allowed(&why)) {
        DetachSteps(rec, d, usbipPort); // Windows lets go first, so no usbip port is left pointing at a server that is gone
    } else {
        rec.add("Windows ports", true, "not touched: host actions are off on this machine (" + Clip(why, 80) + ")");
    }
    const SimResult s = d.stopSimulator(controlPort);
    rec.add("stop simulator", s.ok, s.detail);
    Finish(r);
    return r;
}

HostOpResult CleanUpAll(std::uint16_t controlPort, unsigned usbipPort, const InstallOptions& opt, const HostOpsDeps& deps) {
    const Resolved d = ResolveOps(deps, opt);
    HostOpResult r;
    r.title = "Clean up all";
    Rec rec{r, d.sys};
    std::string why;
    if (!d.sys.allowed(&why)) {
        r.refused = true;
        rec.add("guard: disposable machine", false, why);
        return r;
    }
    rec.add("guard: disposable machine", true, why);

    // 1) Windows must let go of the device before the simulator disappears and before the driver is removed
    DetachSteps(rec, d, usbipPort);

    // 2) The simulator: drop every instance. Not running is fine - there is nothing to detach then.
    const std::string body = d.sim(controlPort, "POST", "/detach");
    if (body.empty()) rec.add("simulator detach", true, "uacsimd is not running - nothing to detach");
    else rec.add("simulator detach", body.find("\"ok\":true") != std::string::npos, Clip(body, 120));

    // 3) The simulator process itself: it would otherwise keep the ports and the virtual device after everything else is clean
    const SimResult stopped = d.stopSimulator(controlPort);
    rec.add("stop simulator", stopped.ok, stopped.detail);

    // 4) The driver (best effort, every step is tried; see UninstallDriver)
    std::string state;
    if (!d.sys.serviceInstalled(&state)) {
        rec.add("driver", true, "not installed - nothing to remove");
    } else {
        const InstallResult u = UninstallDriver(opt, d.sys);
        for (const auto& s : u.steps) rec.add("driver: " + s.name, s.ok, s.detail);
        r.rebootRequired = u.rebootRequired;
    }
    Finish(r);
    return r;
}

HostState QueryHostState(std::uint16_t controlPort, const InstallOptions& opt, unsigned usbipPort, const HostOpsDeps& deps) {
    const Resolved d = ResolveOps(deps, opt);
    HostState s;
    s.simulatorReachable = !d.sim(controlPort, "GET", "/status").empty();
    s.allowed = d.sys.allowed(&s.allowedWhy);
    s.admin = d.sys.isAdmin();
    std::string state;
    s.driverInstalled = d.sys.serviceInstalled(&state);
    s.usbipPath = d.findUsbip();
    s.usbipPresent = !s.usbipPath.empty();
    // The expensive parts only matter when the buttons could be used at all
    if (s.allowed && !s.driverInstalled && !opt.payloadRoot.empty()) {
        const PayloadCheck pc = CheckPayload(opt.payloadRoot);
        s.payloadOk = pc.ok;
        s.payloadWhy = pc.error;
    }
    if (s.allowed && s.driverInstalled && s.usbipPresent)
        s.ourPorts = ParseOurUsbipPorts(d.sys.run(s.usbipPath, "port", 15000).out, usbipPort);
    return s;
}

std::vector<ActionView> PlanHostActions(const HostState& s) {
    std::vector<ActionView> v;
    auto add = [&](HostAction a, bool enabled, const std::string& why) {
        v.push_back({a, s.allowed && enabled, s.allowed ? (enabled ? "" : why) : s.allowedWhy});
    };
    // The simulator process is ours to start and stop on any machine: no guard
    v.push_back({HostAction::StartSimulator, !s.simulatorReachable, s.simulatorReachable ? "a simulator is already running" : ""});
    v.push_back({HostAction::StopSimulator, s.simulatorReachable, s.simulatorReachable ? "" : "no simulator is running"});
    add(HostAction::InstallDriver, s.payloadOk && !s.driverInstalled,
        s.driverInstalled ? "the driver is already installed"
                          : "no usable driver payload" + (s.payloadWhy.empty() ? std::string() : " (" + s.payloadWhy + ")"));
    add(HostAction::UninstallDriver, s.driverInstalled, "the driver is not installed");
    add(HostAction::AttachToWindows, s.driverInstalled && s.usbipPresent,
        s.driverInstalled ? "usbip.exe was not found" : "the driver is not installed");
    add(HostAction::DetachFromWindows, s.usbipPresent && !s.ourPorts.empty(), "no device of this simulator is attached to Windows");
    add(HostAction::CleanUpAll, s.driverInstalled || !s.ourPorts.empty(), "nothing to clean up");
    return v;
}

} // namespace uacstudio
