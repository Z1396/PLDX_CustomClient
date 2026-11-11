cmake_minimum_required(VERSION 3.12)
project(RoboGo)

# set options
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# fix issues in MSVC
if(MSVC)
    add_compile_options("/utf-8")
endif()

# third party
include(FetchContent)

# 3rdparty_downloads 中已有源码时直接复用，不再联网下载（兼容代理不可用/离线环境）
set(dep_names directxmath directx-headers directxtk12 glfw abseil-cpp spdlog boost async_mqtt readerwriterqueue protobuf stb_src imgui_src)
set(dep_dirs directxmath directx-headers directxtk12 glfw abseil-cpp spdlog boost async_mqtt readerwriterqueue protobuf stb imgui)
list(LENGTH dep_names dep_count)
math(EXPR dep_last "${dep_count} - 1")
foreach(i RANGE 0 ${dep_last})
    list(GET dep_names ${i} dep_name)
    list(GET dep_dirs ${i} dep_dir)
    file(GLOB dep_entries "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/${dep_dir}/*")
    if(dep_entries)
        string(TOUPPER "${dep_name}" dep_upper)
        set(FETCHCONTENT_SOURCE_DIR_${dep_upper} "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/${dep_dir}" CACHE PATH "本地依赖源码目录" FORCE)
    endif()
endforeach()

set(CMAKE_SKIP_INSTALL_RULES ON) # we can't use an option to skip async_mqtt install rules, which is failed because some boost targets are not export
set(BOOST_ENABLE_COMPATIBILITY_TARGETS ON)
set(ASYNC_MQTT_USE_STATIC_BOOST ON)
set(ASYNC_MQTT_BUILD_LIB ON)
set(protobuf_INSTALL OFF)
set(protobuf_BUILD_TESTS OFF)
set(protobuf_BUILD_PROTOC_BINARIES OFF)
set(protobuf_BUILD_LIBPROTOC OFF)
set(protobuf_BUILD_LIBUPB OFF)
set(protobuf_LOCAL_DEPENDENCIES_ONLY ON)
set(protobuf_BUILD_SHARED_LIBS OFF)
set(protobuf_BUILD_PROTOBUF_LIBRARIES OFF)
set(protobuf_WITH_ZLIB OFF)
set(GLFW_BUILD_DOCS OFF)
set(GLFW_INSTALL OFF)
set(GLFW_LIBRARY_TYPE STATIC)

