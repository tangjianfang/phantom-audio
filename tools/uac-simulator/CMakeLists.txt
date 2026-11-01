cmake_minimum_required(VERSION 3.24)
project(uac_simulator LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 只支持 MSVC。没有 MSVC 环境时 CMake 会悄悄选中 PATH 里的 MinGW g++（CI runner 上就有），
# 配置"成功"、编译才在满屏 MSVC 专用写法上失败——在这里提前、明确地报错。
if(NOT MSVC)
  message(FATAL_ERROR
    "需要 MSVC 工具链（当前: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER}）。"
    "请通过 scripts/msvc-env.cmd 运行 cmake，或在 VS 开发者命令行中运行。")
endif()

# 警告当错误：窄化转换、未使用参数、转义序列写错（C4129 曾让反斜杠过滤失效而无人察觉）
option(UAC_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(MSVC)
  # /W4 高警告; /utf-8 统一源码编码; /MP 并行编译
  add_compile_options(/W4 /permissive- /Zc:preprocessor /utf-8 /MP)
  if(UAC_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
  # 静态链接 CRT（/MT、Debug 为 /MTd）：产物不依赖 VC++ Redistributable，免安装运行库
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
  src/msos20.cpp
  src/path_policy.cpp
)
target_include_directories(uac_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_compile_definitions(uac_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(uac_core PUBLIC ws2_32)

# ---------------- 服务编排层（uacsimd 与单测共用） ----------------
# SimHost 是 HTTP→设备模型的编排层，含 attach/路径校验等安全相关逻辑；
# 之前只编进 uacsimd.exe，单测碰不到，所以那里的 bug 没被抓住。
add_library(uac_host STATIC apps/uacsimd/sim_host.cpp apps/uacsimd/speaker_monitor.cpp)
target_include_directories(uac_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/apps/uacsimd)
target_link_libraries(uac_host PUBLIC uac_core winmm)

# ---------------- uacstudio 的非窗口逻辑（GUI 与单测共用） ----------------
# 控制层（按钮表/请求体构建）与一键自动测试的用例表、运行器。主窗口不在这里，
# 所以能在开发机上单测；GUI/--autotest/Sandbox 三个入口共用同一张用例表。
# env_check.cpp 虽含环境检查窗口，但测试只调用其中的 RunEnvChecks（逻辑部分）；窗口代码不会被测试引用。
# 不链接 uac_core：它的 PUBLIC 编译定义会改变 uacstudio 里既有源文件的 windows.h 环境。
add_library(uac_studio_core STATIC
  apps/uacstudio/auto_test.cpp
  apps/uacstudio/controller.cpp
  apps/uacstudio/driver_installer.cpp
  apps/uacstudio/env_check.cpp
  apps/uacstudio/host_ops.cpp
  apps/uacstudio/sim_process.cpp
)
target_include_directories(uac_studio_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/apps/uacstudio
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(uac_studio_core PUBLIC ws2_32 shell32 advapi32 ole32 bcrypt)

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
  tests/test_msos20.cpp
  tests/test_coverage_gaps.cpp
  tests/test_coverage_gaps2.cpp
  tests/test_coverage_gaps3.cpp
  tests/test_path_policy.cpp
  tests/test_sim_host.cpp
  tests/test_hardening.cpp
  tests/test_controller.cpp
  tests/test_auto_test.cpp
  tests/test_driver_installer.cpp
  tests/test_http_client.cpp
  tests/test_env_check.cpp
  tests/test_host_ops.cpp
  tests/test_sim_process.cpp
)
target_link_libraries(uac_tests PRIVATE uac_host uac_studio_core uac_core ws2_32)
# 测试要读 profiles/*.json：用源码树绝对路径，不依赖 ctest 的工作目录
target_compile_definitions(uac_tests PRIVATE UAC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
# test_sim_process.cpp starts the real uacsimd.exe: it has to be built before the tests run
add_dependencies(uac_tests uacsimd)
add_test(NAME uac_tests COMMAND uac_tests)

# ---------------- 应用 ----------------
add_executable(uacsimd apps/uacsimd/main.cpp)
target_link_libraries(uacsimd PRIVATE uac_host uac_core ws2_32)
# uacsimd 从 exe 旁的 profiles/ 解析文件型 profile（uac2-48k / uac2-hid-headset / uac1-razer-clio
# 不在内置表里）：开发树不部署的话 GUI/CLI 只剩内置名可用（Sandbox zip 由 package-sandbox.ps1
# 部署同一目录）。部署契约由 test_sim_host 的"GUI 名单全部可解析"用例看着。
add_custom_command(TARGET uacsimd POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/profiles
            $<TARGET_FILE_DIR:uacsimd>/profiles)

add_executable(uacsim apps/uacsim/main.cpp)
target_include_directories(uacsim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(uacsim PRIVATE ws2_32)

add_executable(host_probe apps/host_probe/main.cpp)
target_link_libraries(host_probe PRIVATE uac_core ole32)

add_executable(uacstudio WIN32
  apps/uacstudio/main.cpp
  apps/uacstudio/gui_selfcheck.cpp
  apps/uacstudio/uacstudio.rc
)
target_include_directories(uacstudio PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(uacstudio PRIVATE uac_studio_core ws2_32 comctl32 advapi32 shell32)
set_target_properties(uacstudio PROPERTIES LINK_FLAGS "/ENTRY:wWinMainCRTStartup")
