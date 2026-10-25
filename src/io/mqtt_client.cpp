#include "mqtt_client.h"
#include "logging/logging.h"

// 匿名命名空间：本编译单元内部的辅助函数与类型，不对外暴露
namespace {

    // MQTT 异步客户端类型别名：使用 MQTT 3.1.1 协议、纯 MQTT（非 WebSocket）传输
    using MqttAsyncClient = async_mqtt::client<async_mqtt::protocol_version::v3_1_1, async_mqtt::protocol::mqtt>;

    // 单次 MQTT 操作（连接/订阅/收发等）的最长等待时间
    constexpr auto mqtt_operation_timeout = std::chrono::seconds(5);
    // 连接失败后重连前的等待间隔
    constexpr auto mqtt_reconnect_delay = std::chrono::milliseconds(100);
    // MQTT 心跳（keep-alive）间隔，单位秒
    constexpr std::uint16_t mqtt_keep_alive_seconds = 5;

    // 功能：等待一次 MQTT 操作超时（用定时器协程实现）。
    // 思路：创建一个 steady_timer，时长 mqtt_operation_timeout；到点若未被取消则抛出异常，从而打断等待中的 MQTT 操作。
    boost::asio::awaitable<void> WaitForMqttOperationTimeout() {
        // 取当前协程所在执行器，定时器需要在同一执行器上运行
        auto executor = co_await boost::asio::this_coro::executor;
        boost::asio::steady_timer timer(executor, mqtt_operation_timeout);
        // 异步等待并捕获错误码；as_tuple 把结果拆成 (error_code) 形式
        auto [error_code] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
        if (error_code) {
            // 定时器本身出错时抛出异常，交由上层处理
            throw boost::system::system_error(error_code);
        }
    }

    // 功能：安全关闭 MQTT 客户端连接，忽略“已中断/未连接/描述符失效”等可预期的关闭错误。
    // 思路：调用 async_close，对特定错误码（operation_aborted / not_connected / bad_descriptor）不告警，其余才记录 WARN。
    boost::asio::awaitable<void> CloseMqttClientNoThrow(MqttAsyncClient &client, std::string_view peer, std::string_view reason) {
        try {
            co_await client.async_close(boost::asio::use_awaitable);
        } catch (const boost::system::system_error &error) {
            if (error.code() != boost::asio::error::operation_aborted &&
                error.code() != boost::asio::error::not_connected &&
                error.code() != boost::asio::error::bad_descriptor) {
                // 仅对真正的异常错误码记录告警，避免重连/超时时刷屏
                SPDLOG_WARN("MQTT关闭连接失败 peer={} reason={} code={} message={}",
                            peer,
                            reason,
                            static_cast<long long>(error.code().value()),
                            error.code().message());
            }
        }
    }

    // 功能：为任意 MQTT 操作包裹超时控制，超时则按策略关闭连接并抛异常。
    // 思路：用 awaitable_operators 的 || 并发等待“业务操作”与“超时等待”，谁先完成谁获胜；
    //       若超时分支先完成（result.index()==1），记录告警并可选地关闭客户端后抛出 timed_out。
    // 参数：client 客户端；peer 用于日志的“地址:端口”；operation_name 操作名；operation_factory 返回协程的可调用对象；
    //       close_client_on_timeout 超时后是否主动关闭连接（publish 等轻量操作传 false）。
    template <typename OperationFactory>
    boost::asio::awaitable<void> RunMqttOperationWithTimeout(MqttAsyncClient &client,
                                                             std::string_view peer,
                                                             std::string_view operation_name,
                                                             OperationFactory &&operation_factory,
                                                             bool close_client_on_timeout = true) {
        using namespace boost::asio::experimental::awaitable_operators;

        auto operation = std::forward<OperationFactory>(operation_factory)();
        // || 表示“任意一个完成即返回”，variant 的 index 指示哪个分支先完成：0=操作，1=超时
        auto result = co_await (std::move(operation) || WaitForMqttOperationTimeout());
        if (result.index() == 1) {
            // 超时分支先完成，记录告警
            const auto timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(mqtt_operation_timeout).count();
            SPDLOG_WARN("MQTT操作超时 peer={} operation={} timeout_ms={}", peer, operation_name, timeout_ms);
            if (close_client_on_timeout) {
                // 默认超时后直接关闭连接，回收底层资源
                co_await CloseMqttClientNoThrow(client, peer, operation_name);
            } else {
                // 部分场景（如 publish）交由调用方自行处理，不主动关闭
                SPDLOG_WARN("MQTT操作超时后不主动关闭连接 peer={} operation={}", peer, operation_name);
            }
            // 抛出 timed_out，使上层 run() 捕获并进入重连流程
            throw boost::system::system_error(boost::asio::error::timed_out);
        }
    }

