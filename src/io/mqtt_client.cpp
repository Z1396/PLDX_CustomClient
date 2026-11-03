// ============================================================================
// 文件：src/io/mqtt_client.cpp
// 层级：业务功能层 - 网络 IO
// 职责：实现 MqttClient 的构造、参数设置与协程主循环 run()，以及与 broker 交互的若干辅助协程：
//       WaitForMqttOperationTimeout（超时定时器）、CloseMqttClientNoThrow（安全关连）、
//       RunMqttOperationWithTimeout（超时包裹器）、PublishMqttMessage（上行发布）。
// 异步模型：所有网络操作均为 async_mqtt 的 awaitable，跑在共用的 Boost.Asio io_context 上；
//       run() 以 detached 协程常驻，内部「连接→订阅→recv 循环→异常重连」，并通过
//       原子修订号(endpoint_revision)与标志(is_client_id_updated)感知主线程的参数变更并重建连接。
// 依赖：async_mqtt、Boost.Asio 协程、spdlog、RoboGo::logging（网络抓包）。
// ============================================================================
#include "mqtt_client.h"
// 本编译单元的主头文件(声明 MqttClient)
#include "logging/logging.h"  // 网络报文抓包接口(log_network_payload)

// 匿名命名空间：本编译单元内部的辅助函数与类型，不对外暴露
namespace {  // 匿名命名空间：本编译单元内部辅助函数与类型，不对外暴露

    // MQTT 异步客户端类型别名：使用 MQTT 3.1.1 协议、纯 MQTT（非 WebSocket）传输
    using MqttAsyncClient = async_mqtt::client<async_mqtt::protocol_version::v3_1_1, async_mqtt::protocol::mqtt>;  // 模板参数依次为"协议版本"与"传输层协议"

    // 单次 MQTT 操作（连接/订阅/收发等）的最长等待时间
    constexpr auto mqtt_operation_timeout = std::chrono::seconds(5);  // 5 秒；既用于操作超时，也用作 PINGRESP 响应超时
    // 连接失败后重连前的等待间隔
    constexpr auto mqtt_reconnect_delay = std::chrono::milliseconds(100);  // 100ms 退避：既避免空转打满 CPU，又能快速恢复
    // MQTT 心跳（keep-alive）间隔，单位秒
    constexpr std::uint16_t mqtt_keep_alive_seconds = 5;  // 交给 broker 的心跳周期，超过 1.5 倍未通信即判定断线

