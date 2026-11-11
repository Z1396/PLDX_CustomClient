// 实现 MqttClient 的构造、参数设置与协程主循环 run()，以及与 broker 交互的若干辅助协程：
// WaitForMqttOperationTimeout（超时定时器）、RunMqttOperationWithTimeout（超时包裹器）、
// PublishMqttMessage（上行发布）。
// 异步模型：所有网络操作均为 async_mqtt 的 awaitable，跑在共用的 Boost.Asio io_context 上；
// run() 以 detached 协程常驻，内部「连接→订阅→recv 循环→异常重连」，并通过
// 原子修订号(endpoint_revision)与标志(is_client_id_updated)感知主线程的参数变更并重建连接。
// 依赖：async_mqtt、Boost.Asio 协程、spdlog、RoboGo::logging（网络抓包）。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <async_mqtt/all.hpp>
// [/pch-split]
#include "mqtt_client.h"
// 本编译单元的主头文件(声明 MqttClient)
#include "logging/logging.h"  // 网络报文抓包接口(log_network_payload)
#include "mqtt/mqtt_router.h"   // AllTopics()：订阅清单的唯一来源  // 网络报文抓包接口(log_network_payload)

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

    /**
     * @brief 发布一条 MQTT 消息（QoS1，至少一次）。
     * @note 思路：先取得唯一 packet_id（会话未就绪则跳过），按 network_capture 格式记录发送负载，再用超时包裹的 publish 发出；捕获 system_error
     *        仅记录错误，不向上抛（发布失败不应中断主循环）。参数 client：目标连接（共享指针，跨协程保活）；topic：上行话题；message：已序列化负载；peer：日志用的“地址:端口”。
     * @return awaitable<void>；成功或会话未就绪/失败都正常返回，不向外抛异常。
     */
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
            }
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
            }
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
        }
    }

}  // namespace

namespace RoboGo::io {  // 进入网络 IO 命名空间

// [run-split] 单轮连接所需的共享状态。原来 run() 每轮重新声明的局部变量集中到这里；
// waiting_for_client_id_logged 跨轮保留（ResetRound 不清它），其余字段每轮重置。
struct RoundContext {
    boost::asio::any_io_executor executor;                 // 整轮复用，对应原 run() 开头的 executor
    std::uint64_t endpoint_revision_snapshot = 0;         // 本轮参数基线
    std::string host_ip_cloned;                           // 本轮地址副本
    std::string port_cloned;                              // 本轮端口副本
    std::string peer;                                     // "host:port"，仅供日志
    std::string client_id_cloned;                         // 本轮 client_id 副本
    std::shared_ptr<async_mqtt::client<async_mqtt::protocol_version::v3_1_1,
                                       async_mqtt::protocol::mqtt>> client;  // 本轮连接对象
    bool need_reconnect = false;                          // 是否需要在清理后走 100ms 退避
    bool waiting_for_client_id_logged = false;            // 跨轮保留：等待登录提示只打一次

    void ResetRound() {                                   // 等价于原来每轮重新声明局部变量
        endpoint_revision_snapshot = 0;                      // 本轮参数基线
        host_ip_cloned.clear();                                 // 本轮地址副本
        port_cloned.clear();                                  // 本轮端口副本
        peer.clear();                                           // "host:port"，仅供日志
        client_id_cloned.clear();                             // 本轮 client_id 副本    
        client.reset();                                         // 本轮连接对象重置
        need_reconnect = false;                                // 本轮是否需要重连
    }
};

