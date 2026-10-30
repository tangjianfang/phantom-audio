// Windows-side operations behind the GUI buttons (attach to Windows, detach, clean up all) and the rules for which buttons
// are enabled. Everything runs against fakes; the real thing only happens in a Sandbox / VM / a machine the user chose.
// What matters most and is pinned down here: a refused guard runs nothing, we never touch usbip ports that are not ours,
// the bus id cannot inject into the command line, and one failing cleanup step does not skip the next ones.
#include <doctest/doctest.h>
#include <host_ops.h>
#include <windows.h>
#include <filesystem>
#include <fstream>

using namespace uacstudio;
namespace fs = std::filesystem;

namespace {

// Real `usbip port` output from the Sandbox run of 2026-10-30, plus a device that belongs to somebody else.
const std::string kOurs =
    "Imported USB devices\n====================\n"
    "Port 01: device in use at High Speed(480Mbps)\n"
    "         OpenMoko, Inc. : unknown product (1d50:6222)\n"
    "           -> usbip://127.0.0.1:3240/1-1\n"
    "           -> remote bus/dev: 001/001\n"
    "           -> serial: \n"
    "           -> mode: zero-copy\n";
const std::string kForeign =
    "Port 02: device in use at High Speed(480Mbps)\n"
    "         Some Vendor : A real remote device (1234:5678)\n"
    "           -> usbip://192.168.1.50:3240/3-2\n"
    "           -> remote bus/dev: 003/002\n";

fs::path fresh_dir(const char* name) {
    const fs::path d = fs::temp_directory_path() / "uac_hostops_test" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

// Records every command and every call to the simulator, in order, and plays the part of usbip / pnputil / devnode.
struct Fake {
    std::vector<std::string> events;   // "run <exe> <args>" and "sim <METHOD> <path>"
    bool allowed = true, admin = true, driver = true;
    std::string usbip = "C:/fake/usbip.exe";
    std::string simStatus = R"({"listening":true,"imported":false})";
    std::string simDetachReply = R"({"ok":true})";
    std::string portText = kOurs;      // what `usbip port` prints right now
    bool detachWorks = true;           // does `usbip detach -p N` really remove the port?
    int attachExit = 0;
    bool attachTimesOut = false;
    bool simStopOk = true;             // does stopping the simulator process work?

    int count(const std::string& what) const {
        int n = 0;
        for (const auto& e : events) n += contains(e, what) ? 1 : 0;
        return n;
    }
    int index_of(const std::string& what) const {
        for (std::size_t i = 0; i < events.size(); ++i)
            if (contains(events[i], what)) return static_cast<int>(i);
        return -1;
    }

    HostOpsDeps deps(const fs::path& infDir = {}) {
        HostOpsDeps d;
        d.sys.allowed = [this](std::string* why) {
            if (why) *why = allowed ? "fake disposable machine" : "refused: not a disposable machine";
            return allowed;
        };
        d.sys.isAdmin = [this] { return admin; };
        d.sys.serviceInstalled = [this](std::string* st) {
            if (st) *st = "running";
            return driver;
        };
        d.sys.infDir = infDir.string();
        d.sys.run = [this](const std::string& exe, const std::string& args, int) {
            events.push_back("run " + exe + " " + args);
            ToolResult t;
            t.started = true;
            if (contains(exe, "usbip") && args == "port") {
                t.out = portText;
            } else if (contains(exe, "usbip") && contains(args, "attach")) {
                t.timedOut = attachTimesOut;
                t.code = static_cast<unsigned long>(attachExit);
                t.out = attachExit == 0 ? "successfully attached to port 1" : "usbip: error: connection refused";
            } else if (contains(exe, "usbip") && contains(args, "detach -p")) {
                if (detachWorks) {
                    const auto p = args.substr(args.find("-p ") + 3);
                    const auto at = portText.find("Port " + p + ":");
                    if (at != std::string::npos) {
                        const auto next = portText.find("Port ", at + 1);
                        portText.erase(at, next == std::string::npos ? std::string::npos : next - at);
                    }
                }
                t.out = "port detached";
            }
            return t; // devnode / pnputil: exit code 0
        };
        d.findUsbip = [this] { return usbip; };
        d.stopSimulator = [this](std::uint16_t) {
            events.push_back("stopsim");
            return SimResult{simStopOk, simStopOk ? "the simulator stopped" : "the simulator did not stop"};
        };
        d.sim = [this](std::uint16_t, const std::string& method, const std::string& path) {
            events.push_back("sim " + method + " " + path);
            return path == "/status" ? simStatus : simDetachReply;
        };
        return d;
    }
};

InstallOptions opts_with_devnode(const char* name) {
    const fs::path dest = fresh_dir(name) / "USBip";
    std::error_code ec;
    fs::create_directories(dest, ec);
    std::ofstream(dest / "devnode.exe") << "fake";
    return {"", dest.string()};
}

} // namespace

TEST_CASE("usbip port parsing: all ports vs only the ports of this simulator") {
    CHECK(ParseUsbipPorts(kOurs + kForeign) == std::set<std::string>{"01", "02"});
    CHECK(ParseOurUsbipPorts(kOurs + kForeign) == std::vector<std::string>{"01"});
    CHECK(ParseOurUsbipPorts(kForeign).empty());
    CHECK(ParseOurUsbipPorts(kOurs, 3250).empty());          // another simulator port is not ours
    CHECK(ParseOurUsbipPorts(kOurs + kOurs).size() == 2);    // two blocks, two entries
    CHECK(ParseOurUsbipPorts("").empty());
    CHECK(ParseOurUsbipPorts("Imported USB devices\n====================\n").empty());
}

TEST_CASE("attach to windows: the guard refuses -> nothing runs and the simulator is not even asked") {
    Fake f;
    f.allowed = false;
    const auto r = AttachToWindows(3241, "1-1", {}, f.deps());
    CHECK_FALSE(r.ok);
    CHECK(r.refused);
    CHECK(f.events.empty());
    CHECK(contains(r.Summary(), "REFUSED"));
}

TEST_CASE("attach to windows: the bus id cannot inject into the command line") {
    for (const char* bad : {"", "1", "1-", "-1", "x-1", "1-1; calc", "1-1 & calc", "1-1\" -x", "1--1", "1-1-1", "1-12345678901234567"}) {
        INFO("busid '" << bad << "'");
        Fake f;
        const auto r = AttachToWindows(3241, bad, {}, f.deps());
        CHECK_FALSE(r.ok);
        CHECK(f.count("attach") == 0);
    }
    Fake good;
    CHECK(AttachToWindows(3241, "1-1", {}, good.deps()).ok);
    Fake second;
    CHECK(AttachToWindows(3241, "1-12", {}, second.deps()).ok);
}

TEST_CASE("attach to windows: needs usbip, a reachable simulator and an attached profile - and says which is missing") {
    {   // no usbip.exe: the driver is not installed
        Fake f;
        f.usbip.clear();
        const auto r = AttachToWindows(3241, "1-1", {}, f.deps());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.Summary(), "install the driver first"));
        CHECK(f.count("attach") == 0);
    }
    {   // uacsimd is not running
        Fake f;
        f.simStatus.clear();
        const auto r = AttachToWindows(3241, "1-1", {}, f.deps());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.Summary(), "not reachable"));
        CHECK(f.count("attach") == 0);
    }
    {   // uacsimd runs but no profile is attached
        Fake f;
        f.simStatus = R"({"listening":false,"imported":false})";
        const auto r = AttachToWindows(3241, "1-1", {}, f.deps());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.Summary(), "no profile is attached"));
        CHECK(f.count("attach") == 0);
    }
}

