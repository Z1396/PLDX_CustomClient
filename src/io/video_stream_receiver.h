// ============================================================================
// 文件：src/io/video_stream_receiver.h
// 层级：业务功能层 - 网络 IO（RoboGo 后台服务 BackgroundServices 组件之一）
// 职责：声明 VideoStreamReceiver 类，基于 Boost.Asio 的 UDP socket 监听图传流端口，
//       把每一个到达的 UDP 数据报（图传帧片段）通过回调 on_receive_packet 交给下游解码管线（FFmpeg 解码 Worker）。
// 异步模型：构造时在 io_context 上 detached 启动 receive_loop() 协程，阻塞等待 async_receive_from；
//       接收循环与 MQTT 共用同一 IO 线程；停止时通过关闭 socket 使挂起的异步等待立即出错返回，从而干净退出。
// 依赖：Boost.Asio（UDP + 协程）、spdlog、RoboGo::logging（网络抓包）。
// 注意：本文件只声明接口；接收循环实现见 video_stream_receiver.cpp。
// ============================================================================
// video_stream_receiver.h：图传 UDP 接收器接口——基于 Boost.Asio 监听指定端口，把每个收到的 UDP 包回调给解码管线。
#pragma once

#include <pch.h>

namespace RoboGo::io {

    // 功能：RoboGo 的图传 UDP 接收器封装。
    // 思路：构造时打开并绑定 UDP socket，detached 启动 receive_loop() 协程持续收包；每个包原样回调给解码管线，
    //       由下游负责组帧/解码。停止时关闭 socket 使挂起的异步接收立即返回错误，循环据此退出。
    // 参数：构造时传入 io_context（共用 IO 线程）、endpoint（监听地址:端口，默认 0.0.0.0:3334）、
    //      on_receive_packet（每收到一个 UDP 包的回调）。
    // 返回值：无（类类型）。
    class VideoStreamReceiver {
    private:
        boost::asio::ip::udp::socket socket;  // 用于接收图传 UDP 流的 socket
        std::function<void(const uint8_t *data, size_t size)> on_receive_packet;  // 收到数据包后的回调（交给解码器）
        std::atomic<bool> is_running = true;  // 运行标志，stop() 置 false 退出接收循环

    public:
        // 功能：构造接收器——绑定并打开 UDP socket，随后启动接收协程循环。
        // 思路：以 endpoint 在 io_context 上打开并绑定 UDP socket，调大接收缓冲区以承受高码率图传，
        //       再 co_spawn 启动 detached 的 receive_loop() 协程开始收包。
        // 参数 io_context：外部提供的 Asio 执行上下文（与 MQTT 等共用同一 IO 线程）；
        //      endpoint：要监听的本地地址与端口（图传默认 0.0.0.0:3334）；
        //      on_receive_packet：每收到一个 UDP 包的回调（数据指针 + 长度），通常接解码器。
        // 返回值：无。
        explicit VideoStreamReceiver(boost::asio::io_context &io_context,
                                     const boost::asio::ip::udp::endpoint &endpoint,
                                     const std::function<void(const uint8_t *data, size_t size)> &on_receive_packet);

        // 功能：停止接收器。
        // 思路：置 is_running=false，并关闭 socket——正在进行的 async_receive_from 会立即以错误返回，
        //       使 receive_loop 从异常分支干净退出，无需额外通知机制。
        // 参数：无。
        // 返回值：无。
        void stop();

    private:
        // 功能：UDP 接收主协程——循环接收图传数据报并投递给解码管线。
        // 思路：在 is_running 下反复 async_receive_from 阻塞等待下一个 UDP 包，记录原始负载（rx）后回调 on_receive_packet；
        //       收包异常时区分「主动停止（socket 被关闭）」与「运行异常」，前者属预期退出。复用同一接收缓冲，
        //       故回调必须在本轮内完成拷贝/解析，不可跨迭代持有 data 指针。
        // 参数：无（使用类成员）。
        // 返回值：awaitable<void>，由 co_spawn(detached) 驱动，随 stop() 退出。
        boost::asio::awaitable<void> receive_loop();  // 持续接收 UDP 包的主协程
    };

}// namespace RoboGo::io