    /**
     * @brief 构造 MQTT 客户端并启动后台连接协程。
     * @note 思路：保存 host/port 与下行回调，co_spawn 启动 run() 协程（detached 自管理生命周期）。
     */
    MqttClient::MqttClient(boost::asio::io_context &io_context,  // 共用 IO 上下文
                           std::string host_ip,  // broker 地址
                           std::string port,  // broker 端口
                           const std::function<void(const std::string &topic, const std::string &payload)> &on_receive_message)  // 下行回调
        : host_ip(std::move(host_ip)),  // 移动构造，避免多一次字符串深拷贝
          port(std::move(port)),  // 同上
          on_receive_message(on_receive_message) {  // 拷贝 std::function(可能已捕获 this，需保证生命周期长于本对象)
        SPDLOG_INFO("创建MQTT客户端 host={} port={}", this->host_ip, this->port);  // 显式 this-> 以区分入参与同名成员
        // 在 io_context 上分离式启动连接主循环
        /*1. 第一个参数：io_context
        协程的调度器，协程里面所有 `co_await`（async_read/async_connect/async_write）都跑在这个 io_context 事件循环上。
        2. 第二个参数：run ()
        这是一个asio 可等待协程函数，签名一般长这样：
        asio::awaitable<void> run()
        {
            // 里面 co_await 异步操作，比如 connect、读写socket
            co_return;
        }
        > 注意：这里写 `run()` 是调用这个协程函数，拿到 awaitable 对象传给 co_spawn。
        3. 第三个参数：boost::asio::detached
        这是重点。
        - 普通 co_spawn 你可以传一个回调 / `use_future`，用来等协程结束、拿结果、抓异常；
        - `detached` = 我不管这个协程了，启动之后直接撒手不管**
        ✅ 协程自持（self-owned），协程内部生命周期自己管理，不需要外部持有句柄
        ✅ `co_spawn` 调用**立刻返回**，不会阻塞在这里等连接 / 等 run 跑完
        ❗ 坑点：**协程里面抛异常没人捕获，程序直接崩溃！**
        > detached 的语义就是：放弃接收协程的完成通知和异常。
         */
        boost::asio::co_spawn(io_context, run(), boost::asio::detached);  // detached：协程自持，构造立即返回，不等连接结果
    }

