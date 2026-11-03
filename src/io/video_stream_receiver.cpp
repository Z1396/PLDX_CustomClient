// ============================================================================
// 文件：src/io/video_stream_receiver.cpp
// 层级：业务功能层 - 网络 IO
// 职责：实现 VideoStreamReceiver 的构造、stop() 与接收主协程 receive_loop()。
//       receive_loop() 在共用的 Boost.Asio io_context 上以 detached 协程持续接收图传 UDP 包，
//       按 network_capture 格式记录原始负载后回调 on_receive_packet（交给 FFmpeg 解码 Worker）。
// 异步模型：与 MQTT 共用同一 IO 线程；收包为 async_receive_from awaitable，停止时关闭 socket
//       使挂起的异步等待立即出错返回，循环据此退出。
// 容错策略（2027 赛场要求"图传断了 UI 仍可用"）：
//   1) 打开/绑定全程用 error_code，端口被占用也不抛异常、不影响客户端启动；
//   2) 收包出错不退出协程，而是关闭 socket、退避 200ms 后重绑，网线抖动/图传发送端重启都能自愈；
//   3) 从未绑定成功时每 500ms 重试一次，等占用端口的进程退出后自动接上。
// 依赖：Boost.Asio（UDP + 协程）、spdlog、RoboGo::logging（网络抓包）。
// ============================================================================
#include "video_stream_receiver.h"  // 本编译单元的主头文件(声明 VideoStreamReceiver)
#include "logging/logging.h"  // 网络报文抓包接口(log_network_payload)

namespace RoboGo::io {  // 进入网络 IO 命名空间

    namespace {  // 匿名命名空间：本文件内部的常量
        // 收包出错后重绑前的退避时间：足够让瞬时抖动过去，又不会让操作手等太久
        constexpr auto kRebindDelay = std::chrono::milliseconds(200);
        // socket 尚未绑定成功时的重试间隔（端口被别的进程占用时走这条路径）
        constexpr auto kBindRetryDelay = std::chrono::milliseconds(500);
        // 请求的内核接收缓冲区大小：高码率图传下减少丢包
        constexpr std::size_t kReceiveBufferBytes = 20 * 1024 * 1024;
    }  // 匿名命名空间结束

    // 功能：构造视频流接收器并启动后台接收协程。
    // 思路：先用 error_code 版本打开并绑定 UDP socket（失败只记日志，绝不抛异常），
    //       再启动 detached 的 receive_loop 持续收包；绑定失败时由循环内部周期重试。
    VideoStreamReceiver::VideoStreamReceiver(boost::asio::io_context &io_context,  // 共用的 Asio 执行上下文，接收协程也跑在这里
                                             const boost::asio::ip::udp::endpoint &endpoint,  // 要绑定的本地地址(IP:端口)
                                             const std::function<void(const uint8_t *data, size_t size)> &on_receive_packet)  // 每个数据报的消费回调
        : socket(io_context),  // 只绑定执行器：真正的 open/bind 放到 OpenAndBind 里做，便于失败重试
          listen_endpoint(endpoint),  // 记住目标端点，重绑时复用
          on_receive_packet(on_receive_packet) {  // 拷贝保存回调(shared 语义由 std::function 持有，生命周期随本对象)
        if (!OpenAndBind()) {  // 首次绑定失败（最常见原因是端口被上一个没退干净的实例占用）
            SPDLOG_ERROR("视频流首次绑定失败，将在后台每 {}ms 重试一次 endpoint={}:{}",
                         kBindRetryDelay.count(),
                         listen_endpoint.address().to_string(),
                         listen_endpoint.port());  // 明确告诉现场：客户端仍会启动，只是暂时没有画面
        }  // 结束首次绑定分支
        // 在 io_context 上分离式启动接收循环；即使当前未绑定成功也要启动，循环内部会重试
        boost::asio::co_spawn(io_context, receive_loop(), boost::asio::detached);  // detached：协程自管理生命周期，不阻塞构造函数
    }  // 构造函数结束

