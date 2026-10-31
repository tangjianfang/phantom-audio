// usbip-win2 精简载荷的校验与安装。设计与步骤见 driver_installer.h。
#include "driver_installer.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace uacstudio {

namespace fs = std::filesystem;

namespace {

// ---- UTF-8 <-> 宽字符 / filesystem::path（与 auto_test.cpp 里同名辅助函数一致；不用已弃用的 u8path） ----
std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
fs::path P(const std::string& utf8) { return fs::path(ToWide(utf8)); }
std::string S(const fs::path& p) { return ToUtf8(p.wstring()); }

std::string Lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string Clip(const std::string& s, std::size_t n = 600) {
    const auto t = Trim(s);
    return t.size() <= n ? t : t.substr(0, n) + "...";
}

const char* const kBinFiles[] = {"devnode.exe", "usbip.exe", "libusbip.dll", "resources.dll"};
const char* const kRuntimeFiles[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                                     "msvcp140_atomic_wait.dll"};
constexpr const char* kHwId = "ROOT\\USBIP_WIN2\\UDE"; // 官方 setup.iss 里的 CLIENT_HWID

std::string SystemDir() {
    wchar_t buf[MAX_PATH]{};
    GetSystemDirectoryW(buf, MAX_PATH);
    return ToUtf8(buf);
}

std::string WinDir() {
    wchar_t buf[MAX_PATH]{};
    GetWindowsDirectoryW(buf, MAX_PATH);
    return ToUtf8(buf);
}

// 在 driver\ 的子目录里找包含 <infName> 的那个包目录，并要求同目录有 .sys 和 .cat（同主干名）。
bool FindPackage(const fs::path& driverDir, const std::string& infName, std::string* infPath, std::string* err) {
    std::error_code ec;
    std::vector<fs::path> hits;
    if (fs::is_directory(driverDir, ec))
        for (const auto& e : fs::directory_iterator(driverDir, ec))
            if (e.is_directory(ec) && fs::is_regular_file(e.path() / P(infName), ec)) hits.push_back(e.path());
    if (hits.empty()) {
        *err = "no driver package containing " + infName + " under driver\\";
        return false;
    }
    if (hits.size() > 1) {
        *err = "more than one driver package contains " + infName + " (remove the stale ones): " + S(hits[0].filename()) +
               ", " + S(hits[1].filename());
        return false;
    }
    const std::string stem = infName.substr(0, infName.size() - 4);
    for (const char* ext : {".sys", ".cat"})
        if (!fs::is_regular_file(hits[0] / P(stem + ext), ec)) {
            *err = "driver package " + S(hits[0].filename()) + " has no " + stem + ext;
            return false;
        }
    *infPath = S(hits[0] / P(infName));
    return true;
}

// 令牌里 Administrators 组是否启用。UAC 开着且未提权时该组是 deny-only，返回 false；
// UAC 关闭（如 Sandbox 里）或已提权时返回 true——比只看 TokenIsElevated 更稳。
bool DefaultIsAdmin() {
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
        return false;
    BOOL member = FALSE;
    const BOOL ok = CheckTokenMembership(nullptr, admins, &member);
    FreeSid(admins);
    return ok && member;
}

bool DefaultServiceInstalled(std::string* state) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, L"usbip2_ude", SERVICE_QUERY_STATUS);
    const bool ok = svc != nullptr;
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

InstallDeps Resolve(InstallDeps d) {
    if (!d.run) d.run = [](const std::string& exe, const std::string& args, int ms) { return RunTool(exe, args, ms); };
    if (!d.allowed) d.allowed = [](std::string* why) { return HostCasesAllowed(true, why); };
    if (!d.isAdmin) d.isAdmin = DefaultIsAdmin;
    if (!d.serviceInstalled) d.serviceInstalled = DefaultServiceInstalled;
    if (d.infDir.empty()) d.infDir = WinDir() + "\\INF";
    return d;
}

struct Verdict {
    bool ok = false;
    bool reboot = false;
    std::string note;
};