    /**
     * @brief 动态更新服务端地址。
     * @note 思路：空地址忽略；加锁比较新旧地址，仅在确实变化时记录并递增 endpoint_revision 触发 run 循环重建连接。
     */
    void MqttClient::set_server_endpoint(std::string host_ip, std::string port) {  // 可在任意线程调用(如 UI 修改配置)
        if (host_ip.empty() || port.empty()) {  // 空值视为无效输入
            SPDLOG_WARN("忽略空的MQTT服务端地址 host={} port={}", host_ip, port);  // 打印被忽略的入参，便于排查配置错误
            return;  // 直接返回，不修改现有配置
        }

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
            }
        }  // 作用域结束，锁释放

        if (!endpoint_changed) {  // 地址没变就不需要任何后续动作
            // 地址未变则无需任何动作
            return;  // 提前返回，避免无谓的修订号自增导致 run() 重连
        }

        // 修订号 +1，run 循环通过对比快照检测变更并重建连接
        endpoint_revision.fetch_add(1, std::memory_order_acq_rel);  // acq_rel：保证地址写入先于修订号可见
        bool has_client_id = false;  // 是否已配置 client_id
        {  // 单独作用域，只保护 client_id 读取
            std::lock_guard<std::mutex> lock(client_id_mutex);  // 加锁拷贝，避免读到半更新状态
            has_client_id = !client_id.empty();  // 非空即视为已配置
        }
        // 有 client_id 才进入 Connecting 触发连接；否则停留在 Idle 等待 client_id
        connection_state.store(has_client_id ? ConnectionState::Connecting : ConnectionState::Idle, std::memory_order_release);  // release：使上面的地址修改对读端可见
    }

    /**
     * @brief 获取当前服务端地址（线程安全副本）。
     * @note 思路：加锁拷贝，返回 pair<host, port>。
     */
    std::pair<std::string, std::string> MqttClient::get_server_endpoint() const {  // const：logical const，仅读成员
        std::lock_guard<std::mutex> lock(endpoint_mutex);  // endpoint_mutex 为 mutable，const 函数中也可加锁
        return {host_ip, port};  // 返回值拷贝，调用方拿到后不受后台修改影响
    }

    /**
     * @brief 设置客户端 ID。
     * @note 思路：保存 ID 后，有 ID 则置 Connecting 触发连接；同时置 is_client_id_updated=true 供 run 循环在连接前/中检测并重建。
     */
    void MqttClient::set_client_id(std::string client_id) {  // 传入值语义，内部移动保存
        SPDLOG_INFO("更新MQTT客户端ID old={} new={}", this->client_id, client_id);  // 打印新旧 ID(注意：此处读 this->client_id 未加锁，仅用于日志)
        bool has_client_id = false;  // 新 ID 是否非空
        {  // 只在写成员时持锁
            std::lock_guard<std::mutex> lock(client_id_mutex);  // 与 run() 中的读取互斥
            this->client_id = std::move(client_id);  // 移动保存，减少拷贝
            has_client_id = !this->client_id.empty();  // 以保存后的值为准
        }
        connection_state.store(has_client_id ? ConnectionState::Connecting : ConnectionState::Idle, std::memory_order_release);  // 有 ID 则驱动 run() 去连接，无 ID 则回空闲
        // 标记 client_id 已更新，使正在进行的连接流程感知并重启
        is_client_id_updated.store(true, std::memory_order_release);  // 与修订号等价的"参数已变"信号，run() 在每个检查点读取
    }

    /**
     * @brief 获取当前连接状态。
     * @note 思路：原子读取，无需加锁。
     */
    MqttClient::ConnectionState MqttClient::get_connection_state() const {  // 高频调用(UI 每帧)
        return connection_state.load(std::memory_order_acquire);  // acquire：与写入端的 release 配对，保证看到完整状态
    }

    /**
     * @brief 获取当前客户端 ID 副本（线程安全）。
     * @note 思路：加锁拷贝返回。
     */
    std::string MqttClient::get_client_id() const {  // 返回副本而非引用
        std::lock_guard<std::mutex> lock(client_id_mutex);  // 加锁避免与 set_client_id 数据竞争
        return client_id;  // 拷贝返回
    }

    /**
     * @brief 请求停止客户端。
     * @note 思路：置 is_running=false，并把状态复位为 Idle；run 循环检测到后退出，socket 会被关闭。
     */
    void MqttClient::stop() {  // 析构前由 BackgroundServices::Stop() 调用
        const auto [host, service_port] = get_server_endpoint();  // 结构化绑定取出 host/port，仅用于日志
        SPDLOG_INFO("收到MQTT停止请求 host={} port={}", host, service_port);  // 记录停止时的目标地址
        is_running.store(false, std::memory_order_relaxed);  // relaxed 足够：仅作循环条件，不承担跨变量同步
        connection_state.store(ConnectionState::Idle, std::memory_order_release);  // 对外立即呈现"空闲"，避免 UI 仍显示已连接
    }

    /**
     * @brief 连接/订阅/收消息的主协程循环（核心异步状态机）。
     * @note 思路：在 is_running 下循环：每次取 endpoint 快照，若地址/ID 变化或连接失败则重建 client；依次完成底层握手→connect→subscribe→循环
     *        recv；任意阶段检测到参数变更或异常则关闭并重连。
     * @note 思路：只负责「取执行器 → 循环 → 三段调用」，具体一轮的全部逻辑见 ConnectOnce / ReceiveLoop / CleanupAndBackoff。
     * @return awaitable<void>，由 co_spawn(detached) 驱动，正常随 stop() 退出。
     */
    boost::asio::awaitable<void> MqttClient::run() {  
        // 由构造函数 co_spawn(detached) 启动，全程在 IO 线程执行
        RoundContext round;
        // 获取当前协程所绑定的 Asio 执行器,协程跑，直到碰到 `co_await xxx`：协程暂停，注册异步 IO 等待事件，把控制权还给 io_context；
        // 所以：这个 executor 是 io_context 的代理，不是 io_context 本身。
        round.executor = co_await boost::asio::this_coro::executor;  

        // 外层循环：只要仍在运行就不断尝试连接、收消息、重连
        // stop() 置 false 后，下一轮不再进入
        while (is_running.load(std::memory_order_relaxed)) {  
            round.ResetRound();  // 等价于原来每轮重新声明局部变量；等待登录提示跨轮保留
            co_await ConnectOnce(round);  // 一整轮；内部的 co_return 等价于原来的 continue
        }
        // 循环结束（收到 stop）：复位状态并记录
        connection_state.store(ConnectionState::Idle, std::memory_order_release);
        SPDLOG_INFO("MQTT运行循环已退出");  // 协程正常返回
    }


    /**
     * @brief 完成一轮「取参数快照 → 建连接 → 收包 → 断开 → 清理」。
     * @note 思路：原 run() 的循环体原样提取。所有 continue 改为 co_return（同样跳过清理与退避）；只有正常走完整轮或落入 catch 时才会调用 CleanupAndBackoff。
     */
    boost::asio::awaitable<void> MqttClient::ConnectOnce(RoundContext &round) {
        // 把 round 字段绑定为局部引用，使下面的逻辑与提取前逐句一致。
        auto &executor = round.executor;  // 引用 io_context 的代理，不是 io_context 本身。
        std::uint64_t &endpoint_revision_snapshot = round.endpoint_revision_snapshot;// 记录本轮 endpoint 修订号快照，用于后续"是否发生变化"的快速比对
        std::string &host_ip_cloned = round.host_ip_cloned;// 记录本轮 host_ip，用于后续"是否发生变化"的快速比对
        std::string &port_cloned = round.port_cloned;// 记录本轮 port，用于后续"是否发生变化"的快速比对
        std::string &peer = round.peer;// 记录本轮 peer，用于后续日志复用
        std::string &client_id_cloned = round.client_id_cloned;// 记录本轮 client_id，用于后续"是否发生变化"的快速比对
        auto &client = round.client;// 引用当前连接的 client 对象，用于后续操作
        bool &need_reconnect = round.need_reconnect;// 记录本轮是否需要退避后重连
        bool &waiting_for_client_id_logged = round.waiting_for_client_id_logged;// 记录本轮是否等待 client_id 登录

        // 记录本轮 endpoint 修订号快照，用于后续"是否发生变化"的快速比对
        endpoint_revision_snapshot = endpoint_revision.load(std::memory_order_acquire);
        {  // 加锁拷贝配置，避免持锁跨越 co_await
            std::lock_guard<std::mutex> lock(endpoint_mutex);
            host_ip_cloned = host_ip;
            port_cloned = port;
        }
        peer = host_ip_cloned + ":" + port_cloned;  // 预拼一次，后续日志复用
        const auto is_endpoint_updated = [&]() {  // 按引用捕获快照，调用开销仅一次原子读
            return endpoint_revision.load(std::memory_order_acquire) != endpoint_revision_snapshot;
        };
        need_reconnect = false;  // 本轮是否需要退避后重连
        try {  // 包住整轮"握手-连接-订阅-收包"，任何系统错误都转为重连
            if (host_ip_cloned.empty() || port_cloned.empty()) {  // 尚未配置服务端地址
                connection_state.store(ConnectionState::Idle, std::memory_order_release);
                SPDLOG_WARN("MQTT服务端地址为空，等待后重试");
                boost::asio::steady_timer timer(executor, std::chrono::milliseconds(100));  // 100ms 轮询间隔
                co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                co_return;  // 原 continue：回到 run() 的 while 顶部重新取配置
            }

            // 每次连接都创建新的 client 对象
            client = std::make_shared<MqttAsyncClient>(executor);  // 共享指针：便于 publish() 跨协程保活
            client->set_pingresp_recv_timeout(mqtt_operation_timeout);  // 心跳无响应超过 5s 即判定链路断开
            {  // 发布"当前活动连接"给其它线程使用
                std::lock_guard<std::mutex> lock(client_mutex);
                active_client = client;  // publish() 据此发送
            }

            is_client_id_updated.store(false, std::memory_order_relaxed);  // 清除上一轮的"ID 已更新"标记
            {  // 加锁读取 client_id
                std::lock_guard<std::mutex> lock(client_id_mutex);
                client_id_cloned = client_id;
            }
            if (client_id_cloned.empty()) {  // 没有 client_id 就无法 CONNECT
                connection_state.store(ConnectionState::Idle, std::memory_order_release);
                if (!waiting_for_client_id_logged) {  // 只在进入等待态时提示一次，避免 10Hz 刷屏
                    SPDLOG_INFO("MQTT 尚未设置客户端 ID，等待登录（本提示只出现一次）");
                    waiting_for_client_id_logged = true;
                }
                boost::asio::steady_timer timer(executor, std::chrono::milliseconds(100));
                co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
                co_return;  // 原 continue
            }
            waiting_for_client_id_logged = false;  // 已经有 ID：下次再回到等待态时重新提示一次
            if (connection_state.load(std::memory_order_acquire) != ConnectionState::Failed) {
                connection_state.store(ConnectionState::Connecting, std::memory_order_release);
            }

            if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                SPDLOG_DEBUG("MQTT参数在连接前已更新，跳过本次连接 peer={}", peer);
                co_return;  // 原 continue：丢弃刚创建的 client，用新参数重建
            }

            // 底层网络协议握手
            SPDLOG_INFO("MQTT开始底层网络协议握手 peer={} client_id={}", peer, client_id_cloned);
            auto [handshake_ec] = co_await client->async_underlying_handshake(
                    host_ip_cloned, port_cloned,
                    boost::asio::as_tuple(boost::asio::use_awaitable));
            if (handshake_ec) {
                SPDLOG_WARN("MQTT底层握手失败 peer={} code={} message={}",
                            peer, static_cast<long long>(handshake_ec.value()), handshake_ec.message());
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
                co_return;  // 原 continue（注意：跳过清理与退避，与原实现一致）
            }
            SPDLOG_INFO("MQTT底层网络协议握手成功 peer={} client_id={}", peer, client_id_cloned);

            if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                SPDLOG_INFO("MQTT握手完成后检测到参数变更，关闭当前连接 peer={}", peer);
                // 先断开连接，再关闭 socket
                // async_close 是非同步阻塞的，不会阻塞当前协程
                // boost::asio::use_awaitable 是为了返回 awaitable<void> 类型的 awaitable 对象
                // 而不是 void 类型的返回值
                // 这样才能在 co_await 后面继续执行后续代码
                // 而不是阻塞等待 close 操作完成
                co_await client->async_close(boost::asio::as_tuple(boost::asio::use_awaitable));
                co_return;  // 原 continue
            }

            // 发送连接请求并接收回复
            SPDLOG_INFO("MQTT开始连接 peer={} client_id={}", peer, client_id_cloned);
            auto [start_ec, connack_opt] = co_await client->async_start(
                    async_mqtt::v3_1_1::connect_packet{
                            true,  // clean_session
                            mqtt_keep_alive_seconds,
                            std::move(client_id_cloned)},
                    boost::asio::as_tuple(boost::asio::use_awaitable));
            if (start_ec) {
                SPDLOG_WARN("MQTT连接失败 peer={} code={} message={}",
                            peer, static_cast<long long>(start_ec.value()), start_ec.message());
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
                co_return;  // 原 continue
            }
            SPDLOG_INFO("MQTT连接成功 peer={}", peer);

            if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                SPDLOG_INFO("MQTT连接后检测到参数变更，主动断开 peer={}", peer);
                //async_disconnect 是非同步阻塞的，不会阻塞当前协程;作用是发送 DISCONNECT 报文，等待服务器响应
                co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{},
                                                  boost::asio::as_tuple(boost::asio::use_awaitable));
                // async_close 是非同步阻塞的，不会阻塞当前协程;作用是关闭 socket 连接
                co_await client->async_close(boost::asio::as_tuple(boost::asio::use_awaitable));
                co_return;  // 原 continue
            }

            // 订阅：清单从 MQTT 分发表派生，避免与分发表各维护一份
            SPDLOG_INFO("MQTT开始订阅话题 peer={}", peer);
            const auto topics = RoboGo::mqtt::AllTopics();
            std::vector<async_mqtt::topic_subopts> sub_entry;
            sub_entry.reserve(topics.size());
            for (const auto &entry : topics) {
                sub_entry.emplace_back(std::string(entry.topic), async_mqtt::qos::at_most_once);
            }
            SPDLOG_INFO("MQTT订阅话题数={}", sub_entry.size());
            const auto subscribe_packet_id = client->acquire_unique_packet_id();
            if (!subscribe_packet_id.has_value()) {
                SPDLOG_WARN("MQTT订阅失败：无可用报文ID peer={}", peer);
                need_reconnect = true;
                co_return;  // 原 continue
            }
            auto [subscribe_ec, suback_opt] = co_await client->async_subscribe(
                    async_mqtt::v3_1_1::subscribe_packet{
                            *subscribe_packet_id,
                            async_mqtt::force_move(sub_entry)},
                    boost::asio::as_tuple(boost::asio::use_awaitable));
            if (subscribe_ec) {
                SPDLOG_WARN("MQTT订阅失败 peer={} code={} message={}",
                            peer, static_cast<long long>(subscribe_ec.value()), subscribe_ec.message());
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
                co_return;  // 原 continue
            }
            SPDLOG_INFO("MQTT话题订阅成功 peer={} topic_count={}", peer, 23);
            connection_state.store(ConnectionState::Connected, std::memory_order_release);  // publish() 从这一刻起才真正发送

            // 订阅后检测到参数变更，主动断开连接
            if (is_client_id_updated.load(std::memory_order_relaxed) || is_endpoint_updated()) {
                connection_state.store(ConnectionState::Connecting, std::memory_order_release);
                SPDLOG_INFO("MQTT订阅后检测到参数变更，主动断开 peer={}", peer);
                co_await client->async_disconnect(async_mqtt::v3_1_1::disconnect_packet{},
                                                  boost::asio::as_tuple(boost::asio::use_awaitable));
                co_await client->async_close(boost::asio::as_tuple(boost::asio::use_awaitable));
                co_return;  // 原 continue
            }

            // 接收信息：内层收包循环整体提取到 ReceiveLoop
            co_await ReceiveLoop(round);

            // 发送断开连接请求（同样用 as_tuple：断线后 DISCONNECT 必然失败，不能让异常再走展开路径）
            SPDLOG_INFO("MQTT开始主动断开连接 peer={}", peer);
            {
                auto [disconnect_ec] = co_await client->async_disconnect(
                        async_mqtt::v3_1_1::disconnect_packet{},
                        boost::asio::as_tuple(boost::asio::use_awaitable));
                if (disconnect_ec) {  // 对端已断开时这里几乎必然失败，属正常现象
                    SPDLOG_DEBUG("MQTT发送DISCONNECT失败(通常对端已断开) peer={} code={}",
                                 peer, static_cast<long long>(disconnect_ec.value()));
                } else {
                    SPDLOG_INFO("MQTT断开连接成功 peer={}", peer);
                }
            }
            // async_close 的完成签名没有 error_code，因此不取返回值
            co_await client->async_close(boost::asio::as_tuple(boost::asio::use_awaitable));

        } catch (const boost::system::system_error &error) {  // 网络/协议层异常(含超时 timed_out)
            SPDLOG_ERROR("MQTT运行异常 peer={} code={} message={}",
                         peer, static_cast<long long>(error.code().value()), error.code().message());
            if (is_running.load(std::memory_order_relaxed)) {
                connection_state.store(ConnectionState::Failed, std::memory_order_release);
            }
            need_reconnect = true;
        } catch (const std::exception &error) {  // 其它标准异常(如内存分配失败)
            SPDLOG_ERROR("MQTT运行异常 peer={} message={}", peer, error.what());
            if (is_running.load(std::memory_order_relaxed)) {
                connection_state.store(ConnectionState::Failed, std::memory_order_release);
            }
            need_reconnect = true;
        }

        // 原实现中只有非 continue 路径能走到清理与退避
        co_await CleanupAndBackoff(round);
        co_return;
    }

    /**
     * @brief 持续接收下行报文，直到停止、client_id 变更或 endpoint 变更。
     * @note 说明：原 run() 的内层 while 原样搬来；break 只退出本循环，回到 ConnectOnce 继续断开。
     */
    boost::asio::awaitable<void> MqttClient::ReceiveLoop(RoundContext &round) {
        std::string &peer = round.peer;
        auto &client = round.client;
        bool &need_reconnect = round.need_reconnect;

        while (is_running.load(std::memory_order_relaxed)
               && !is_client_id_updated.load(std::memory_order_relaxed)
               && !EndpointChangedSince(round.endpoint_revision_snapshot)) {
            // 关键：这里用 as_tuple 而不是 use_awaitable。
            // 链路断开时 use_awaitable 会抛 boost::system::system_error，
            // 而实测"抛异常 + 栈展开"这条路径在 Windows 上会触发访问冲突（0xC0000005）。
            // 改成 as_tuple 后错误码直接作为返回值，全程不产生异常，断线可以平滑走到重连。
            auto [recv_ec, pv_opt] = co_await client->async_recv(
                    boost::asio::as_tuple(boost::asio::use_awaitable));
            if (recv_ec) {  // 收包失败：对端关闭 / 连接被复位 / 心跳超时
                SPDLOG_WARN("MQTT收包中断 peer={} code={} message={}",
                            peer, static_cast<long long>(recv_ec.value()), recv_ec.message());
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
                break;
            }
            if (!pv_opt.has_value()) {  // 没有错误码但没有报文：对端已正常关闭连接
                SPDLOG_INFO("MQTT连接已被对端关闭 peer={}", peer);
                if (is_running.load(std::memory_order_relaxed)) {
                    connection_state.store(ConnectionState::Failed, std::memory_order_release);
                }
                need_reconnect = true;
                break;
            }
            {  // 有报文才分发
                pv_opt->visit(
                        async_mqtt::overload{
                                // 发布报文
                                [this, &peer](async_mqtt::v3_1_1::publish_packet const &p) {
                                    const std::string topic = p.topic();
                                    const std::string payload = p.payload();
                                    RoboGo::logging::log_network_payload("mqtt", "rx", topic, payload, peer);
                                    on_receive_message(topic, payload);  // 此回调在 IO 线程执行，必须快速返回
                                },
                                // 订阅确认报文
                                [&](async_mqtt::v3_1_1::puback_packet const &packet) {
                                    SPDLOG_DEBUG("收到MQTT PUBACK peer={} packet_id={}", peer, packet.packet_id());
                                },
                                // 其他非业务报文，如心跳、订阅确认等
                                [&](auto const &) {
                                    SPDLOG_TRACE("收到非业务MQTT数据包 peer={}", peer);
                                }});
            }
        }
        co_return;
    }

    /**
     * @brief 清理本轮活动连接，并在出错时做 100ms 退避。
     * @note 说明：原实现里 continue 路径会跳过这一段，因此只由 ConnectOnce 的正常/异常收尾路径调用。
     */
    boost::asio::awaitable<void> MqttClient::CleanupAndBackoff(RoundContext &round) {
        std::string &peer = round.peer;
        {  // 清理本轮活动连接，必须先把共享指针置空
            std::lock_guard<std::mutex> lock(client_mutex);
            active_client.reset();  // 若 publish() 已取走副本，则等其发送完再真正析构
        }
        SPDLOG_DEBUG("MQTT活动连接已清理 peer={}", peer);

        if (round.need_reconnect) {  // 仅在出错时退避，正常参数变更无需等待
            SPDLOG_WARN("MQTT将在{}ms后尝试重连 peer={}",
                        std::chrono::duration_cast<std::chrono::milliseconds>(mqtt_reconnect_delay).count(),
                        peer);
            boost::asio::steady_timer timer(round.executor, mqtt_reconnect_delay);
            auto [error_code] = co_await timer.async_wait(boost::asio::as_tuple(boost::asio::use_awaitable));
            if (error_code) {  // 定时器被取消(如 io_context 停止)
                SPDLOG_ERROR("MQTT重连等待失败 peer={} message={}", peer, error_code.message());
            }
        }
        co_return;
    }

    /**
     * @brief 发布上行消息（线程安全入口）。
     * @note 思路：未连接则跳过；取 active_client 共享指针副本，在其执行器上 detached 启动 PublishMqttMessage 协程，避免阻塞调用方。
     */
    void MqttClient::publish(const std::string &topic, const std::string &message) {  // 可在 UI 线程调用，内部立即返回
        std::string host_ip_cloned;  // 仅用于日志的对端地址副本
        std::string port_cloned;  // 同上
        {  // 加锁拷贝配置
            std::lock_guard<std::mutex> lock(endpoint_mutex);  // 与 set_server_endpoint 互斥
            host_ip_cloned = host_ip;  // 拷贝主机
            port_cloned = port;  // 拷贝端口
        }
        const std::string peer = host_ip_cloned + ":" + port_cloned;  // 预拼日志字符串
        if (connection_state.load(std::memory_order_acquire) != ConnectionState::Connected) {  // 只有已订阅成功才算"可用"
            // 仅在已连接态发送，否则跳过（避免向未就绪连接写入）
            SPDLOG_DEBUG("MQTT尚未登录，跳过发送 topic={} bytes={} peer={}", topic, message.size(), peer);  // DEBUG：未连接时高频调用不刷屏
            return;  // 直接丢弃本次发送
        }

        // 获取当前活动的 client 对象的共享指针副本
        std::shared_ptr<MqttAsyncClient> client_copy;  // 空表示 run() 正在重建/已清理
        {  // 加锁取副本
            std::lock_guard<std::mutex> lock(client_mutex);  // 与 run() 中的 reset 互斥
            if (active_client) {  // 有活动连接才拷贝
                client_copy = active_client;  // 拷贝后引用计数 +1，发送期间不会被析构
            }
        }

        if (client_copy) {  // 成功拿到连接
            // 在 client 执行器上异步发送，detached 自管理
            boost::asio::co_spawn(client_copy->get_executor(),  // 投递到 IO 线程执行，保证与收包在同一执行器
                                  PublishMqttMessage(client_copy, topic, message, peer),  // 参数按值传入，detached 协程自行持有
                                  boost::asio::detached);  // detached：不等结果，异常已在协程内部捕获
        } else {  // 状态是 Connected 但连接对象已被清空(极短的重建窗口)
            SPDLOG_WARN("MQTT客户端未连接，跳过发送 topic={} bytes={} peer={}", topic, message.size(), peer);  // 记录这次丢弃
        }
    }

}  // namespace RoboGo::io

