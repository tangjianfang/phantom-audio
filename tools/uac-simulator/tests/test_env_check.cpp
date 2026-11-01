// Environment check (RunEnvChecks) and the "all UI text is English" rule.
//
// Why: (1) the check blocks (a connect to a closed loopback port, a SHA-256 over the driver payload) and used to run on the
// UI thread, freezing the window; it now reports progress step by step so the window can say what it is doing.
// (2) Windows Sandbox has no CJK fonts, so Chinese UI text showed up as boxes; a source scan keeps it from coming back.
#include <doctest/doctest.h>
#include <http_client.h>   // winsock2 before windows.h
#include <env_check.h>
#include <uac/control_api.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace uacstudio;
using Clock = std::chrono::steady_clock;

namespace {

std::uint16_t unused_port() {
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

bool ascii(const std::wstring& w) {
    for (wchar_t c : w)
        if (c > 127) return false;
    return true;
}

// Line numbers of string literals that contain a non-ASCII byte. Comments and char literals are skipped (comments may
// stay Chinese: they are never shown).
std::vector<int> non_ascii_literals(const std::string& src) {
    enum { Code, LineComment, BlockComment, Str, Chr } st = Code;
    std::vector<int> hits;
    int line = 1;
    bool flagged = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = i + 1 < src.size() ? src[i + 1] : '\0';
        if (c == '\n') ++line;
        switch (st) {
        case Code:
            if (c == '/' && n == '/') { st = LineComment; ++i; }
            else if (c == '/' && n == '*') { st = BlockComment; ++i; }
            else if (c == '"') { st = Str; flagged = false; }
            else if (c == '\'') st = Chr;
            break;
        case LineComment:
            if (c == '\n') st = Code;
            break;
        case BlockComment:
            if (c == '*' && n == '/') { st = Code; ++i; }
            break;
        case Str:
            if (c == '\\') { ++i; if (n == '\n') ++line; }
            else if (c == '"' || c == '\n') st = Code;
            else if (static_cast<unsigned char>(c) > 127 && !flagged) { hits.push_back(line); flagged = true; }
            break;
        case Chr:
            if (c == '\\') ++i;
            else if (c == '\'' || c == '\n') st = Code;
            break;
        }
    }
    return hits;
}

} // namespace

TEST_CASE("env check: every step is announced, the text is English, and a closed port does not stall it") {
    const auto port = unused_port();
    std::vector<std::wstring> said;
    const auto t0 = Clock::now();
    const auto items = RunEnvChecks(port, [&](const std::wstring& s) { said.push_back(s); });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    INFO("took " << ms << " ms");

    CHECK(items.size() == 7); // 3b) VHCI 根设备探测（服务在 != 主控在，2026-11-01 真机教训）
    CHECK(said.size() >= items.size()); // at least one "what I am doing" line per item
    for (const auto& s : said) {
        CHECK_MESSAGE(!s.empty(), "an empty progress line tells the user nothing");
        CHECK(ascii(s));
    }
    for (const auto& it : items) {
        CHECK(ascii(it.name));
        CHECK(ascii(it.detail));
        CHECK(ascii(it.fixNote));
        CHECK(ascii(it.fixCmd));
        CHECK(!it.name.empty());
        CHECK(!it.detail.empty());
    }
    // The old blocking connect took ~2.0 s here; the bounded one must stay well below that (the rest is registry/service
    // queries and a payload check that finds no payload next to the test executable).
    CHECK(ms < 1500);
}

TEST_CASE("env check: the uacsimd row follows reality - closed port is missing, a live server turns it ready") {
    const auto closedPort = unused_port();
    const auto down = RunEnvChecks(closedPort);
    REQUIRE(down.size() >= 2);
    CHECK(down[1].state == 0);
    CHECK(!down[1].fixNote.empty()); // a missing item must say how to fix it

    uac::ControlCallbacks cb;
    cb.status = [] { return std::string(R"({"running":true})"); };
    uac::ControlApi api;
    REQUIRE(api.start(0, cb));
    const auto up = RunEnvChecks(api.port());
    REQUIRE(up.size() >= 2);
    CHECK(up[1].state == 2); // same call, only the server differs
}

TEST_CASE("env check: a progress callback is optional") {
    const auto items = RunEnvChecks(unused_port());
    CHECK(items.size() == 7); // 3b) VHCI 根设备探测（服务在 != 主控在，2026-11-01 真机教训）
}

TEST_CASE("english-only rule: the scanner flags non-ASCII string literals and ignores comments") {
    const std::string cn = "\xE4\xB8\xAD\xE6\x96\x87"; // two CJK characters in UTF-8
    CHECK(non_ascii_literals("const char* a = \"plain\";\n").empty());
    CHECK(non_ascii_literals("const char* a = \"" + cn + "\";\n") == std::vector<int>{1});
    CHECK(non_ascii_literals("int x; // " + cn + " \"" + cn + "\"\n").empty());   // line comment
    CHECK(non_ascii_literals("/* " + cn + "\n " + cn + " */ int x;\n").empty());   // block comment over two lines
    CHECK(non_ascii_literals("char q = '\"'; const char* a = \"ok\";\n").empty()); // a quote inside a char literal
    CHECK(non_ascii_literals("a\n\"x\\\"" + cn + "\"\n") == std::vector<int>{2});  // escaped quote does not end the literal
    CHECK(non_ascii_literals("L\"" + cn + "\"\n") == std::vector<int>{1});         // wide literal
}

TEST_CASE("english-only rule: no source file that ships in the package has a non-ASCII string literal") {
    namespace fs = std::filesystem;
    const fs::path root = UAC_SOURCE_DIR;
    std::size_t files = 0;
    for (const char* sub : {"apps", "src", "include"}) {
        for (const auto& e : fs::recursive_directory_iterator(root / sub)) {
            const auto ext = e.path().extension();
            if (!e.is_regular_file() || (ext != ".cpp" && ext != ".h")) continue;
            std::ifstream f(e.path(), std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            ++files;
            const auto hits = non_ascii_literals(ss.str());
            for (int line : hits) {
                FAIL_CHECK(e.path().string() << ":" << line
                           << " has a string literal with non-ASCII text (Sandbox has no CJK fonts; keep UI text English)");
            }
        }
    }
    CHECK(files > 20); // the scan must really have looked at the tree
}