// pnputil：0 成功；3010 成功但需重启；259(ERROR_NO_MORE_ITEMS) 已进驱动库、暂无匹配设备（filter 挂在 USB 根集线器上，
// 没有匹配设备时会这样）；其余视为失败。
Verdict JudgePnputil(const ToolResult& t) {
    if (!t.started) return {false, false, "cannot start pnputil: " + Clip(t.out)};
    if (t.timedOut) return {false, false, "pnputil did not finish in time"};
    if (t.code == 0) return {true, false, "ok"};
    if (t.code == 3010) return {true, true, "ok, reboot required (exit 3010)"};
    if (t.code == 259) return {true, false, "added to the driver store, no matching device yet (exit 259)"};
    return {false, false, "pnputil exit " + std::to_string(t.code) + ": " + Clip(t.out)};
}

// devnode：0 成功；只有它自己判定需要时才打印 "Reboot is required to finish setup."
Verdict JudgeDevnode(const ToolResult& t) {
    if (!t.started) return {false, false, "cannot start devnode.exe: " + Clip(t.out)};
    if (t.timedOut) return {false, false, "devnode.exe did not finish in time"};
    const bool reboot = t.out.find("Reboot is required") != std::string::npos;
    if (t.code == 0) return {true, reboot, reboot ? "ok, reboot required" : "ok"};
    return {false, reboot, "devnode.exe exit " + std::to_string(t.code) + ": " + Clip(t.out)};
}

// 步骤记录器：同时写日志
struct Recorder {
    InstallResult& r;
    const InstallDeps& d;
    bool add(const std::string& name, bool ok, const std::string& detail) {
        r.steps.push_back({name, ok, detail});
        if (d.log) d.log(std::string(ok ? "[ok]   " : "[FAIL] ") + name + (detail.empty() ? "" : " - " + detail));
        return ok;
    }
};

// 判定：文件内容里是否含 needle（不区分大小写；兼容 UTF-16——oem*.inf 可能是 UTF-16LE，故丢掉 NUL 字节再比）
bool ContainsNoCase(const std::string& bytes, const std::string& needleLower) {
    std::string flat;
    flat.reserve(bytes.size());
    for (char c : bytes)
        if (c != '\0') flat.push_back(c);
    return Lower(flat).find(needleLower) != std::string::npos;
}

} // namespace

bool IsProcessAdmin() { return DefaultIsAdmin(); }

// ============================================================ SHA256 / 载荷校验

