// ============================================================================
// 文件：src/io/mqtt_client.h
// 层级：业务功能层 - 网络 IO（RoboGo 后台服务 BackgroundServices 组件之一）
// 职责：声明 MqttClient 类，封装与 RoboMaster 2026 裁判系统 MQTT broker 的异步连接。
//       - 对外暴露 set_server_endpoint / set_client_id 等接口，支持运行时动态修改服务端地址与客户端ID；
//       - 内部以协程（Boost.Asio + async_mqtt）维护「连接→订阅→接收下行→自动重连」主循环（实现见 .cpp）；
//       - 下行报文通过 on_receive_message 回调交给业务层（Protobuf 解析）；上行调用 publish() 发送；
//       - 连接状态以原子量 connection_state 暴露，供 UI 无锁查询。
// 依赖：async_mqtt（MQTT 3.1.1 异步客户端）、Boost.Asio（协程执行器）、spdlog（日志）。
// 注意：本文件只声明接口；topic 订阅列表、超时/重连常量与协程实现均位于 mqtt_client.cpp。
// ============================================================================
// mqtt_client.h：MQTT 客户端接口——封装与裁判系统的异步连接、上行发布与下行消息回调，并以原子量对外暴露连接状态。
#pragma once

#include <pch.h>

namespace RoboGo::io {

    // 功能：RoboGo 与 RoboMaster 2026 裁判系统之间的 MQTT 异步客户端封装。
    // 思路：连接/订阅/收包/自动重连全部跑在单个 Asio 协程 run() 中（detached 自管理生命周期）；
    //       通过原子修订号(endpoint_revision)与标志(is_client_id_updated)配合互斥锁，实现「主线程改参数、
    //       IO 协程感知并重建连接」的安全解耦；下行报文经 on_receive_message 回调交给业务层解析 Protobuf，
    //       上行消息由 publish() 异步发出；连接状态用原子量 connection_state 暴露供 UI 无锁查询。
    // 参数：构造时传入 io_context（共用 IO 线程）、host/port（broker 地址）、on_receive_message（下行回调）。
    // 返回值：无（类类型）；运行状态通过 get_connection_state() 等原子接口读取。
    class MqttClient {
    public:
        // 功能：描述客户端与 broker 的连接生命周期状态，供 UI 与调用方无锁查询。
        // 思路：用 enum class 避免命名污染；状态机 Idle→Connecting→Connected，异常或参数变更→Failed，
        //       由 run() 协程与 set_* 接口原子地写、get_connection_state() 原子地读，读端无需加锁。
        // 取值：Idle=未配置/已停止；Connecting=正在建连或重连；Connected=已连接且完成订阅；Failed=出错待重连。
        enum class ConnectionState {
            Idle,       // 空闲：尚未设置 client_id，或已被置空，不尝试连接
            Connecting, // 连接中：已设置 client_id，协程正在建立 TCP/MQTT 连接
            Connected,  // 已连接：连接建立且已完成订阅，可正常收发消息
            Failed,     // 连接失败：连接或订阅出错，等待下一次重试（自动重连）
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
        // 功能：构造 MQTT 客户端——记录服务端地址与收消息回调，并启动后台运行协程。
        // 思路：仅保存参数与回调，不立即连接；在 io_context 上 co_spawn 启动 run() 协程（detached），
        //       由 run() 自身根据 client_id 是否已设置决定是否发起连接。
        // 参数 io_context：共用的 Asio 执行上下文；host_ip/port：裁判系统 MQTT 地址（默认 192.168.12.1:3333）；
        //      on_receive_message：收到下行消息时的回调（话题 + 负载），由上层解析 protobuf。
        // 返回值：无。
        // 说明：此处只保存参数不立即连接，需后续调用 set_client_id() 才会真正发起连接。
        explicit MqttClient(boost::asio::io_context &io_context,
                            std::string host_ip,
                            std::string port,
                            const std::function<void(const std::string &topic, const std::string &payload)> &on_receive_message);

        // 功能：动态更新服务端地址（host:port）。
        // 思路：空地址直接忽略；加锁比较新旧地址，仅当确实变化时递增 endpoint_revision（供 run() 检测变更并重建连接），
        //       并按是否已有 client_id 置 Connecting/Idle，从而触发或暂缓连接。
        // 参数 host_ip/port：新的 broker 地址；为空则被忽略不生效。
        // 返回值：无。
        void set_server_endpoint(std::string host_ip, std::string port);

        // 功能：获取当前服务端地址（线程安全副本）。
        // 思路：加锁拷贝 host_ip/port 后返回，避免调用方读到半更新状态。
        // 参数：无。
        // 返回值：std::pair<host, port> 的字符串副本。
        [[nodiscard]] std::pair<std::string, std::string> get_server_endpoint() const;

        // 功能：设置客户端 ID（裁判系统据此区分客户端）。
        // 思路：保存 ID 后按非空/空置 Connecting/Idle；并置 is_client_id_updated=true，使正在进行的连接流程
        //       在下一次「参数变更检查点」感知并主动重建连接，无需外部再触发。
        // 参数 client_id：新客户端 ID；空字符串表示回到 Idle 不连接。
        // 返回值：无。
        void set_client_id(std::string client_id);

        // 功能：获取当前连接状态。
        // 思路：原子读取 connection_state，无需加锁，可安全在 UI 线程高频调用。
        // 参数：无。
        // 返回值：ConnectionState 枚举（Idle/Connecting/Connected/Failed）。
        [[nodiscard]] ConnectionState get_connection_state() const;

        // 功能：获取当前客户端 ID（线程安全副本）。
        // 思路：加锁拷贝后返回，避免与 set_client_id / run() 并发读写竞争。
        // 参数：无。
        // 返回值：当前 client_id 字符串副本（可能为空）。
        [[nodiscard]] std::string get_client_id() const;

        // 功能：请求停止客户端并复位状态。
        // 思路：置 is_running=false（run() 循环据此退出），并把连接状态复位为 Idle；
        //       run() 退出时会关闭底层 socket，回收资源。
        // 参数：无。
        // 返回值：无。
        void stop();

    private:
        // 功能：连接/订阅/收消息的主协程循环（核心异步状态机）。
        // 思路：在 is_running 下反复循环——每次取 endpoint 快照，若地址/ID 变化或连接失败则重建 client 对象；
        //       依次完成 底层握手→connect→subscribe→recv 循环；在握手/连接/订阅各阶段后都检查参数是否变更，
        //       变更则关闭并重连；任意异常捕获后置 Failed 并等待 mqtt_reconnect_delay 重连。所有网络操作用
        //       RunMqttOperationWithTimeout 包裹超时，避免永久挂死。
        // 参数：无（使用类成员）。
        // 返回值：awaitable<void>，由 co_spawn(detached) 驱动，正常随 stop() 退出。
        boost::asio::awaitable<void> run();  // 连接/订阅/收消息的主协程循环

    public:
        // 功能：发布一条上行消息（topic + 负载）。
        // 思路：仅当状态为 Connected 才发送；取 active_client 共享指针副本，在其执行器上 detached 启动
        //       PublishMqttMessage 协程异步发送，从而不阻塞调用方（多为 UI/业务线程）。
        // 参数 topic：目标话题（如 "CustomByteBlock"）；message：已序列化的 Protobuf 负载字节串。
        // 返回值：无。
        void publish(const std::string &topic, const std::string &message);
    };


}// namespace RoboGo::io