TEST_CASE("attach to windows: success runs exactly the usbip attach for that bus id") {
    Fake f;
    f.portText = "";
    const auto r = AttachToWindows(3241, "1-1", {}, f.deps());
    INFO(r.Summary());
    CHECK(r.ok);
    CHECK(f.index_of("sim GET /status") >= 0);
    REQUIRE(f.count("attach -r 127.0.0.1 -b 1-1") == 1);
    CHECK(f.index_of("sim GET /status") < f.index_of("attach -r"));   // check first, attach second
    CHECK(f.count("detach") == 0);                                      // never detaches as a side effect
}

TEST_CASE("attach to windows: a failing or hanging usbip is reported, and the non-elevated hint appears only when it applies") {
    {
        Fake f;
        f.attachExit = 1;
        f.admin = false;
        const auto r = AttachToWindows(3241, "1-1", {}, f.deps());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.Summary(), "exit code 1"));
        CHECK(contains(r.Summary(), "not elevated"));
    }
    {
        Fake f;
        f.attachExit = 1;
        f.admin = true;
        const auto r = AttachToWindows(3241, "1-1", {}, f.deps());
        CHECK_FALSE(r.ok);
        CHECK_FALSE(contains(r.Summary(), "not elevated")); // already elevated: the hint would mislead
    }
    {
        Fake f;
        f.attachTimesOut = true;
        const auto r = AttachToWindows(3241, "1-1", {}, f.deps());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.Summary(), "did not finish"));
    }
}

