cmake_minimum_required(VERSION 3.24)
project(uac_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # /W4 高警告; /utf-8 统一源码编码; /MP 并行编译
  add_compile_options(/W4 /permissive- /Zc:preprocessor /utf-8 /MP)
endif()

# ---------------- 核心静态库 ----------------
add_library(uac_core STATIC
  src/usbip_proto.cpp
  src/usbip_server.cpp
  src/usb_desc.cpp
  src/desc_uac1.cpp
  src/ude_adapter.cpp
  src/log_store.cpp
  src/ac_processor.cpp
  src/as_engine.cpp
  src/wav.cpp
  src/device_model.cpp
  src/desc_uac2.cpp
  src/event_injector.cpp
  src/desc_hid.cpp
  src/profile.cpp
  src/control_api.cpp
  src/fault_injector.cpp
  src/goertzel.cpp
)
target_include_directories(uac_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(uac_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(uac_core PUBLIC ws2_32)

# ---------------- 测试 ----------------
enable_testing()
add_executable(uac_tests
  tests/test_smoke.cpp
  tests/test_usbip_proto.cpp
  tests/test_usbip_server.cpp
  tests/test_usb_desc.cpp
  tests/test_desc_uac1.cpp
  tests/test_ac_processor.cpp
  tests/test_as_engine.cpp
  tests/test_device_model_loopback.cpp
  tests/test_desc_uac2.cpp
  tests/test_event_injector.cpp
  tests/test_desc_hid.cpp
  tests/test_profile.cpp
  tests/test_control_api.cpp
  tests/test_fault_injector.cpp
  tests/test_goertzel.cpp
)
target_link_libraries(uac_tests PRIVATE uac_core ws2_32)
add_test(NAME uac_tests COMMAND uac_tests)

# ---------------- 应用 ----------------
add_executable(uacsimd
  apps/uacsimd/main.cpp
  apps/uacsimd/sim_host.cpp
)
target_link_libraries(uacsimd PRIVATE uac_core ws2_32)

add_executable(uacsim apps/uacsim/main.cpp)
target_link_libraries(uacsim PRIVATE ws2_32)

add_executable(host_probe apps/host_probe/main.cpp)
target_link_libraries(host_probe PRIVATE uac_core ole32)

add_executable(uacstudio WIN32 apps/uacstudio/main.cpp)
target_link_libraries(uacstudio PRIVATE ws2_32 comctl32)
set_target_properties(uacstudio PROPERTIES LINK_FLAGS "/ENTRY:wWinMainCRTStartup")
