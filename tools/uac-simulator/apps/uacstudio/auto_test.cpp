// 一键自动测试实现：用例表、运行器（单用例线程 + 超时）、报告。设计见 auto_test.h。
#include "auto_test.h"
#include "host_ops.h"
#include <windows.h>
#include <shlobj.h>
#include <winsvc.h>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

namespace uacstudio {

using nlohmann::json;
namespace fs = std::filesystem;

const char* StatusName(CheckStatus s) {
    switch (s) {
    case CheckStatus::Pass: return "PASS";
    case CheckStatus::Fail: return "FAIL";
    default: return "SKIP";
    }
}

// ============================================================ 共享状态 / Ctx

struct Shared {
    AutoTestOptions opts;
    Fixture fx;
    std::atomic<bool> abandoned{false}; // 运行器已放弃（用例超时）：迟到的日志只进文件
    std::mutex logMtx;
    std::ofstream logFile; // 边跑边写、逐行 flush：进程崩了也留下现场
    std::string logText;

    void Log(const std::string& line) {
        std::lock_guard lk(logMtx);
        logText += line + "\n";
        if (logFile.is_open()) logFile << line << "\n" << std::flush;
        if (opts.log && !abandoned.load()) opts.log(line);
    }
};

void Ctx::Log(const std::string& line) { s_->Log(line); }
const AutoTestOptions& Ctx::opts() const { return s_->opts; }
Fixture& Ctx::fx() { return s_->fx; }
std::string Ctx::Get(const std::string& path) { return http_req(s_->opts.port, "GET", path, ""); }
std::string Ctx::Post(const std::string& path, const std::string& body,
                      const std::string& extraHeaders) {
    return http_req(s_->opts.port, "POST", path, body, extraHeaders);
}

namespace {

json ParseOrFail(Ctx& c, const std::string& what, const std::string& raw) {
    if (raw.empty())
        c.Fail(what + ": no response (is uacsimd running on port " + std::to_string(c.opts().port) +
               "?)");
    auto j = json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) c.Fail(what + ": response is not JSON: " + raw.substr(0, 200));
    return j;
}

} // namespace

json Ctx::GetJson(const std::string& path) { return ParseOrFail(*this, "GET " + path, Get(path)); }
json Ctx::PostJson(const std::string& path, const std::string& body) {
    return ParseOrFail(*this, "POST " + path, Post(path, body));
}

// ============================================================ 通用小工具

namespace {

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// UTF-8 字符串 <-> filesystem::path。不用 fs::u8path（C++20 起弃用，/WX 下是错误），
// 也不用 path::u8string()（C++20 起返回 std::u8string 而不是 std::string）。
fs::path P(const std::string& utf8) { return fs::path(ToWide(utf8)); }
std::string S(const fs::path& p) { return ToUtf8(p.wstring()); }

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string SelfExeDir() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return ToUtf8(fs::path(p).parent_path().wstring());
}

std::string TempDir() {
    wchar_t t[MAX_PATH]{};
    GetTempPathW(MAX_PATH, t);
    return ToUtf8(fs::path(t).wstring());
}

std::string DesktopDir() {
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)) && p) out = ToUtf8(p);
    CoTaskMemFree(p);
    return out;
}

bool DirWritable(const std::string& dir) {
    std::error_code ec;
    fs::create_directories(P(dir), ec);
    const auto probe = P(dir) / ".uacsim-write-probe";
    std::ofstream f(probe);
    const bool ok = f.is_open();
    f.close();
    fs::remove(probe, ec);
    return ok;
}

std::string RegString(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return {};
    wchar_t buf[256]{};
    DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
    std::string out;
    if (RegQueryValueExW(k, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz) ==
            ERROR_SUCCESS &&
        type == REG_SZ)
        out = ToUtf8(buf);
    RegCloseKey(k);
    return out;
}

bool ServiceInstalled(const wchar_t* name, std::string* state) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    bool ok = svc != nullptr;
    if (svc) {
        SERVICE_STATUS st{};
        if (QueryServiceStatus(svc, &st) && state)
            *state = st.dwCurrentState == SERVICE_RUNNING ? "running"
                     : st.dwCurrentState == SERVICE_STOPPED ? "stopped"
                                                            : "state " + std::to_string(st.dwCurrentState);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

// ---- 子进程：stdout+stderr 合并读入（读线程避免管道写满阻塞对端）----
class Proc {
public:
    explicit Proc(const std::wstring& cmdline) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) return;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        // 放进 Job Object：超时要杀整棵进程树（cmd.exe 的子进程会继续握着管道写端，
        // 只杀父进程的话读线程要等孙进程自己结束）；KILL_ON_JOB_CLOSE 保证本进程退出时不留孤儿。
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof(li));
        }
        std::wstring cl = cmdline; // CreateProcessW 可能改写缓冲区
        PROCESS_INFORMATION pi{};
        started_ = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi) != 0;
        if (!started_) startError_ = GetLastError();
        CloseHandle(wr);
        if (!started_) { CloseHandle(rd); return; }
        if (job_) AssignProcessToJobObject(job_, pi.hProcess); // 先入 Job 再恢复，子进程不会逃出去
        ResumeThread(pi.hThread);
        proc_ = pi.hProcess;
        CloseHandle(pi.hThread);
        reader_ = std::thread([this, rd] {
            char buf[4096];
            DWORD n = 0;
            while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) {
                std::lock_guard lk(m_);
                if (out_.size() < (1u << 20)) out_.append(buf, n);
            }
            CloseHandle(rd);
        });
    }
    ~Proc() {
        Kill();
        if (proc_) CloseHandle(proc_);
        if (job_) CloseHandle(job_);
        if (reader_.joinable()) reader_.join();
    }
    Proc(const Proc&) = delete;
    Proc& operator=(const Proc&) = delete;

    bool started() const { return started_; }
    DWORD startError() const { return startError_; }
    // 等待结束；超时则终止进程并标记 timedOut
    DWORD Finish(int timeoutMs) {
        if (!proc_) return 2;
        if (WaitForSingleObject(proc_, static_cast<DWORD>(timeoutMs)) == WAIT_TIMEOUT) {
            Kill();
            WaitForSingleObject(proc_, 5000);
            timedOut_ = true;
        }
        DWORD code = 0;
        GetExitCodeProcess(proc_, &code);
        if (reader_.joinable()) reader_.join();
        return code;
    }
    bool timedOut() const { return timedOut_; }
    std::string output() {
        std::lock_guard lk(m_);
        return out_;
    }

