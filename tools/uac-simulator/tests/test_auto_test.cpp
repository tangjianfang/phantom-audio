// 一键自动测试框架本身的单测。
// api.* 用例对着进程内的 SimHost 真跑（这同时是对 SimHost 的端到端测试）；host.* 用例需要
// 驱动与 Windows 音频栈，这里只验证它们"在不该跑的时候会 SKIP 而不是失败或误动系统"。
// 运行器的超时/异常/清理语义用自定义用例表检验——这些逻辑不能等到 Sandbox 里才第一次运行。
#include <doctest/doctest.h>
#include <auto_test.h>
#include <driver_installer.h>
#include <sim_host.h>
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace uacstudio;
namespace fs = std::filesystem;

namespace {

std::string report_dir() {
    char tmp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmp);
    return (fs::path(tmp) / "uac_autotest_unit").string();
}

AutoTestOptions options_for(std::uint16_t port) {
    AutoTestOptions o;
    o.port = port;
    o.reportDir = report_dir();
    o.caseTimeoutSec = 20;
    o.allowHost = false;
    return o;
}

const CheckResult* find(const AutoTestOutcome& out, const std::string& id) {
    for (const auto& r : out.results)
        if (r.id == id) return &r;
    return nullptr;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// ---- 自定义用例（函数指针：不能捕获）----
void CasePass(Ctx& c) { c.Note("all good"); }
void CaseFail(Ctx& c) { c.Fail("deliberate failure"); }
void CaseSkip(Ctx& c) { c.Skip("deliberate skip"); }
void CaseThrows(Ctx&) { throw std::runtime_error("boom"); }
void CaseExpect(Ctx& c) { c.Expect(1 + 1 == 3, "arithmetic broke"); }
void CaseHang(Ctx&) { Sleep(3000); }
std::atomic<int> g_cleanupRan{0};
void CaseCleanupMarks(Ctx&) { ++g_cleanupRan; }

CaseDef def(const char* id, CaseFn fn, int timeoutSec = 0) {
    return CaseDef{{id, "test", "unit test case"}, fn, timeoutSec};
}

} // namespace

TEST_CASE("auto test: case table is well formed") {
    const auto& cases = Cases();
    REQUIRE(cases.size() > 5);
    CHECK(cases.front().info.id == "env.api");
    CHECK(cases.back().info.id == "cleanup.devices"); // 清理在最后，且只有它

    std::set<std::string> ids;
    int host = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        INFO("case " << c.info.id);
        CHECK(ids.insert(c.info.id).second);       // id 唯一
        CHECK_FALSE(c.info.feature.empty());        // 每项有功能分组
        CHECK_FALSE(c.info.title.empty());          // 和说明
        CHECK(c.fn != nullptr);                     // 表里的每个 id 都有函数
        CHECK(c.info.id.find('.') != std::string::npos); // "area.name"
        if (starts_with(c.info.id, "cleanup.")) CHECK(i == cases.size() - 1);
        if (starts_with(c.info.id, "host.")) ++host;
    }
    CHECK(host >= 3);
    CHECK(ListChecks().size() == cases.size());
    CHECK_FALSE(ManualChecklist().empty()); // 做不到自动验证的事项不能悄悄省略
}

TEST_CASE("auto test: api cases pass against a real in-process simulator, host cases skip") {
    uac::SimHost host(std::string(UAC_SOURCE_DIR) + "/profiles");
    REQUIRE(host.start(0, 0));
    const auto out = RunAutoTest(options_for(host.http_port()));

    for (const auto& r : out.results) {
        INFO(r.id << ": " << r.detail);
        if (starts_with(r.id, "host.") || r.id == "env.driver") {
            CHECK(r.status == CheckStatus::Skip); // 没有 --allow-host：不得误动系统
            CHECK_FALSE(r.detail.empty());        // SKIP 必须说明原因
        } else {
            CHECK(r.status == CheckStatus::Pass);
        }
    }
    CHECK(out.failed == 0);
    CHECK(out.passed + out.skipped == static_cast<int>(out.results.size()));
    CHECK(out.results.size() == Cases().size());
    CHECK(host.instance_count() == 0); // cleanup.devices 把仿真器归零

    // 报告：RESULT 行在最前、每个用例一行、人工确认清单、完整日志
    REQUIRE(fs::exists(out.reportPath));
    const auto text = read_file(out.reportPath);
    CHECK(starts_with(text, "RESULT: "));
    std::ostringstream want;
    want << "RESULT: " << out.passed << " passed, " << out.failed << " failed, " << out.skipped << " skipped";
    CHECK(text.find(want.str()) == 0);
    for (const auto& c : Cases()) CHECK(text.find(c.info.id) != std::string::npos);
    CHECK(text.find("NEEDS HUMAN CONFIRMATION") != std::string::npos);
    CHECK(text.find("---- full log ----") != std::string::npos);
    fs::remove(out.reportPath);

    host.stop();
}

