#include "video_stream_receiver.h"
#include "logging/logging.h"

namespace RoboGo::io {

    // 功能：构造视频流接收器并启动后台接收协程。
    // 思路：以给定 endpoint 打开并绑定 UDP socket，调大接收缓冲区（20MB）以承受高码率图传；
    //       启动 detached 的 receive_loop 持续收包，每个包回调 on_receive_packet。
    VideoStreamReceiver::VideoStreamReceiver(boost::asio::io_context &io_context,
                                             const boost::asio::ip::udp::endpoint &endpoint,
                                             const std::function<void(const uint8_t *data, size_t size)> &on_receive_packet)
        : socket(io_context, endpoint),
          on_receive_packet(on_receive_packet) {
        // 调大 socket 接收缓冲区，减少高带宽图传下的丢包
        socket.set_option(boost::asio::socket_base::receive_buffer_size(20 * 1024 * 1024));
        SPDLOG_INFO("创建视频流接收器 local_endpoint={} receive_buffer_bytes={}",
                    socket.local_endpoint().address().to_string() + ":" + std::to_string(socket.local_endpoint().port()),
                    20 * 1024 * 1024);
        // 在 io_context 上分离式启动接收循环
        boost::asio::co_spawn(io_context, receive_loop(), boost::asio::detached);
    }

    // 功能：停止接收器。
    // 思路：置 is_running=false 并通过关闭 socket 使正在进行的 async_receive_from 立即返回错误，从而退出循环。
    void VideoStreamReceiver::stop() {
        SPDLOG_INFO("收到视频流接收器停止请求");
        is_running.store(false, std::memory_order_relaxed);
        boost::system::error_code error_code;
        socket.close(error_code);
        if (error_code) {
            SPDLOG_ERROR("关闭视频流socket失败 message={}", error_code.message());
        }
    }

    // 功能：UDP 接收主协程：循环接收图传数据包并投递给解码管线。
    // 思路：用固定 64KB 缓冲接收每个 UDP 包，记录原始负载（rx）后回调 on_receive_packet；
    //       收包异常时区分“主动停止”与“运行异常”，停止则正常退出。
    boost::asio::awaitable<void> VideoStreamReceiver::receive_loop() {
        // UDP 单包最大 64KB（以太网 MTU 常见上限的保守值）
        std::vector<uint8_t> buffer(65535);
        boost::asio::ip::udp::endpoint sender_endpoint;
        try {
            while (is_running.load(std::memory_order_relaxed)) {
                // 阻塞等待下一个 UDP 数据报，返回接收字节数
                size_t length = co_await socket.async_receive_from(
                        boost::asio::buffer(buffer),
                        sender_endpoint,
                        boost::asio::use_awaitable);

                // 记录发送方地址，用于日志与抓包
                const std::string sender = sender_endpoint.address().to_string() + ":" + std::to_string(sender_endpoint.port());
                // 按 network_capture 格式记录原始视频报文（rx），供复盘
                RoboGo::logging::log_network_payload("udp-video",
                                                     "rx",
                                                     "video-stream",
                                                     std::span<const uint8_t>(buffer.data(), length),
                                                     sender);
                // 把数据指针与长度交给解码管线
                on_receive_packet(buffer.data(), length);
            }
        } catch (const boost::system::system_error &error) {
            if (is_running.load(std::memory_order_relaxed)) {
                // 仍在运行却出错：记录错误
                SPDLOG_ERROR("视频流接收异常 code={} message={}",
                             static_cast<long long>(error.code().value()),
                             error.code().message());
            } else {
                // 因停止请求（socket 被关闭）而退出：属于预期
                SPDLOG_INFO("视频流接收循环因停止请求退出 message={}", error.code().message());
            }
        }
        SPDLOG_INFO("视频流接收循环已退出");
    }

}// namespace RoboGo::io