private:
    void Kill() { // 杀整棵进程树；没进 Job 时退化为只杀本进程
        if (!proc_ || WaitForSingleObject(proc_, 0) != WAIT_TIMEOUT) {
            if (job_) TerminateJobObject(job_, 1); // 父进程已退出也要清掉可能残留的子进程
            return;
        }
        if (job_) TerminateJobObject(job_, 1);
        else TerminateProcess(proc_, 1);
    }

    bool started_ = false, timedOut_ = false;
    DWORD startError_ = 0;
    HANDLE proc_ = nullptr;
    HANDLE job_ = nullptr;
    std::thread reader_;
    std::mutex m_;
    std::string out_;
};

std::wstring Quote(const std::string& s) { return L"\"" + ToWide(s) + L"\""; }

ToolResult RunToolImpl(const std::string& exe, const std::string& args, int timeoutMs) {
    Proc p(Quote(exe) + L" " + ToWide(args));
    ToolResult r;
    r.started = p.started();
    if (!r.started) {
        r.out = "cannot start " + exe + " (error " + std::to_string(p.startError()) + ")";
        return r;
    }
    r.code = p.Finish(timeoutMs);
    r.timedOut = p.timedOut();
    r.out = p.output();
    return r;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// host_probe enum 的每行 JSON：{"flow":"render|capture","id":"...","name":"..."}
struct Endpoint { std::string flow, id, name; };
bool EnumEndpoints(Ctx& c, std::vector<Endpoint>* out) {
    const auto r = RunTool(c.fx().probeExe, "enum", 20000);
    if (!r.started || r.timedOut || r.code != 0) {
        c.Log("host_probe enum failed: " + r.out);
        return false;
    }
    std::istringstream is(r.out);
    std::string line;
    while (std::getline(is, line)) {
        auto j = json::parse(Trim(line), nullptr, false);
        if (j.is_object() && j.contains("id"))
            out->push_back({j.value("flow", ""), j.value("id", ""), j.value("name", "")});
    }
    return true;
}

std::set<std::string> PortsFromUsbipPort(const std::string& text) { return ParseUsbipPorts(text); } // shared with host_ops

std::string FindUsbip() {
    wchar_t buf[MAX_PATH]{};
    if (SearchPathW(nullptr, L"usbip.exe", nullptr, MAX_PATH, buf, nullptr)) return ToUtf8(buf);
    wchar_t pf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
        const auto p = fs::path(pf) / L"USBip" / L"usbip.exe";
        if (fs::exists(p)) return ToUtf8(p.wstring());
    }
    return {};
}

} // namespace

ToolResult RunTool(const std::string& exe, const std::string& args, int timeoutMs) {
    return RunToolImpl(exe, args, timeoutMs);
}

// ============================================================ 一次性机器守卫

bool IsDisposableMachine(std::string* why) {
    auto set = [&](const std::string& s) { if (why) *why = s; return true; };
    wchar_t user[256]{};
    DWORD n = 256;
    if (GetUserNameW(user, &n) && std::wstring(user) == L"WDAGUtilityAccount")
        return set("Windows Sandbox (user WDAGUtilityAccount)");
    if (!RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Virtual Machine\\Guest\\Parameters",
                   L"VirtualMachineName")
             .empty())
        return set("Hyper-V guest");
    const auto manu = RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS",
                                L"SystemManufacturer");
    const auto prod = RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS",
                                L"SystemProductName");
    for (const char* tag : {"VMware", "innotek", "QEMU", "Xen", "Parallels"})
        if (manu.find(tag) != std::string::npos || prod.find(tag) != std::string::npos)
            return set(std::string("virtual machine (") + tag + ")");
    if (manu.find("Microsoft") != std::string::npos && prod.find("Virtual Machine") != std::string::npos)
        return set("Hyper-V virtual machine");
    if (why) *why = "not a recognised disposable machine (manufacturer=" + manu + ")";
    return false;
}

bool HostCasesAllowed(bool flag, std::string* why) {
    if (!flag) {
        if (why) *why = "host cases need --allow-host (they install nothing, but attach a virtual USB device to this Windows)";
        return false;
    }
    std::string dwhy;
    if (IsDisposableMachine(&dwhy)) {
        if (why) *why = dwhy;
        return true;
    }
    wchar_t v[8]{};
    if (GetEnvironmentVariableW(L"UACSIM_FORCE_HOST_TESTS", v, 8) && std::wstring(v) == L"1") {
        if (why) *why = "forced by UACSIM_FORCE_HOST_TESTS=1 (" + dwhy + ")";
        return true;
    }
    if (why)
        *why = "refused: " + dwhy + ". Host cases only run in Windows Sandbox / a VM; "
               "set UACSIM_FORCE_HOST_TESTS=1 to override on a machine you can throw away";
    return false;
}

// ============================================================ 用例：环境

