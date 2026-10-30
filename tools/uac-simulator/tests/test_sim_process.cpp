// Starting / stopping uacsimd from the GUI. The logic runs against a fake process; two tests then use the real uacsimd.exe
// (built next to this test executable) to prove the pieces fit: /shutdown ends the process, and --parent-pid ends it when
// the parent dies - the guarantee that a crashed GUI cannot leave a simulator behind.
#include <doctest/doctest.h>
#include <http_client.h> // winsock2 before windows.h
#include <sim_process.h>
#include <windows.h>
#include <chrono>
#include <filesystem>

using namespace uacstudio;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }

// A pretend uacsimd: reachable once `pollsUntilReachable` status polls have happened after spawn; honours /shutdown unless told not to.
struct FakeSim {
    bool alreadyRunning = false;       // a simulator that someone else started answers from the beginning
    bool exeExists = true;
    bool spawnFails = false;
    int pollsUntilReachable = 2;
    bool diesAtOnce = false;
    bool ignoresShutdown = false;
    bool hung = false;                 // alive but never answers
    // state
    bool reachable = false;
    bool processAlive = false;
    bool spawned = false, terminated = false, closed = false;
    int polls = 0;
    std::string lastArgs, lastExe;
    std::vector<std::string> httpCalls;
    int next = 0x1000;

    SimProcessDeps deps() {
        SimProcessDeps d;
        d.http = [this](std::uint16_t, const std::string& method, const std::string& path) -> std::string {
            httpCalls.push_back(method + " " + path);
            if (method == "POST" && path == "/shutdown") {
                if (!answers()) return "";
                if (!ignoresShutdown) {
                    reachable = false;
                    processAlive = false;
                    alreadyRunning = false;
                }
                return "{\"ok\":true}";
            }
            if (alreadyRunning && !spawned) return "{\"running\":true}";
            if (spawned && !diesAtOnce && !hung && processAlive && ++polls >= pollsUntilReachable) reachable = true;
            return answers() ? "{\"running\":true}" : "";
        };
        d.spawn = [this](const std::string& exe, const std::string& args, std::string* err) -> void* {
            if (spawnFails) {
                if (err) *err = "access denied";
                return nullptr;
            }
            spawned = true;
            processAlive = !diesAtOnce;
            lastExe = exe;
            lastArgs = args;
            return reinterpret_cast<void*>(static_cast<std::intptr_t>(next++));
        };
        d.waitExit = [this](void*, int) { return !processAlive; };
        d.terminate = [this](void*) {
            terminated = true;
            processAlive = false;
            reachable = false;
        };
        d.closeHandle = [this](void*) { closed = true; };
        d.fileExists = [this](const std::string&) { return exeExists; };
        d.sleep = [](int) {};
        return d;
    }
    bool answers() const { return alreadyRunning || (reachable && !hung); }
};

SimStartOptions opts() {
    SimStartOptions o;
    o.exeDir = "C:\\fake\\bin";
    o.httpPort = 3241;
    o.usbipPort = 3240;
    o.profilesDir = "C:\\fake\\bin\\profiles";
    o.parentPid = 4242;
    return o;
}

std::uint16_t free_port() {
    wsa_ensure_init();
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7f000001);
    a.sin_port = 0;
    REQUIRE(bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    int len = sizeof(a);
    REQUIRE(getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0);
    closesocket(s);
    return ntohs(a.sin_port);
}

std::string this_exe_dir() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return fs::path(p).parent_path().string();
}

} // namespace