std::string Sha256File(const std::string& path) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    DWORD objLen = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0);
    std::vector<UCHAR> obj(objLen);
    std::string out;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, obj.data(), objLen, nullptr, 0, 0) >= 0) {
        std::vector<char> buf(1u << 16);
        bool ok = true;
        while (f.read(buf.data(), static_cast<std::streamsize>(buf.size())) || f.gcount() > 0) {
            if (BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(f.gcount()), 0) < 0) {
                ok = false;
                break;
            }
        }
        UCHAR digest[32]{};
        if (ok && BCryptFinishHash(h, digest, sizeof(digest), 0) >= 0) {
            static const char* kHex = "0123456789abcdef";
            for (UCHAR b : digest) {
                out.push_back(kHex[b >> 4]);
                out.push_back(kHex[b & 0xF]);
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
}

PayloadCheck CheckPayload(const std::string& rootStr) {
    PayloadCheck c;
    c.payload.root = rootStr;
    std::error_code ec;
    const fs::path root = P(rootStr);
    if (rootStr.empty() || !fs::is_directory(root, ec)) {
        c.error = "payload folder not found: " + rootStr;
        return c;
    }
    for (const char* n : kBinFiles) {
        const auto p = root / "bin" / n;
        if (!fs::is_regular_file(p, ec)) {
            c.error = std::string("missing bin\\") + n;
            return c;
        }
        c.payload.binFiles.push_back(S(p));
    }
    if (!FindPackage(root / "driver", "usbip2_ude.inf", &c.payload.udeInf, &c.error)) return c;
    if (!FindPackage(root / "driver", "usbip2_filter.inf", &c.payload.filterInf, &c.error)) return c;

    int rt = 0;
    for (const char* n : kRuntimeFiles) {
        const auto p = root / "runtime" / n;
        if (fs::is_regular_file(p, ec)) {
            c.payload.runtimeFiles.push_back(S(p));
            ++rt;
        }
    }
    if (rt == 0)
        c.notes.push_back("no runtime\\ DLLs: usbip.exe/devnode.exe need the VC++ 14.51+ runtime and will not start on a machine that lacks it");
    else if (rt < 4)
        c.notes.push_back("runtime\\ is incomplete (" + std::to_string(rt) + " of 4 DLLs)");

    // MANIFEST.txt：每行 "<sha256> *<相对路径>   (<字节数>)"；其余行（# 注释、文件版本小节）忽略
    const auto manifest = root / "MANIFEST.txt";
    if (!fs::is_regular_file(manifest, ec)) {
        c.notes.push_back("no MANIFEST.txt: file hashes were NOT checked");
    } else {
        static const std::regex re(R"(^([0-9A-Fa-f]{64}) \*(.+?)   \([0-9]+\)\s*$)");
        std::ifstream mf(manifest, std::ios::binary);
        std::string line;
        std::set<std::string> listed;
        int verified = 0;
        while (std::getline(mf, line)) {
            std::smatch m;
            if (!std::regex_match(line, m, re)) continue;
            std::string rel = m[2].str();
            std::replace(rel.begin(), rel.end(), '\\', '/');
            const auto file = root / P(rel);
            if (!fs::is_regular_file(file, ec)) {
                c.error = "MANIFEST lists a file that is missing: " + rel;
                return c;
            }
            if (Sha256File(S(file)) != Lower(m[1].str()) ) {
                c.error = "MANIFEST hash mismatch (file damaged or replaced): " + rel;
                return c;
            }
            listed.insert(Lower(rel));
            ++verified;
        }
        if (verified == 0) {
            c.error = "MANIFEST.txt contains no file hashes";
            return c;
        }
        int unlisted = 0;
        for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
            if (!e.is_regular_file(ec)) continue;
            std::string norm = Lower(S(fs::relative(e.path(), root, ec)));
            std::replace(norm.begin(), norm.end(), '\\', '/');
            if (norm != "manifest.txt" && !listed.count(norm)) ++unlisted;
        }
        c.notes.push_back("MANIFEST verified: " + std::to_string(verified) + " file(s) match their SHA256");
        if (unlisted > 0) c.notes.push_back(std::to_string(unlisted) + " file(s) in the payload are not listed in MANIFEST.txt");
    }
    c.ok = true;
    return c;
}

// ============================================================ 状态 / 安装 / 卸载

std::string DefaultInstallDir() {
    wchar_t pf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) return ToUtf8(pf) + "\\USBip";
    return "C:\\Program Files\\USBip";
}

DriverStatus DetectDriver(const std::string& installDir) {
    DriverStatus s;
    s.serviceInstalled = DefaultServiceInstalled(&s.serviceState);
    std::error_code ec;
    const std::string dir = installDir.empty() ? DefaultInstallDir() : installDir;
    const auto p = P(dir) / "usbip.exe";
    if (fs::is_regular_file(p, ec)) {
        s.usbipPresent = true;
        s.usbipPath = S(p);
    } else {
        wchar_t buf[MAX_PATH]{};
        if (SearchPathW(nullptr, L"usbip.exe", nullptr, MAX_PATH, buf, nullptr)) {
            s.usbipPresent = true;
            s.usbipPath = ToUtf8(buf);
        }
    }
    return s;
}

InstallDeps ResolveDeps(InstallDeps d) { return Resolve(std::move(d)); }

std::string InstallResult::Summary() const {
    std::ostringstream o;
    for (const auto& s : steps) o << (s.ok ? "[ok]   " : "[FAIL] ") << s.name << (s.detail.empty() ? "" : " - " + s.detail) << "\n";
    if (refused) o << "REFUSED: nothing was changed on this machine.\n";
    else if (ok) o << "DONE" << (rebootRequired ? " - a REBOOT IS REQUIRED to finish setup" : "") << " (install dir: " << installDir << ")\n";
    else o << "FAILED - see the first [FAIL] line above.\n";
    return o.str();
}

