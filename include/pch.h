#pragma once
// 全局预编译头：只保留高频、稳定的 STL 与 spdlog。
// 第三方重头(boost/asio、async_mqtt、protobuf、FFmpeg、stb、readerwriterqueue、
// windows/D3D12、GLFW/Vulkan、ImGui 及后端)已按模块显式 include 到各源文件，
// 见源码中的 [pch-split] 标记。这样单 TU 的 preamble 明显变小，IDE 解析/跳转更快。
// this remove unused-includes warning in clangd
// see: https://github.com/include-what-you-use/include-what-you-use/blob/master/docs/IWYUPragmas.md#iwyu-pragma-begin_exportsend_exports
// IWYU pragma: begin_exports
// STL：集中包含项目常用的标准库头文件(容器、算法、并发、字符串、时间、文件系统、类型工具等)，
//      通过预编译头一次编译后供所有 .cpp 复用，显著加速整体编译。
// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
// spdlog：高性能日志库，提供异步日志、文件与控制台 sink。
// spdlog
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
// 下列宏是纯 #define（零预处理体积），保留在 PCH 中可保证在所有 TU 里最先生效，
// 避免各模块 include 顺序不同导致 API/实现差异。
// WIN32_LEAN_AND_MEAN：阻止 windows.h 抢先引入旧的 WinSock.h，
// 否则 boost/asio 后续引入 winsock2.h 时会报 "WinSock.h has already been included"。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// NOMINMAX：阻止 windows.h 定义 min/max 宏，否则 std::max/std::min 会被替换而编译失败。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef BOOST_ASIO_NO_DEPRECATED
#define BOOST_ASIO_NO_DEPRECATED
#endif
#ifndef STBI_ONLY_PNG
#define STBI_ONLY_PNG
#endif
#ifndef GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif

// IWYU pragma: end_exports