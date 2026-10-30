// SimHost（HTTP→设备模型的编排层）单测。此前 SimHost 只编进 uacsimd.exe，测试够不着，
// profile 名过滤漏掉反斜杠的 bug 因此没被发现。这里直接调用各 on_* 回调。
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <sim_host.h>
#include <windows.h>
#include <filesystem>
#include <fstream>

using nlohmann::json;
using uac::SimHost;

namespace {

constexpr char kBs = 0x5C;

std::string profiles_dir() { return std::string(UAC_SOURCE_DIR) + "/profiles"; }

// usbip=0：由系统分配监听端口，测试之间互不冲突
struct Host {
    SimHost h{profiles_dir()};
    Host() { REQUIRE(h.start(0, 0)); }
    ~Host() { h.stop(); }
    json attach(const std::string& name, bool confine = true) {
        return json::parse(h.on_attach(name, confine));
    }
};

std::string temp_wav(const char* leaf) {
    char tmp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + leaf;
}

} // namespace

// POST /shutdown only *requests* the exit (the process main loop does the stopping after the reply went out): the flag is
// what uacsimd.exe polls. Nothing else may set it.
TEST_CASE("sim host: /shutdown sets the shutdown-requested flag, other requests do not") {
    Host host;
    CHECK_FALSE(host.h.shutdown_requested());
    host.h.on_detach();
    host.attach("uac2-48k");
    CHECK_FALSE(host.h.shutdown_requested());

    const auto reply = json::parse(host.h.on_shutdown());
    CHECK(reply.value("ok", false));
    CHECK(host.h.shutdown_requested());
    CHECK(host.h.running()); // requesting is not stopping: the reply must still be able to go out
}

TEST_CASE("sim host: attach by profile file name and by built-in name") {
    Host host;
    auto r = host.attach("uac2-48k"); // 来自 profiles/uac2-48k.json
    CHECK(r.value("attached", "") == "uac2-48k");
    CHECK(r.value("busid", "") == "1-1");
    auto st = json::parse(host.h.on_status());
    CHECK(st["listening"] == true);
    CHECK(st["profile"] == "uac2-48k");
    CHECK(st["instances"] == 1);

    r = host.attach("uac1-cmedia-sim.json"); // 带 .json 后缀也接受
    CHECK(r.value("attached", "") == "uac1-cmedia-sim.json");
    CHECK(host.h.instance_count() == 2);
}

TEST_CASE("sim host: HTTP-supplied profile names are confined to the whitelist") {
    Host host;
    const std::string bs1(1, kBs), bs2(2, kBs);
    const std::string bad[] = {
        "a/b",
        "../x",
        "..",
        "C:foo",
        bs1 + "Windows" + bs1 + "win",                        // 回归：旧黑名单漏掉反斜杠
        // UNC（无点号，回归旧黑名单）：过滤回退时 fs::exists 会访问它；用 localhost 避免对外发请求
        bs2 + "localhost" + bs1 + "share" + bs1 + "x",
        bs2 + "localhost.localdomain" + bs1 + "share" + bs1 + "x", // UNC（带点号）
        "a" + bs1 + "b",
        "",
    };
    for (const auto& name : bad) {
        INFO("name=[" << name << "]");
        const auto r = host.attach(name);
        CHECK(r.value("error", "") == "invalid profile name");
    }
    CHECK(host.h.instance_count() == 0); // 被拒绝的请求不改变状态
    CHECK(json::parse(host.h.on_status())["listening"] == false);
}

TEST_CASE("sim host: valid-but-unknown name reports not found (filter passed, lookup failed)") {
    Host host;
    const auto r = host.attach("no-such-profile");
    CHECK(r.value("error", "").find("profile not found") == 0);
}

TEST_CASE("sim host: CLI path (confineName=false) still accepts a full path") {
    Host host;
    const std::string full = profiles_dir() + "/uac2-48k.json";
    REQUIRE(std::filesystem::exists(full));
    const auto r = host.attach(full, /*confine=*/false);
    // 完整路径经 fs::path(dir)/abs 解析为 abs 本身；只要不是被过滤器拒绝即符合契约
    CHECK(r.value("error", "") != "invalid profile name");
}

TEST_CASE("sim host: instance cap is 4, detach resets everything") {
    Host host;
    for (int i = 1; i <= 4; ++i) {
        const auto r = host.attach("uac2-48k");
        CHECK(r.value("busid", "") == "1-" + std::to_string(i));
    }
    const auto over = host.attach("uac2-48k");
    CHECK(over.contains("error"));
    CHECK(host.h.instance_count() == 4);

    const auto d = json::parse(host.h.on_detach());
    CHECK(d["ok"] == true);
    CHECK(host.h.instance_count() == 0);
    CHECK(json::parse(host.h.on_status())["listening"] == false);
}

