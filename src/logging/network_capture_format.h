// network_capture_format.h：网络抓包文件的二进制格式定义——抓包标志位、摘要头、记录头与 CRC 校验的字节布局常量。
#pragma once  // 头文件包含守卫：防止重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含常用标准库与第三方库
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <readerwriterqueue.h>
// [/pch-split]

namespace RoboGo::logging {  // 进入日志命名空间

// 网络抓包标志位：bit0 表示是否翻转 UDP 视频（与解码/显示朝向有关），其余位预留。
constexpr std::uint32_t kNetworkCaptureFlagInvertUdpVideo = 1U << 0U;  // 1U 左移 0 位 = 0x00000001，恰好占用 bit0

/**
 * @brief 根据 invert_udp_video 开关构造 32 位抓包标志字。
 * @note 思路：开启则置 bit0，否则为 0；供抓包文件头写入，复盘时据此还原处理选项。
 */
constexpr std::uint32_t BuildNetworkCaptureFlags(const bool invert_udp_video) {  // constexpr 允许编译期求值，无运行时开销
    return invert_udp_video ? kNetworkCaptureFlagInvertUdpVideo : 0U;  // 三元运算：true→置 bit0，false→全 0 标志字
}

}  // namespace RoboGo::logging
