// logging.cpp：日志模块实现——spdlog 初始化与关闭、日志目录与会话 ID 管理，以及网络报文按自定义二进制格式的抓取与落盘。
#include "logging.h"
#include "embedded_messages_proto.h"
#include "network_capture_format.h"

namespace RoboGo::logging {

// 匿名命名空间：日志模块内部实现细节（抓包线程、队列、编解码与写入），不对外暴露
namespace {

// 单条网络抓包记录的内存表示
struct NetworkCaptureRecord {
    std::uint64_t timestamp_us = 0;  // 采集时刻，微秒时间戳
    std::uint8_t transport = 0;      // 传输类型编码（见 encode_transport）
    std::uint8_t direction = 0;      // 方向编码（见 encode_direction）
    std::string channel;             // 通道名（如 MQTT topic / "video-stream"）
    std::string peer;                // 对端地址（"host:port"）
    std::vector<std::uint8_t> payload;  // 原始报文负载
};

std::once_flag g_initialize_once;  // call_once 保护，保证 initialize 只执行一次
std::filesystem::path g_log_root_directory;  // 日志根目录
std::filesystem::path g_log_directory;        // 本次会话日志目录
std::filesystem::path g_network_capture_path;  // 抓包文件路径
std::string g_session_id;  // 会话 ID
std::ofstream g_network_capture_stream;  // 抓包文件输出流（二进制）
std::thread g_network_capture_thread;   // 后台抓包写线程
constexpr std::size_t kNetworkCaptureQueueSize = 8192;  // 抓包无锁队列容量
moodycamel::BlockingReaderWriterQueue<NetworkCaptureRecord> g_network_capture_queue{kNetworkCaptureQueueSize};  // 采集->写的无锁队列
std::size_t g_records_since_flush = 0;  // 距上次 flush 已写入的记录数
std::atomic<bool> g_network_stop_requested = false;  // 抓包线程停止请求
bool g_is_initialized = false;  // 初始化完成标志

// 抓包文件头魔数（"RBNCAP1\n"），用于识别文件类型与版本
constexpr std::array<char, 8> kCaptureMagic = {'R', 'B', 'N', 'C', 'A', 'P', '1', '\n'};
// 单条记录魔数（"REC2"），用于校验每条记录边界
constexpr std::array<char, 4> kRecordMagic = {'R', 'E', 'C', '2'};
constexpr std::uint32_t kCaptureVersion = 2;  // 抓包文件格式版本
constexpr std::size_t kSpdlogQueueSize = 8192;  // spdlog 异步队列容量
constexpr auto kNetworkCaptureWaitTimeout = std::chrono::milliseconds(1);  // 队列等待超时（用于及时响应停止）
constexpr std::uint8_t kTransportUnknown = 0;  // 未知传输类型
constexpr std::uint8_t kTransportMqtt = 1;     // MQTT
constexpr std::uint8_t kTransportUdpVideo = 2; // UDP 视频
constexpr std::uint8_t kDirectionUnknown = 0;  // 未知方向
constexpr std::uint8_t kDirectionRx = 1;       // 接收
constexpr std::uint8_t kDirectionTx = 2;       // 发送
constexpr std::string_view kDefaultLogRootDirectory = "logs";  // 默认日志根目录（相对程序运行目录）

// 功能：线程安全地把 time_t 转为本地时间的 std::tm。
// 思路：Windows 用 localtime_s，其它平台用 localtime_r（均避免非线程安全的 localtime）。
std::tm local_time(std::time_t time_point) {
    std::tm result{};
#ifdef _WIN32
    localtime_s(&result, &time_point);
#else
    localtime_r(&time_point, &result);
#endif
    return result;
}

// 功能：生成会话 ID（"年月日_时分秒_毫秒"），保证每次启动目录唯一。
// 思路：取系统时钟，拆分为本地日历时间与毫秒尾数，格式化拼接。
std::string make_session_id() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time_point = std::chrono::system_clock::to_time_t(now);
    const std::tm local_tm = local_time(time_point);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::ostringstream stream;
    // 格式化时间部分（年月日_时分秒）
    stream << std::put_time(&local_tm, "%Y%m%d_%H%M%S")
           << "_"
           << std::setw(3)
           << std::setfill('0')
           << milliseconds.count();
    return stream.str();
}

// 功能：解析日志根目录。
// 思路：优先读取环境变量 ROBOGO_LOG_ROOT，未设置或为空则回退默认 "logs"。
std::filesystem::path resolve_log_root_directory() {
    // 优先读取环境变量 ROBOGO_LOG_ROOT
    if (const char *env_value = std::getenv("ROBOGO_LOG_ROOT"); env_value != nullptr && env_value[0] != '\0') {
        return std::filesystem::path(env_value);
    }
    // 回退默认 "logs"
    return std::filesystem::path(kDefaultLogRootDirectory);
}

// 功能：把传输类型字符串编码为单字节枚举值。
std::uint8_t encode_transport(std::string_view transport) {
    if (transport == "mqtt") {
        return kTransportMqtt;
    }
    if (transport == "udp-video") {
        return kTransportUdpVideo;
    }
    return kTransportUnknown;
}

// 功能：把方向字符串编码为单字节枚举值。
std::uint8_t encode_direction(std::string_view direction) {
    if (direction == "rx") {
        return kDirectionRx;
    }
    if (direction == "tx") {
        return kDirectionTx;
    }
    return kDirectionUnknown;
}

// 功能：以“小端”字节序把整数追加到字节缓冲。
// 思路：逐字节取低 8 位并右移，依次 push，实现与平台无关的小端序列化。
template<typename Integer>
void append_little_endian(std::vector<std::uint8_t> &buffer, Integer value) {
    static_assert(std::is_integral_v<Integer>, "Integer type required");

    using UnsignedInteger = std::make_unsigned_t<Integer>;
    UnsignedInteger unsigned_value = static_cast<UnsignedInteger>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        buffer.push_back(static_cast<std::uint8_t>((unsigned_value >> (index * 8)) & 0xFF));
    }
}