TEST_CASE("detach from windows: only this simulator's ports, never somebody else's") {
    SUBCASE("guard refuses") {
        Fake f;
        f.allowed = false;
        const auto r = DetachFromWindows(3240, {}, f.deps());
        CHECK(r.refused);
        CHECK(f.events.empty());
    }
    SUBCASE("ours and a foreign device") {
        Fake f;
        f.portText = kOurs + kForeign;
        const auto r = DetachFromWindows(3240, {}, f.deps());
        INFO(r.Summary());
        CHECK(r.ok);
        CHECK(f.count("detach -p 01") == 1);
        CHECK(f.count("detach -p 02") == 0);          // the foreign port is none of our business
        CHECK(f.count("detach -a") == 0);             // and `detach -a` would take it too
        CHECK(contains(f.portText, "Port 02:"));
    }
    SUBCASE("only a foreign device: nothing is detached") {
        Fake f;
        f.portText = kForeign;
        const auto r = DetachFromWindows(3240, {}, f.deps());
        CHECK(r.ok);
        CHECK(f.count("detach") == 0);
    }
    SUBCASE("a port that refuses to go away is reported") {
        Fake f;
        f.detachWorks = false;
        const auto r = DetachFromWindows(3240, {}, f.deps());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.Summary(), "still attached: port 01"));
    }
    SUBCASE("no usbip.exe means the driver is not installed: nothing to do, not an error") {
        Fake f;
        f.usbip.clear();
        const auto r = DetachFromWindows(3240, {}, f.deps());
        CHECK(r.ok);
        CHECK(f.events.empty());
    }
}

TEST_CASE("clean up all: Windows ports first, then the simulator, then the driver") {
    Fake f;
    const auto inf = fresh_dir("c_inf");
    const auto opt = opts_with_devnode("c_dest");
    const auto r = CleanUpAll(3241, 3240, opt, f.deps(inf));
    INFO(r.Summary());
    CHECK(r.ok);
    const int detachWin = f.index_of("detach -p 01");
    const int detachSim = f.index_of("sim POST /detach");
    const int stopSim = f.index_of("stopsim");
    const int removeNode = f.index_of("devnode.exe remove");
    REQUIRE(detachWin >= 0);
    REQUIRE(detachSim >= 0);
    REQUIRE(stopSim >= 0);
    REQUIRE(removeNode >= 0);
    CHECK(detachWin < detachSim);   // Windows lets go first
    CHECK(detachSim < stopSim);     // then the simulator is emptied and its process ends
    CHECK(stopSim < removeNode);    // and only then is the driver removed
    CHECK_FALSE(fs::exists(fs::path(opt.installDir) / "devnode.exe")); // our own file is gone
}

TEST_CASE("clean up all: the guard refuses -> no command, no call to the simulator, the simulator is not stopped") {
    Fake f;
    f.allowed = false;
    const auto r = CleanUpAll(3241, 3240, opts_with_devnode("c_ref"), f.deps(fresh_dir("c_ref_inf")));
    CHECK_FALSE(r.ok);
    CHECK(r.refused);
    CHECK(f.events.empty());
}

TEST_CASE("clean up all: a failing step does not stop the next ones, and the result says it failed") {
    Fake f;
    f.detachWorks = false; // Windows keeps the port
    const auto r = CleanUpAll(3241, 3240, opts_with_devnode("c_fail"), f.deps(fresh_dir("c_fail_inf")));
    INFO(r.Summary());
    CHECK_FALSE(r.ok);
    CHECK(contains(r.Summary(), "still attached"));
    CHECK(f.index_of("sim POST /detach") >= 0);   // still dropped the simulator side
    CHECK(f.index_of("stopsim") >= 0);            // still stopped the simulator process
    CHECK(f.index_of("devnode.exe remove") >= 0); // still went on to the driver
}

