#pragma once

#include <pch.h>

namespace RoboGo::logging {

// 功能：初始化日志系统（spdlog 异步 + 控制台/文件双 sink）并启用原始网络抓包。
// 参数：application_name 应用名（写入日志）；invert_udp_video 是否在抓包标志中记录“翻转 UDP 视频”选项。
void initialize(std::string_view application_name, bool invert_udp_video);

// 功能：关闭日志系统：停止抓包线程、等待其退出并关闭 spdlog。
void shutdown();

// 功能：查询日志系统是否已初始化。
bool is_initialized();

// 以下为各全局路径/标识的访问器，返回内部存储的引用（只读使用，勿修改）。
const std::filesystem::path &log_root_directory();  // 日志根目录
const std::filesystem::path &log_directory();        // 本次会话日志目录（根目录/会话ID）
const std::filesystem::path &network_capture_path(); // 网络抓包二进制文件路径
const std::string &session_id();                     // 本次会话 ID（时间戳生成）

// 功能：把文本截断到 max_length，超出则追加 "...(truncated)" 标记。
std::string truncate_text(std::string_view text, std::size_t max_length = 512);

// 功能：记录一笔网络报文到抓包队列（二进制格式，供复盘）。
// 参数：transport 传输类型("mqtt"/"udp-video")；direction 方向("rx"/"tx")；channel 通道(如 topic)；
//       payload 原始载荷；peer 对端地址。本重载接受字节序列。
void log_network_payload(std::string_view transport,
                         std::string_view direction,
                         std::string_view channel,
                         std::span<const uint8_t> payload,
                         std::string_view peer = {});

// 便捷重载：直接接收字符串载荷（如 MQTT 文本负载），内部转为字节序列后调用上述版本。
inline void log_network_payload(std::string_view transport,
                                std::string_view direction,
                                std::string_view channel,
                                std::string_view payload,
                                std::string_view peer = {}) {
    const auto *begin = reinterpret_cast<const uint8_t *>(payload.data());
    log_network_payload(transport,
                        direction,
                        channel,
                        std::span<const uint8_t>(begin, payload.size()),
                        peer);
}

}// namespace RoboGo::logging