// 功能：以“小端”把整数直接写入文件流。
// 思路：先序列化到临时缓冲，再整体 write，减少多次 IO 调用。
template<typename Integer>
void write_little_endian(std::ofstream &stream, Integer value) {
    std::vector<std::uint8_t> buffer;
    buffer.reserve(sizeof(Integer));
    append_little_endian(buffer, value);
    stream.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
}

// 功能：计算数据的 CRC32（IEEE 802.3 多项式，查表法）。
// 思路：首次调用惰性构建 256 项反射表；随后逐字节查表异或，结果取反。
std::uint32_t crc32(std::span<const std::uint8_t> data) {
    // 静态局部变量：仅初始化一次的生成多项式表（反射 0xEDB88320）
    static std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> generated{};
        for (std::uint32_t index = 0; index < generated.size(); ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                if ((value & 1U) != 0U) {
                    value = 0xEDB88320U ^ (value >> 1U);
                } else {
                    value >>= 1U;
                }
            }
            generated[index] = value;
        }
        return generated;
    }();

    std::uint32_t value = 0xFFFFFFFFU;
    for (const std::uint8_t byte : data) {
        value = table[(value ^ byte) & 0xFFU] ^ (value >> 8U);
    }
    return value ^ 0xFFFFFFFFU;
}

