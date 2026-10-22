#pragma once

#include <pch.h>

namespace RoboGo::io {

    // MQTT 客户端：封装 async_mqtt 异步连接裁判系统 broker，支持动态修改服务端地址/客户端ID、订阅下行话题、
    // 发布上行消息，并通过回调把下行报文交给业务层；连接状态以原子量对外暴露。
    class MqttClient {
    public:
        // 连接状态枚举：Idle(空闲未连) / Connecting(连接中) / Connected(已连接) / Failed(失败待重连)
        enum class ConnectionState {
            Idle,
            Connecting,
            Connected,
            Failed,
        };

    private:
        mutable std::mutex endpoint_mutex;  // 保护 host_ip / port 读写的互斥锁
        std::string host_ip;  // 服务端 IP 地址
        std::string port;  // 服务端端口号
        std::atomic<std::uint64_t> endpoint_revision = 0;  // endpoint 修订号，每次改动 +1 供 run 循环检测是否变更
        std::function<void(const std::string &topic, const std::string &payload)> on_receive_message;  // 下行消息回调，交给业务层解析
        std::atomic<bool> is_running = true;  // 运行标志，stop() 置 false 以退出运行循环
        mutable std::mutex client_id_mutex;  // 保护 client_id 读写的互斥锁
        std::string client_id;  // MQTT 客户端 ID（裁判系统用以区分客户端）
        std::atomic<bool> is_client_id_updated = true;  // client_id 是否被更新，用于连接前/中检测参数变化
        std::atomic<ConnectionState> connection_state = ConnectionState::Idle;  // 当前连接状态（原子量，供外部无锁查询）
        // 用于管理当前活动的 client 对象
        std::mutex client_mutex;  // 保护 active_client 读写的互斥锁
        std::shared_ptr<async_mqtt::client<async_mqtt::protocol_version::v3_1_1, async_mqtt::protocol::mqtt>> active_client;  // 当前活动连接，空表示未连接

    public:
        explicit MqttClient(boost::asio::io_context &io_context,
                            std::string host_ip,
                            std::string port,
                            const std::function<void(const std::string &topic, const std::string &payload)> &on_receive_message);

        // 动态更新服务端地址（host:port），仅在确实变化时才递增修订号并触发重连
        void set_server_endpoint(std::string host_ip, std::string port);

        // 获取当前服务端地址副本，线程安全
        [[nodiscard]] std::pair<std::string, std::string> get_server_endpoint() const;

        // 设置客户端 ID，空则回到 Idle，非空则进入 Connecting 以触发连接
        void set_client_id(std::string client_id);

        // 获取当前连接状态，无锁原子读取
        [[nodiscard]] ConnectionState get_connection_state() const;

        // 获取当前客户端 ID 副本，线程安全
        [[nodiscard]] std::string get_client_id() const;

        // 停止运行循环并复位状态
        void stop();

    private:
        boost::asio::awaitable<void> run();  // 连接/订阅/收消息的主协程循环

    public:
        // 发布一条上行消息（topic + 负载），未连接时跳过
        void publish(const std::string &topic, const std::string &message);
    };


}// namespace RoboGo::io