TEST_CASE("sim host: event/inject/source/capture require an attached device") {
    Host host;
    CHECK(json::parse(host.h.on_event("volume_up")).value("error", "") == "not attached");
    CHECK(json::parse(host.h.on_inject("{}")).value("error", "") == "not attached");
    CHECK(json::parse(host.h.on_source("{}")).value("error", "") == "not attached");
    CHECK(json::parse(host.h.on_capture("{}")).value("error", "") == "not attached");
}

TEST_CASE("sim host: events and injection kinds") {
    Host host;
    host.attach("uac2-48k");
    for (const char* e : {"volume_up", "volume_down", "mute", "hid_vol_up", "hid_vol_down",
                          "hid_mute", "hid_release"}) {
        INFO("event=" << e);
        CHECK(json::parse(host.h.on_event(e)).value("ok", false));
    }
    CHECK(json::parse(host.h.on_event("bogus")).contains("error"));

    for (const char* body :
         {"{\"kind\":\"stall\",\"bRequest\":129}", "{\"kind\":\"delay_ms\",\"ms\":5}",
          "{\"kind\":\"corrupt_desc_at\",\"byteOffset\":10}", "{\"kind\":\"half_enumerate\"}",
          "{\"kind\":\"disconnect\",\"afterMs\":0}", "{\"kind\":\"iso_error\",\"everyN\":2}",
          "{\"kind\":\"clear\"}"}) {
        INFO("inject=" << body);
        CHECK(json::parse(host.h.on_inject(body)).value("ok", false));
    }
    CHECK(json::parse(host.h.on_inject("{\"kind\":\"nope\"}")).contains("error"));
    CHECK(json::parse(host.h.on_inject("not json")).contains("error"));
}

TEST_CASE("sim host: source kinds, and file sources go through the path policy") {
    Host host;
    host.attach("uac2-48k");
    CHECK(json::parse(host.h.on_source("{\"kind\":\"sine\",\"hz\":440}")).value("ok", false));
    CHECK(json::parse(host.h.on_source("{\"kind\":\"silence\"}")).value("ok", false));
    CHECK(json::parse(host.h.on_source("{\"kind\":\"zzz\"}")).contains("error"));

    const std::string bs2(2, kBs);
    for (const std::string& f : {std::string("secrets.txt"), std::string("../x.wav"),
                                 bs2 + "host" + kBs + "s" + kBs + "x.wav"}) {
        INFO("file=" << f);
        const auto r = json::parse(host.h.on_source(json{{"kind", "file"}, {"file", f}}.dump()));
        CHECK(r.value("error", "").find("invalid file path") == 0);
    }
}

TEST_CASE("sim host: capture path policy, and a legitimate capture writes a WAV") {
    Host host;
    host.attach("uac2-48k");

    for (const std::string& p : {std::string("notes.txt"), std::string("..") + kBs + "x.wav",
                                 std::string("a.wav:stream")}) {
        INFO("path=" << p);
        const auto r = json::parse(host.h.on_capture(json{{"path", p}}.dump()));
        CHECK(r.value("error", "").find("invalid capture path") == 0);
    }

    const std::string wav = temp_wav("uac_test_sim_host_capture.wav");
    std::filesystem::remove(wav);
    const auto ok = json::parse(host.h.on_capture(json{{"path", wav}}.dump()));
    CHECK(ok.value("ok", false));
    const auto stop = json::parse(host.h.on_capture("{\"path\":\"\"}")); // 空路径=关闭，无需校验
    CHECK(stop.value("ok", false));
    CHECK(std::filesystem::exists(wav));
    CHECK(std::filesystem::file_size(wav) >= 44); // WAV 头
    std::filesystem::remove(wav);
}

TEST_CASE("sim host: status exposes version and stream counters") {
    Host host;
    host.attach("uac2-48k");
    const auto st = json::parse(host.h.on_status());
    CHECK(st["running"] == true);
    CHECK(st.contains("version"));
    CHECK(st["sampleRate"] == 48000);
    CHECK(st.contains("inBytes"));
    CHECK(host.h.http_port() != 0);
}

TEST_CASE("sim host: log file option applies to models attached afterwards") {
    Host host;
    const std::string logf = temp_wav("uac_test_sim_host_log.ndjson");
    std::filesystem::remove(logf);
    host.h.set_log_file(logf);
    host.attach("uac2-48k");
    host.h.on_event("volume_up");
    host.h.on_detach(); // 析构模型，刷出文件
    CHECK(std::filesystem::exists(logf));
    std::filesystem::remove(logf);
}