    // 功能：安全关闭 MQTT 客户端连接，不产生任何异常。
    // 思路：async_close 的完成签名没有 error_code，配合 as_tuple 后关闭失败也只是静默返回。
    // 参数 client：要关闭的连接；peer/reason 仅用于 DEBUG 日志。
    boost::asio::awaitable<void> CloseMqttClientNoThrow(MqttAsyncClient &client, std::string_view peer, std::string_view reason) {
        SPDLOG_DEBUG("MQTT关闭连接 peer={} reason={}", peer, reason);  // 仅调试级别，避免刷屏
        co_await client.async_close(boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常
    }  // CloseMqttClientNoThrow 结束

    // 功能：发布一条 MQTT 消息（QoS1，至少一次）。
    // 思路：先取得唯一 packet_id（会话未就绪则跳过），按 network_capture 格式记录发送负载，再用超时包裹的 publish 发出；
    //       捕获 system_error 仅记录错误，不向上抛（发布失败不应中断主循环）。
    // 参数 client：目标连接（共享指针，跨协程保活）；topic：上行话题；message：已序列化负载；
    //      peer：日志用的“地址:端口”。
    // 返回值：awaitable<void>；成功或会话未就绪/失败都正常返回，不向外抛异常。
    boost::asio::awaitable<void> PublishMqttMessage(std::shared_ptr<MqttAsyncClient> client,  // 共享指针：保证发送期间连接不被 run() 重置
                                                    std::string topic,  // 按值捕获，detached 协程需自行持有数据
                                                    std::string message,  // 同上：负载按值持有，避免调用方提前释放
                                                    std::string peer) {  // 日志用对端地址，按值持有
        try {  // 发送链路的异常全部在此收口，不影响调用方
            // 申请一个本会话内唯一的报文 ID，用于匹配 ACK；无可用 ID 说明会话尚未就绪
            const auto packet_id = client->acquire_unique_packet_id();  // 返回 optional：会话未建立/ID 耗尽时为空
            if (!packet_id.has_value()) {  // 没有可用 ID，无法构成合法的 PUBLISH
                SPDLOG_WARN("MQTT会话未就绪，跳过发送 topic={} bytes={} peer={}",  // 提示本次发送被丢弃
                            topic,  // 目标话题
                            message.size(),  // 负载字节数
                            peer);  // 对端地址
                co_return;  // 直接结束协程，不抛异常
            }  // if 结束
            // 在发送前记录原始报文（上行 tx），供复盘/抓包
            RoboGo::logging::log_network_payload("mqtt",  // 通道名：mqtt
                                                 "tx",  // 方向：tx 表示发送出去的数据
                                                 topic,  // 话题名作为抓包元数据
                                                 message,  // 原始负载(protobuf 序列化字节)
                                                 peer);  // 对端地址
            SPDLOG_DEBUG("发送MQTT消息 topic={} bytes={} peer={}", topic, message.size(), peer);  // DEBUG 级别：高频发送时不刷屏
            // 发送：as_tuple，失败只记日志，绝不抛异常（见文件头关于"异常展开会崩"的说明）
            auto [publish_ec, publish_result] = co_await client->async_publish(  // async_publish：发 PUBLISH，QoS1 等 PUBACK
                    async_mqtt::v3_1_1::publish_packet{  // 手工构造 MQTT 3.1.1 的 PUBLISH 报文
                            *packet_id,  // 报文标识符，用于与 PUBACK 匹配
                            topic,  // 话题名
                            message,  // 负载(payload)
                            async_mqtt::qos::at_least_once},  // QoS1：至少一次
                    boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常
            if (publish_ec) {  // 发送失败（链路断了/超时/未就绪）
                SPDLOG_DEBUG("MQTT发送失败 topic={} code={} message={}",  // 高频发送用 DEBUG，避免刷屏
                             topic, static_cast<long long>(publish_ec.value()), publish_ec.message());  // 错误码与文本
            }  // if 结束
        } catch (const boost::system::system_error &error) {  // 捕获超时/网络错误
            // 发送失败仅记录，不向上抛，保证主循环稳定
            const auto error_code = static_cast<long long>(error.code().value());  // 错误码数值
            const std::string error_message = error.code().message();  // 错误文本(先落成 std::string，避免 spdlog 内部临时对象问题)
            SPDLOG_ERROR("MQTT发送失败 topic={} bytes={} peer={} code={} message={}",  // 记录失败上下文
                         topic,  // 目标话题
                         message.size(),  // 负载大小
                         peer,  // 对端地址
                         error_code,  // 错误码
                         error_message);  // 错误文本
        }  // try-catch 结束
    }  // PublishMqttMessage 结束

}// namespace  // 匿名命名空间结束

namespace RoboGo::io {  // 进入网络 IO 命名空间

    // 功能：构造 MQTT 客户端并启动后台连接协程。
    // 思路：保存 host/port 与下行回调，co_spawn 启动 run() 协程（detached 自管理生命周期）。
    MqttClient::MqttClient(boost::asio::io_context &io_context,  // 共用 IO 上下文
                           std::string host_ip,  // broker 地址
                           std::string port,  // broker 端口
                           const std::function<void(const std::string &topic, const std::string &payload)> &on_receive_message)  // 下行回调
        : host_ip(std::move(host_ip)),  // 移动构造，避免多一次字符串深拷贝
          port(std::move(port)),  // 同上
          on_receive_message(on_receive_message) {  // 拷贝 std::function(可能已捕获 this，需保证生命周期长于本对象)
        SPDLOG_INFO("创建MQTT客户端 host={} port={}", this->host_ip, this->port);  // 显式 this-> 以区分入参与同名成员
        // 在 io_context 上分离式启动连接主循环
        boost::asio::co_spawn(io_context, run(), boost::asio::detached);  // detached：协程自持，构造立即返回，不等连接结果
    }  // 构造函数结束

    // 功能：动态更新服务端地址。
    // 思路：空地址忽略；加锁比较新旧地址，仅在确实变化时记录并递增 endpoint_revision 触发 run 循环重建连接。
    void MqttClient::set_server_endpoint(std::string host_ip, std::string port) {  // 可在任意线程调用(如 UI 修改配置)
        if (host_ip.empty() || port.empty()) {  // 空值视为无效输入
            SPDLOG_WARN("忽略空的MQTT服务端地址 host={} port={}", host_ip, port);  // 打印被忽略的入参，便于排查配置错误
            return;  // 直接返回，不修改现有配置
        }  // if 结束

        bool endpoint_changed = false;  // 标记本次调用是否真的改变了地址
        {  // 缩小锁粒度：只在读写成员时持锁
            // 加锁读取并比较，避免与 run 循环并发写竞争
            std::lock_guard<std::mutex> lock(endpoint_mutex);  // RAII 自动解锁
            if (this->host_ip != host_ip || this->port != port) {  // 任一不同即视为变更
                SPDLOG_INFO("更新MQTT服务端地址 old={}:{} new={}:{}",  // 打印新旧地址，方便定位"连错服务端"
                            this->host_ip,  // 旧主机
                            this->port,  // 旧端口
                            host_ip,  // 新主机
                            port);  // 新端口
                this->host_ip = std::move(host_ip);  // 移动赋值，入参此后不再使用
                this->port = std::move(port);  // 同上
                endpoint_changed = true;  // 记录已变更
            }  // if 结束
        }  // 作用域结束，锁释放

        if (!endpoint_changed) {  // 地址没变就不需要任何后续动作
            // 地址未变则无需任何动作
            return;  // 提前返回，避免无谓的修订号自增导致 run() 重连
        }  // if 结束

        // 修订号 +1，run 循环通过对比快照检测变更并重建连接
        endpoint_revision.fetch_add(1, std::memory_order_acq_rel);  // acq_rel：保证地址写入先于修订号可见
        bool has_client_id = false;  // 是否已配置 client_id
        {  // 单独作用域，只保护 client_id 读取
            std::lock_guard<std::mutex> lock(client_id_mutex);  // 加锁拷贝，避免读到半更新状态
            has_client_id = !client_id.empty();  // 非空即视为已配置
        }  // 作用域结束
        // 有 client_id 才进入 Connecting 触发连接；否则停留在 Idle 等待 client_id
        connection_state.store(has_client_id ? ConnectionState::Connecting : ConnectionState::Idle, std::memory_order_release);  // release：使上面的地址修改对读端可见
    }  // set_server_endpoint 结束

    // 功能：获取当前服务端地址（线程安全副本）。
    // 思路：加锁拷贝，返回 pair<host, port>。
    std::pair<std::string, std::string> MqttClient::get_server_endpoint() const {  // const：logical const，仅读成员
        std::lock_guard<std::mutex> lock(endpoint_mutex);  // endpoint_mutex 为 mutable，const 函数中也可加锁
        return {host_ip, port};  // 返回值拷贝，调用方拿到后不受后台修改影响
    }  // get_server_endpoint 结束

    // 功能：设置客户端 ID。
    // 思路：保存 ID 后，有 ID 则置 Connecting 触发连接；同时置 is_client_id_updated=true 供 run 循环在连接前/中检测并重建。
    void MqttClient::set_client_id(std::string client_id) {  // 传入值语义，内部移动保存
        SPDLOG_INFO("更新MQTT客户端ID old={} new={}", this->client_id, client_id);  // 打印新旧 ID(注意：此处读 this->client_id 未加锁，仅用于日志)
        bool has_client_id = false;  // 新 ID 是否非空
        {  // 只在写成员时持锁
            std::lock_guard<std::mutex> lock(client_id_mutex);  // 与 run() 中的读取互斥
            this->client_id = std::move(client_id);  // 移动保存，减少拷贝
            has_client_id = !this->client_id.empty();  // 以保存后的值为准
        }  // 作用域结束
        connection_state.store(has_client_id ? ConnectionState::Connecting : ConnectionState::Idle, std::memory_order_release);  // 有 ID 则驱动 run() 去连接，无 ID 则回空闲
        // 标记 client_id 已更新，使正在进行的连接流程感知并重启
        is_client_id_updated.store(true, std::memory_order_release);  // 与修订号等价的"参数已变"信号，run() 在每个检查点读取
    }  // set_client_id 结束

    // 功能：获取当前连接状态。
    // 思路：原子读取，无需加锁。
    MqttClient::ConnectionState MqttClient::get_connection_state() const {  // 高频调用(UI 每帧)
        return connection_state.load(std::memory_order_acquire);  // acquire：与写入端的 release 配对，保证看到完整状态
    }  // get_connection_state 结束

    // 功能：获取当前客户端 ID 副本（线程安全）。
    // 思路：加锁拷贝返回。
    std::string MqttClient::get_client_id() const {  // 返回副本而非引用
        std::lock_guard<std::mutex> lock(client_id_mutex);  // 加锁避免与 set_client_id 数据竞争
        return client_id;  // 拷贝返回
    }  // get_client_id 结束

    // 功能：请求停止客户端。
    // 思路：置 is_running=false，并把状态复位为 Idle；run 循环检测到后退出，socket 会被关闭。
    void MqttClient::stop() {  // 析构前由 BackgroundServices::Stop() 调用
        const auto [host, service_port] = get_server_endpoint();  // 结构化绑定取出 host/port，仅用于日志
        SPDLOG_INFO("收到MQTT停止请求 host={} port={}", host, service_port);  // 记录停止时的目标地址
        is_running.store(false, std::memory_order_relaxed);  // relaxed 足够：仅作循环条件，不承担跨变量同步
        connection_state.store(ConnectionState::Idle, std::memory_order_release);  // 对外立即呈现"空闲"，避免 UI 仍显示已连接
    }  // stop 结束

    // 功能：连接/订阅/收消息的主协程循环。
    // 思路：在 is_running 下循环：每次取 endpoint 快照，若地址/ID 变化或连接失败则重建 client；依次完成
    //       底层握手→connect→subscribe→循环 recv；任意阶段检测到参数变更或异常则关闭并重连。
    boost::asio::awaitable<void> MqttClient::run() {  // 由构造函数 co_spawn(detached) 启动，全程在 IO 线程执行
        auto executor = co_await boost::asio::this_coro::executor;  // 获取当前协程所绑定的 Asio 执行器，后续定时器与异步操作都需在同一执行器上运行
        // "等待登录"只在状态变化时提示一次：这段循环每 100ms 跑一轮，原来每次都打一条 DEBUG，
        // 赛前挂 30 分钟就是 1.8 万行同样的日志（SPDLOG_ACTIVE_LEVEL=TRACE 时真的会落盘）。
        bool waiting_for_client_id_logged = false;
        // 外层循环：只要仍在运行就不断尝试连接、收消息、重连
        while (is_running.load(std::memory_order_relaxed)) {  // stop() 置 false 后，下一轮不再进入
            // 记录本轮 endpoint 修订号快照，用于后续“是否发生变化”的快速比对
            const std::uint64_t endpoint_revision_snapshot = endpoint_revision.load(std::memory_order_acquire);  // 本轮基线，之后只与此值比较
            std::string host_ip_cloned;  // 本轮使用的地址副本
            std::string port_cloned;  // 本轮使用的端口副本
            {  // 加锁拷贝配置，避免持锁跨越 co_await
                std::lock_guard<std::mutex> lock(endpoint_mutex);  // 与 set_server_endpoint 互斥
                host_ip_cloned = host_ip;  // 拷贝主机
                port_cloned = port;  // 拷贝端口
            }  // 作用域结束，锁释放(协程挂起时绝不能持锁)
            // peer 形如 "host:port"，仅供日志展示
            const std::string peer = host_ip_cloned + ":" + port_cloned;  // 预拼一次，后续日志复用
            // 闭包：判断 endpoint 是否在快照之后被改动过
            const auto is_endpoint_updated = [&]() {  // 按引用捕获快照，调用开销仅一次原子读
                return endpoint_revision.load(std::memory_order_acquire) != endpoint_revision_snapshot;  // 不等即说明期间被修改
            };  // lambda 结束
            bool need_reconnect = false;  // 本轮是否需要退避后重连
            try {  // 包住整轮"握手-连接-订阅-收包"，任何系统错误都转为重连
                if (host_ip_cloned.empty() || port_cloned.empty()) {  // 尚未配置服务端地址
                    // 地址未配置则停在 Idle，短暂等待后重试
                    connection_state.store(ConnectionState::Idle, std::memory_order_release);  // 对外表现为空闲
                    SPDLOG_WARN("MQTT服务端地址为空，等待后重试");  // 提示配置缺失
                    boost::asio::steady_timer timer(executor, std::chrono::milliseconds(100));  // 100ms 轮询间隔，避免忙等占满 CPU
                    co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));  // 挂起协程让出 IO 线程；不抛异常
                    continue;  // 回到 while 顶部重新取配置
                }  // if 结束

                // 每次连接都创建新的 client 对象
                auto client = std::make_shared<MqttAsyncClient>(executor);  // 共享指针：便于 publish() 跨协程保活
                // 设置 ping 响应超时，超过则视为连接异常
                client->set_pingresp_recv_timeout(mqtt_operation_timeout);  // 心跳无响应超过 5s 即判定链路断开

                {  // 发布"当前活动连接"给其它线程使用
                    std::lock_guard<std::mutex> lock(client_mutex);  // 与 publish() 中的读取互斥
                    active_client = client;  // 保存共享指针，publish() 据此发送
                }  // 作用域结束

                // 发送连接请求
                is_client_id_updated.store(false, std::memory_order_relaxed);  // 清除上一轮的"ID 已更新"标记，作为本轮基线
                std::string client_id_cloned;  // 本轮使用的 client_id 副本
                {  // 加锁读取 client_id
                    std::lock_guard<std::mutex> lock(client_id_mutex);  // 与 set_client_id 互斥
                    client_id_cloned = client_id;  // 拷贝一份，后续 connect_packet 会 move 它
                }  // 作用域结束
                if (client_id_cloned.empty()) {  // 没有 client_id 就无法 CONNECT
                    // 尚未配置 client_id，停在 Idle 稍后重试
                    connection_state.store(ConnectionState::Idle, std::memory_order_release);  // 对外为空闲
                    if (!waiting_for_client_id_logged) {  // 只在进入等待态时提示一次，避免 10Hz 刷屏
                        SPDLOG_INFO("MQTT 尚未设置客户端 ID，等待登录（本提示只出现一次）");  // INFO 级：现场知道"还没登录"就够了
                        waiting_for_client_id_logged = true;  // 记住已提示
                    }  // 结束首次提示
                    boost::asio::steady_timer timer(executor, std::chrono::milliseconds(100));  // 同样 100ms 轮询
                    co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));  // 让出 IO 线程；不抛异常
                    continue;  // 重新开始一轮
                }  // if 结束
                waiting_for_client_id_logged = false;  // 已经有 ID：下次再回到等待态时重新提示一次
                if (connection_state.load(std::memory_order_acquire) != ConnectionState::Failed) {  // Failed 是对外暴露的"上次失败"提示，不要立刻覆盖
                    // 非失败态则标记为“连接中”
                    connection_state.store(ConnectionState::Connecting, std::memory_order_release);  // UI 显示"连接中"
                }  // if 结束

                // 检查在准备连接过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {  // 两个变更信号任一成立
                    SPDLOG_DEBUG("MQTT参数在连接前已更新，跳过本次连接 peer={}", peer);  // 说明参数已变，本轮作废
                    continue;  // 丢弃刚创建的 client，回顶部用新参数重建
                }  // if 结束