TEST_CASE("auto test: api.volume has teeth - it fails if the device ignores the event") {
    // 把仿真器换成"吞掉事件"的假服务：api.volume 必须失败，而不是恒真
    uac::ControlApi api;
    uac::ControlCallbacks cb;
    cb.status = [] { return std::string(R"({"running":true,"version":"fake","volCurDb":0.0,"mute":false,"listening":true,"instances":1,"sampleRate":48000})"); };
    cb.event = [](const std::string&) { return std::string(R"({"ok":true})"); }; // 回 ok，但状态不变
    cb.detach = [] { return std::string(R"({"ok":true})"); };
    cb.attach = [](const std::string&) { return std::string(R"({"attached":"uac2-48k","instances":1,"busid":"1-1"})"); };
    REQUIRE(api.start(0, cb));
    auto o = options_for(api.port());
    const auto out = RunAutoTest(o);
    const auto* vol = find(out, "api.volume");
    REQUIRE(vol != nullptr);
    CHECK(vol->status == CheckStatus::Fail);
    CHECK(vol->detail.find("did not lower") != std::string::npos);
    const auto* mute = find(out, "api.mute");
    REQUIRE(mute != nullptr);
    CHECK(mute->status == CheckStatus::Fail); // 同理：mute 状态没翻转
    CHECK(out.failed >= 2);
    fs::remove(out.reportPath);
}

TEST_CASE("auto test: unreachable simulator fails env.api once and skips the rest") {
    std::uint16_t deadPort = 0;
    {
        uac::SimHost tmp(std::string(UAC_SOURCE_DIR) + "/profiles");
        REQUIRE(tmp.start(0, 0));
        deadPort = tmp.http_port();
        tmp.stop(); // 端口不再有人监听
    }
    const auto out = RunAutoTest(options_for(deadPort));
    const auto* api = find(out, "env.api");
    REQUIRE(api != nullptr);
    CHECK(api->status == CheckStatus::Fail);
    CHECK(api->detail.find("no response") != std::string::npos);
    CHECK(out.failed == 1); // 只有一个失败，而不是一串雪崩
    for (const auto& r : out.results) {
        if (r.id == "env.api") continue;
        INFO(r.id << ": " << r.detail);
        CHECK(r.status != CheckStatus::Fail);
    }
    const auto* vol = find(out, "api.volume");
    REQUIRE(vol != nullptr);
    CHECK(vol->detail.find("control API unreachable") != std::string::npos);
    fs::remove(out.reportPath);
}

TEST_CASE("auto test runner: pass / fail / skip / exception map to statuses and later cases still run") {
    const std::vector<CaseDef> cases = {def("t.pass", CasePass), def("t.fail", CaseFail),
                                        def("t.skip", CaseSkip), def("t.throws", CaseThrows),
                                        def("t.expect", CaseExpect), def("t.after", CasePass)};
    const auto out = RunCases(options_for(1), cases);
    REQUIRE(out.results.size() == 6);
    CHECK(out.results[0].status == CheckStatus::Pass);
    CHECK(out.results[0].detail == "all good");
    CHECK(out.results[1].status == CheckStatus::Fail);
    CHECK(out.results[1].detail == "deliberate failure");
    CHECK(out.results[2].status == CheckStatus::Skip);
    CHECK(out.results[2].detail == "deliberate skip");
    CHECK(out.results[3].status == CheckStatus::Fail);
    CHECK(out.results[3].detail.find("unexpected error: boom") == 0);
    CHECK(out.results[4].status == CheckStatus::Fail);
    CHECK(out.results[4].detail == "arithmetic broke");
    CHECK(out.results[5].status == CheckStatus::Pass); // 前面失败不影响后面的用例
    CHECK(out.passed == 2);
    CHECK(out.failed == 3);
    CHECK(out.skipped == 1);
    fs::remove(out.reportPath);
}

TEST_CASE("auto test runner: a hung case is a timed-out FAIL, later cases are skipped, cleanup still runs") {
    g_cleanupRan = 0;
    const std::vector<CaseDef> cases = {def("t.ok", CasePass), def("t.hang", CaseHang, 1),
                                        def("t.never", CasePass), def("cleanup.x", CaseCleanupMarks)};
    const auto t0 = std::chrono::steady_clock::now();
    const auto out = RunCases(options_for(1), cases);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    CHECK(ms < 2800); // 1 s 超时就放弃，没有等满 3 s 的睡眠
    REQUIRE(out.results.size() == 4);
    CHECK(out.results[0].status == CheckStatus::Pass);
    CHECK(out.results[1].status == CheckStatus::Fail);
    CHECK(out.results[1].detail.find("no answer within 1 s") == 0);
    CHECK(out.results[2].status == CheckStatus::Skip);
    CHECK(out.results[2].detail.find("earlier case did not return") != std::string::npos);
    CHECK(out.results[3].status == CheckStatus::Pass); // 清理照跑
    CHECK(g_cleanupRan == 1);
    CHECK(fs::exists(out.reportPath));                  // 报告照写
    fs::remove(out.reportPath);
    Sleep(2300); // 让被放弃的工作线程跑完，免得它在进程退出时还在睡眠
}