TEST_CASE("clean up all: a simulator that will not stop is reported and the driver is still removed") {
    Fake f;
    f.simStopOk = false;
    const auto r = CleanUpAll(3241, 3240, opts_with_devnode("c_nostop"), f.deps(fresh_dir("c_nostop_inf")));
    INFO(r.Summary());
    CHECK_FALSE(r.ok);
    CHECK(contains(r.Summary(), "did not stop"));
    CHECK(f.index_of("devnode.exe remove") >= 0);
}

TEST_CASE("stop simulator: Windows ports first when allowed; only the simulator when not") {
    SUBCASE("allowed: detach this simulator's ports, then stop - and not the foreign ones") {
        Fake f;
        f.portText = kOurs + kForeign;
        const auto r = StopSimulatorOp(3241, 3240, {}, f.deps());
        INFO(r.Summary());
        CHECK(r.ok);
        REQUIRE(f.index_of("detach -p 01") >= 0);
        REQUIRE(f.index_of("stopsim") >= 0);
        CHECK(f.index_of("detach -p 01") < f.index_of("stopsim"));
        CHECK(f.count("detach -p 02") == 0);
    }
    SUBCASE("not allowed: usbip is never touched, the simulator is still stopped") {
        Fake f;
        f.allowed = false;
        const auto r = StopSimulatorOp(3241, 3240, {}, f.deps());
        INFO(r.Summary());
        CHECK(r.ok);
        CHECK(f.count("usbip") == 0);   // no `usbip port`, no `usbip detach`
        CHECK(f.count("stopsim") == 1);
        CHECK(contains(r.Summary(), "not touched"));
    }
    SUBCASE("a simulator that will not stop makes the operation fail") {
        Fake f;
        f.simStopOk = false;
        CHECK_FALSE(StopSimulatorOp(3241, 3240, {}, f.deps()).ok);
    }
}

TEST_CASE("FromSim: one step, ok follows the result") {
    CHECK(FromSim("Start simulator", "start", SimResult{true, "started"}).ok);
    const auto bad = FromSim("Start simulator", "start", SimResult{false, "no uacsimd.exe"});
    CHECK_FALSE(bad.ok);
    REQUIRE(bad.steps.size() == 1);
    CHECK(bad.steps[0].detail == "no uacsimd.exe");
}

TEST_CASE("clean up all: nothing to clean is not an error; a missing driver means no driver commands; a stopped simulator is fine") {
    Fake f;
    f.driver = false;
    f.usbip.clear();
    f.simDetachReply.clear(); // uacsimd is not running
    const auto r = CleanUpAll(3241, 3240, opts_with_devnode("c_none"), f.deps(fresh_dir("c_none_inf")));
    INFO(r.Summary());
    CHECK(r.ok);
    CHECK(f.count("devnode") == 0);
    CHECK(f.count("pnputil") == 0);
    CHECK(contains(r.Summary(), "not installed - nothing to remove"));
    CHECK(contains(r.Summary(), "not running - nothing to detach"));
}

TEST_CASE("host state: expensive queries only when the guard allows them") {
    SUBCASE("guard refuses -> `usbip port` is never run") {
        Fake f;
        f.allowed = false;
        const auto s = QueryHostState(3241, {}, 3240, f.deps());
        CHECK_FALSE(s.allowed);
        CHECK(s.ourPorts.empty());
        CHECK(f.count("run ") == 0); // not one system command (asking the simulator over HTTP is not one)
        CHECK(f.count("usbip") == 0);
    }
    SUBCASE("simulator reachability follows the control port - guard or not") {
        Fake up;
        up.allowed = false;
        CHECK(QueryHostState(3241, {}, 3240, up.deps()).simulatorReachable);
        Fake down;
        down.simStatus.clear();
        CHECK_FALSE(QueryHostState(3241, {}, 3240, down.deps()).simulatorReachable);
    }
    SUBCASE("allowed, driver installed -> our ports are listed, foreign ones are not") {
        Fake f;
        f.portText = kOurs + kForeign;
        const auto s = QueryHostState(3241, {}, 3240, f.deps());
        CHECK(s.allowed);
        CHECK(s.driverInstalled);
        CHECK(s.usbipPresent);
        CHECK(s.ourPorts == std::vector<std::string>{"01"});
    }
}

