// usbip-win2 精简载荷的校验与安装/卸载流程。
// 真正的安装只会发生在 Sandbox 里；这里用假的执行器验证：步骤顺序、失败即停、重启判定、
// 以及最重要的——守卫拒绝 / 非管理员 / 载荷损坏时一条系统命令都不会执行。
#include <doctest/doctest.h>
#include <driver_installer.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace uacstudio;
namespace fs = std::filesystem;

namespace {

constexpr char kBs = 0x5C; // 反斜杠；不手写 "\\" 字面量

fs::path fresh_dir(const char* name) {
    const fs::path d = fs::temp_directory_path() / "uac_drv_test" / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct PayloadOpts {
    bool runtime = true;
    bool manifest = true;
};

// 合成一个与 collect-usbip-payload.bat 输出同布局的载荷（内容是假的，只有结构和哈希是真的）
fs::path make_payload(const char* name, PayloadOpts o = {}) {
    const fs::path root = fresh_dir(name);
    for (const char* n : {"devnode.exe", "usbip.exe", "libusbip.dll", "resources.dll"})
        write_file(root / "bin" / n, std::string("fake ") + n);
    for (const char* pkg : {"usbip2_ude", "usbip2_filter"}) {
        const fs::path d = root / "driver" / (std::string(pkg) + ".inf_amd64_0123456789abcdef");
        for (const char* ext : {".inf", ".sys", ".cat"}) write_file(d / (std::string(pkg) + ext), std::string("fake ") + pkg + ext);
    }
    if (o.runtime)
        for (const char* n : {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll"})
            write_file(root / "runtime" / n, std::string("fake ") + n);
    if (o.manifest) {
        std::ostringstream m;
        m << "# fake manifest\r\n";
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
            if (!e.is_regular_file()) continue;
            std::string rel = fs::relative(e.path(), root).string();
            for (auto& c : rel)
                if (c == '/') c = kBs;
            m << Sha256File(e.path().string()) << " *" << rel << "   (" << fs::file_size(e.path()) << ")\r\n";
        }
        m << "\r\n# file versions (ignored by the checker)\r\nvcruntime140.dll = 14.51.36247.0\r\n";
        write_file(root / "MANIFEST.txt", m.str());
    }
    return root;
}

// 假的系统：记录每条命令，按 responder 回答
struct FakeSys {
    std::vector<std::string> calls;
    std::function<ToolResult(const std::string&, const std::string&)> responder = [](const std::string&, const std::string&) {
        ToolResult t;
        t.started = true;
        t.code = 0;
        return t;
    };
    bool allowed = true, admin = true;
    bool serviceBefore = false; // 安装前服务是否已存在（第一次查询）
    bool service = true;        // 安装后服务是否存在（之后的查询）
    int serviceCalls = 0;
    std::string whyNot = "test: not a disposable machine";
    fs::path infDir;
};

InstallDeps deps_for(FakeSys& s) {
    InstallDeps d;
    d.run = [&s](const std::string& exe, const std::string& args, int) {
        s.calls.push_back(exe + " " + args);
        return s.responder(exe, args);
    };
    d.allowed = [&s](std::string* why) {
        if (why) *why = s.allowed ? "test machine" : s.whyNot;
        return s.allowed;
    };
    d.isAdmin = [&s] { return s.admin; };
    d.serviceInstalled = [&s](std::string* st) {
        const bool present = (s.serviceCalls++ == 0) ? s.serviceBefore : s.service;
        if (st && present) *st = "running";
        return present;
    };
    if (!s.infDir.empty()) d.infDir = s.infDir.string();
    return d;
}

ToolResult result(unsigned long code, const std::string& out = "") {
    ToolResult t;
    t.started = true;
    t.code = code;
    t.out = out;
    return t;
}

bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

} // namespace

// ---------------------------------------------------------------- SHA256

TEST_CASE("sha256: known test vectors, including a multi-chunk file") {
    const fs::path d = fresh_dir("sha");
    write_file(d / "empty", "");
    write_file(d / "abc", "abc");
    write_file(d / "million_a", std::string(1000000, 'a')); // 跨越多个 64 KB 读块
    CHECK(Sha256File((d / "empty").string()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256File((d / "abc").string()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256File((d / "million_a").string()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(Sha256File((d / "does_not_exist").string()).empty());
}

// ---------------------------------------------------------------- CheckPayload

TEST_CASE("payload: a well-formed payload passes and every file is hash-checked") {
    const auto root = make_payload("p_ok");
    const auto c = CheckPayload(root.string());
    INFO(c.error);
    CHECK(c.ok);
    CHECK(c.error.empty());
    CHECK(c.payload.binFiles.size() == 4);
    CHECK(c.payload.runtimeFiles.size() == 4);
    CHECK(contains(c.payload.udeInf, "usbip2_ude.inf"));
    CHECK(contains(c.payload.filterInf, "usbip2_filter.inf"));
    bool verified = false;
    for (const auto& n : c.notes) verified = verified || contains(n, "MANIFEST verified: 14 file(s)");
    CHECK(verified);
}

TEST_CASE("payload: damage is detected - tampered byte, missing file, missing .cat, duplicate package") {
    SUBCASE("nonexistent folder") {
        const auto c = CheckPayload((fresh_dir("p_none") / "nope").string());
        CHECK_FALSE(c.ok);
        CHECK(contains(c.error, "not found"));
    }
    SUBCASE("a tampered file fails the MANIFEST check") {
        const auto root = make_payload("p_tamper");
        write_file(root / "bin" / "devnode.exe", "fake devnode.exe but modified");
        const auto c = CheckPayload(root.string());
        CHECK_FALSE(c.ok);
        CHECK(contains(c.error, "hash mismatch"));
        CHECK(contains(c.error, "devnode.exe"));
    }
    SUBCASE("a runtime DLL listed in MANIFEST but deleted") {
        const auto root = make_payload("p_missing");
        fs::remove(root / "runtime" / "msvcp140.dll");
        const auto c = CheckPayload(root.string());
        CHECK_FALSE(c.ok);
        CHECK(contains(c.error, "missing"));
    }
    SUBCASE("a required bin file is absent") {
        const auto root = make_payload("p_nobin", {true, false});
        fs::remove(root / "bin" / "usbip.exe");
        const auto c = CheckPayload(root.string());
        CHECK_FALSE(c.ok);
        CHECK(contains(c.error, "missing bin"));
        CHECK(contains(c.error, "usbip.exe"));
    }
    SUBCASE("a driver package without its catalog") {
        const auto root = make_payload("p_nocat", {true, false});
        fs::remove(root / "driver" / "usbip2_ude.inf_amd64_0123456789abcdef" / "usbip2_ude.cat");
        const auto c = CheckPayload(root.string());
        CHECK_FALSE(c.ok);
        CHECK(contains(c.error, "usbip2_ude.cat"));
    }
    SUBCASE("two packages of the same driver (stale + new) is ambiguous") {
        const auto root = make_payload("p_dup", {true, false});
        write_file(root / "driver" / "usbip2_filter.inf_amd64_ffffffffffffffff" / "usbip2_filter.inf", "x");
        const auto c = CheckPayload(root.string());
        CHECK_FALSE(c.ok);
        CHECK(contains(c.error, "more than one"));
    }
    SUBCASE("a MANIFEST with no hashes in it") {
        const auto root = make_payload("p_emptyman", {true, false});
        write_file(root / "MANIFEST.txt", "# nothing here\r\n");
        CHECK_FALSE(CheckPayload(root.string()).ok);
    }
}

TEST_CASE("payload: optional parts are reported, not silently accepted") {
    SUBCASE("no MANIFEST: still ok, but says hashes were not checked") {
        const auto c = CheckPayload(make_payload("p_noman", {true, false}).string());
        CHECK(c.ok);
        bool said = false;
        for (const auto& n : c.notes) said = said || contains(n, "NOT checked");
        CHECK(said);
    }
    SUBCASE("no runtime: still ok, but warns the tools may not start") {
        const auto c = CheckPayload(make_payload("p_nort", {false, false}).string());
        CHECK(c.ok);
        CHECK(c.payload.runtimeFiles.empty());
        bool said = false;
        for (const auto& n : c.notes) said = said || contains(n, "runtime");
        CHECK(said);
    }
}

TEST_CASE("payload: the real collected payload (when present on this machine) verifies") {
    const fs::path real = fs::path(UAC_SOURCE_DIR) / "third_party" / "usbip-win2" / "usbip-payload";
    if (!fs::is_directory(real)) {
        MESSAGE("real payload not present at " << real.string() << " - skipped");
        return;
    }
    const auto c = CheckPayload(real.string());
    INFO(c.error);
    CHECK(c.ok);
    bool verified = false;
    for (const auto& n : c.notes) verified = verified || contains(n, "MANIFEST verified: 14 file(s)");
    CHECK(verified);
    CHECK(c.payload.runtimeFiles.size() == 4);
}

// ---------------------------------------------------------------- InstallDriver

TEST_CASE("install: happy path runs pnputil(filter) then devnode(ude), in that order, with the right arguments") {
    const auto payload = make_payload("i_ok");
    const auto dest = fresh_dir("i_ok_dest") / "USBip";
    FakeSys sys;
    const auto r = InstallDriver({payload.string(), dest.string()}, deps_for(sys));
    INFO(r.Summary());
    CHECK(r.ok);
    CHECK_FALSE(r.refused);
    CHECK_FALSE(r.rebootRequired);
    REQUIRE(sys.calls.size() == 2);
    CHECK(contains(sys.calls[0], "pnputil.exe"));
    CHECK(contains(sys.calls[0], "/add-driver"));
    CHECK(contains(sys.calls[0], "usbip2_filter.inf"));
    CHECK(contains(sys.calls[0], "/install"));
    CHECK(contains(sys.calls[1], (dest / "devnode.exe").string())); // 用拷到安装目录里的 devnode
    CHECK(contains(sys.calls[1], "install"));
    CHECK(contains(sys.calls[1], "usbip2_ude.inf"));
    CHECK(contains(sys.calls[1], std::string("ROOT") + kBs + "USBIP_WIN2" + kBs + "UDE"));
    // 8 个文件（4 个工具 + 4 个运行库）拷到了安装目录，内容一致
    int n = 0;
    for (const auto& e : fs::directory_iterator(dest)) {
        ++n;
        CHECK(read_file(e.path()).rfind("fake ", 0) == 0);
    }
    CHECK(n == 8);
    CHECK(contains(r.Summary(), "DONE"));
}

TEST_CASE("install: reboot is reported only when the tools say so") {
    const auto payload = make_payload("i_reboot");
    SUBCASE("devnode prints 'Reboot is required'") {
        FakeSys sys;
        sys.responder = [](const std::string& exe, const std::string&) {
            return contains(exe, "devnode") ? result(0, "Reboot is required to finish setup.\r\n") : result(0);
        };
        const auto r = InstallDriver({payload.string(), (fresh_dir("i_r1") / "d").string()}, deps_for(sys));
        CHECK(r.ok);
        CHECK(r.rebootRequired);
        CHECK(contains(r.Summary(), "REBOOT"));
    }
    SUBCASE("pnputil exits 3010") {
        FakeSys sys;
        sys.responder = [](const std::string& exe, const std::string&) { return contains(exe, "pnputil") ? result(3010) : result(0); };
        const auto r = InstallDriver({payload.string(), (fresh_dir("i_r2") / "d").string()}, deps_for(sys));
        CHECK(r.ok);
        CHECK(r.rebootRequired);
    }
    SUBCASE("nothing says reboot: it is not claimed") {
        FakeSys sys;
        CHECK_FALSE(InstallDriver({payload.string(), (fresh_dir("i_r3") / "d").string()}, deps_for(sys)).rebootRequired);
    }
}

TEST_CASE("install: pnputil exit 259 (no matching device yet) is accepted, other codes are failures") {
    const auto payload = make_payload("i_259");
    {
        FakeSys sys;
        sys.responder = [](const std::string& exe, const std::string&) { return contains(exe, "pnputil") ? result(259) : result(0); };
        const auto r = InstallDriver({payload.string(), (fresh_dir("i_259a") / "d").string()}, deps_for(sys));
        CHECK(r.ok);
        CHECK(sys.calls.size() == 2); // 继续装了 ude
    }
    {
        FakeSys sys;
        sys.responder = [](const std::string& exe, const std::string&) {
            return contains(exe, "pnputil") ? result(1, "Failed to add driver package: access denied") : result(0);
        };
        const auto r = InstallDriver({payload.string(), (fresh_dir("i_259b") / "d").string()}, deps_for(sys));
        CHECK_FALSE(r.ok);
        CHECK(sys.calls.size() == 1); // 失败即停：devnode 没有被调用
        CHECK(contains(r.steps.back().detail, "access denied")); // 失败原因带出来
    }
}

TEST_CASE("install: devnode failure stops before the service check; a missing service fails the verify step") {
    const auto payload = make_payload("i_dev");
    {
        FakeSys sys;
        sys.responder = [](const std::string& exe, const std::string&) {
            return contains(exe, "devnode") ? result(1, "SetupDiCallClassInstaller failed") : result(0);
        };
        const auto r = InstallDriver({payload.string(), (fresh_dir("i_dev1") / "d").string()}, deps_for(sys));
        CHECK_FALSE(r.ok);
        CHECK(contains(r.steps.back().name, "devnode"));
        for (const auto& s : r.steps) CHECK_FALSE(contains(s.name, "verify")); // 没走到校验
    }
    {
        FakeSys sys;
        sys.service = false;
        const auto r = InstallDriver({payload.string(), (fresh_dir("i_dev2") / "d").string()}, deps_for(sys));
        CHECK_FALSE(r.ok);
        CHECK(contains(r.steps.back().name, "verify"));
        CHECK(contains(r.steps.back().detail, "not present"));
    }
}

TEST_CASE("install: already installed -> nothing is run and nothing is copied (devnode install would add a 2nd controller)") {
    const auto payload = make_payload("i_again");
    FakeSys sys;
    sys.serviceBefore = true;
    const auto dest = fresh_dir("i_again_dest") / "USBip";
    const auto r = InstallDriver({payload.string(), dest.string()}, deps_for(sys));
    CHECK(r.ok);
    CHECK_FALSE(r.rebootRequired);
    CHECK(sys.calls.empty());
    CHECK_FALSE(fs::exists(dest));
    CHECK(contains(r.steps.back().name, "already installed"));
}

TEST_CASE("install: guard, administrator and payload failures touch NOTHING") {
    const auto payload = make_payload("i_guard");
    SUBCASE("not a disposable machine -> refused, no command, no install dir") {
        FakeSys sys;
        sys.allowed = false;
        const auto dest = fresh_dir("i_g1") / "USBip";
        const auto r = InstallDriver({payload.string(), dest.string()}, deps_for(sys));
        CHECK_FALSE(r.ok);
        CHECK(r.refused);
        CHECK(sys.calls.empty());
        CHECK_FALSE(fs::exists(dest));
        CHECK(contains(r.Summary(), "REFUSED"));
        CHECK(contains(r.steps.front().detail, "not a disposable machine"));
    }
    SUBCASE("not administrator -> no command, no install dir") {
        FakeSys sys;
        sys.admin = false;
        const auto dest = fresh_dir("i_g2") / "USBip";
        const auto r = InstallDriver({payload.string(), dest.string()}, deps_for(sys));
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.refused);
        CHECK(sys.calls.empty());
        CHECK_FALSE(fs::exists(dest));
    }
    SUBCASE("damaged payload -> no command, no install dir") {
        write_file(payload / "bin" / "usbip.exe", "tampered");
        FakeSys sys;
        const auto dest = fresh_dir("i_g3") / "USBip";
        const auto r = InstallDriver({payload.string(), dest.string()}, deps_for(sys));
        CHECK_FALSE(r.ok);
        CHECK(sys.calls.empty());
        CHECK_FALSE(fs::exists(dest));
        CHECK(contains(r.steps.back().detail, "hash mismatch"));
    }
}

// ---------------------------------------------------------------- UninstallDriver

TEST_CASE("uninstall: removes the device node, only usbip2_* driver packages, and only our own files") {
    const auto inf = fresh_dir("u_inf");
    write_file(inf / "oem1.inf", "; installed from usbip2_ude.inf\r\nCatalogFile=usbip2_ude.cat\r\n");
    write_file(inf / "oem2.inf", "; some other vendor's driver\r\n");
    {   // 一部分 oem*.inf 是 UTF-16LE（官方 README 的 findstr 也带了 Unicode 开关）
        std::ofstream f(inf / "oem3.inf", std::ios::binary);
        const std::wstring w = L"; UsbIp2_Filter extension\r\n";
        f.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size() * sizeof(wchar_t)));
    }
    write_file(inf / "not_an_oem.inf", "mentions usbip2_ude but is not an oem copy"); // 名字不是 oem*.inf：不动

    const auto dest = fresh_dir("u_dest") / "USBip";
    write_file(dest / "usbip.exe", "ours");
    write_file(dest / "devnode.exe", "ours");
    write_file(dest / "vcruntime140.dll", "ours");
    write_file(dest / "unins000.exe", "NOT ours - left by the official installer");

    FakeSys sys;
    sys.infDir = inf;
    const auto r = UninstallDriver({"", dest.string()}, deps_for(sys));
    INFO(r.Summary());
    CHECK(r.ok);
    REQUIRE(sys.calls.size() == 3);
    CHECK(contains(sys.calls[0], "devnode.exe"));
    CHECK(contains(sys.calls[0], "remove"));
    CHECK(contains(sys.calls[0], std::string("ROOT") + kBs + "USBIP_WIN2" + kBs + "UDE"));
    CHECK(contains(sys.calls[0], " root"));
    CHECK(contains(sys.calls[1], "/delete-driver oem1.inf /uninstall /force"));
    CHECK(contains(sys.calls[2], "/delete-driver oem3.inf /uninstall /force"));
    for (const auto& c : sys.calls) CHECK_FALSE(contains(c, "oem2.inf")); // 别人的驱动不碰
    CHECK_FALSE(fs::exists(dest / "usbip.exe"));
    CHECK_FALSE(fs::exists(dest / "devnode.exe"));
    CHECK_FALSE(fs::exists(dest / "vcruntime140.dll"));
    CHECK(fs::exists(dest / "unins000.exe")); // 不是我们拷的：留着
    CHECK(fs::exists(dest));                  // 目录里还有别人的文件：不删目录
}

TEST_CASE("uninstall: an empty install directory is removed; refusal runs nothing") {
    {
        const auto dest = fresh_dir("u_empty") / "USBip";
        write_file(dest / "usbip.exe", "ours");
        FakeSys sys;
        sys.infDir = fresh_dir("u_empty_inf");
        sys.responder = [](const std::string&, const std::string&) { return result(0); };
        CHECK(UninstallDriver({"", dest.string()}, deps_for(sys)).ok == false); // devnode.exe 不在：如实报告没能移除设备节点
        CHECK_FALSE(fs::exists(dest));                                           // 但文件和空目录仍被清理
    }
    {
        FakeSys sys;
        sys.allowed = false;
        const auto r = UninstallDriver({"", (fresh_dir("u_ref") / "USBip").string()}, deps_for(sys));
        CHECK(r.refused);
        CHECK_FALSE(r.ok);
        CHECK(sys.calls.empty());
    }
}

TEST_CASE("detect: runs without touching anything and reports a coherent state") {
    const auto s = DetectDriver((fresh_dir("detect") / "none").string());
    if (!s.serviceInstalled) CHECK(s.serviceState.empty());
    CHECK(DefaultInstallDir().find("USBip") != std::string::npos);
}