if(WIN32)
    # DirectXTK12 在 MinGW 下强制要求 directxmath / directx-headers 作为 CMake 包提供
    FetchContent_Declare(
            directxmath
            GIT_REPOSITORY https://github.com/microsoft/DirectXMath.git
            GIT_TAG may2026
            GIT_SHALLOW TRUE
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/directxmath"
            OVERRIDE_FIND_PACKAGE
    )
    FetchContent_Declare(
            directx-headers
            GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
            GIT_TAG v1.619.5
            GIT_SHALLOW TRUE
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/directx-headers"
            OVERRIDE_FIND_PACKAGE
    )
    FetchContent_MakeAvailable(directxmath directx-headers)
    # DirectXMath 的构建树中没有 Microsoft:: 命名空间的别名目标，需要手动补上
    if(NOT TARGET Microsoft::DirectXMath)
        add_library(Microsoft::DirectXMath ALIAS DirectXMath)
    endif()
    # DirectXTK12 需要用 dxc.exe 编译着色器，这里预先定位 Windows SDK 中自带的版本
    if(NOT DIRECTX_DXC_TOOL)
        file(GLOB dxc_tools "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/dxc.exe")
        list(SORT dxc_tools COMPARE NATURAL)
        list(LENGTH dxc_tools dxc_tools_count)
        if(dxc_tools_count GREATER 0)
            math(EXPR dxc_tools_last "${dxc_tools_count} - 1")
            list(GET dxc_tools ${dxc_tools_last} DIRECTX_DXC_TOOL)
            set(DIRECTX_DXC_TOOL "${DIRECTX_DXC_TOOL}" CACHE FILEPATH "DirectXShaderCompiler executable")
        endif()
    endif()
    FetchContent_Declare(
            directxtk12
            URL https://github.com/microsoft/DirectXTK12/archive/refs/tags/may2026.tar.gz
            URL_HASH SHA256=665af51a84243001cce1000265ec638e81b56d323516314689a91c3ea41f91b5
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/directxtk12"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
else ()
    FetchContent_Declare(
            glfw
            URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
            URL_HASH SHA256=c038d34200234d071fae9345bc455e4a8f2f544ab60150765d7704e08f3dac01
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/glfw"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()
FetchContent_Declare(
    abseil-cpp
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG fe126c727b925b7c3b2742f6f54690973216952b # May 29, 2026
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/abseil-cpp"
)
FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/spdlog"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    boost
    URL https://github.com/boostorg/boost/releases/download/boost-1.91.0-1/boost-1.91.0-1-cmake.tar.xz
    URL_HASH SHA256=cc5dc5006ecbdf0051f90979be31b4eee5987d9ae14ae9fb9c03cfa43fa3cdad
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/boost"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_Declare(
    async_mqtt
    URL https://github.com/redboltz/async_mqtt/archive/refs/tags/10.3.0.tar.gz
    URL_HASH SHA256=01daa741f970a04551c8375553bc96ce66a0bf542466a88a141863cc8c2a690c
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/async_mqtt"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    readerwriterqueue
    GIT_REPOSITORY https://github.com/cameron314/readerwriterqueue.git
    GIT_TAG 131af2c479c6ba36142ee9805e9f60fc4adfa22c # Mar 29, 2026
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/readerwriterqueue"
)
FetchContent_Declare(
    protobuf
    URL https://github.com/protocolbuffers/protobuf/archive/refs/tags/v35.0.tar.gz
    URL_HASH SHA256=e127ea69dd7be4e88abdd95845fb6c30d25d96971d95827e92b70e2e910d46a1
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/protobuf"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    stb_src
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4 # Apr 16, 2026
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/stb"
)
FetchContent_Declare(
    imgui_src
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.8.tar.gz
    URL_HASH SHA256=fecb33d33930e12ff53a34064e9d3a06c8f7c3e04408f14cd36c80e3faac863b
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
if(WIN32)
    FetchContent_MakeAvailable(directxtk12)
    # DirectXTK12 自带 CMake 把 C++ 标准锁在较低版本（内部 set(CMAKE_CXX_STANDARD 17)）。
    # 在项目层把它的 target 属性统一到全局标准，避免同一个 exe 里混用不同 /std: 编译的 STL 代码。
    if(TARGET DirectXTK12)
        set_target_properties(DirectXTK12 PROPERTIES
            CXX_STANDARD ${CMAKE_CXX_STANDARD}
            CXX_STANDARD_REQUIRED ON)
    endif()
else ()
    FetchContent_MakeAvailable(glfw)
endif()
FetchContent_MakeAvailable(abseil-cpp spdlog boost async_mqtt readerwriterqueue protobuf imgui_src stb_src)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/stb)
add_library(imgui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/misc/cpp/imgui_stdlib.cpp
)
target_compile_definitions(imgui PUBLIC
    IMGUI_USER_CONFIG="my_imgui_config.h"
)
if(WIN32)
    target_sources(imgui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends/imgui_impl_dx12.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends/imgui_impl_win32.cpp
    )
else()
    target_sources(imgui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends/imgui_impl_vulkan.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends/imgui_impl_glfw.cpp
    )
endif()
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty_downloads/imgui/misc/cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/imgui/include
)
if(WIN32)
    target_link_libraries(imgui PUBLIC
        d3d12.lib dxgi.lib
        DirectXTK12
    )
else()
    target_link_libraries(imgui PUBLIC
        glfw
        vulkan
    )