                // 底层网络协议握手
                SPDLOG_INFO("MQTT开始底层网络协议握手 peer={} client_id={}", peer, client_id_cloned);  // 握手=建立 TCP 连接(非 TLS 时)
                auto [handshake_ec] = co_await client->async_underlying_handshake(  // 按 host/port 建立底层 TCP 连接
                        host_ip_cloned, port_cloned,  // 目标地址
                        boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常，失败返回错误码
                if (handshake_ec) {  // 连接失败：拒绝/不可达/超时
                    SPDLOG_WARN("MQTT底层握手失败 peer={} code={} message={}",  // 记录失败原因
                                peer, static_cast<long long>(handshake_ec.value()), handshake_ec.message());  // 错误码与文本
                    if (is_running.load(std::memory_order_relaxed)) {  // 非 stop() 触发
                        connection_state.store(ConnectionState::Failed, std::memory_order_release);  // 对外显示"连接失败"
                    }  // if 结束
                    need_reconnect = true;  // 走退避重连
                    continue;  // 本轮作废
                }  // if 结束
                SPDLOG_INFO("MQTT底层网络协议握手成功 peer={} client_id={}", peer, client_id_cloned);  // TCP 已连通

                // 检查在握手过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {  // 握手期间参数被改
                    SPDLOG_INFO("MQTT握手完成后检测到参数变更，关闭当前连接 peer={}", peer);  // 说明需要立刻重建
                    co_await client->async_close(boost::asio::as_tuple(boost::asio::use_awaitable));  // 只关了 TCP，未发 DISCONNECT(此时还没 CONNECT)；不抛异常
                    continue;  // 重新开始
                }  // if 结束