TEST_CASE("button plan: each button follows the state (and flips when the state flips)") {
    auto enabled = [](const std::vector<ActionView>& v, HostAction a) {
        for (const auto& x : v)
            if (x.action == a) return x.enabled;
        return false;
    };
    auto why = [](const std::vector<ActionView>& v, HostAction a) {
        for (const auto& x : v)
            if (x.action == a) return x.why;
        return std::string("<missing>");
    };

    HostState s;
    s.allowed = false;
    s.allowedWhy = "refused: not a disposable machine";
    {   // guard says no: everything off, and the reason is the guard's
        const auto v = PlanHostActions(s);
        REQUIRE(v.size() == 7);
        for (const auto& x : v) {
            if (x.action == HostAction::StartSimulator || x.action == HostAction::StopSimulator) continue; // not guarded
            CHECK_FALSE(x.enabled);
            CHECK(x.why == s.allowedWhy);
        }
        // Starting / stopping the simulator is available on any machine: it changes nothing outside our own process
        CHECK(enabled(v, HostAction::StartSimulator));
        CHECK_FALSE(enabled(v, HostAction::StopSimulator));
        CHECK(why(v, HostAction::StopSimulator) == "no simulator is running");
        s.simulatorReachable = true;
        const auto running = PlanHostActions(s);
        CHECK_FALSE(enabled(running, HostAction::StartSimulator));
        CHECK(why(running, HostAction::StartSimulator) == "a simulator is already running");
        CHECK(enabled(running, HostAction::StopSimulator));
        s.simulatorReachable = false;
    }
    s.allowed = true;
    s.payloadOk = true;
    {   // fresh machine: only Install
        const auto v = PlanHostActions(s);
        CHECK(enabled(v, HostAction::InstallDriver));
        CHECK_FALSE(enabled(v, HostAction::UninstallDriver));
        CHECK_FALSE(enabled(v, HostAction::AttachToWindows));
        CHECK_FALSE(enabled(v, HostAction::DetachFromWindows));
        CHECK_FALSE(enabled(v, HostAction::CleanUpAll));
        CHECK(why(v, HostAction::UninstallDriver) == "the driver is not installed");
    }
    s.payloadOk = false;
    s.payloadWhy = "MANIFEST hash mismatch";
    {   // damaged payload: Install is off and says why
        const auto v = PlanHostActions(s);
        CHECK_FALSE(enabled(v, HostAction::InstallDriver));
        CHECK(contains(why(v, HostAction::InstallDriver), "MANIFEST hash mismatch"));
    }
    s.driverInstalled = true;
    s.usbipPresent = true;
    s.usbipPath = "C:/fake/usbip.exe";
    {   // driver installed, nothing attached: no second install, Uninstall / Attach / Clean up available
        const auto v = PlanHostActions(s);
        CHECK_FALSE(enabled(v, HostAction::InstallDriver)); // devnode install would create a second host controller
        CHECK(why(v, HostAction::InstallDriver) == "the driver is already installed");
        CHECK(enabled(v, HostAction::UninstallDriver));
        CHECK(enabled(v, HostAction::AttachToWindows));
        CHECK_FALSE(enabled(v, HostAction::DetachFromWindows));
        CHECK(enabled(v, HostAction::CleanUpAll));
    }
    s.ourPorts = {"01"};
    {   // something attached: Detach becomes available
        const auto v = PlanHostActions(s);
        CHECK(enabled(v, HostAction::DetachFromWindows));
    }
    s.usbipPresent = false;
    {   // driver installed but usbip.exe gone: cannot attach or detach
        const auto v = PlanHostActions(s);
        CHECK_FALSE(enabled(v, HostAction::AttachToWindows));
        CHECK_FALSE(enabled(v, HostAction::DetachFromWindows));
        CHECK(contains(why(v, HostAction::AttachToWindows), "usbip.exe"));
    }
}