    // 功能：以 error_code 方式打开并绑定 UDP socket（可重复调用）。
    // 返回：true 表示已绑定成功，可以开始收包。
    bool VideoStreamReceiver::OpenAndBind() {
        boost::system::error_code error_code;  // 复用错误码，避免异常
        if (socket.is_open()) {  // 复用前先关掉旧句柄（重绑路径）
            socket.close(error_code);  // 关闭失败也无所谓，下面会重新 open
        }  // 结束旧句柄关闭
        socket.open(listen_endpoint.protocol(), error_code);  // 打开 IPv4 UDP socket
        if (error_code) {  // 打开失败（描述符耗尽等）
            SPDLOG_ERROR("打开视频流socket失败 message={}", error_code.message());  // 记录原因
            return false;  // 交给上层重试
        }  // 结束打开失败分支
        // 调大接收缓冲区；失败不致命（系统上限可能更低），仅提示实际请求值
        socket.set_option(boost::asio::socket_base::receive_buffer_size(kReceiveBufferBytes), error_code);
        if (error_code) {  // 缓冲区设置失败不阻断收包
            SPDLOG_WARN("设置视频流接收缓冲区失败 message={} 请求值={}", error_code.message(), kReceiveBufferBytes);
            error_code.clear();  // 清掉错误码，继续 bind
        }  // 结束缓冲区设置分支
        socket.bind(listen_endpoint, error_code);  // 绑定监听端口
        if (error_code) {  // 端口被占用/权限不足
            SPDLOG_ERROR("绑定视频流端口失败 endpoint={}:{} message={}（端口可能被其它程序占用）",
                         listen_endpoint.address().to_string(),
                         listen_endpoint.port(),
                         error_code.message());  // 现场最常见的原因：上一个 RoboGo 实例还没退干净
            socket.close(error_code);  // 关掉半开的 socket，避免句柄泄漏
            return false;  // 交给上层重试
        }  // 结束绑定失败分支
        SPDLOG_INFO("视频流接收器已就绪 local_endpoint={} receive_buffer_bytes={}",  // 打印实际绑定成功的地址，便于确认端口是否被占用/重绑
                    socket.local_endpoint().address().to_string() + ":" + std::to_string(socket.local_endpoint().port()),  // 拼成 "ip:port" 便于阅读
                    kReceiveBufferBytes);  // 日志里同时记录请求的缓冲区大小
        return true;  // 绑定成功
    }  // OpenAndBind 结束

    // 功能：停止接收器。
    // 思路：置 is_running=false 并通过关闭 socket 使正在进行的 async_receive_from 立即返回错误，从而退出循环。
    void VideoStreamReceiver::stop() {  // 可从主线程调用：只做原子写 + 关闭文件描述符，不阻塞等待协程
        SPDLOG_INFO("收到视频流接收器停止请求");  // 记录停止动作，便于排查"画面突然静止"的原因
        is_running.store(false, std::memory_order_relaxed);  // relaxed 足够：此标志仅用于本协程的循环条件，不承担跨变量同步职责
        boost::system::error_code error_code;  // 使用 error_code 版本，避免析构/停止路径抛异常
        socket.close(error_code);  // 关闭 socket 会让挂起中的 async_receive_from 以 operation_aborted 立刻返回
        if (error_code) {  // 关闭失败一般说明 socket 早已关闭或描述符失效
            SPDLOG_ERROR("关闭视频流socket失败 message={}", error_code.message());  // 记录失败原因但不向上抛，保证析构安全
        }  // if 结束
    }  // stop 结束