    // 功能：发布一条 MQTT 消息（QoS1，至少一次）。
    // 思路：先取得唯一 packet_id（会话未就绪则跳过），按 network_capture 格式记录发送负载，再用超时包裹的 publish 发出；
    //       捕获 system_error 仅记录错误，不向上抛（发布失败不应中断主循环）。
    boost::asio::awaitable<void> PublishMqttMessage(std::shared_ptr<MqttAsyncClient> client,
                                                    std::string topic,
                                                    std::string message,
                                                    std::string peer) {
        try {
            // 申请一个本会话内唯一的报文 ID，用于匹配 ACK；无可用 ID 说明会话尚未就绪
            const auto packet_id = client->acquire_unique_packet_id();
            if (!packet_id.has_value()) {
                SPDLOG_WARN("MQTT会话未就绪，跳过发送 topic={} bytes={} peer={}",
                            topic,
                            message.size(),
                            peer);
                co_return;
            }
            // 在发送前记录原始报文（上行 tx），供复盘/抓包
            RoboGo::logging::log_network_payload("mqtt",
                                                 "tx",
                                                 topic,
                                                 message,
                                                 peer);
            SPDLOG_DEBUG("发送MQTT消息 topic={} bytes={} peer={}", topic, message.size(), peer);
            // 用超时包裹 publish，注意此处 close_client_on_timeout=false，避免单次发送超时误关连接
            co_await RunMqttOperationWithTimeout(*client,
                                                 peer,
                                                 "publish",
                                                 [&]() -> boost::asio::awaitable<void> {
                                                     co_await client->async_publish(
                                                             async_mqtt::v3_1_1::publish_packet{
                                                                     *packet_id,
                                                                     topic,
                                                                     message,
                                                                     async_mqtt::qos::at_least_once},
                                                             boost::asio::use_awaitable);
                                                 },
                                                 false);
        } catch (const boost::system::system_error &error) {
            // 发送失败仅记录，不向上抛，保证主循环稳定
            const auto error_code = static_cast<long long>(error.code().value());
            const std::string error_message = error.code().message();
            SPDLOG_ERROR("MQTT发送失败 topic={} bytes={} peer={} code={} message={}",
                         topic,
                         message.size(),
                         peer,
                         error_code,
                         error_message);
        }
    }

}// namespace

namespace RoboGo::io {

    // 功能：构造 MQTT 客户端并启动后台连接协程。
    // 思路：保存 host/port 与下行回调，co_spawn 启动 run() 协程（detached 自管理生命周期）。
    MqttClient::MqttClient(boost::asio::io_context &io_context,
                           std::string host_ip,
                           std::string port,
                           const std::function<void(const std::string &topic, const std::string &payload)> &on_receive_message)
        : host_ip(std::move(host_ip)),
          port(std::move(port)),
          on_receive_message(on_receive_message) {
        SPDLOG_INFO("创建MQTT客户端 host={} port={}", this->host_ip, this->port);
        // 在 io_context 上分离式启动连接主循环
        boost::asio::co_spawn(io_context, run(), boost::asio::detached);
    }

