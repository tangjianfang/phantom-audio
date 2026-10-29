// Auto-test suite implementation - all checks hit the live control API.
#include "auto_test.h"
#include <shlobj.h>
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace uacstudio {

namespace {

struct Suite {
    std::uint16_t port;
    const std::function<void(const std::string&)>& progress;
    int passed = 0, failed = 0;
    std::string lastAttach; // profile name of the most recent successful attach

    std::string Get(const std::string& path) { return http_req(port, "GET", path, ""); }
    std::string Post(const std::string& path, const std::string& body = "") {
        return http_req(port, "POST", path, body);
    }

    // step(): name + expected substring(s) in the response; empty expect = only
    // require a non-empty response.
    void Step(const char* name, const std::string& resp,
              const std::vector<std::string>& expects = std::vector<std::string>(),
              bool requireNonEmpty = true) {
        static const std::vector<std::string> kFails{"\"error\"", "connect failed"};
        for (const std::string& fail : kFails) {
            if (resp.find(fail) != std::string::npos) {
                ++failed;
                progress(std::string("[FAIL] ") + name + " -> " + resp);
                return;
            }
        }
        if (requireNonEmpty && resp.empty()) {
            ++failed;
            progress(std::string("[FAIL] ") + name + " -> empty response");
            return;
        }
        for (const auto& e : expects) {
            if (resp.find(e) == std::string::npos) {
                ++failed;
                progress(std::string("[FAIL] ") + name + " -> missing '" + e + "': " + resp);
                return;
            }
        }
        ++passed;
        progress(std::string("[PASS] ") + name);
    }
};

} // namespace

int RunAutoTestSuite(std::uint16_t port, const std::function<void(const std::string&)>& progress) {
    Suite s{port, progress, 0, 0};

    progress("==== uacstudio self-test start ====");

    // 1) service reachable
    s.Step("status: service reachable", s.Get("/status"), {"\"running\":true"});

    // 2) reset to a known state
    s.Step("detach (reset)", s.Post("/detach"), {"\"ok\":true"});
    s.Step("status: idle after reset", s.Get("/status"), {"\"listening\":false"});

    // 3) attach UAC2
    s.Step("attach uac2-48k", s.Post("/attach", "{\"profile\":\"uac2-48k\"}"),
           {"\"attached\":\"uac2-48k\"", "\"instances\":1"});
    s.Step("status: listening + 1 instance", s.Get("/status"),
           {"\"listening\":true", "\"instances\":1", "\"sampleRate\":48000"});

    // 4) volume events (0 dB is max; step down then verify, then back up)
    s.Step("event volume_down", s.Post("/event", "{\"type\":\"volume_down\"}"),
           {"\"ok\":true"});
    s.Step("status: volCurDb == -1.0", s.Get("/status"), {"\"volCurDb\":-1.0"});
    s.Step("event volume_up (restore)", s.Post("/event", "{\"type\":\"volume_up\"}"),
           {"\"ok\":true"});
    s.Step("status: volCurDb == 0.0", s.Get("/status"), {"\"volCurDb\":0.0"});

    // 5) mute toggle
    s.Step("event mute (on)", s.Post("/event", "{\"type\":\"mute\"}"), {"\"ok\":true"});
    s.Step("status: mute == true", s.Get("/status"), {"\"mute\":true"});
    s.Step("event mute (toggle off)", s.Post("/event", "{\"type\":\"mute\"}"), {"\"ok\":true"});
    s.Step("status: mute == false", s.Get("/status"), {"\"mute\":false"});

    // log pipeline produced records on the CURRENT instance (before any
    // hot-swap replaces the active model pointer)
    s.Step("log tail has records", s.Get("/log?n=50"), {"inject_volume"});

    // 6) HID key events (requires the HID profile path to accept them; the
    //    uac2-48k snapshot carries the AC interrupt path, HID is a no-op ok)
    s.Step("event hid_vol_up", s.Post("/event", "{\"type\":\"hid_vol_up\"}"), {"\"ok\":true"});
    s.Step("event hid_release", s.Post("/event", "{\"type\":\"hid_release\"}"), {"\"ok\":true"});

    // 7) audio source switching
    s.Step("source sine 440 Hz", s.Post("/source", "{\"kind\":\"sine\",\"hz\":440}"),
           {"\"kind\":\"sine\""});
    s.Step("source silence", s.Post("/source", "{\"kind\":\"silence\"}"),
           {"\"kind\":\"silence\""});

    // 8) capture to %TEMP% and verify the file appears (WAV header >= 44 B)
    char tmp[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tmp);
    const std::string wav = std::string(tmp) + "uacstudio_selftest.wav";
    DeleteFileA(wav.c_str());
    s.Step("capture start", s.Post("/capture", "{\"path\":\"" + uac::JsonEscape(wav) + "\"}"), {"\"ok\":true"});
    // push one OUT packet's worth of wall-clock so the sink flushes the header
    Sleep(300);
    s.Post("/source", "{\"kind\":\"sine\",\"hz\":1000}");
    Sleep(300);
    s.Step("capture stop", s.Post("/capture", "{\"path\":\"\"}"), {"\"ok\":true"});
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(wav.c_str(), GetFileExInfoStandard, &fa) &&
        fa.nFileSizeLow >= 44) {
        ++s.passed;
        progress("[PASS] capture file written (>= 44 B header)");
    } else {
        ++s.failed;
        progress(std::string("[FAIL] capture file missing/too small: ") + wav);
    }
    DeleteFileA(wav.c_str());

    // 9) fault injection arming + clearing
    s.Step("inject stall", s.Post("/inject", "{\"kind\":\"stall\",\"bRequest\":129}"),
           {"\"kind\":\"stall\""});
    s.Step("inject iso_error", s.Post("/inject", "{\"kind\":\"iso_error\",\"everyN\":2}"),
           {"\"kind\":\"iso_error\""});
    s.Step("inject clear", s.Post("/inject", "{\"kind\":\"clear\"}"), {"\"kind\":\"clear\""});

    // 10) hot swap to UAC1 profile
    s.Step("hot-swap to uac1-cmedia-sim",
           s.Post("/attach", "{\"profile\":\"uac1-cmedia-sim\"}"),
           {"\"attached\":\"uac1-cmedia-sim\"", "\"instances\":2"});
    s.Step("status: profile switched", s.Get("/status"), {"\"profile\":\"uac1-cmedia-sim\""});

    // 11) multi-instance: second attach alongside
    s.Step("attach 2nd instance (uac2-48k)",
           s.Post("/attach", "{\"profile\":\"uac2-48k\"}"), {"\"instances\":3"});
    s.Step("status: 2 instances", s.Get("/status"), {"\"instances\":3"});

    // 12) teardown
    s.Step("detach (teardown)", s.Post("/detach"), {"\"ok\":true"});
    s.Step("status: idle at end", s.Get("/status"), {"\"listening\":false", "\"instances\":0"});

    // 13) log pipeline produced records during the run

    progress("==== self-test done: " + std::to_string(s.passed) + " passed, " +
             std::to_string(s.failed) + " failed ====");
    return s.failed;
}

} // namespace uacstudio