TEST_CASE("sim process: start") {
    SUBCASE("a simulator that already answers is left alone") {
        FakeSim f;
        f.alreadyRunning = true;
        SimProcess sim(f.deps());
        const auto r = sim.Start(opts());
        CHECK(r.ok);
        CHECK(contains(r.detail, "already answering"));
        CHECK_FALSE(f.spawned);
        CHECK_FALSE(sim.Owns()); // not ours
    }
    SUBCASE("uacsimd.exe missing -> clear error, nothing spawned") {
        FakeSim f;
        f.exeExists = false;
        SimProcess sim(f.deps());
        const auto r = sim.Start(opts());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.detail, "uacsimd.exe was not found"));
        CHECK_FALSE(f.spawned);
    }
    SUBCASE("spawn fails -> the reason is reported") {
        FakeSim f;
        f.spawnFails = true;
        SimProcess sim(f.deps());
        const auto r = sim.Start(opts());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.detail, "access denied"));
    }
    SUBCASE("success: ports, profiles and the parent pid go on the command line; the process is ours") {
        FakeSim f;
        f.pollsUntilReachable = 3;
        SimProcess sim(f.deps());
        const auto r = sim.Start(opts());
        INFO(r.detail);
        CHECK(r.ok);
        CHECK(f.spawned);
        CHECK(contains(f.lastExe, "uacsimd.exe"));
        CHECK(contains(f.lastArgs, "--http-port 3241"));
        CHECK(contains(f.lastArgs, "--usbip-port 3240"));
        CHECK(contains(f.lastArgs, "--profiles \"C:\\fake\\bin\\profiles\""));
        CHECK(contains(f.lastArgs, "--parent-pid 4242")); // the guarantee that a dead GUI leaves no simulator behind
        CHECK(sim.Owns());
    }
    SUBCASE("the default parent is this process") {
        FakeSim f;
        SimProcess sim(f.deps());
        auto o = opts();
        o.parentPid = 0;
        CHECK(sim.Start(o).ok);
        CHECK(contains(f.lastArgs, "--parent-pid " + std::to_string(GetCurrentProcessId())));
    }
    SUBCASE("a process that dies at once is reported with the likely reason, and is not kept") {
        FakeSim f;
        f.diesAtOnce = true;
        SimProcess sim(f.deps());
        const auto r = sim.Start(opts());
        CHECK_FALSE(r.ok);
        CHECK(contains(r.detail, "exited right after"));
        CHECK(contains(r.detail, "3241"));
        CHECK(f.closed);
        CHECK_FALSE(sim.Owns());
    }
    SUBCASE("a process that never answers is stopped again") {
        FakeSim f;
        f.hung = true;
        SimProcess sim(f.deps());
        const auto r = sim.Start(opts(), 500);
        CHECK_FALSE(r.ok);
        CHECK(contains(r.detail, "did not answer"));
        CHECK(f.terminated);
        CHECK_FALSE(sim.Owns());
    }
}

TEST_CASE("sim process: stop") {
    SUBCASE("polite first: /shutdown, the process ends by itself, nothing is terminated") {
        FakeSim f;
        SimProcess sim(f.deps());
        REQUIRE(sim.Start(opts()).ok);
        const auto r = sim.Stop(3241);
        INFO(r.detail);
        CHECK(r.ok);
        CHECK(contains(r.detail, "stopped"));
        CHECK_FALSE(f.terminated);
        CHECK(f.closed);
        CHECK_FALSE(sim.Owns());
        bool asked = false;
        for (const auto& c : f.httpCalls) asked = asked || c == "POST /shutdown";
        CHECK(asked);
    }
    SUBCASE("our own process that ignores the request is terminated") {
        FakeSim f;
        SimProcess sim(f.deps());
        REQUIRE(sim.Start(opts()).ok);
        f.ignoresShutdown = true;
        const auto r = sim.Stop(3241, 500);
        CHECK(r.ok);
        CHECK(contains(r.detail, "terminated"));
        CHECK(f.terminated);
        CHECK_FALSE(sim.Owns());
    }
    SUBCASE("our own process that hangs (does not answer at all) is terminated") {
        FakeSim f;
        SimProcess sim(f.deps());
        REQUIRE(sim.Start(opts()).ok);
        f.hung = true;
        const auto r = sim.Stop(3241, 500);
        CHECK(r.ok);
        CHECK(f.terminated);
    }
    SUBCASE("a simulator somebody else started is asked, never killed") {
        FakeSim f;
        f.alreadyRunning = true;
        SimProcess sim(f.deps());
        const auto r = sim.Stop(3241);
        CHECK(r.ok);
        CHECK_FALSE(f.terminated);
        CHECK_FALSE(f.alreadyRunning); // it honoured the request
    }
    SUBCASE("a foreign simulator that ignores the request is reported, not terminated") {
        FakeSim f;
        f.alreadyRunning = true;
        f.ignoresShutdown = true;
        SimProcess sim(f.deps());
        const auto r = sim.Stop(3241, 500);
        CHECK_FALSE(r.ok);
        CHECK(contains(r.detail, "was not killed"));
        CHECK_FALSE(f.terminated);
    }
    SUBCASE("nothing running is not an error") {
        FakeSim f;
        SimProcess sim(f.deps());
        const auto r = sim.Stop(3241);
        CHECK(r.ok);
        CHECK(contains(r.detail, "no simulator is running"));
        CHECK_FALSE(f.terminated);
    }
}