TEST_CASE("auto test runner: cancel skips remaining cases but not cleanup") {
    std::atomic<bool> cancel{true};
    auto o = options_for(1);
    o.cancel = &cancel;
    g_cleanupRan = 0;
    // 第一个用例先于取消检查运行？不——取消在每个用例前检查，所以全部（清理除外）被跳过。
    // 清理要求"至少跑过一个用例"，这里一个都没跑，所以也跳过：不去清理没建过的东西。
    const auto out = RunCases(o, {def("t.a", CasePass), def("cleanup.x", CaseCleanupMarks)});
    CHECK(out.results[0].status == CheckStatus::Skip);
    CHECK(out.results[0].detail.find("cancelled") != std::string::npos);
    CHECK(out.results[1].status == CheckStatus::Skip);
    CHECK(g_cleanupRan == 0);
    fs::remove(out.reportPath);
}

TEST_CASE("auto test: report format puts the machine-readable RESULT line first") {
    const std::vector<CheckResult> rs = {{"a.one", CheckStatus::Pass, "fine", 5},
                                         {"a.two", CheckStatus::Fail, "bad", 7},
                                         {"a.three", CheckStatus::Skip, "why", 0}};
    const auto text = FormatReport(rs, "LOGLINE\n");
    CHECK(text.find("RESULT: 1 passed, 1 failed, 1 skipped\n") == 0);
    CHECK(text.find("PASS  a.one") != std::string::npos);
    CHECK(text.find("FAIL  a.two") != std::string::npos);
    CHECK(text.find("SKIP  a.three") != std::string::npos);
    CHECK(text.find("LOGLINE") > text.find("NEEDS HUMAN CONFIRMATION")); // 完整日志在最后
}

TEST_CASE("auto test: host cases need --allow-host AND a disposable machine") {
    std::string why;
    CHECK_FALSE(HostCasesAllowed(false, &why));
    CHECK(why.find("--allow-host") != std::string::npos);

    // 守卫逻辑本身：允许 ⇔ (提权) 且 (一次性机器 或 显式强制)。这里不假设本机是哪种
    //（CI 的 runner 是 Hyper-V 虚拟机），也不假设测试进程是否提权——两侧都按实际值断言。
    const bool admin = IsProcessAdmin();
    _putenv_s("UACSIM_FORCE_HOST_TESTS", "");
    const bool disposable = IsDisposableMachine(&why);
    if (admin) {
        CHECK(HostCasesAllowed(true, &why) == disposable);
        if (!disposable) CHECK(why.find("refused") != std::string::npos);
    } else {
        CHECK_FALSE(HostCasesAllowed(true, &why)); // 非提权：一律 SKIP，usbip 只认管理员
        CHECK(why.find("administrator") != std::string::npos);
    }

    _putenv_s("UACSIM_FORCE_HOST_TESTS", "1");
    CHECK(HostCasesAllowed(true, &why) == admin); // 显式强制也越过不了提权要求
    if (!admin) CHECK(why.find("administrator") != std::string::npos);
    CHECK_FALSE(HostCasesAllowed(false, &why)); // 但仍然必须先有 --allow-host
    _putenv_s("UACSIM_FORCE_HOST_TESTS", "");
}

TEST_CASE("auto test: RunTool runs programs, reports exit code, merges stderr, enforces timeout") {
    char comspec[MAX_PATH]{};
    REQUIRE(GetEnvironmentVariableA("ComSpec", comspec, MAX_PATH) > 0);
    const std::string cmd = comspec;

    auto r = RunTool(cmd, "/c echo hello", 10000);
    CHECK(r.started);
    CHECK_FALSE(r.timedOut);
    CHECK(r.code == 0);
    CHECK(r.out.find("hello") != std::string::npos);

    r = RunTool(cmd, "/c exit 7", 10000);
    CHECK(r.started);
    CHECK(r.code == 7);

    r = RunTool(cmd, "/c echo to-stderr 1>&2", 10000);
    CHECK(r.out.find("to-stderr") != std::string::npos); // stderr 并入输出

    const auto t0 = std::chrono::steady_clock::now();
    r = RunTool(cmd, "/c ping -n 8 127.0.0.1 >nul", 500);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.timedOut);
    CHECK(ms < 4000); // 超时即终止，没等 ping 跑完

    r = RunTool("C:\\no\\such\\dir\\prog.exe", "", 1000);
    CHECK_FALSE(r.started);
    CHECK(r.out.find("cannot start") != std::string::npos);
}
