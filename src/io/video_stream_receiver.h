#pragma once

#include <pch.h>

namespace RoboGo::io {

    // 视频流接收器：基于 Boost.Asio 的 UDP 接收，监听指定 endpoint，把每个 UDP 数据包（图传帧片段）回调给解码管线。
    class VideoStreamReceiver {
    private:
        boost::asio::ip::udp::socket socket;  // 用于接收图传 UDP 流的 socket
        std::function<void(const uint8_t *data, size_t size)> on_receive_packet;  // 收到数据包后的回调（交给解码器）
        std::atomic<bool> is_running = true;  // 运行标志，stop() 置 false 退出接收循环

    public:
        explicit VideoStreamReceiver(boost::asio::io_context &io_context,
                                     const boost::asio::ip::udp::endpoint &endpoint,
                                     const std::function<void(const uint8_t *data, size_t size)> &on_receive_packet);

        // 停止接收：置标志并关闭 socket（使异步等待立即出错返回以退出循环）
        void stop();

    private:
        boost::asio::awaitable<void> receive_loop();  // 持续接收 UDP 包的主协程
    };

}// namespace RoboGo::io
