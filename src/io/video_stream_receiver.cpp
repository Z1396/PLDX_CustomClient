// ============================================================================
// 文件：src/io/video_stream_receiver.cpp
// 层级：业务功能层 - 网络 IO
// 职责：实现 VideoStreamReceiver 的构造、stop() 与接收主协程 receive_loop()。
//       receive_loop() 在共用的 Boost.Asio io_context 上以 detached 协程持续接收图传 UDP 包，
//       按 network_capture 格式记录原始负载后回调 on_receive_packet（交给 FFmpeg 解码 Worker）。
// 异步模型：与 MQTT 共用同一 IO 线程；收包为 async_receive_from awaitable，停止时关闭 socket
//       使挂起的异步等待立即出错返回，循环据此退出。
// 依赖：Boost.Asio（UDP + 协程）、spdlog、RoboGo::logging（网络抓包）。
// ============================================================================
#include "video_stream_receiver.h"  // 本编译单元的主头文件(声明 VideoStreamReceiver)
#include "logging/logging.h"  // 网络报文抓包接口(log_network_payload)

namespace RoboGo::io {  // 进入网络 IO 命名空间

    // 功能：构造视频流接收器并启动后台接收协程。
    // 思路：以给定 endpoint 打开并绑定 UDP socket，调大接收缓冲区（20MB）以承受高码率图传；
    //       启动 detached 的 receive_loop 持续收包，每个包回调 on_receive_packet。
    // 参数 io_context：外部 Asio 执行上下文（与 MQTT 共用 IO 线程）；endpoint：监听的本地地址:端口；
    //      on_receive_packet：每收到一个 UDP 包的回调（数据指针 + 长度）。
    // 返回值：无。
    VideoStreamReceiver::VideoStreamReceiver(boost::asio::io_context &io_context,  // 共用的 Asio 执行上下文，接收协程也跑在这里
                                             const boost::asio::ip::udp::endpoint &endpoint,  // 要绑定的本地地址(IP:端口)
                                             const std::function<void(const uint8_t *data, size_t size)> &on_receive_packet)  // 每个数据报的消费回调
        : socket(io_context, endpoint),  // 构造 socket 时即完成 open + bind：以后收包只等数据到达，无额外建连开销
          on_receive_packet(on_receive_packet) {  // 拷贝保存回调(shared 语义由 std::function 持有，生命周期随本对象)
        // 调大 socket 接收缓冲区，减少高带宽图传下的丢包
        socket.set_option(boost::asio::socket_base::receive_buffer_size(20 * 1024 * 1024));  // 请求 20MB 内核接收缓冲(实际值受系统上限约束)
        SPDLOG_INFO("创建视频流接收器 local_endpoint={} receive_buffer_bytes={}",  // 打印实际绑定成功的地址，便于确认端口是否被占用/重绑
                    socket.local_endpoint().address().to_string() + ":" + std::to_string(socket.local_endpoint().port()),  // 拼成 "ip:port" 便于阅读
                    20 * 1024 * 1024);  // 日志里同时记录请求的缓冲区大小
        // 在 io_context 上分离式启动接收循环
        boost::asio::co_spawn(io_context, receive_loop(), boost::asio::detached);  // detached：协程自管理生命周期，异常不外抛，不阻塞构造函数
    }  // 构造函数结束

    // 功能：停止接收器。
    // 思路：置 is_running=false 并通过关闭 socket 使正在进行的 async_receive_from 立即返回错误，从而退出循环。
    // 参数：无。
    // 返回值：无。
    void VideoStreamReceiver::stop() {  // 可从主线程调用：只做原子写 + 关闭文件描述符，不阻塞等待协程
        SPDLOG_INFO("收到视频流接收器停止请求");  // 记录停止动作，便于排查"画面突然静止"的原因
        is_running.store(false, std::memory_order_relaxed);  // relaxed 足够：此标志仅用于本协程的循环条件，不承担跨变量同步职责
        boost::system::error_code error_code;  // 使用 error_code 版本，避免析构/停止路径抛异常
        socket.close(error_code);  // 关闭 socket 会让挂起中的 async_receive_from 以 operation_aborted 立刻返回
        if (error_code) {  // 关闭失败一般说明 socket 早已关闭或描述符失效
            SPDLOG_ERROR("关闭视频流socket失败 message={}", error_code.message());  // 记录失败原因但不向上抛，保证析构安全
        }  // if 结束
    }  // stop 结束