endif()
add_subdirectory(3rdparty/ffmpeg)

set(GENERATED_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/resource")
file(MAKE_DIRECTORY "${GENERATED_RESOURCE_DIR}")

function(write_file_if_different output_path content)
    set(temp_file "${output_path}.tmp")
    file(WRITE "${temp_file}" "${content}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${temp_file}" "${output_path}"
        COMMAND_ERROR_IS_FATAL ANY
    )
    file(REMOVE "${temp_file}")
endfunction()

function(add_embedded_resource input_relative_path generated_name)
    set(input_file "${CMAKE_CURRENT_SOURCE_DIR}/${input_relative_path}")
    set(output_header "${GENERATED_RESOURCE_DIR}/${generated_name}.h")
    set(output_source "${GENERATED_RESOURCE_DIR}/${generated_name}.cpp")

    file(SIZE "${input_file}" embedded_resource_size)
    file(READ "${input_file}" embedded_resource_hex HEX)
    string(TOLOWER "${embedded_resource_hex}" embedded_resource_hex)

    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " embedded_resource_bytes "${embedded_resource_hex}")
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f], ){32})" "\\1\n    " embedded_resource_bytes "${embedded_resource_bytes}")

    string(CONCAT output_header_content
        "#pragma once\n\n"
        "extern const unsigned char ${generated_name}_data[];\n"
        "extern const unsigned int ${generated_name}_data_len;\n"
    )
    write_file_if_different("${output_header}" "${output_header_content}")

    string(CONCAT output_source_content
        "#include \"${generated_name}.h\"\n\n"
        "const unsigned char ${generated_name}_data[] = {\n"
        "    ${embedded_resource_bytes}\n"
        "};\n\n"
        "const unsigned int ${generated_name}_data_len = ${embedded_resource_size}u;\n"
    )
    write_file_if_different("${output_source}" "${output_source_content}")

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${input_file}"
    )
    set(EMBEDDED_RESOURCE_SOURCES ${EMBEDDED_RESOURCE_SOURCES} "${output_source}" PARENT_SCOPE)
endfunction()

set(EMBEDDED_RESOURCE_SOURCES "")
add_embedded_resource("resources/map.png" "embedded_map_png")
add_embedded_resource("resources/map_terrain.png" "embedded_map_terrain_png")  # 地形分层叠加层（由 2027 高度掩膜生成，带 alpha）
add_embedded_resource("resources/zcool.ttf" "embedded_zcool_font")
add_embedded_resource("proto/messages.proto" "embedded_messages_proto")

# add executable
add_executable(RoboGo WIN32
    src/main.cpp
    src/app/application.cpp
    src/app/config.cpp
    src/app/custom_control_publisher.cpp
    src/app/robot_registry.cpp
    src/app/perf_stats.cpp
    src/app/imgui_setup.cpp
    src/custom_control_payload.cpp
    src/main_window.cpp
    src/mqtt/event_formatter.cpp
    src/mqtt/handlers_match.cpp
    src/mqtt/handlers_robot.cpp
    src/mqtt/handlers_ui.cpp
    src/mqtt/mqtt_router.cpp
    src/keyboard_mouse_state_manager.cpp
    src/panel/bottom_panel.cpp
    src/panel/buff_panel.cpp
    src/panel/commander_panel.cpp
    src/panel/crosshair.cpp
    src/panel/header_panel.cpp
    src/panel/log_panel.cpp
    src/panel/mini_map.cpp
    src/panel/panel_layout.cpp
    src/panel/perf_panel.cpp
    src/panel/robot_status_panel.cpp
    src/panel/start_panel.cpp
    src/logging/logging.cpp
    src/io/mqtt_client.cpp
    src/io/video_stream_receiver.cpp
    src/video/rgba_image_transform.cpp
    src/video/video_decoder.cpp
    src/video/video_shards_manager.cpp
    src/video/video_stream_manager.cpp
    src/platform/linux_platform_backend.cpp
    src/platform/windows_platform_backend.cpp
    src/resource/resources.cpp
    gen/messages.pb.cc
    ${EMBEDDED_RESOURCE_SOURCES}
)
if(WIN32)
    # 资源脚本单独编译，避免继承 RoboGo 的全部 include 路径导致 windres/rc 命令行超长
    add_library(robogo_resources OBJECT resources/resources.rc)
    target_include_directories(robogo_resources PRIVATE include resources)
    target_link_libraries(RoboGo PRIVATE robogo_resources)