namespace {

bool IsCleanup(const std::string& id) { return id.rfind("cleanup.", 0) == 0; }

void NeedHost(Ctx& c) {
    if (!c.fx().hostEnabled) c.Skip(c.fx().hostWhy);
}
void NeedDriver(Ctx& c) {
    NeedHost(c);
    if (!c.fx().driver) c.Skip("usbip-win2 driver (service usbip2_ude) is not installed");
}
void NeedTools(Ctx& c) {
    auto& fx = c.fx();
    if (fx.probeExe.empty() || !fs::exists(P(fx.probeExe)))
        c.Fail("host_probe.exe not found next to uacstudio.exe (" + fx.probeExe + ")");
    if (fx.usbipExe.empty())
        c.Fail("usbip.exe not found (driver is installed, so the usbip CLI should be too)");
}

void CaseEnvApi(Ctx& c) {
    const auto st = c.GetJson("/status");
    c.Expect(st.value("running", false), "status.running is not true: " + st.dump());
    c.fx().apiUp = true;
    c.Note("uacsimd " + st.value("version", std::string("?")) + " on port " +
           std::to_string(c.opts().port));
}

void CaseEnvMachine(Ctx& c) {
    std::string why;
    const bool disposable = IsDisposableMachine(&why);
    c.fx().hostEnabled = HostCasesAllowed(c.opts().allowHost, &c.fx().hostWhy);
    c.Note(std::string(disposable ? "disposable: " : "NOT disposable: ") + why +
           (c.fx().hostEnabled ? "; host cases ENABLED" : "; host cases off (" + c.fx().hostWhy + ")"));
}

void CaseEnvDriver(Ctx& c) {
    NeedHost(c);
    auto& fx = c.fx();
    std::string state;
    fx.driver = ServiceInstalled(L"usbip2_ude", &state);
    if (!fx.driver) c.Skip("usbip-win2 driver (service usbip2_ude) is not installed");
    const std::string dir = c.opts().exeDir.empty() ? SelfExeDir() : c.opts().exeDir;
    fx.probeExe = S(P(dir) / "host_probe.exe");
    fx.usbipExe = FindUsbip();
    c.Note("usbip2_ude " + state + "; usbip=" + (fx.usbipExe.empty() ? "?" : fx.usbipExe));
}

// ============================================================ 用例：控制 API（任何机器）

void CaseApiReset(Ctx& c) {
    c.Expect(c.PostJson("/detach").value("ok", false), "detach did not answer ok");
    const auto st = c.GetJson("/status");
    c.Expect(!st.value("listening", true) && st.value("instances", -1) == 0,
             "not idle after detach: " + st.dump());
}

void CaseApiAttach(Ctx& c) {
    const auto r = c.PostJson("/attach", R"({"profile":"uac2-48k"})");
    c.Expect(r.value("attached", "") == "uac2-48k" && r.value("instances", 0) == 1,
             "attach uac2-48k answered " + r.dump());
    const auto st = c.GetJson("/status");
    c.Expect(st.value("listening", false), "not listening after attach: " + st.dump());
    c.Expect(st.value("sampleRate", 0) == 48000, "sampleRate != 48000: " + st.dump());
    c.Note("busid " + r.value("busid", std::string("?")));
}

void CaseApiVolume(Ctx& c) {
    const double v0 = c.GetJson("/status").value("volCurDb", 99.0);
    c.Expect(c.PostJson("/event", R"({"type":"volume_down"})").value("ok", false), "volume_down failed");
    const double v1 = c.GetJson("/status").value("volCurDb", 99.0);
    // 每步 1 dB。必须真的变了——若固件把请求吞掉，下一条断言会失败，而不是恒真
    c.Expect(v1 < v0 - 0.5, "volume_down did not lower the level: " + std::to_string(v0) + " -> " + std::to_string(v1));
    c.Expect(c.PostJson("/event", R"({"type":"volume_up"})").value("ok", false), "volume_up failed");
    const double v2 = c.GetJson("/status").value("volCurDb", 99.0);
    c.Expect(v2 > v1 + 0.5, "volume_up did not raise the level: " + std::to_string(v1) + " -> " + std::to_string(v2));
    c.Note(std::to_string(v0) + " -> " + std::to_string(v1) + " -> " + std::to_string(v2) + " dB");
}

void CaseApiMute(Ctx& c) {
    const bool m0 = c.GetJson("/status").value("mute", false);
    c.Expect(c.PostJson("/event", R"({"type":"mute"})").value("ok", false), "mute toggle failed");
    const bool m1 = c.GetJson("/status").value("mute", false);
    c.Expect(m1 != m0, "mute did not toggle");
    c.Expect(c.PostJson("/event", R"({"type":"mute"})").value("ok", false), "mute toggle back failed");
    c.Expect(c.GetJson("/status").value("mute", false) == m0, "mute did not toggle back");
}

void CaseApiHid(Ctx& c) {
    for (const char* t : {"hid_vol_up", "hid_vol_down", "hid_mute", "hid_release"})
        c.Expect(c.PostJson("/event", std::string("{\"type\":\"") + t + "\"}").value("ok", false),
                 std::string("event ") + t + " failed");
    // 对照：未知事件必须被拒，否则上面的 ok 可能是恒真
    c.Expect(c.PostJson("/event", R"({"type":"definitely_not_an_event"})").contains("error"),
             "unknown event was not rejected");
}

void CaseApiLog(Ctx& c) {
    const auto log = c.GetJson("/log?n=200");
    c.Expect(log.is_array() && !log.empty(), "log is empty after events");
    bool vol = false;
    for (const auto& l : log)
        if (l.is_string() && l.get<std::string>().find("inject_volume") != std::string::npos) vol = true;
    c.Expect(vol, "no inject_volume record in the request log");
    c.Note(std::to_string(log.size()) + " records");
}

void CaseApiSource(Ctx& c) {
    c.Expect(c.PostJson("/source", R"({"kind":"sine","hz":440})").value("ok", false), "sine failed");
    c.Expect(c.PostJson("/source", R"({"kind":"silence"})").value("ok", false), "silence failed");
    c.Expect(c.PostJson("/source", R"({"kind":"nope"})").contains("error"), "unknown source kind accepted");
}

void CaseApiCapture(Ctx& c) {
    const std::string wav = S(P(TempDir()) / "uacstudio_autotest.wav");
    std::error_code ec;
    fs::remove(P(wav), ec);
    c.Expect(c.PostJson("/capture", json{{"path", wav}}.dump()).value("ok", false), "capture start failed");
    SleepMs(200);
    c.Expect(c.PostJson("/capture", R"({"path":""})").value("ok", false), "capture stop failed");
    c.Expect(fs::exists(P(wav), ec), "capture file was not created: " + wav);
    c.Expect(fs::file_size(P(wav), ec) >= 44, "capture file smaller than a WAV header");
    fs::remove(P(wav), ec);
    // 对照：非 .wav 目标必须被拒（路径策略有牙齿）
    const auto bad = c.PostJson(
        "/capture", json{{"path", S(P(TempDir()) / "uacstudio_autotest.txt")}}.dump());
    c.Expect(bad.contains("error"), "capture to a .txt path was accepted: " + bad.dump());
}

void CaseApiInject(Ctx& c) {
    for (const char* body : {R"({"kind":"stall","bRequest":129})", R"({"kind":"iso_error","everyN":2})",
                             R"({"kind":"delay_ms","ms":5})", R"({"kind":"clear"})"})
        c.Expect(c.PostJson("/inject", body).value("ok", false), std::string("inject failed: ") + body);
    c.Expect(c.PostJson("/inject", R"({"kind":"no_such_fault"})").contains("error"),
             "unknown fault kind accepted");
}

void CaseApiMulti(Ctx& c) {
    const int n0 = c.GetJson("/status").value("instances", 0);
    const auto a = c.PostJson("/attach", R"({"profile":"uac1-cmedia-sim"})");
    c.Expect(a.value("instances", 0) == n0 + 1, "attach uac1 did not add an instance: " + a.dump());
    c.Expect(c.GetJson("/status").value("profile", "") == "uac1-cmedia-sim", "status.profile not switched");
    const auto b = c.PostJson("/attach", R"({"profile":"uac2-48k"})");
    c.Expect(b.value("instances", 0) == n0 + 2, "second attach did not add an instance: " + b.dump());
    c.Note("instances " + std::to_string(n0) + " -> " + std::to_string(n0 + 2));
}

void CaseApiSecurity(Ctx& c) {
    // 回归：profile 名走白名单（旧黑名单漏掉反斜杠，UNC 名可穿透到 fs::exists）
    const char bs = 0x5C;
    const std::string names[] = {std::string(2, bs) + "localhost" + bs + "share" + bs + "x",
                                 std::string(1, bs) + "Windows" + bs + "win", "../x", "a/b", "C:foo"};
    for (const auto& n : names) {
        const auto r = c.PostJson("/attach", json{{"profile", n}}.dump());
        c.Expect(r.value("error", "") == "invalid profile name",
                 "profile name [" + n + "] was not rejected: " + r.dump());
    }
    // 浏览器跨域请求（带 Origin）必须在到达处理器之前被拒
    const auto cross = c.Post("/inject", R"({"kind":"clear"})",
                              "Origin: http://evil.example\r\nContent-Type: text/plain\r\n");
    c.Expect(cross.find("cross-origin") != std::string::npos, "request with Origin was not rejected: " + cross);
    // 路径策略：UNC / 上级目录
    const auto unc = c.PostJson("/capture", json{{"path", std::string(2, bs) + "localhost" + bs + "s" + bs + "a.wav"}}.dump());
    c.Expect(unc.contains("error"), "UNC capture path was accepted: " + unc.dump());
}

// ============================================================ 用例：宿主侧（Sandbox / VM）

// attach 成功但音频端点没出现时，结果文件必须自己说明现场，而不是只留一句"没出现"。
// 来历：第一次 Sandbox 运行，UAC1 设备被 usbaudio.sys 认领并探测了全部控件、UAC2 设备毫无反应，
// 两者都"30 秒内没出现端点"——光凭那一行失败信息分不出是驱动没起来、端点被建成了禁用状态，还是这个会话看不到本地音频硬件。
// 全部只读：host_probe 枚举、服务状态、CIM 查询。
// Returns true when this is a remote (RDP-style) session - Windows Sandbox is one.
bool DumpAttachDiagnostics(Ctx& c) {
    // Each step is announced BEFORE it runs: the last line on the screen then names the step that is slow, instead of the
    // console going silent (a silent minute looks exactly like a hang).
    auto announce = [&](const std::string& title) { c.Log("diagnostics: " + title + " ..."); };
    auto dump = [&](const std::string& title, const ToolResult& r) {
        c.Log("---- diagnostics: " + title + (r.started ? "" : " (could not start)") + (r.timedOut ? " (timed out)" : "") + " ----\n" +
              Trim(r.out));
    };
    announce("session");
    const auto session = RunTool(c.fx().probeExe, "session", 15000);
    dump("session (remote sessions may hide local audio hardware)", session);
    const bool remote = session.out.find("\"remoteSession\":true") != std::string::npos;
    announce("audio endpoints in all states");
    dump("audio endpoints in ALL states", RunTool(c.fx().probeExe, "enum --all", 20000));
    announce("uacsimd /status");
    dump("uacsimd /status (imported=true means the Windows host really claimed the device)", ToolResult{0, true, false, c.Get("/status")});
    announce("usbip port");
    dump("usbip port", RunTool(c.fx().usbipExe, "port", 15000));
    announce("PnP nodes via WMI (the first WMI query on a fresh machine can take 20-30 s)");
    const std::string ps =
        "-NoProfile -NonInteractive -Command \"Get-Service Audiosrv,AudioEndpointBuilder -ErrorAction SilentlyContinue | "
        "ForEach-Object { 'service {0} = {1}' -f $_.Name, $_.Status }; "
        "Get-CimInstance Win32_PnPEntity | Where-Object { $_.DeviceID -like 'USB\\*' -or $_.PNPClass -in 'MEDIA','AudioEndpoint' } | "
        "ForEach-Object { 'pnp {0} | service={1} | status={2} | problem={3} | class={4}' -f $_.DeviceID, $_.Service, $_.Status, "
        "$_.ConfigManagerErrorCode, $_.PNPClass }\"";
    dump("audio services and USB / media PnP nodes (problem 0 = started OK)", RunTool("powershell.exe", ps, 40000));
    return remote;
}

void CaseHostAttach(Ctx& c) {
    NeedDriver(c);
    NeedTools(c);
    auto& fx = c.fx();
    std::vector<Endpoint> before;
    c.Expect(EnumEndpoints(c, &before), "host_probe enum failed before attach");
    fx.baselineIds.clear();
    for (const auto& e : before) fx.baselineIds.insert(e.id);

    c.Expect(c.PostJson("/detach").value("ok", false), "detach failed");
    const std::string profile = c.opts().hostProfile;
    const auto att = c.PostJson("/attach", json{{"profile", profile}}.dump());
    c.Expect(att.value("attached", "") == profile, "attach " + profile + " failed: " + att.dump());

    const auto portsBefore = RunTool(fx.usbipExe, "port", 15000);
    fx.usbipPortsBefore = PortsFromUsbipPort(portsBefore.out);

    fx.hostTouched = true;
    const auto r = RunTool(fx.usbipExe, "attach -r 127.0.0.1 -b 1-1", 30000);
    c.Log("usbip attach -> exit " + std::to_string(r.code) + ": " + Trim(r.out));
    c.Expect(r.started && !r.timedOut && r.code == 0,
             "usbip attach failed (exit " + std::to_string(r.code) + "): " + Trim(r.out));
    fx.attached = true;

    const auto portsAfter = RunTool(fx.usbipExe, "port", 15000);
    for (const auto& p : PortsFromUsbipPort(portsAfter.out))
        if (!fx.usbipPortsBefore.count(p)) fx.usbipPortsOurs.insert(p);

    // 新端点出现需要几秒（PnP 枚举 + 类驱动绑定 + 音频端点创建）
    // The wait can run the full 30 s when no endpoint ever appears (that is what Windows Sandbox does), so say that it is
    // waiting and report progress: a console that stays silent for 30+ s was reported as a hang.
    const long long start = NowMs();
    const long long deadline = start + 30000;
    long long nextNote = start + 5000;
    std::size_t active = 0;
    c.Log("waiting up to 30 s for a new render + capture endpoint (progress every 5 s) ...");
    for (;;) {
        std::vector<Endpoint> now;
        if (EnumEndpoints(c, &now)) {
            active = now.size();
            fx.newRenderId.clear();
            fx.newCaptureId.clear();
            for (const auto& e : now) {
                if (fx.baselineIds.count(e.id)) continue;
                (e.flow == "render" ? fx.newRenderId : fx.newCaptureId) = e.id;
            }
            if (!fx.newRenderId.empty() && !fx.newCaptureId.empty()) break;
        }
        if (NowMs() > deadline) break;
        if (NowMs() >= nextNote) {
            c.Log("  ... " + std::to_string((NowMs() - start) / 1000) + " s: " + std::to_string(active) + " active endpoint(s), " +
                  (fx.newRenderId.empty() ? "no new render" : "new render found") + ", " +
                  (fx.newCaptureId.empty() ? "no new capture" : "new capture found"));
            nextNote += 5000;
        }
        SleepMs(1000);
    }
    bool remoteSession = false;
    if (fx.newRenderId.empty() || fx.newCaptureId.empty()) remoteSession = DumpAttachDiagnostics(c);
    c.Expect(!fx.newRenderId.empty() && !fx.newCaptureId.empty(),
             std::string("after usbip attach the Windows audio stack did not show a new ") +
                 (fx.newRenderId.empty() ? "render (speaker)" : "") +
                 (fx.newRenderId.empty() && fx.newCaptureId.empty() ? " and " : "") +
                 (fx.newCaptureId.empty() ? "capture (microphone)" : "") + " endpoint within 30 s" +
                 // Not a pass and not a skip - the case stays red - but say where to look: in a remote session Windows
                 // normally shows only the redirected "Remote Audio" device, so a missing endpoint does not by itself
                 // mean the driver failed (host.driver_binding says whether it did). Seen in the Sandbox, 2026-10-30.
                 (remoteSession ? ". NOTE: this is a REMOTE session (no console session); Windows usually exposes only the redirected "
                                  "'Remote Audio' endpoint here. Check host.driver_binding, and repeat in a console session "
                                  "(Hyper-V VM, basic session) - see docs/sandbox.md"
                                : ""));
    c.Note(profile + ": new render " + fx.newRenderId.substr(0, 40) + "..., capture " +
           fx.newCaptureId.substr(0, 40) + "...");
}

// 内置类驱动：UAC2 -> usbaudio2.sys，UAC1 -> usbaudio.sys（RFC 1.4；这是方案成立的前提）
std::string ExpectedClassDriver(const std::string& profile) {
    if (profile.rfind("uac2", 0) == 0) return "usbaudio2";
    if (profile.rfind("uac1", 0) == 0) return "usbaudio";
    return {};
}

void CaseHostDriverBinding(Ctx& c) {
    NeedDriver(c);
    auto& fx = c.fx();
    // Only needs the device to be attached, not for audio endpoints to exist: "does the built-in class driver claim the
    // device" and "does the audio stack turn it into endpoints" are separate questions, and when the second fails the first
    // is exactly what tells you where it broke.
    if (!fx.attached) c.Skip("device not attached (usbip attach did not succeed in host.attach_enum)");
    const std::string want = ExpectedClassDriver(c.opts().hostProfile);
    if (want.empty()) c.Skip("no expected class driver known for profile " + c.opts().hostProfile);
    // 列出所有 USB 即插即用设备及其绑定的服务；复合设备的父节点是 usbccgp，音频接口子节点才是 usbaudio*
    const std::string ps =
        "-NoProfile -NonInteractive -Command \"Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'USB\\VID_*' } | "
        "ForEach-Object { $s = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Service "
        "-ErrorAction SilentlyContinue).Data; '{0}|{1}|{2}' -f $_.InstanceId, $s, $_.Status }\"";
    const auto r = RunTool("powershell.exe", ps, 60000);
    c.Log("PnP USB devices:\n" + Trim(r.out));
    c.Expect(r.started && !r.timedOut && r.code == 0, "Get-PnpDevice failed (exit " + std::to_string(r.code) + "): " + Trim(r.out));
    bool found = false;
    std::string services;
    std::istringstream is(r.out);
    std::string line;
    while (std::getline(is, line)) {
        line = Trim(line);
        const auto a = line.find('|');
        const auto b = line.find('|', a == std::string::npos ? 0 : a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        const std::string svc = line.substr(a + 1, b - a - 1);
        services += (services.empty() ? "" : ", ") + svc;
        if (svc == want) found = true; // 精确匹配：usbaudio 不能顶替 usbaudio2
    }
    c.Expect(found, "no USB device node is bound to " + want + ".sys (services seen: " + services +
                        "). The simulated device is not being claimed by the built-in class driver.");
    c.Note("bound to " + want + ".sys");
}

void CaseHostVolumeEvent(Ctx& c) {
    NeedDriver(c);
    auto& fx = c.fx();
    if (fx.newRenderId.empty()) c.Skip("no new render endpoint (host.attach_enum did not pass)");
    // 对照：不注入任何事件时，监听器不应报告音量变化（证明下面的命中来自注入）
    {
        const auto quiet = RunTool(fx.probeExe, "volume-watch --seconds 3 --endpoint " + fx.newRenderId, 20000);
        c.Expect(quiet.started && quiet.code == 0, "volume-watch (baseline) failed: " + Trim(quiet.out));
        c.Expect(quiet.out.find("\"event\":\"volume\"") == std::string::npos,
                 "volume notification arrived without any injected event: " + Trim(quiet.out));
    }
    Proc watch(Quote(fx.probeExe) + L" volume-watch --seconds 8 --endpoint " + ToWide(fx.newRenderId));
    c.Expect(watch.started(), "cannot start host_probe volume-watch");
    SleepMs(2500); // 等监听器注册完成
    c.Expect(c.PostJson("/event", R"({"type":"volume_down"})").value("ok", false), "volume_down failed");
    SleepMs(1000);
    c.Expect(c.PostJson("/event", R"({"type":"mute"})").value("ok", false), "mute failed");
    const DWORD code = watch.Finish(20000);
    const auto out = watch.output();
    c.Log("volume-watch output: " + Trim(out));
    c.Expect(code == 0, "volume-watch exited with " + std::to_string(code));
    // UAC2 的 Status Word 路径有官方文档背书（RFC 1.3）；UAC1 没有（R-006）：用 uac1 profile
    // 再跑一遍，这条的结果就是 R-006 的实测答案——失败不一定是 bug，可能就是 usbaudio.sys 不响应。
    const bool uac1 = c.opts().hostProfile.rfind("uac1", 0) == 0;
    c.Expect(out.find("\"event\":\"volume\"") != std::string::npos,
             "Windows reported no volume/mute change on the new render endpoint after the device injected "
             "Status Word events on profile " + c.opts().hostProfile +
                 (uac1 ? " (R-006: usbaudio.sys may simply not react to Status Word; use the HID channel instead)"
                       : " (UAC2 interrupt endpoint path: usbaudio2.sys is documented to support it)"));
    c.Note("volume callback received on " + c.opts().hostProfile);
}

void CaseHostCaptureTone(Ctx& c) {
    NeedDriver(c);
    auto& fx = c.fx();
    if (fx.newCaptureId.empty()) c.Skip("no new capture endpoint (host.attach_enum did not pass)");
    c.Expect(c.PostJson("/source", R"({"kind":"sine","hz":1000,"amp":0.5})").value("ok", false), "set sine source failed");
    const auto hit = RunTool(fx.probeExe, "capture --freq 1000 --seconds 3 --endpoint " + fx.newCaptureId, 30000);
    c.Log("capture 1000 Hz -> exit " + std::to_string(hit.code) + ": " + Trim(hit.out));
    c.Expect(hit.started && !hit.timedOut, "host_probe capture did not finish: " + Trim(hit.out));
    c.Expect(hit.code == 0, "1 kHz tone not detected on the simulated microphone (exit " + std::to_string(hit.code) + "): " + Trim(hit.out));
    // 对照：同一路流里找 3 kHz 必须检不出——证明 Goertzel 判定不是恒真
    const auto miss = RunTool(fx.probeExe, "capture --freq 3000 --seconds 2 --endpoint " + fx.newCaptureId, 30000);
    c.Expect(miss.code == 1, "3 kHz was 'detected' in a 1 kHz stream (exit " + std::to_string(miss.code) + "): the detector is not discriminating");
    c.Note("1 kHz detected, 3 kHz correctly absent");
}

// ============================================================ 用例：清理

// The GUI's "Attach to Windows" / "Detach from Windows" buttons call AttachToWindows / DetachFromWindows (host_ops.h). This
// runs the same functions on the target system and checks the result with `usbip port` itself, so the buttons are covered
// by the same one-key test as everything else. Needs the device to have been attached by host.attach_enum.
void CaseHostOpsRoundtrip(Ctx& c) {
    NeedDriver(c);
    NeedTools(c);
    auto& fx = c.fx();
    if (!fx.attached) c.Skip("device not attached (usbip attach did not succeed in host.attach_enum)");
    const InstallOptions opt; // default install dir, like the GUI
    const unsigned usbipPort = 3240;
    auto oursNow = [&] { return ParseOurUsbipPorts(RunTool(fx.usbipExe, "port", 15000).out, usbipPort); };

    // 1) Start from "not attached to Windows", through the code of the Detach button
    const HostOpResult d0 = DetachFromWindows(usbipPort, opt);
    c.Log("Detach from Windows (first):\n" + d0.Summary());
    c.Expect(d0.ok, "Detach from Windows failed:\n" + d0.Summary());
    c.Expect(oursNow().empty(), "usbip port still lists this simulator's device after Detach from Windows");

    // 2) Fresh simulator side (a Windows client that went away may or may not leave the exporter listening)
    c.Expect(c.PostJson("/detach").value("ok", false), "simulator detach failed");
    const std::string profile = c.opts().hostProfile;
    const auto att = c.PostJson("/attach", json{{"profile", profile}}.dump());
    c.Expect(att.value("attached", "") == profile, "attach " + profile + " failed: " + att.dump());

    // 3) The Attach button's code
    const HostOpResult a = AttachToWindows(c.opts().port, "1-1", opt);
    c.Log("Attach to Windows:\n" + a.Summary());
    c.Expect(a.ok, "Attach to Windows failed:\n" + a.Summary());
    const auto ours = oursNow();
    c.Expect(!ours.empty(), "after Attach to Windows, usbip port does not list this simulator's device");
    for (const auto& p : ours) fx.usbipPortsOurs.insert(p); // so that cleanup knows about it

    // 4) And the Detach button's code again
    const HostOpResult d1 = DetachFromWindows(usbipPort, opt);
    c.Log("Detach from Windows (second):\n" + d1.Summary());
    c.Expect(d1.ok, "Detach from Windows failed:\n" + d1.Summary());
    c.Expect(oursNow().empty(), "usbip port still lists this simulator's device after the second Detach from Windows");
    c.Note("Attach to Windows / Detach from Windows work end to end (same code as the GUI buttons)");
}

void CaseCleanup(Ctx& c) {
    auto& fx = c.fx();
    std::string problems;
    // 1) Only the ports of this simulator - never `usbip detach -a`: on a machine that is not purely disposable (a test PC the
    //    user chose) that would also drop usbip devices that are none of our business. Ports recorded at attach time that
    //    are already gone (the GUI-operations case detaches them) are skipped, not counted as failures.
    if (fx.hostTouched && !fx.usbipExe.empty()) {
        const auto listed = RunTool(fx.usbipExe, "port", 15000).out;
        std::set<std::string> todo(fx.usbipPortsOurs.begin(), fx.usbipPortsOurs.end());
        for (const auto& p : ParseOurUsbipPorts(listed)) todo.insert(p);
        const auto stillThere = ParseUsbipPorts(listed);
        for (const auto& p : todo) {
            if (!stillThere.count(p)) {
                c.Log("usbip port " + p + " is already detached");
                continue;
            }
            const auto r = RunTool(fx.usbipExe, "detach -p " + p, 20000);
            c.Log("usbip detach -p " + p + " -> exit " + std::to_string(r.code) + ": " + Trim(r.out));
            if (r.code != 0) problems += "usbip detach -p " + p + " failed; ";
        }
    }
    // 2) 仿真器侧归零
    const auto d = c.PostJson("/detach");
    if (!d.value("ok", false)) problems += "simulator detach failed; ";
    const auto st = c.GetJson("/status");
    if (st.value("listening", true) || st.value("instances", -1) != 0) problems += "simulator not idle: " + st.dump() + "; ";
    // 3) 宿主侧端点回到基线（有牙齿：残留的新端点 = 清理失败）
    if (fx.hostTouched && !fx.probeExe.empty()) {
        const long long deadline = NowMs() + 20000;
        std::vector<std::string> leftover;
        for (;;) {
            std::vector<Endpoint> now;
            leftover.clear();
            if (EnumEndpoints(c, &now))
                for (const auto& e : now)
                    if (!fx.baselineIds.count(e.id)) leftover.push_back(e.id);
            if (leftover.empty() || NowMs() > deadline) break;
            SleepMs(1000);
        }
        if (!leftover.empty()) problems += std::to_string(leftover.size()) + " new audio endpoint(s) still present after detach; ";
    }
    c.Expect(problems.empty(), problems);
}

} // namespace

// ============================================================ 用例表

const std::vector<CaseDef>& Cases() {
    static const std::vector<CaseDef> kCases = {
        {{"env.api", "environment", "control API of uacsimd is reachable"}, CaseEnvApi, 15},
        {{"env.machine", "environment", "is this a disposable machine, and are host cases enabled"}, CaseEnvMachine, 15},
        {{"env.driver", "environment", "usbip-win2 driver installed (host cases only)"}, CaseEnvDriver, 15},
        {{"api.reset", "control-api", "detach brings the simulator back to idle"}, CaseApiReset, 0},
        {{"api.attach", "control-api", "attach uac2-48k: listening, 48 kHz"}, CaseApiAttach, 0},
        {{"api.volume", "device-events", "volume up/down events change the device level"}, CaseApiVolume, 0},
        {{"api.mute", "device-events", "mute event toggles and toggles back"}, CaseApiMute, 0},
        {{"api.hid", "device-events", "HID consumer-volume events accepted; unknown event rejected"}, CaseApiHid, 0},
        {{"api.log", "observability", "request log records the injected events"}, CaseApiLog, 0},
        {{"api.source", "audio-stream", "microphone source: sine / silence; bad kind rejected"}, CaseApiSource, 0},
        {{"api.capture", "audio-stream", "speaker capture writes a WAV; non-.wav target rejected"}, CaseApiCapture, 0},
        {{"api.inject", "fault-injection", "fault kinds accepted and cleared; unknown kind rejected"}, CaseApiInject, 0},
        {{"api.multi", "control-api", "second profile and second instance can be attached"}, CaseApiMulti, 0},
        {{"api.security", "hardening", "bad profile names, cross-origin requests and UNC paths are rejected"}, CaseApiSecurity, 0},
        {{"host.attach_enum", "windows-audio", "usbip attach makes Windows create render + capture endpoints"}, CaseHostAttach, 120},
        {{"host.driver_binding", "windows-audio", "Windows binds the built-in class driver (usbaudio2.sys / usbaudio.sys)"}, CaseHostDriverBinding, 90},
        {{"host.volume_event", "windows-audio", "device Status Word reaches Windows as a volume/mute callback"}, CaseHostVolumeEvent, 90},
        {{"host.capture_tone", "windows-audio", "1 kHz tone from the simulated mic is captured; 3 kHz is absent"}, CaseHostCaptureTone, 90},
        {{"host.gui_ops", "gui-operations", "Attach to Windows / Detach from Windows (the GUI buttons' code) work end to end"}, CaseHostOpsRoundtrip, 120},
        {{"cleanup.devices", "cleanup", "detach what this run attached; Windows endpoints return to baseline"}, CaseCleanup, 90},
    };
    return kCases;
}

std::vector<CheckInfo> ListChecks() {
    std::vector<CheckInfo> v;
    for (const auto& c : Cases()) v.push_back(c.info);
    return v;
}

const std::vector<std::string>& ManualChecklist() {
    static const std::vector<std::string> k = {
        "Real peer software (e.g. Razer Synapse) recognises and behaves correctly with the simulated device - not covered here.",
        "10+ minute continuous playback without drift/pops (host.capture_tone only streams for a few seconds).",
        "UAC1 profile: does usbaudio.sys react to Status Word events (RFC R-006)? Re-run host.volume_event with uac1-cmedia-sim and read the result.",
        "Device Manager shows the expected class driver (usbaudio2.sys for UAC2, usbaudio.sys for UAC1): see sandbox bootstrap log.",
        "uacstudio window layout/colours/text fit: only the non-visual self-check (--selftest) has run; nobody has looked at it. That includes the 'Environment and driver' window with its five action buttons.",
        "GUI 'Start simulator' / 'Stop simulator' and the exit clean-up: the logic is tested with a fake process and with the real uacsimd.exe (stop, parent exit), and the smoke script checks that closing or killing the GUI takes its simulator along; the buttons themselves have never been clicked.",
        "GUI 'Uninstall driver' and 'Clean up all' have only run against fake executors on the development machine. Press 'Clean up all' once in a Sandbox / VM and confirm: no port left in `usbip port`, service usbip2_ude gone, no leftover virtual USB host controller in Device Manager, files under Program Files\\USBip removed.",
        "host.* status: in Windows Sandbox the driver installs, usbip attach/detach work and the built-in class driver claims the device (host.driver_binding passes for UAC1 and UAC2), but Sandbox is a remote session and exposes no local audio endpoint, so host.attach_enum fails and host.volume_event / host.capture_tone have never passed on any machine. They need a console session (e.g. a Hyper-V VM in basic session). See the diagnostics in the full log of host.attach_enum.",
    };
    return k;
}

// ============================================================ 运行器与报告

std::string FormatReport(const std::vector<CheckResult>& results, const std::string& fullLog) {
    int p = 0, f = 0, s = 0;
    for (const auto& r : results) (r.status == CheckStatus::Pass ? p : r.status == CheckStatus::Fail ? f : s)++;
    std::ostringstream o;
    o << "RESULT: " << p << " passed, " << f << " failed, " << s << " skipped\n";
    for (const auto& r : results) {
        o << StatusName(r.status) << "  " << r.id;
        for (std::size_t i = r.id.size(); i < 22; ++i) o << ' ';
        o << " " << r.ms << " ms";
        if (!r.detail.empty()) o << "  " << r.detail;
        o << "\n";
    }
    o << "\nNEEDS HUMAN CONFIRMATION (not checked by this run):\n";
    for (const auto& m : ManualChecklist()) o << " - " << m << "\n";
    o << "\n---- full log ----\n" << fullLog;
    return o.str();
}

AutoTestOutcome RunCases(const AutoTestOptions& opts, const std::vector<CaseDef>& cases) {
    AutoTestOutcome out;
    auto sh = std::make_shared<Shared>();
    sh->opts = opts;
    if (sh->opts.caseTimeoutSec <= 0) sh->opts.caseTimeoutSec = 60;

    // 结果文件：桌面 → %TEMP%；文件名带时间
    std::string dir = opts.reportDir;
    if (dir.empty() || !DirWritable(dir)) {
        dir = DesktopDir();
        if (dir.empty() || !DirWritable(dir)) dir = TempDir();
    }
    {
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
        localtime_s(&tmv, &t);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        out.reportPath = S(P(dir) / (std::string("uacstudio-autotest-") + stamp + ".txt"));
    }
    sh->logFile.open(P(out.reportPath), std::ios::trunc);
    sh->Log("== uacstudio auto test, port " + std::to_string(opts.port) + ", host profile " +
            opts.hostProfile + ", report " + out.reportPath);

    bool hung = false, anyRan = false;
    const std::size_t n = cases.size();
    for (std::size_t i = 0; i < n; ++i) {
        const CaseDef& def = cases[i];
        CheckResult res;
        res.id = def.info.id;
        const bool cleanup = IsCleanup(def.info.id);
        sh->Log("== [" + std::to_string(i + 1) + "/" + std::to_string(n) + "] " + def.info.id + " - " + def.info.title);

        const long long t0 = NowMs();
        auto skip = [&](const std::string& why) {
            res.status = CheckStatus::Skip;
            res.detail = why;
        };
        // 仅当用例表以 env.api 开头（即完整用例表）时，才用"API 不可达"门控后续用例；
        // 自定义用例表（单测运行器本身）没有这个前提
        const bool apiGate = !cases.empty() && cases.front().info.id == "env.api";
        const bool needsApi = apiGate && !cleanup && def.info.id.rfind("env.", 0) != 0;
        if (opts.cancel && opts.cancel->load() && !cleanup) {
            skip("not run: cancelled");
        } else if (hung && !cleanup) {
            skip("not run: an earlier case did not return");
        } else if (cleanup && !anyRan) {
            skip("nothing ran, nothing to clean up");
        } else if (needsApi && !sh->fx.apiUp) {
            skip("not run: control API unreachable (env.api failed)");
        } else if (cleanup && apiGate && !sh->fx.apiUp && !sh->fx.hostTouched) {
            skip("control API unreachable and nothing was attached");
        } else {
            anyRan = true;
            struct State {
                std::mutex m;
                std::condition_variable cv;
                bool done = false;
                CheckStatus st = CheckStatus::Fail;
                std::string detail;
            };
            auto state = std::make_shared<State>();
            const int timeoutSec = def.timeoutSec > 0 ? def.timeoutSec : sh->opts.caseTimeoutSec;
            const CaseFn fn = def.fn;
            std::thread worker([sh, state, fn] {
                Ctx ctx(sh);
                CheckStatus st = CheckStatus::Pass;
                std::string detail;
                try {
                    fn(ctx);
                    detail = ctx.note();
                } catch (const CheckFailed& e) {
                    st = CheckStatus::Fail;
                    detail = e.why;
                } catch (const CheckSkipped& e) {
                    st = CheckStatus::Skip;
                    detail = e.why;
                } catch (const std::exception& e) {
                    st = CheckStatus::Fail;
                    detail = std::string("unexpected error: ") + e.what();
                } catch (...) {
                    st = CheckStatus::Fail;
                    detail = "unexpected non-standard exception";
                }
                std::lock_guard lk(state->m);
                state->st = st;
                state->detail = detail;
                state->done = true;
                state->cv.notify_all();
            });
            std::unique_lock lk(state->m);
            if (state->cv.wait_for(lk, std::chrono::seconds(timeoutSec), [&] { return state->done; })) {
                res.status = state->st;
                res.detail = state->detail;
                lk.unlock();
                worker.join();
            } else {
                lk.unlock();
                // 线程停不掉（卡在对 Windows/驱动的调用里）：放弃它，记 FAIL，不再跑后面的（清理除外）
                sh->abandoned.store(true);
                worker.detach();
                hung = true;
                res.status = CheckStatus::Fail;
                res.detail = "no answer within " + std::to_string(timeoutSec) +
                             " s: a call into the simulator, Windows or the driver did not return "
                             "(the remaining cases are not run)";
            }
        }
        res.ms = NowMs() - t0;
        sh->Log(std::string("-> ") + StatusName(res.status) + ": " + res.detail + " (" + std::to_string(res.ms) + " ms)");
        out.results.push_back(res);
    }

    for (const auto& r : out.results) (r.status == CheckStatus::Pass ? out.passed : r.status == CheckStatus::Fail ? out.failed : out.skipped)++;

    // 结果在最前、完整日志在后；用同名文件替换边跑边写的那个
    std::string fullLog;
    {
        std::lock_guard lk(sh->logMtx);
        fullLog = sh->logText;
        if (sh->logFile.is_open()) sh->logFile.close();
    }
    {
        std::ofstream f(P(out.reportPath), std::ios::trunc | std::ios::binary);
        f << FormatReport(out.results, fullLog);
    }
    sh->Log("report: " + out.reportPath);
    return out;
}

} // namespace uacstudio
