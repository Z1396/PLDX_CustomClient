// video_stream_receiver.h：图传 UDP 接收器接口——基于 Boost.Asio 监听指定端口，把每个收到的 UDP 包回调给解码管线。
//
// 声明 VideoStreamReceiver 类，基于 Boost.Asio 的 UDP socket 监听图传流端口，
// 把每一个到达的 UDP 数据报（图传帧片段）通过回调 on_receive_packet 交给下游解码管线（FFmpeg 解码 Worker）。
// 异步模型：构造时在 io_context 上 detached 启动 receive_loop() 协程，阻塞等待 async_receive_from；
// 接收循环与 MQTT 共用同一 IO 线程；停止时通过关闭 socket 使挂起的异步等待立即出错返回，从而干净退出。
// 依赖：Boost.Asio（UDP + 协程）、spdlog、RoboGo::logging（网络抓包）。
// 注意：本文件只声明接口；接收循环实现见 video_stream_receiver.cpp。
#pragma once  // 头文件包含守卫：防止重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含常用标准库与第三方库(async_mqtt/Boost.Asio/spdlog 等)
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <boost/asio.hpp>
#include <readerwriterqueue.h>
// [/pch-split]

namespace RoboGo::io {  // 进入网络 IO 命名空间

    /**
     * @brief RoboGo 的图传 UDP 接收器封装。
     * @note 思路：构造时打开并绑定 UDP socket，detached 启动 receive_loop() 协程持续收包；每个包原样回调给解码管线，由下游负责组帧/解码。停止时关闭 socket
     *        使挂起的异步接收立即返回错误，循环据此退出。
     * @note 参数：构造时传入 io_context（共用 IO 线程）、endpoint（监听地址:端口，默认 0.0.0.0:3334）、on_receive_packet（每收到一个
     *        UDP 包的回调）。
     * @note 返回值：无（类类型）。
     */
    class VideoStreamReceiver {  // 无虚函数、不可拷贝（持有 socket 与协程），仅作为后台服务成员长期存活
    private:  // 内部状态：socket 句柄、包回调与运行标志，外部无需感知
        boost::asio::ip::udp::socket socket;  // 用于接收图传 UDP 流的 socket（构造时只绑定执行器，端口在 OpenAndBind 里绑）
        boost::asio::ip::udp::endpoint listen_endpoint;  // 期望监听的本地地址与端口，断线重绑时复用
        std::function<void(const uint8_t *data, size_t size)> on_receive_packet;  // 收到数据包后的回调（交给解码器）
        std::atomic<bool> is_running = true;  // 运行标志，stop() 置 false 退出接收循环

    public:  // 对外只暴露"构造即开始收包"和"stop 停止"两个动作
        /**
         * @brief 构造接收器——尝试绑定 UDP socket，随后启动接收协程循环。
         * @note 思路：用 error_code 版本打开/绑定（端口被占用等失败只记日志、不抛异常，进程照常启动），再 co_spawn 启动 detached 的 receive_loop()
         *        ；循环内发现 socket 不可用会周期性重试绑定，因此"先开客户端后插网线/后关占端口的进程"都能自动恢复。参数 io_context：外部提供的 Asio 执行上下文（与
         *        MQTT 等共用同一 IO 线程）；endpoint：要监听的本地地址与端口（图传默认 0.0.0.0:3334）；on_receive_packet：每收到一个 UDP 包的回调（数据指针
         *        + 长度），通常接解码器。
         * @note 返回值：无。
         */
        explicit VideoStreamReceiver(boost::asio::io_context &io_context,  // explicit：禁止隐式转换构造，避免误传端点
                                     const boost::asio::ip::udp::endpoint &endpoint,  // 监听的本地地址与端口
                                     const std::function<void(const uint8_t *data, size_t size)> &on_receive_packet);  // 每个 UDP 数据报的消费回调

        /**
         * @brief 停止接收器。
         * @note 思路：置 is_running=false，并关闭 socket——正在进行的 async_receive_from 会立即以错误返回，使 receive_loop 干净退出，
         *        无需额外通知机制。
         * @note 返回值：无。
         */
        void stop();  // 幂等停止：重复调用仅关闭已关闭的 socket，不会抛异常(使用 error_code 版本)

    private:  // 接收循环为内部实现细节，外部只能通过构造/stop 间接驱动
        /**
         * @brief 打开并绑定 UDP socket（全程 error_code，不抛异常）。
         * @return true 表示 socket 已就绪可收包；false 表示绑定失败（端口被占用/权限不足等）。
         */
        bool OpenAndBind();

        /**
         * @brief UDP 接收主协程——循环接收图传数据报并投递给解码管线。
         * @note 思路：在 is_running 下反复 async_receive_from 阻塞等待下一个 UDP 包（as_tuple，错误码返回不抛异常），记录原始负载（rx）后回调 on_receive_packet；
         *        出错时关闭 socket、退避后重绑，而不是退出协程。复用同一接收缓冲，故回调必须在本轮内完成拷贝/解析，不可跨迭代持有 data 指针。
         * @note 参数：无（使用类成员）。
         * @return awaitable<void>，由 co_spawn(detached) 驱动，仅在 stop() 时返回。
         */
        boost::asio::awaitable<void> receive_loop();  // 持续接收 UDP 包的主协程
    };

}  // namespace RoboGo::io