endif()
target_precompile_headers(RoboGo PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/pch.h>")
target_include_directories(RoboGo PRIVATE
    include
    gen
    src
    ${GENERATED_RESOURCE_DIR}
)

if(WIN32)
    target_compile_definitions(RoboGo PRIVATE _WIN32_WINNT=0x0601)
endif()
# 日志级别：Release 只编到 INFO，DEBUG/TRACE 语句会被编译器整条剔除（连参数都不求值）；
# Debug 构建保留 TRACE 便于排查。
# 这一条直接影响帧时间：视频路径里原本每帧都有一条 SPDLOG_DEBUG，Release 下等于每帧白做一次字符串格式化。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(RoboGo PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
else()
    target_compile_definitions(RoboGo PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()

set_source_files_properties(
    src/resource/resources.cpp
    ${EMBEDDED_RESOURCE_SOURCES}
    PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
)

target_link_libraries(RoboGo PRIVATE
    spdlog::spdlog
    utf8_validity
    protobuf::libprotobuf-lite
    async_mqtt_asio_bind
    ffmpeg
    imgui
    readerwriterqueue
    stb
)

# 拆分全局 PCH 后，第三方头改由各 TU 显式包含，原本只在构建 PCH 时报一次的风格类诊断
# 会成倍放大（实测 99% 的 warning 来自 boost/spdlog/imgui/ffmpeg，而非项目源码）。
# 先关掉全部诊断组，随后再开 -Wall/-Wextra，保留项目代码的基础诊断；不改变生成代码。
# 注意顺序：-Wno-everything 必须在 -Wall/-Wextra 之前，否则会把它们一起关掉。
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(RoboGo PRIVATE -Wno-everything)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(RoboGo PRIVATE -Wall -Wextra)
    set_source_files_properties(
        ${EMBEDDED_RESOURCE_SOURCES}
        PROPERTIES
        COMPILE_FLAGS -g0
    )
elseif(MSVC)
    target_compile_options(RoboGo PRIVATE /bigobj /W4)
    target_link_options(RoboGo PRIVATE /MAP)
endif()

# ---------------------------------------------------------------------------
# 单元测试：只覆盖纯逻辑（载荷布局 / 发布节流 / 事件文案 / ID 映射），不依赖 ImGui 与平台后端。
# 构建：cmake --build build/release --target RoboGoTests
# 运行：ctest --test-dir build/release --output-on-failure
# ---------------------------------------------------------------------------
enable_testing()

add_executable(RoboGoTests
    tests/test_main.cpp
    src/app/robot_registry.cpp
    src/custom_control_payload.cpp
    src/mqtt/event_formatter.cpp
    gen/messages.pb.cc
)
target_include_directories(RoboGoTests PRIVATE include gen src)
target_precompile_headers(RoboGoTests PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/pch.h>")
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(RoboGoTests PRIVATE -Wno-everything -Wall -Wextra)
endif()
target_compile_definitions(RoboGoTests PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
if(WIN32)
    target_compile_definitions(RoboGoTests PRIVATE _WIN32_WINNT=0x0601)
endif()
target_link_libraries(RoboGoTests PRIVATE
    spdlog::spdlog
    utf8_validity
    protobuf::libprotobuf-lite
    async_mqtt_asio_bind
    ffmpeg
    imgui
    readerwriterqueue
    stb
)
add_test(NAME robogo_unit COMMAND RoboGoTests)