                // 发送连接请求并接收回复
                SPDLOG_INFO("MQTT开始连接 peer={} client_id={}", peer, client_id_cloned);  // 发送 CONNECT 并等 CONNACK
                auto [start_ec, connack_opt] = co_await client->async_start(  // 发送 CONNECT 并等待 CONNACK
                        async_mqtt::v3_1_1::connect_packet{  // 构造 MQTT 3.1.1 CONNECT 报文
                                true, // clean_session=true：断开后 broker 不保留订阅与未确认报文，重连即全新会话
                                mqtt_keep_alive_seconds,  // keep-alive 秒数，broker 据此判定超时
                                std::move(client_id_cloned)},  // 客户端 ID：move 进报文，避免大字符串拷贝
                        boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常，失败返回错误码
                if (start_ec) {  // 连接被拒绝/超时
                    SPDLOG_WARN("MQTT连接失败 peer={} code={} message={}",  // 记录原因（凭据错、broker 拒绝等）
                                peer, static_cast<long long>(start_ec.value()), start_ec.message());  // 错误码与文本
                    if (is_running.load(std::memory_order_relaxed)) {  // 非 stop 触发
                        connection_state.store(ConnectionState::Failed, std::memory_order_release);  // 对外显示失败
                    }  // if 结束
                    need_reconnect = true;  // 退避重连
                    continue;  // 本轮作废
                }  // if 结束
                SPDLOG_INFO("MQTT连接成功 peer={}", peer);  // CONNACK 返回成功

                // 检查在连接过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {  // 连接期间参数被改
                    SPDLOG_INFO("MQTT连接后检测到参数变更，主动断开 peer={}", peer);  // 已建会话，需先 DISCONNECT 再 close
                    co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{},  // 发送 DISCONNECT
                                                      boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常
                    co_await client->async_close(boost::asio::as_tuple(boost::asio::use_awaitable));  // 关闭底层 socket，不抛异常
                    continue;  // 回到顶部重建
                }  // if 结束