// ---------------------------------------------------------------- with the real uacsimd.exe

TEST_CASE("sim process (real uacsimd.exe): start, answer, /shutdown ends the process") {
    SimStartOptions o;
    o.exeDir = this_exe_dir();
    REQUIRE(fs::exists(fs::path(o.exeDir) / "uacsimd.exe"));
    o.httpPort = free_port();
    o.usbipPort = free_port();
    o.profilesDir = std::string(UAC_SOURCE_DIR) + "/profiles";
    SimProcess sim;
    const auto started = sim.Start(o);
    INFO(started.detail);
    REQUIRE(started.ok);
    CHECK(sim.Owns());
    CHECK_FALSE(http_req(o.httpPort, "GET", "/status", "").empty());
    // Control: before Stop it is still there a moment later (it does not just exit by itself)
    Sleep(600);
    CHECK(sim.Owns());
    CHECK_FALSE(http_req(o.httpPort, "GET", "/status", "").empty());

    const auto stopped = sim.Stop(o.httpPort);
    INFO(stopped.detail);
    CHECK(stopped.ok);
    CHECK(stopped.detail == "the simulator stopped"); // by itself, not terminated
    CHECK_FALSE(sim.Owns());
    CHECK(http_req(o.httpPort, "GET", "/status", "").empty());
}

TEST_CASE("sim process (real uacsimd.exe): it ends by itself when its parent ends") {
    // A short-lived parent: cmd running a ~2 s ping
    wchar_t cmd[] = L"cmd.exe /c ping -n 3 127.0.0.1 >nul";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    REQUIRE(CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    CloseHandle(pi.hThread);

    SimStartOptions o;
    o.exeDir = this_exe_dir();
    o.httpPort = free_port();
    o.usbipPort = free_port();
    o.profilesDir = std::string(UAC_SOURCE_DIR) + "/profiles";
    o.parentPid = pi.dwProcessId;
    SimProcess sim;
    const auto started = sim.Start(o);
    INFO(started.detail);
    REQUIRE(started.ok);
    // While the parent lives, the simulator lives (otherwise the check below proves nothing)
    const bool parentStillRunning = WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT;
    CHECK(parentStillRunning);
    CHECK(sim.Owns());

    REQUIRE(WaitForSingleObject(pi.hProcess, 15000) == WAIT_OBJECT_0);
    CloseHandle(pi.hProcess);
    // ... and once the parent is gone it follows within a moment
    bool gone = false;
    for (int i = 0; i < 40 && !gone; ++i) {
        gone = !sim.Owns();
        if (!gone) Sleep(100);
    }
    CHECK(gone);
    CHECK(http_req(o.httpPort, "GET", "/status", "").empty());
}