    // 功能：UDP 接收主协程：循环接收图传数据包并投递给解码管线。
    // 思路：用固定 64KB 缓冲接收每个 UDP 包，记录原始负载（rx）后回调 on_receive_packet；
    //       收包异常时区分“主动停止”与“运行异常”，停止则正常退出。
    // 参数：无（使用类成员 socket / on_receive_packet / is_running）。
    // 返回值：awaitable<void>，随 stop() 关闭 socket 后从异常分支退出。
    boost::asio::awaitable<void> VideoStreamReceiver::receive_loop() {  // co_spawn 时进入，直到 stop() 或异常才返回
        // UDP 单包最大 64KB（以太网 MTU 常见上限的保守值）
        // 注意：该缓冲在循环内复用，每次迭代覆盖写入；on_receive_packet 必须在本次回调内完成拷贝/解析，
        //       不可跨迭代持有 data 指针，否则下一包到达后内容即被覆盖。
        std::vector<uint8_t> buffer(65535);  // 65535 字节 = UDP 载荷理论最大值，一次最多收满一整个数据报
        boost::asio::ip::udp::endpoint sender_endpoint;  // 记录每个 UDP 数据报的发送方地址（IP:端口），用于日志与抓包溯源
        try {  // 包住整个循环：socket 被关闭时 async_receive_from 会抛 system_error，需要在协程内捕获
            while (is_running.load(std::memory_order_relaxed)) {  // 每轮先检查停止标志，避免 stop 后再发起新的收包
                // 阻塞等待下一个 UDP 数据报，返回接收字节数
                size_t length = co_await socket.async_receive_from(  // co_await 挂起当前协程，数据到达后由 IO 线程恢复
                        boost::asio::buffer(buffer),  // 接收缓冲区视图：把 vector 首地址+容量交给 Asio
                        sender_endpoint,  // 出参：Asio 回填发送方地址
                        boost::asio::use_awaitable);  // 让异步操作返回 awaitable，从而可直接 co_await

                // 记录发送方地址，用于日志与抓包
                const std::string sender = sender_endpoint.address().to_string() + ":" + std::to_string(sender_endpoint.port());  // 拼成 "ip:port" 字符串
                // 按 network_capture 格式记录原始视频报文（rx），供复盘
                RoboGo::logging::log_network_payload("udp-video",  // 通道名：标记这是 UDP 图传流量
                                                     "rx",  // 方向：rx 表示接收到的数据
                                                     "video-stream",  // 伪话题名：UDP 无 topic，用固定串占位
                                                     std::span<const uint8_t>(buffer.data(), length),  // 只暴露本次实际收到的 length 字节，而非整块 64KB
                                                     sender);  // 发送方地址，便于多路流溯源
                // 把数据指针与长度交给解码管线
                on_receive_packet(buffer.data(), length);  // 回调内必须完成拷贝/解析，返回后 buffer 可能被下一包覆盖
            }  // while 结束
        } catch (const boost::system::system_error &error) {  // 捕获 Asio 层错误(如 operation_aborted / bad_descriptor)
            if (is_running.load(std::memory_order_relaxed)) {  // 仍在运行却出错：说明是真实网络异常，而非主动停止
                // 仍在运行却出错：记录错误
                SPDLOG_ERROR("视频流接收异常 code={} message={}",  // 错误码数值 + 文本，便于定位
                             static_cast<long long>(error.code().value()),  // 转成有符号整型，spdlog 格式化更友好
                             error.code().message());  // 系统给出的可读错误描述
            } else {  // 否则说明 socket 是被 stop() 关掉的
                // 因停止请求（socket 被关闭）而退出：属于预期
                SPDLOG_INFO("视频流接收循环因停止请求退出 message={}", error.code().message());  // 用 INFO 级别，避免退出时误报错误
            }  // if-else 结束
        }  // try-catch 结束
        SPDLOG_INFO("视频流接收循环已退出");  // 协程正常返回前的最后一条日志
    }  // receive_loop 结束

}// namespace RoboGo::io  // 网络 IO 命名空间结束