                // 发送要订阅的话题
                // 订阅策略：裁判系统下行话题统一用 QoS0（at_most_once）。状态/图传类消息本就高频且允许丢包，
                // 不重传可降低延迟与抖动，也避免 broker 积压未确认报文。
                SPDLOG_INFO("MQTT开始订阅话题 peer={}", peer);  // 进入订阅阶段
                std::vector<async_mqtt::topic_subopts> sub_entry{  // topic_subopts = "话题名 + QoS" 的订阅项
                        {"GameStatus", async_mqtt::qos::at_most_once},                       // 比赛整体状态（阶段、胜负、倒计时）
                        {"GlobalUnitStatus", async_mqtt::qos::at_most_once},                 // 全队单位状态汇总
                        {"GlobalLogisticsStatus", async_mqtt::qos::at_most_once},           // 全队后勤/补给状态
                        {"GlobalSpecialMechanism", async_mqtt::qos::at_most_once},          // 全队特殊机关状态
                        {"Event", async_mqtt::qos::at_most_once},                           // 比赛事件流（击杀、增益、犯规等）
                        {"RobotInjuryStat", async_mqtt::qos::at_most_once},                 // 机器人受伤/被击中统计
                        {"RobotRespawnStatus", async_mqtt::qos::at_most_once},              // 机器人复活状态
                        {"RobotStaticStatus", async_mqtt::qos::at_most_once},               // 机器人静态属性（类型、编号、阵营）
                        {"RobotDynamicStatus", async_mqtt::qos::at_most_once},              // 机器人动态状态（血量、热量、能量）
                        {"RobotModuleStatus", async_mqtt::qos::at_most_once},               // 机器人模块状态（射击/底盘/云台等）
                        {"RobotPosition", async_mqtt::qos::at_most_once},                   // 机器人实时坐标
                        {"Buff", async_mqtt::qos::at_most_once},                            // 增益/神符触发状态
                        {"PenaltyInfo", async_mqtt::qos::at_most_once},                     // 惩罚信息
                        {"RobotPathPlanInfo", async_mqtt::qos::at_most_once},               // 机器人路径规划信息
                        {"RadarInfoToClient", async_mqtt::qos::at_most_once},               // 雷达站下发给客户端的信息
                        {"MapClickInfo", async_mqtt::qos::at_most_once},                    // 地图点击交互信息
                        {"CustomByteBlock", async_mqtt::qos::at_most_once},                 // 自定义字节块（自定义客户端收发通道）
                        {"TechCoreMotionStateSync", async_mqtt::qos::at_most_once},         // 技术核心运动状态同步
                        {"RobotPerformanceSelectionSync", async_mqtt::qos::at_most_once},   // 机器人性能方案选择同步
                        {"RuneStatusSync", async_mqtt::qos::at_most_once},                  // 神符状态同步
                        {"SentryStatusSync", async_mqtt::qos::at_most_once},                // 哨兵机器人状态同步
                        {"DartSelectTargetStatusSync", async_mqtt::qos::at_most_once},      // 飞镖选择目标状态同步
                        {"SentryCtrlResult", async_mqtt::qos::at_most_once}};               // 哨兵控制结果反馈（2027 删除部署/空中支援后，本项为列表末项）
                const auto subscribe_packet_id = client->acquire_unique_packet_id();  // 申请唯一 packet_id 用于匹配 SUBACK
                if (!subscribe_packet_id.has_value()) {  // 会话未就绪时拿不到 ID
                    SPDLOG_WARN("MQTT订阅失败：无可用报文ID peer={}", peer);  // 记录原因
                    need_reconnect = true;  // 重连重建会话
                    continue;  // 本轮作废
                }  // if 结束
                auto [subscribe_ec, suback_opt] = co_await client->async_subscribe(  // 发送 SUBSCRIBE 并等待 SUBACK
                        async_mqtt::v3_1_1::subscribe_packet{  // 构造 SUBSCRIBE 报文
                                *subscribe_packet_id,  // 报文标识符
                                async_mqtt::force_move(sub_entry)},  // force_move 把订阅表移入报文，避免一次大向量拷贝
                        boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常，失败返回错误码
                if (subscribe_ec) {  // 订阅失败
                    SPDLOG_WARN("MQTT订阅失败 peer={} code={} message={}",  // 记录原因
                                peer, static_cast<long long>(subscribe_ec.value()), subscribe_ec.message());  // 错误码与文本
                    if (is_running.load(std::memory_order_relaxed)) {  // 非 stop 触发
                        connection_state.store(ConnectionState::Failed, std::memory_order_release);  // 对外显示失败
                    }  // if 结束
                    need_reconnect = true;  // 退避重连
                    continue;  // 本轮作废
                }  // if 结束
                SPDLOG_INFO("MQTT话题订阅成功 peer={} topic_count={}", peer, 23);  // 23 与上面订阅项数量一致（2027 下行话题总数）
                // 订阅完成即视为已连接，唤醒业务层上行发布
                connection_state.store(ConnectionState::Connected, std::memory_order_release);  // publish() 只在此状态之后才真正发送

                // 检查在订阅过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {  // 订阅期间参数被改
                    connection_state.store(ConnectionState::Connecting, std::memory_order_release);  // 先退回"连接中"，避免对外短暂显示可用
                    SPDLOG_INFO("MQTT订阅后检测到参数变更，主动断开 peer={}", peer);  // 记录触发重连的原因
                    co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{},  // 正常结束会话
                                                      boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常
                    co_await client->async_close(boost::asio::as_tuple(boost::asio::use_awaitable));  // 关闭 socket，不抛异常
                    continue;  // 回到顶部
                }  // if 结束

