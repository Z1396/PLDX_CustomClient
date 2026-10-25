// network_capture_format.h：网络抓包文件的二进制格式定义——抓包标志位、摘要头、记录头与 CRC 校验的字节布局常量。
#pragma once

#include <pch.h>

namespace RoboGo::logging {

// 网络抓包标志位：bit0 表示是否翻转 UDP 视频（与解码/显示朝向有关），其余位预留。
constexpr std::uint32_t kNetworkCaptureFlagInvertUdpVideo = 1U << 0U;

// 功能：根据 invert_udp_video 开关构造 32 位抓包标志字。
// 思路：开启则置 bit0，否则为 0；供抓包文件头写入，复盘时据此还原处理选项。
constexpr std::uint32_t BuildNetworkCaptureFlags(const bool invert_udp_video) {
    return invert_udp_video ? kNetworkCaptureFlagInvertUdpVideo : 0U;
}

}// namespace RoboGo::logging