    // 功能：动态更新服务端地址。
    // 思路：空地址忽略；加锁比较新旧地址，仅在确实变化时记录并递增 endpoint_revision 触发 run 循环重建连接。
    void MqttClient::set_server_endpoint(std::string host_ip, std::string port) {
        if (host_ip.empty() || port.empty()) {
            SPDLOG_WARN("忽略空的MQTT服务端地址 host={} port={}", host_ip, port);
            return;
        }

        bool endpoint_changed = false;
        {
            // 加锁读取并比较，避免与 run 循环并发写竞争
            std::lock_guard<std::mutex> lock(endpoint_mutex);
            if (this->host_ip != host_ip || this->port != port) {
                SPDLOG_INFO("更新MQTT服务端地址 old={}:{} new={}:{}",
                            this->host_ip,
                            this->port,
                            host_ip,
                            port);
                this->host_ip = std::move(host_ip);
                this->port = std::move(port);
                endpoint_changed = true;
            }
        }

        if (!endpoint_changed) {
            // 地址未变则无需任何动作
            return;
        }

        // 修订号 +1，run 循环通过对比快照检测变更并重建连接
        endpoint_revision.fetch_add(1, std::memory_order_acq_rel);
        bool has_client_id = false;
        {
            std::lock_guard<std::mutex> lock(client_id_mutex);
            has_client_id = !client_id.empty();
        }
        // 有 client_id 才进入 Connecting 触发连接；否则停留在 Idle 等待 client_id
        connection_state.store(has_client_id ? ConnectionState::Connecting : ConnectionState::Idle, std::memory_order_release);
    }

    // 功能：获取当前服务端地址（线程安全副本）。
    // 思路：加锁拷贝，返回 pair<host, port>。
    std::pair<std::string, std::string> MqttClient::get_server_endpoint() const {
        std::lock_guard<std::mutex> lock(endpoint_mutex);
        return {host_ip, port};
    }

    // 功能：设置客户端 ID。
    // 思路：保存 ID 后，有 ID 则置 Connecting 触发连接；同时置 is_client_id_updated=true 供 run 循环在连接前/中检测并重建。
    void MqttClient::set_client_id(std::string client_id) {
        SPDLOG_INFO("更新MQTT客户端ID old={} new={}", this->client_id, client_id);
        bool has_client_id = false;
        {
            std::lock_guard<std::mutex> lock(client_id_mutex);
            this->client_id = std::move(client_id);
            has_client_id = !this->client_id.empty();
        }
        connection_state.store(has_client_id ? ConnectionState::Connecting : ConnectionState::Idle, std::memory_order_release);
        // 标记 client_id 已更新，使正在进行的连接流程感知并重启
        is_client_id_updated.store(true, std::memory_order_release);
    }

    // 功能：获取当前连接状态。
    // 思路：原子读取，无需加锁。
    MqttClient::ConnectionState MqttClient::get_connection_state() const {
        return connection_state.load(std::memory_order_acquire);
    }

    // 功能：获取当前客户端 ID 副本（线程安全）。
    // 思路：加锁拷贝返回。
    std::string MqttClient::get_client_id() const {
        std::lock_guard<std::mutex> lock(client_id_mutex);
        return client_id;
    }

    // 功能：请求停止客户端。
    // 思路：置 is_running=false，并把状态复位为 Idle；run 循环检测到后退出，socket 会被关闭。
    void MqttClient::stop() {
        const auto [host, service_port] = get_server_endpoint();
        SPDLOG_INFO("收到MQTT停止请求 host={} port={}", host, service_port);
        is_running.store(false, std::memory_order_relaxed);
        connection_state.store(ConnectionState::Idle, std::memory_order_release);
    }