                // 接收信息
                // 持续接收下行报文，直到停止、client_id 变更或 endpoint 变更
                while (is_running.load(std::memory_order_relaxed)  // 未收到 stop
                       && !is_client_id_updated.load(std::memory_order_relaxed)  // client_id 未被修改
                       && !is_endpoint_updated()) {  // endpoint 未被修改
                    // 关键：这里用 as_tuple 而不是 use_awaitable。
                    // 链路断开时 use_awaitable 会抛 boost::system::system_error，
                    // 而实测"抛异常 + 栈展开"这条路径在 Windows 上会触发访问冲突（0xC0000005，
                    // 崩溃栈落在 _CxxFrameHandler3 / is_exception_typeof / RtlCaptureContext2，即异常展开阶段）。
                    // 改成 as_tuple 后错误码直接作为返回值，全程不产生异常，断线可以平滑走到重连。
                    auto [recv_ec, pv_opt] = co_await client->async_recv(  // async_recv：等下一个报文
                            boost::asio::as_tuple(boost::asio::use_awaitable));  // as_tuple：出错不抛异常，返回 error_code
                    if (recv_ec) {  // 收包失败：对端关闭 / 连接被复位 / 心跳超时
                        SPDLOG_WARN("MQTT收包中断 peer={} code={} message={}",  // 记录原因，便于区分"网络断"与"服务端主动踢"
                                    peer,  // 对端地址
                                    static_cast<long long>(recv_ec.value()),  // 错误码
                                    recv_ec.message());  // 错误文本
                        if (is_running.load(std::memory_order_relaxed)) {  // 非 stop() 触发的取消
                            connection_state.store(ConnectionState::Failed, std::memory_order_release);  // 对外显示"连接失败"
                        }  // if 结束
                        need_reconnect = true;  // 交给下面的退避重连
                        break;  // 退出收包循环
                    }  // if 结束
                    if (!pv_opt.has_value()) {  // 没有错误码但没有报文：对端已正常关闭连接
                        SPDLOG_INFO("MQTT连接已被对端关闭 peer={}", peer);  // 记录一次
                        if (is_running.load(std::memory_order_relaxed)) {  // 同样只在非 stop 时标失败
                            connection_state.store(ConnectionState::Failed, std::memory_order_release);  // 对外显示"连接失败"
                        }  // if 结束
                        need_reconnect = true;  // 触发重连
                        break;  // 退出收包循环
                    }  // if 结束
                    {  // 有报文才分发（花括号避免与上方变量作用域混淆）
                        // 对收到的报文按类型分发处理
                        pv_opt->visit(  // visit：按 variant 的实际类型分发到下面某个 lambda
                                async_mqtt::overload{  // overload：把多个 lambda 合成一个可按类型重载的访问器
                                        [this, &peer](async_mqtt::v3_1_1::publish_packet const &p) {  // 收到 PUBLISH(业务下行报文)
                                            // 接收到对方发送的信息
                                            const std::string topic = p.topic();  // 取出话题名
                                            const std::string payload = p.payload();  // 取出负载(protobuf 字节)
                                            // 记录下行原始报文（rx）供抓包/复盘
                                            RoboGo::logging::log_network_payload("mqtt",  // 通道名
                                                                                 "rx",  // 方向：接收
                                                                                 topic,  // 话题名
                                                                                 payload,  // 原始负载
                                                                                 peer);  // 对端地址
                                            // 交给业务层回调解析
                                            on_receive_message(topic, payload);  // 注意：此回调在 IO 线程执行，必须快速返回(通常是入队)
                                        },  // PUBLISH 处理结束
                                        [&](async_mqtt::v3_1_1::puback_packet const &packet) {  // 收到 PUBACK(我们发出 QoS1 消息的确认)
                                            // 对方接收到信息的回应
                                            SPDLOG_DEBUG("收到MQTT PUBACK peer={} packet_id={}", peer, packet.packet_id());  // 仅 DEBUG 记录，便于核对发送是否被确认
                                        },  // PUBACK 处理结束
                                        [&](auto const &) {  // 其它类型报文(CONNACK/PINGRESP/SUBACK 等)
                                            // 其它非业务类型报文（如 PINGRESP）仅追踪
                                            SPDLOG_TRACE("收到非业务MQTT数据包 peer={}", peer);  // TRACE 级别：正常情况下不输出
                                        }});  // visit 调用结束
                    }  // 报文分发作用域结束
                }  // 收包 while 结束