InstallResult InstallDriver(const InstallOptions& opt, const InstallDeps& deps) {
    const InstallDeps d = Resolve(deps);
    InstallResult r;
    r.installDir = opt.installDir.empty() ? DefaultInstallDir() : opt.installDir;
    Recorder rec{r, d};

    // 1) 守卫：会往系统装内核驱动，只在一次性机器上做。拒绝时什么都不碰
    std::string why;
    if (!d.allowed(&why)) {
        r.refused = true;
        rec.add("guard: disposable machine", false, why);
        return r;
    }
    rec.add("guard: disposable machine", true, why);

    // 2) 管理员
    if (!d.isAdmin()) {
        rec.add("administrator", false, "must run elevated (start the program with 'Run as administrator')");
        return r;
    }
    rec.add("administrator", true, "elevated");

    // 已装过就什么都不做：devnode install 每次都会新建一个设备节点，重复执行会得到第二个虚拟主控
    //（例如 Sandbox 重启后 LogonCommand 把脚本又跑了一遍）。要重装请先 --uninstall-driver。
    {
        std::string before;
        if (d.serviceInstalled(&before)) {
            rec.add("already installed", true, "service usbip2_ude is " + before + "; nothing was changed (uninstall first to reinstall)");
            r.ok = true;
            return r;
        }
    }

    // 3) 载荷
    const PayloadCheck pc = CheckPayload(opt.payloadRoot);
    for (const auto& n : pc.notes)
        if (d.log) d.log("[note] " + n);
    if (!pc.ok) {
        rec.add("payload", false, pc.error);
        return r;
    }
    rec.add("payload", true, opt.payloadRoot + (pc.notes.empty() ? "" : " (" + pc.notes.front() + ")"));

    // 4) 拷贝 exe/dll 与运行库到安装目录（app-local：同目录下的 DLL 优先于系统目录）
    std::error_code ec;
    fs::create_directories(P(r.installDir), ec);
    if (ec) {
        rec.add("copy files", false, "cannot create " + r.installDir + ": " + ec.message());
        return r;
    }
    std::vector<std::string> files = pc.payload.binFiles;
    files.insert(files.end(), pc.payload.runtimeFiles.begin(), pc.payload.runtimeFiles.end());
    for (const auto& src : files) {
        const auto dst = P(r.installDir) / P(src).filename();
        fs::copy_file(P(src), dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            rec.add("copy files", false, S(P(src).filename()) + ": " + ec.message());
            return r;
        }
        if (fs::file_size(dst, ec) != fs::file_size(P(src), ec)) {
            rec.add("copy files", false, S(P(src).filename()) + ": size differs after copy");
            return r;
        }
    }
    rec.add("copy files", true, std::to_string(files.size()) + " file(s) -> " + r.installDir);

    // 5) filter 驱动（USB 根集线器的上层过滤；安装会重启 USB 3 集线器，已连接的 USB 设备会短暂掉线）
    const Verdict vf = JudgePnputil(d.run(SystemDir() + "\\pnputil.exe",
                                          "/add-driver \"" + pc.payload.filterInf + "\" /install", 120000));
    if (!rec.add("driver usbip2_filter (pnputil)", vf.ok, vf.note)) return r;
    r.rebootRequired = r.rebootRequired || vf.reboot;

    // 6) UDE 驱动 + 虚拟主控设备节点
    const Verdict vu = JudgeDevnode(d.run(r.installDir + "\\devnode.exe",
                                          "install \"" + pc.payload.udeInf + "\" " + kHwId, 180000));
    if (!rec.add("driver usbip2_ude + virtual host controller (devnode)", vu.ok, vu.note)) return r;
    r.rebootRequired = r.rebootRequired || vu.reboot;

    // 7) 校验：服务出现
    // 先调用、再读 state：同一个调用里的实参求值顺序是未指定的
    std::string state;
    const bool svcOk = d.serviceInstalled(&state);
    if (!rec.add("verify: service usbip2_ude", svcOk,
                 svcOk ? "service is " + state
                       : "service not present after install" + std::string(r.rebootRequired ? " (a reboot is required first)" : "")))
        return r;

    r.ok = true;
    return r;
}