// 功能：把一条记录序列化为二进制 body。
// 思路：先对变长字段（channel/peer/payload）做长度截断（uint16/uint32 上限），再按固定顺序以小端写入
//       时间戳、transport、direction、各长度与变长内容；body 之后由 write_record 追加 CRC。
std::vector<std::uint8_t> build_record_body(const NetworkCaptureRecord &record) {
    const auto channel_length = static_cast<std::uint16_t>(std::min<std::size_t>(record.channel.size(), std::numeric_limits<std::uint16_t>::max()));
    const auto peer_length = static_cast<std::uint16_t>(std::min<std::size_t>(record.peer.size(), std::numeric_limits<std::uint16_t>::max()));
    const auto payload_length = static_cast<std::uint32_t>(std::min<std::size_t>(record.payload.size(), std::numeric_limits<std::uint32_t>::max()));

    std::vector<std::uint8_t> body;
    body.reserve(sizeof(std::uint64_t) + sizeof(std::uint8_t) * 2 + sizeof(std::uint16_t) * 2 + sizeof(std::uint32_t)
                 + channel_length + peer_length + payload_length);
    append_little_endian<std::uint64_t>(body, record.timestamp_us);
    body.push_back(record.transport);
    body.push_back(record.direction);
    append_little_endian<std::uint16_t>(body, channel_length);
    append_little_endian<std::uint16_t>(body, peer_length);
    append_little_endian<std::uint32_t>(body, payload_length);
    body.insert(body.end(), record.channel.begin(), record.channel.begin() + static_cast<std::ptrdiff_t>(channel_length));
    body.insert(body.end(), record.peer.begin(), record.peer.begin() + static_cast<std::ptrdiff_t>(peer_length));
    body.insert(body.end(), record.payload.begin(), record.payload.begin() + static_cast<std::ptrdiff_t>(payload_length));
    return body;
}

// 功能：把一条记录按文件格式写入流（魔数 + body 长度 + CRC + body）。
// 思路：由 build_record_body 得到 body，计算长度与 CRC32，依次写入首部字段与正文，供复盘精确还原。
void write_record(std::ofstream &stream, const NetworkCaptureRecord &record) {
    const std::vector<std::uint8_t> body = build_record_body(record);
    const std::uint32_t body_size = static_cast<std::uint32_t>(body.size());
    const std::uint32_t body_crc = crc32(body);

    stream.write(kRecordMagic.data(), static_cast<std::streamsize>(kRecordMagic.size()));
    write_little_endian<std::uint32_t>(stream, body_size);
    write_little_endian<std::uint32_t>(stream, body_crc);
    stream.write(reinterpret_cast<const char *>(body.data()), static_cast<std::streamsize>(body.size()));
}

// 功能：按需 flush 抓包文件。
// 思路：强制 flush 或累计记录达到 32 条时落盘，平衡性能与崩溃丢数。
void flush_capture_stream(bool force_flush) {
    if (!g_network_capture_stream.is_open()) {
        return;
    }
    if (force_flush || g_records_since_flush >= 32) {
        g_network_capture_stream.flush();
        g_records_since_flush = 0;
    }
}