    // 功能：UDP 接收主协程：循环接收图传数据包并投递给解码管线；出错时自愈重绑。
    // 思路：用固定 64KB 缓冲接收每个 UDP 包，记录原始负载（rx）后回调 on_receive_packet；
    //       收包异常时区分"主动停止"与"运行异常"，后者关闭 socket、退避后重绑并继续，不退出协程。
    boost::asio::awaitable<void> VideoStreamReceiver::receive_loop() {  // co_spawn 时进入，直到 stop() 才返回
        // UDP 单包最大 64KB（以太网 MTU 常见上限的保守值）
        // 注意：该缓冲在循环内复用，每次迭代覆盖写入；on_receive_packet 必须在本次回调内完成拷贝/解析，
        //       不可跨迭代持有 data 指针，否则下一包到达后内容即被覆盖。
        std::vector<uint8_t> buffer(65535);  // 65535 字节 = UDP 载荷理论最大值，一次最多收满一整个数据报
        boost::asio::ip::udp::endpoint sender_endpoint;  // 记录每个 UDP 数据报的发送方地址（IP:端口），用于日志与抓包溯源
        const auto executor = co_await boost::asio::this_coro::executor;  // 取当前执行器，用于退避定时器

        while (is_running.load(std::memory_order_relaxed)) {  // 每轮先检查停止标志，避免 stop 后再发起新的收包
            if (!socket.is_open()) {  // 尚未绑定成功（首次失败，或刚被上一个错误路径关掉）
                if (!OpenAndBind()) {  // 重试绑定
                    boost::asio::steady_timer retry_timer(executor, kBindRetryDelay);  // 等一会儿再试，避免打满 CPU
                    co_await retry_timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常
                    continue;  // 回到循环顶部再试
                }  // 结束重试失败分支
            }  // 结束未绑定判断

            // 关键：这里用 as_tuple 而不是 use_awaitable。
            // 图传发送端重启、网线抖动、ICMP 端口不可达都可能让收包返回错误；
            // use_awaitable 会抛 system_error，而本项目已确认"异常 + 协程栈展开"在 Windows 上会触发访问冲突
            //（MQTT 层踩过同样的坑，见 mqtt_client.cpp 的说明）。改成错误码返回后，断流可以平滑走到重绑。
            auto [recv_ec, length] = co_await socket.async_receive_from(  // co_await 挂起当前协程，数据到达后由 IO 线程恢复
                    boost::asio::buffer(buffer),  // 接收缓冲区视图：把 vector 首地址+容量交给 Asio
                    sender_endpoint,  // 出参：Asio 回填发送方地址
                    boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常，错误码作为返回值

            if (recv_ec) {  // 收包失败：对端关闭 / 端口不可达 / socket 被 stop() 关闭
                if (!is_running.load(std::memory_order_relaxed)) {  // stop() 触发的关闭属预期
                    break;  // 退出循环
                }  // 结束主动停止判断
                SPDLOG_WARN("视频流接收中断 code={} message={}，{}ms 后重绑",
                            static_cast<long long>(recv_ec.value()),
                            recv_ec.message(),
                            kRebindDelay.count());  // 记录原因，现场能凭这条日志区分"网线"与"图传端"
                boost::system::error_code close_ec;  // 关闭时的错误码（忽略即可）
                socket.close(close_ec);  // 关掉当前 socket，下一轮 OpenAndBind 会重新建一个
                boost::asio::steady_timer rebind_timer(executor, kRebindDelay);  // 退避定时器
                co_await rebind_timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常
                continue;  // 回到顶部重绑并继续收包
            }  // 结束收包失败分支

            // 记录发送方地址，用于日志与抓包
            const std::string sender = sender_endpoint.address().to_string() + ":" + std::to_string(sender_endpoint.port());  // 拼成 "ip:port" 字符串
            // 按 network_capture 格式记录原始视频报文（rx），供复盘
            // 注意：是否真正落盘由 logging 的抓包档位决定（默认只抓 MQTT），这里不做判断。
            RoboGo::logging::log_network_payload("udp-video",  // 通道名：标记这是 UDP 图传流量
                                                 "rx",  // 方向：rx 表示接收到的数据
                                                 "video-stream",  // 伪话题名：UDP 无 topic，用固定串占位
                                                 std::span<const uint8_t>(buffer.data(), length),  // 只暴露本次实际收到的 length 字节，而非整块 64KB
                                                 sender);  // 发送方地址，便于多路流溯源
            // 把数据指针与长度交给解码管线
            on_receive_packet(buffer.data(), length);  // 回调内必须完成拷贝/解析，返回后 buffer 可能被下一包覆盖
        }  // while 结束

        SPDLOG_INFO("视频流接收循环已退出");  // 协程正常返回前的最后一条日志
    }  // receive_loop 结束

}// namespace RoboGo::io  // 网络 IO 命名空间结束