InstallResult UninstallDriver(const InstallOptions& opt, const InstallDeps& deps) {
    const InstallDeps d = Resolve(deps);
    InstallResult r;
    r.installDir = opt.installDir.empty() ? DefaultInstallDir() : opt.installDir;
    Recorder rec{r, d};

    std::string why;
    if (!d.allowed(&why)) {
        r.refused = true;
        rec.add("guard: disposable machine", false, why);
        return r;
    }
    rec.add("guard: disposable machine", true, why);
    if (!d.isAdmin()) {
        rec.add("administrator", false, "must run elevated");
        return r;
    }
    rec.add("administrator", true, "elevated");

    // 卸载是尽力而为：每一步都尝试，汇总结果（和安装不同，不会在第一个失败处停下）
    bool allOk = true;
    std::error_code ec;

    // 1) 虚拟主控设备节点。devnode 优先用安装目录里的，其次载荷里的
    std::string devnode;
    std::vector<fs::path> candidates = {P(r.installDir) / "devnode.exe"};
    if (!opt.payloadRoot.empty()) candidates.push_back(P(opt.payloadRoot) / "bin" / "devnode.exe"); // 空串会变成相对当前目录的路径
    for (const auto& cand : candidates)
        if (fs::is_regular_file(cand, ec)) {
            devnode = S(cand);
            break;
        }
    if (devnode.empty()) {
        allOk = rec.add("remove virtual host controller (devnode)", false, "devnode.exe not found in " + r.installDir + " or the payload") && allOk;
    } else {
        const Verdict v = JudgeDevnode(d.run(devnode, std::string("remove ") + kHwId + " root", 120000));
        allOk = rec.add("remove virtual host controller (devnode)", v.ok, v.note) && allOk;
        r.rebootRequired = r.rebootRequired || v.reboot;
    }

    // 2) 驱动库里属于 usbip2_* 的 oem*.inf（官方 README 的卸载命令做的就是这件事）
    int found = 0;
    if (fs::is_directory(P(d.infDir), ec)) {
        std::vector<std::string> oems;
        for (const auto& e : fs::directory_iterator(P(d.infDir), ec)) {
            const std::string name = Lower(S(e.path().filename()));
            if (name.size() < 8 || name.rfind("oem", 0) != 0 || name.substr(name.size() - 4) != ".inf") continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (ContainsNoCase(bytes, "usbip2_ude") || ContainsNoCase(bytes, "usbip2_filter")) oems.push_back(name);
        }
        std::sort(oems.begin(), oems.end());
        for (const auto& oem : oems) {
            ++found;
            const Verdict v = JudgePnputil(d.run(SystemDir() + "\\pnputil.exe", "/delete-driver " + oem + " /uninstall /force", 120000));
            allOk = rec.add("remove driver package " + oem + " (pnputil)", v.ok, v.note) && allOk;
            r.rebootRequired = r.rebootRequired || v.reboot;
        }
    }
    if (found == 0) rec.add("remove driver packages", true, "no usbip2_* packages found in " + d.infDir);

    // 3) 只删我们自己拷的文件（按名字），目录空了才删目录——不动官方安装器留下的 unins000.* 等
    int removed = 0;
    for (const char* n : kBinFiles) removed += fs::remove(P(r.installDir) / n, ec) ? 1 : 0;
    for (const char* n : kRuntimeFiles) removed += fs::remove(P(r.installDir) / n, ec) ? 1 : 0;
    if (fs::is_directory(P(r.installDir), ec) && fs::is_empty(P(r.installDir), ec)) fs::remove(P(r.installDir), ec);
    rec.add("remove files", true, std::to_string(removed) + " file(s) removed from " + r.installDir);

    r.ok = allOk;
    return r;
}

} // namespace uacstudio