                // 发送断开连接请求（同样用 as_tuple：断线后 DISCONNECT 必然失败，不能让异常再走展开路径）
                SPDLOG_INFO("MQTT开始主动断开连接 peer={}", peer);  // 走到这里说明需要停/重建连接
                {
                    auto [disconnect_ec] = co_await client->async_disconnect(  // 发送 DISCONNECT，礼貌结束会话
                            async_mqtt::v3_1_1::disconnect_packet{},  // 断开报文
                            boost::asio::as_tuple(boost::asio::use_awaitable));  // 不抛异常
                    if (disconnect_ec) {  // 对端已断开时这里几乎必然失败，属正常现象
                        SPDLOG_DEBUG("MQTT发送DISCONNECT失败(通常对端已断开) peer={} code={}",  // 仅 DEBUG
                                     peer, static_cast<long long>(disconnect_ec.value()));  // 错误码
                    } else {  // 正常发出
                        SPDLOG_INFO("MQTT断开连接成功 peer={}", peer);  // broker 已收到断开通知
                    }  // if-else 结束
                }

                // 关闭底层socket，确保状态重置。
                // async_close 的完成签名没有 error_code（as_tuple 得到空 tuple），因此这里不取返回值；
                // 关闭失败也不会抛异常，异常路径彻底绕开。
                co_await client->async_close(boost::asio::as_tuple(boost::asio::use_awaitable));  // 释放文件描述符，避免句柄泄漏

            } catch (const boost::system::system_error &error) {  // 网络/协议层异常(含超时 timed_out)
                // 网络/协议层异常：记录并将状态置 Failed，准备重连
                SPDLOG_ERROR("MQTT运行异常 peer={} code={} message={}",  // 记录错误码与文本
                             peer,  // 对端地址
                             static_cast<long long>(error.code().value()),  // 错误码
                             error.code().message());  // 错误文本
                if (is_running.load(std::memory_order_relaxed)) {  // 若是 stop() 触发的取消，就不必标 Failed
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);  // 对外显示"连接失败"
                }  // if 结束
                need_reconnect = true;  // 标记需要退避重连
            } catch (const std::exception &error) {  // 其它标准异常(如内存分配失败)
                // 其它标准异常同样处理为重连
                SPDLOG_ERROR("MQTT运行异常 peer={} message={}", peer, error.what());  // 记录异常描述
                if (is_running.load(std::memory_order_relaxed)) {  // 仅在仍运行时有意义
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);  // 标记失败
                }  // if 结束
                need_reconnect = true;  // 同样触发重连
            }  // try-catch 结束

            {  // 清理本轮活动连接，必须先把共享指针置空
                std::lock_guard<std::mutex> lock(client_mutex);  // 与 publish() 的读取互斥
                active_client.reset();  // 释放本轮的 client；若 publish() 已取走副本，则等其发送完再真正析构
            }  // 作用域结束
            SPDLOG_DEBUG("MQTT活动连接已清理 peer={}", peer);  // 便于确认资源已归还

            if (need_reconnect) {  // 仅在出错时退避，正常参数变更无需等待
                // 出现错误，等待 100ms 再重新连接
                SPDLOG_WARN("MQTT将在{}ms后尝试重连 peer={}",  // 提示即将重连及退避时长
                            std::chrono::duration_cast<std::chrono::milliseconds>(mqtt_reconnect_delay).count(),  // 常量转毫秒
                            peer);  // 对端地址
                boost::asio::steady_timer timer(executor, mqtt_reconnect_delay);  // 退避定时器
                auto [error_code] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));  // as_tuple：取消/出错不抛异常
                if (error_code) {  // 定时器被取消(如 io_context 停止)
                    SPDLOG_ERROR("MQTT重连等待失败 peer={} message={}", peer, error_code.message());  // 记录但继续，靠外层 while 判断是否还要重连
                }  // if 结束
            }  // if 结束
        }  // 外层 while 结束
        // 循环结束（收到 stop）：复位状态并记录
        connection_state.store(ConnectionState::Idle, std::memory_order_release);  // 退出前对外呈现空闲
        SPDLOG_INFO("MQTT运行循环已退出");  // 协程正常返回
    }  // run 结束

    // 功能：发布上行消息（线程安全入口）。
    // 思路：未连接则跳过；取 active_client 共享指针副本，在其执行器上 detached 启动 PublishMqttMessage 协程，避免阻塞调用方。
    void MqttClient::publish(const std::string &topic, const std::string &message) {  // 可在 UI 线程调用，内部立即返回
        std::string host_ip_cloned;  // 仅用于日志的对端地址副本
        std::string port_cloned;  // 同上
        {  // 加锁拷贝配置
            std::lock_guard<std::mutex> lock(endpoint_mutex);  // 与 set_server_endpoint 互斥
            host_ip_cloned = host_ip;  // 拷贝主机
            port_cloned = port;  // 拷贝端口
        }  // 作用域结束
        const std::string peer = host_ip_cloned + ":" + port_cloned;  // 预拼日志字符串
        if (connection_state.load(std::memory_order_acquire) != ConnectionState::Connected) {  // 只有已订阅成功才算"可用"
            // 仅在已连接态发送，否则跳过（避免向未就绪连接写入）
            SPDLOG_DEBUG("MQTT尚未登录，跳过发送 topic={} bytes={} peer={}", topic, message.size(), peer);  // DEBUG：未连接时高频调用不刷屏
            return;  // 直接丢弃本次发送
        }  // if 结束

        // 获取当前活动的 client 对象的共享指针副本
        std::shared_ptr<MqttAsyncClient> client_copy;  // 空表示 run() 正在重建/已清理
        {  // 加锁取副本
            std::lock_guard<std::mutex> lock(client_mutex);  // 与 run() 中的 reset 互斥
            if (active_client) {  // 有活动连接才拷贝
                client_copy = active_client;  // 拷贝后引用计数 +1，发送期间不会被析构
            }  // if 结束
        }  // 作用域结束

        if (client_copy) {  // 成功拿到连接
            // 在 client 执行器上异步发送，detached 自管理
            boost::asio::co_spawn(client_copy->get_executor(),  // 投递到 IO 线程执行，保证与收包在同一执行器
                                  PublishMqttMessage(client_copy, topic, message, peer),  // 参数按值传入，detached 协程自行持有
                                  boost::asio::detached);  // detached：不等结果，异常已在协程内部捕获
        } else {  // 状态是 Connected 但连接对象已被清空(极短的重建窗口)
            SPDLOG_WARN("MQTT客户端未连接，跳过发送 topic={} bytes={} peer={}", topic, message.size(), peer);  // 记录这次丢弃
        }  // if-else 结束
    }  // publish 结束

}// namespace RoboGo::io  // 网络 IO 命名空间结束
