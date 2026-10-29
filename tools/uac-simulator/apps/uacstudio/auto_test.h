// Auto-test suite: scripted end-to-end self-test against the live uacsimd
// HTTP API, driving every state and function (attach/hot-swap/events/HID/
// source/capture/inject/multi-instance/detach). Runs on a worker thread;
// reports progress lines through a caller-supplied callback.
#pragma once
#include "http_client.h"
#include <cstdint>
#include <functional>
#include <string>

namespace uacstudio {

// Blocking run of the full suite. `progress` is called from the worker thread
// with one line per step: "[PASS] name", "[FAIL] name -> detail", or a
// summary line. Returns the number of failed steps.
int RunAutoTestSuite(std::uint16_t port, const std::function<void(const std::string&)>& progress);

} // namespace uacstudio
