#pragma once

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
// asio：Boost.Asio 异步网络/IO 框架，用于 MQTT 通信与协程式异步逻辑。
// asio
#define BOOST_ASIO_NO_DEPRECATED
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
// protobuf：Google 协议缓冲区，用于机器人通信消息的序列化/反序列化。
// protobuf
#include <google/protobuf/message.h>
#include <messages.pb.h>
// async_mqtt：异步 MQTT 客户端库，用于与机器人下位机/裁判系统通信。
// async_mqtt
#include <async_mqtt/all.hpp>
// ffmpeg：音视频编解码与图像处理库，用于图传视频流的解码与缩放。
// ffmpeg
extern "C" {   // 以 C 语言链接方式包含 FFmpeg 的 C 接口头文件，避免 C++ 名称修饰导致链接错误
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
// readerwriterqueue：单生产者单消费者(SPSC)无锁队列，用于图传解码线程与渲染线程间安全传递帧数据。
// readerwriterqueue
#include <readerwriterqueue.h>
// stb：轻量单头文件工具库；此处仅启用 PNG 解码，使用 stb_image 加载图像。
// stb
#define STBI_ONLY_PNG   // 仅编译 PNG 解码器，剔除其他格式以减小体积
#include <stb_image.h>
#ifdef _WIN32
// windows：Windows 平台 API 与外壳相关接口。
// windows
#include <windows.h>
#include <shellapi.h>
// dx12：DirectX 12 渲染后端相关头文件(Windows 下使用 D3D12 渲染 ImGui)。
// dx12
#include <d3d12.h>
#include <dxgidebug.h>
#include <dxgi1_5.h>
#else
// glfw：非 Windows 平台使用 GLFW 窗口管理，并预包含 Vulkan 头。
// glfw
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#endif
// imgui：Dear ImGui 即时模式 GUI 库及其平台/渲染后端封装，外加内部接口与 std::string 扩展。
// imgui
#include "imgui.h"
#ifdef _WIN32
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#else
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#endif
#include "imgui_internal.h"   // ImGui 内部接口(部分内部工具/数据结构)
#include "imgui_stdlib.h"     // ImGui 与 std::string 的桥接(InputText 等支持 std::string)

// IWYU pragma: end_exports