// 功能：后台抓包写线程主循环。
// 思路：带超时地阻塞取队列；收到停止请求且队列空则退出。每取一批记录写盘，并按阈值 flush。
void network_capture_loop() {
    SPDLOG_INFO("网络抓包线程已启动");

    NetworkCaptureRecord record;
    for (;;) {
        // 最多等 1ms，超时则检查停止标志，避免退出延迟
        if (!g_network_capture_queue.wait_dequeue_timed(record, kNetworkCaptureWaitTimeout)) {
            if (g_network_stop_requested.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        // 取出一条即写入并计数
        write_record(g_network_capture_stream, record);
        g_records_since_flush += 1;

        // 一次性尽量清空队列，减少线程切换
        while (g_network_capture_queue.try_dequeue(record)) {
            write_record(g_network_capture_stream, record);
            g_records_since_flush += 1;
        }
        flush_capture_stream(false);
    }

    // 退出前强制落盘并关闭文件
    flush_capture_stream(true);
    if (g_network_capture_stream.is_open()) {
        g_network_capture_stream.close();
    }
    SPDLOG_INFO("网络抓包线程已退出");
}

}// namespace

// 功能：初始化日志与抓包子系统（仅执行一次）。
// 思路：解析根目录与生成会话 ID，建立会话目录；保存当前 proto 定义便于旧日志解析；配置 spdlog 异步双 sink；
//       打开抓包文件并写入文件头（魔数/版本/标志）；启动抓包后台线程。
void initialize(std::string_view application_name, const bool invert_udp_video) {
    //std::call_once表示调用一次，后续调用不会执行
    std::call_once(g_initialize_once, [application_name, invert_udp_video]() {
        // 解析日志根目录
        // 日志根目录：应用安装目录下的 log 子目录
        // 会话 ID：当前时间戳（毫秒级）
        g_log_root_directory = resolve_log_root_directory();
        // 生成会话 ID
        // 会话 ID：当前时间戳（毫秒级）
        // 用于区分不同运行实例的日志文件
        g_session_id = make_session_id();
        // 会话目录：日志根目录下的会话 ID 子目录
        // 用于存储该会话的所有日志文件
        g_log_directory = g_log_root_directory / g_session_id;

        // 创建根目录(就是项目根目录下的logs目录)
        // 用于存储该会话的所有日志文件
        std::filesystem::create_directories(g_log_root_directory);
        // 会话目录(会话 ID(当前时间戳（毫秒级）))
        std::filesystem::create_directories(g_log_directory);

        const auto app_log_path = g_log_directory / "robogo.log";
        g_network_capture_path = g_log_directory / "network_capture.bin";

        // 保存当前 proto 定义到日志目录，用于后续日志分析
        // 即使 proto 文件后续变更，旧日志仍可基于保存的 proto 进行解析
        {
            const auto proto_path = g_log_directory / "messages.proto";
            std::ofstream proto_stream(proto_path, std::ios::binary | std::ios::trunc);
            if (proto_stream.is_open()) {
                // 把内嵌的 proto 文本原样写入，供离线解析历史日志
                proto_stream.write(reinterpret_cast<const char *>(embedded_messages_proto_data),
                                   static_cast<std::streamsize>(embedded_messages_proto_data_len));
                proto_stream.close();
                SPDLOG_INFO("已保存 proto 定义到日志目录 path={} size={}B",
                            proto_path.string(), embedded_messages_proto_data_len);
            } else {
                SPDLOG_WARN("无法写入 proto 文件到日志目录 path={}", proto_path.string());
            }
        }

        // 应用日志文件 sink：纯文件、带级别着色模式
        auto app_file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(app_log_path.string(), true);
        app_file_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");

        std::vector<spdlog::sink_ptr> sinks{app_file_sink};
        // 控制台 sink：带颜色，便于开发期观察
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");
        sinks.push_back(console_sink);

        // 初始化 spdlog 异步线程池（单线程、队列 8192）
        spdlog::init_thread_pool(kSpdlogQueueSize, 1);
        // 构造异步 logger：block 策略在队列满时阻塞调用方，避免丢日志
        auto app_logger = std::make_shared<spdlog::async_logger>(
                "robogo",
                sinks.begin(),
                sinks.end(),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::block);
        app_logger->set_level(spdlog::level::trace);  // 记录全部级别
        app_logger->flush_on(spdlog::level::info);    // 遇到 info 及以上立即 flush
        spdlog::set_default_logger(app_logger);

        // 打开二进制抓包文件（截断），失败则抛异常
        g_network_capture_stream.open(g_network_capture_path, std::ios::binary | std::ios::trunc);
        if (!g_network_capture_stream.is_open()) {
            throw std::runtime_error("无法打开网络抓包文件");
        }
        // 写入文件头：魔数 + 格式版本 + 标志字
        g_network_capture_stream.write(kCaptureMagic.data(), static_cast<std::streamsize>(kCaptureMagic.size()));
        write_little_endian<std::uint32_t>(g_network_capture_stream, kCaptureVersion);
        const std::uint32_t capture_flags = BuildNetworkCaptureFlags(invert_udp_video);
        write_little_endian<std::uint32_t>(g_network_capture_stream, capture_flags);
        g_network_capture_stream.flush();

        // 复位状态并启动抓包线程
        g_network_stop_requested.store(false, std::memory_order_release);
        g_records_since_flush = 0;
        g_network_capture_thread = std::thread(network_capture_loop);
        g_is_initialized = true;

        SPDLOG_INFO("日志系统已初始化 application={} session={} log_root={} log_dir={}",
                    application_name,
                    g_session_id,
                    g_log_root_directory.string(),
                    g_log_directory.string());
        SPDLOG_INFO("应用日志已切换为异步模式 thread_count=1 queue_size={}", kSpdlogQueueSize);
        SPDLOG_INFO("原始网络抓包已启用 path={} format=binary-v2 flags={} invert_udp_video={}",
                    g_network_capture_path.string(),
                    capture_flags,
                    invert_udp_video);
    });
}

// 功能：关闭日志系统。
// 思路：未初始化直接返回；置停止标志并 join 抓包线程（其会落盘关文件），随后关闭 spdlog 并复位标志。
void shutdown() {
    if (!g_is_initialized) {
        return;
    }
    SPDLOG_INFO("日志系统准备关闭 session={}", g_session_id);
    g_network_stop_requested.store(true, std::memory_order_release);
    if (g_network_capture_thread.joinable()) {
        g_network_capture_thread.join();
    }
    spdlog::shutdown();
    g_is_initialized = false;
}

// 功能：返回是否已初始化。
bool is_initialized() {
    return g_is_initialized;
}

// 以下为全局路径/标识的访问器
const std::filesystem::path &log_root_directory() {
    return g_log_root_directory;
}

// 功能：返回本次运行的日志目录（logs/ 下的会话子目录），供外部写附加文件。
const std::filesystem::path &log_directory() {
    return g_log_directory;
}

// 功能：返回网络抓包文件路径（network_capture.bin 等），由网络模块在记录报文时使用。
const std::filesystem::path &network_capture_path() {
    return g_network_capture_path;
}

// 功能：返回本次运行的唯一会话 ID（用于日志目录命名与抓包文件关联）。
const std::string &session_id() {
    return g_session_id;
}

// 功能：把文本截断到 max_length，超出追加 "...(truncated)"。
// 思路：长度不足直接返回副本；否则取前缀并加标记。
std::string truncate_text(std::string_view text, std::size_t max_length) {
    if (text.size() <= max_length) {
        return std::string(text);
    }
    std::string truncated(text.substr(0, max_length));
    truncated += "...(truncated)";
    return truncated;
}

// 功能：把一笔网络报文入队（供抓包线程写盘）。
// 思路：未初始化或已请求停止则跳过；填充记录各字段，尝试入队，队列满则告警丢弃（不阻塞采集方）。
void log_network_payload(std::string_view transport,
                         std::string_view direction,
                         std::string_view channel,
                         std::span<const std::uint8_t> payload,
                         std::string_view peer) {
    if (!g_is_initialized || g_network_stop_requested.load(std::memory_order_acquire)) {
        return;
    }

    NetworkCaptureRecord record;
    const auto timestamp = std::chrono::system_clock::now();
    // 记录微秒级时间戳
    record.timestamp_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(timestamp.time_since_epoch()).count());
    record.transport = encode_transport(transport);
    record.direction = encode_direction(direction);
    record.channel.assign(channel);
    record.peer.assign(peer);
    record.payload.assign(payload.begin(), payload.end());

    // 非阻塞入队；满则丢弃并告警，保护实时业务不被日志拖慢
    if (!g_network_capture_queue.emplace(std::move(record))) {
        SPDLOG_WARN("网络抓包队列已满，丢弃记录 channel={} peer={} bytes={}",
                    channel,
                    peer,
                    payload.size());
    }
}

}// namespace RoboGo::logging