    // 功能：连接/订阅/收消息的主协程循环。
    // 思路：在 is_running 下循环：每次取 endpoint 快照，若地址/ID 变化或连接失败则重建 client；依次完成
    //       底层握手→connect→subscribe→循环 recv；任意阶段检测到参数变更或异常则关闭并重连。
    boost::asio::awaitable<void> MqttClient::run() {
        auto executor = co_await boost::asio::this_coro::executor;
        // 外层循环：只要仍在运行就不断尝试连接、收消息、重连
        while (is_running.load(std::memory_order_relaxed)) {
            // 记录本轮 endpoint 修订号快照，用于后续“是否发生变化”的快速比对
            const std::uint64_t endpoint_revision_snapshot = endpoint_revision.load(std::memory_order_acquire);
            std::string host_ip_cloned;
            std::string port_cloned;
            {
                std::lock_guard<std::mutex> lock(endpoint_mutex);
                host_ip_cloned = host_ip;
                port_cloned = port;
            }
            // peer 形如 "host:port"，仅供日志展示
            const std::string peer = host_ip_cloned + ":" + port_cloned;
            // 闭包：判断 endpoint 是否在快照之后被改动过
            const auto is_endpoint_updated = [&]() {
                return endpoint_revision.load(std::memory_order_acquire) != endpoint_revision_snapshot;
            };
            bool need_reconnect = false;
            try {
                if (host_ip_cloned.empty() || port_cloned.empty()) {
                    // 地址未配置则停在 Idle，短暂等待后重试
                    connection_state.store(ConnectionState::Idle, std::memory_order_release);
                    SPDLOG_WARN("MQTT服务端地址为空，等待后重试");
                    boost::asio::steady_timer timer(executor, std::chrono::milliseconds(100));
                    co_await timer.async_wait(boost::asio::use_awaitable);
                    continue;
                }

                // 每次连接都创建新的 client 对象
                auto client = std::make_shared<MqttAsyncClient>(executor);
                // 设置 ping 响应超时，超过则视为连接异常
                client->set_pingresp_recv_timeout(mqtt_operation_timeout);

                {
                    std::lock_guard<std::mutex> lock(client_mutex);
                    active_client = client;
                }

                // 发送连接请求
                is_client_id_updated.store(false, std::memory_order_relaxed);
                std::string client_id_cloned;
                {
                    std::lock_guard<std::mutex> lock(client_id_mutex);
                    client_id_cloned = client_id;
                }
                if (client_id_cloned.empty()) {
                    // 尚未配置 client_id，停在 Idle 稍后重试
                    connection_state.store(ConnectionState::Idle, std::memory_order_release);
                    SPDLOG_DEBUG("MQTT尚未设置客户端ID，等待后重试 peer={}", peer);
                    boost::asio::steady_timer timer(executor, std::chrono::milliseconds(100));
                    co_await timer.async_wait(boost::asio::use_awaitable);
                    continue;
                }
                if (connection_state.load(std::memory_order_acquire) != ConnectionState::Failed) {
                    // 非失败态则标记为“连接中”
                    connection_state.store(ConnectionState::Connecting, std::memory_order_release);
                }

                // 检查在准备连接过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                    SPDLOG_DEBUG("MQTT参数在连接前已更新，跳过本次连接 peer={}", peer);
                    continue;
                }

                // 底层网络协议握手
                SPDLOG_INFO("MQTT开始底层网络协议握手 peer={} client_id={}", peer, client_id_cloned);
                co_await RunMqttOperationWithTimeout(*client,
                                                     peer,
                                                     "underlying_handshake",
                                                     [&]() -> boost::asio::awaitable<void> {
                                                         co_await client->async_underlying_handshake(host_ip_cloned, port_cloned, boost::asio::use_awaitable);
                                                     });
                SPDLOG_INFO("MQTT底层网络协议握手成功 peer={} client_id={}", peer, client_id_cloned);

                // 检查在握手过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                    SPDLOG_INFO("MQTT握手完成后检测到参数变更，关闭当前连接 peer={}", peer);
                    co_await client->async_close(boost::asio::use_awaitable);
                    continue;
                }

                // 发送连接请求并接收回复
                SPDLOG_INFO("MQTT开始连接 peer={} client_id={}", peer, client_id_cloned);
                co_await RunMqttOperationWithTimeout(*client,
                                                     peer,
                                                     "connect",
                                                     [&]() -> boost::asio::awaitable<void> {
                                                         co_await client->async_start(
                                                                 async_mqtt::v3_1_1::connect_packet{
                                                                         true,
                                                                         mqtt_keep_alive_seconds,
                                                                         std::move(client_id_cloned)},
                                                                 boost::asio::use_awaitable);
                                                     });
                SPDLOG_INFO("MQTT连接成功 peer={}", peer);

                // 检查在连接过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                    SPDLOG_INFO("MQTT连接后检测到参数变更，主动断开 peer={}", peer);
                    co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{}, boost::asio::use_awaitable);
                    co_await client->async_close(boost::asio::use_awaitable);
                    continue;
                }

                // 发送要订阅的话题
                SPDLOG_INFO("MQTT开始订阅话题 peer={}", peer);
                std::vector<async_mqtt::topic_subopts> sub_entry{
                        {"GameStatus", async_mqtt::qos::at_most_once},
                        {"GlobalUnitStatus", async_mqtt::qos::at_most_once},
                        {"GlobalLogisticsStatus", async_mqtt::qos::at_most_once},
                        {"GlobalSpecialMechanism", async_mqtt::qos::at_most_once},
                        {"Event", async_mqtt::qos::at_most_once},
                        {"RobotInjuryStat", async_mqtt::qos::at_most_once},
                        {"RobotRespawnStatus", async_mqtt::qos::at_most_once},
                        {"RobotStaticStatus", async_mqtt::qos::at_most_once},
                        {"RobotDynamicStatus", async_mqtt::qos::at_most_once},
                        {"RobotModuleStatus", async_mqtt::qos::at_most_once},
                        {"RobotPosition", async_mqtt::qos::at_most_once},
                        {"Buff", async_mqtt::qos::at_most_once},
                        {"PenaltyInfo", async_mqtt::qos::at_most_once},
                        {"RobotPathPlanInfo", async_mqtt::qos::at_most_once},
                        {"RadarInfoToClient", async_mqtt::qos::at_most_once},
                        {"MapClickInfo", async_mqtt::qos::at_most_once},
                        {"CustomByteBlock", async_mqtt::qos::at_most_once},
                        {"TechCoreMotionStateSync", async_mqtt::qos::at_most_once},
                        {"RobotPerformanceSelectionSync", async_mqtt::qos::at_most_once},
                        {"DeployModeStatusSync", async_mqtt::qos::at_most_once},
                        {"RuneStatusSync", async_mqtt::qos::at_most_once},
                        {"SentryStatusSync", async_mqtt::qos::at_most_once},
                        {"DartSelectTargetStatusSync", async_mqtt::qos::at_most_once},
                        {"SentryCtrlResult", async_mqtt::qos::at_most_once},
                        {"AirSupportStatusSync", async_mqtt::qos::at_most_once}};
                co_await RunMqttOperationWithTimeout(*client,
                                                     peer,
                                                     "subscribe",
                                                     [&]() -> boost::asio::awaitable<void> {
                                                         co_await client->async_subscribe(
                                                                 async_mqtt::v3_1_1::subscribe_packet{
                                                                         *client->acquire_unique_packet_id(),
                                                                         async_mqtt::force_move(sub_entry)},
                                                                 boost::asio::use_awaitable);
                                                     });
                SPDLOG_INFO("MQTT话题订阅成功 peer={} topic_count={}", peer, 25);
                // 订阅完成即视为已连接，唤醒业务层上行发布
                connection_state.store(ConnectionState::Connected, std::memory_order_release);

                // 检查在订阅过程中是否已经更新了客户端ID
                if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                    connection_state.store(ConnectionState::Connecting, std::memory_order_release);
                    SPDLOG_INFO("MQTT订阅后检测到参数变更，主动断开 peer={}", peer);
                    co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{}, boost::asio::use_awaitable);
                    co_await client->async_close(boost::asio::use_awaitable);
                    continue;
                }

                // 接收信息
                // 持续接收下行报文，直到停止、client_id 变更或 endpoint 变更
                while (is_running.load(std::memory_order_relaxed)
                       && !is_client_id_updated.load(std::memory_order_relaxed)
                       && !is_endpoint_updated()) {
                    if (std::optional<async_mqtt::packet_variant> pv_opt = co_await client->async_recv(boost::asio::use_awaitable)) {
                        // 对收到的报文按类型分发处理
                        pv_opt->visit(
                                async_mqtt::overload{
                                        [this, &peer](async_mqtt::v3_1_1::publish_packet const &p) {
                                            // 接收到对方发送的信息
                                            const std::string topic = p.topic();
                                            const std::string payload = p.payload();
                                            // 记录下行原始报文（rx）供抓包/复盘
                                            RoboGo::logging::log_network_payload("mqtt",
                                                                                 "rx",
                                                                                 topic,
                                                                                 payload,
                                                                                 peer);
                                            // 交给业务层回调解析
                                            on_receive_message(topic, payload);
                                        },
                                        [&](async_mqtt::v3_1_1::puback_packet const &packet) {
                                            // 对方接收到信息的回应
                                            SPDLOG_DEBUG("收到MQTT PUBACK peer={} packet_id={}", peer, packet.packet_id());
                                        },
                                        [&](auto const &) {
                                            // 其它非业务类型报文（如 PINGRESP）仅追踪
                                            SPDLOG_TRACE("收到非业务MQTT数据包 peer={}", peer);
                                        }});
                    }
                }

                // 发送断开连接请求
                SPDLOG_INFO("MQTT开始主动断开连接 peer={}", peer);
                co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{}, boost::asio::use_awaitable);
                SPDLOG_INFO("MQTT断开连接成功 peer={}", peer);

                // 关闭底层socket，确保状态重置
                co_await client->async_close(boost::asio::use_awaitable);

            } catch (const boost::system::system_error &error) {
                // 网络/协议层异常：记录并将状态置 Failed，准备重连
                SPDLOG_ERROR("MQTT运行异常 peer={} code={} message={}",
                             peer,
                             static_cast<long long>(error.code().value()),
                             error.code().message());
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
            } catch (const std::exception &error) {
                // 其它标准异常同样处理为重连
                SPDLOG_ERROR("MQTT运行异常 peer={} message={}", peer, error.what());
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
            }

            {
                std::lock_guard<std::mutex> lock(client_mutex);
                active_client.reset();
            }
            SPDLOG_DEBUG("MQTT活动连接已清理 peer={}", peer);

            if (need_reconnect) {
                // 出现错误，等待 100ms 再重新连接
                SPDLOG_WARN("MQTT将在{}ms后尝试重连 peer={}",
                            std::chrono::duration_cast<std::chrono::milliseconds>(mqtt_reconnect_delay).count(),
                            peer);
                boost::asio::steady_timer timer(executor, mqtt_reconnect_delay);
                auto [error_code] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                if (error_code) {
                    SPDLOG_ERROR("MQTT重连等待失败 peer={} message={}", peer, error_code.message());
                }
            }
        }
        // 循环结束（收到 stop）：复位状态并记录
        connection_state.store(ConnectionState::Idle, std::memory_order_release);
        SPDLOG_INFO("MQTT运行循环已退出");
    }

    // 功能：发布上行消息（线程安全入口）。
    // 思路：未连接则跳过；取 active_client 共享指针副本，在其执行器上 detached 启动 PublishMqttMessage 协程，避免阻塞调用方。
    void MqttClient::publish(const std::string &topic, const std::string &message) {
        std::string host_ip_cloned;
        std::string port_cloned;
        {
            std::lock_guard<std::mutex> lock(endpoint_mutex);
            host_ip_cloned = host_ip;
            port_cloned = port;
        }
        const std::string peer = host_ip_cloned + ":" + port_cloned;
        if (connection_state.load(std::memory_order_acquire) != ConnectionState::Connected) {
            // 仅在已连接态发送，否则跳过（避免向未就绪连接写入）
            SPDLOG_DEBUG("MQTT尚未登录，跳过发送 topic={} bytes={} peer={}", topic, message.size(), peer);
            return;
        }

        // 获取当前活动的 client 对象的共享指针副本
        std::shared_ptr<MqttAsyncClient> client_copy;
        {
            std::lock_guard<std::mutex> lock(client_mutex);
            if (active_client) {
                client_copy = active_client;
            }
        }

        if (client_copy) {
            // 在 client 执行器上异步发送，detached 自管理
            boost::asio::co_spawn(client_copy->get_executor(),
                                  PublishMqttMessage(client_copy, topic, message, peer),
                                  boost::asio::detached);
        } else {
            SPDLOG_WARN("MQTT客户端未连接，跳过发送 topic={} bytes={} peer={}", topic, message.size(), peer);
        }
    }

}// namespace RoboGo::io
