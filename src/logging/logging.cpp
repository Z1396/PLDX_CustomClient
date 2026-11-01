// logging.cpp：日志模块实现——spdlog 初始化与关闭、日志目录与会话 ID 管理，以及网络报文按自定义二进制格式的抓取与落盘。
#include "logging.h"  // 本编译单元的主头文件(声明 initialize/shutdown 等)
#include "embedded_messages_proto.h"  // 内嵌的 messages.proto 原始文本(用于落盘保存)
#include "network_capture_format.h"  // 抓包标志位与 BuildNetworkCaptureFlags

namespace RoboGo::logging {  // 进入日志命名空间

// 匿名命名空间：日志模块内部实现细节（抓包线程、队列、编解码与写入），不对外暴露
namespace {  // 进入匿名命名空间，限定本文件内部符号

// 单条网络抓包记录的内存表示
struct NetworkCaptureRecord {  // 采集线程填充、写盘线程消费的 POD 结构
    std::uint64_t timestamp_us = 0;  // 采集时刻，微秒时间戳
    std::uint8_t transport = 0;      // 传输类型编码（见 encode_transport）
    std::uint8_t direction = 0;      // 方向编码（见 encode_direction）
    std::string channel;             // 通道名（如 MQTT topic / "video-stream"）
    std::string peer;                // 对端地址（"host:port"）
    std::vector<std::uint8_t> payload;  // 原始报文负载
};  // NetworkCaptureRecord 结构体定义结束

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
constexpr std::array<char, 8> kCaptureMagic = {'R', 'B', 'N', 'C', 'A', 'P', '1', '\n'};  // 8 字节定长文件签名
// 单条记录魔数（"REC2"），用于校验每条记录边界
constexpr std::array<char, 4> kRecordMagic = {'R', 'E', 'C', '2'};  // 4 字节定长记录签名
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
std::tm local_time(std::time_t time_point) {  // 入参为秒级时间戳
    std::tm result{};  // 存放本地时间分解结果的临时结构
#ifdef _WIN32  // Windows 平台的线程安全转换
    localtime_s(&result, &time_point);  // Windows 平台：线程安全的本地时间转换
#else  // 类 Unix 平台
    localtime_r(&time_point, &result);  // POSIX 平台：线程安全的本地时间转换
#endif  // 结束平台条件编译
    return result;  // 返回填充好的本地时间分解结构
}  // local_time 结束

// 功能：生成会话 ID（"年月日_时分秒_毫秒"），保证每次启动目录唯一。
// 思路：取系统时钟，拆分为本地日历时间与毫秒尾数，格式化拼接。
std::string make_session_id() {  // 返回形如 2026-11-01_16-45-03.042 的字符串
    // 获取当前系统时钟的时间点（C++ chrono 时间对象）
    const auto now = std::chrono::system_clock::now();  // 系统壁钟时刻，可映射为日历时间

    // 把 chrono 的时间点转成老式 time_t（秒级时间戳）
    const std::time_t time_point = std::chrono::system_clock::to_time_t(now);  // 丢掉不足一秒的部分

    // time_t → tm结构体，tm里面拆开存年、月、日、时、分、秒（本地时区时间）
    const std::tm local_tm = local_time(time_point);  // 调用上方的线程安全转换

    // 拿到从纪元开始到现在总毫秒数，对1000取模，单独取出【毫秒部分 0~999】
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;  // duration 取模得到毫秒尾数

    // 字符串流，用来拼接字符串，类似printf但是输出到string
    std::ostringstream stream;  // 用于拼接会话 ID 字符串的输出流

    // 格式化输出%Y-%m-%d_%H-%M-%S 就是 年-月-日_时-分-秒
    // 注意：Windows 目录名不允许冒号(':')与空格，故用下划线/减号替代，避免日志目录创建失败导致启动崩溃
        stream << std::put_time(&local_tm, "%Y-%m-%d_%H-%M-%S")  // put_time 按 strftime 风格格式化 tm
            << "."  // 秒与毫秒之间的分隔点
            << std::setw(3) // 确保毫秒部分有3位，不足补0       
            << std::setfill('0')    // 不够3位前面补0
            << milliseconds.count();// 毫秒，比如42 → 变成 042

    // 把流里面拼接好的文本转成std::string返回
    return stream.str();  // str() 取出流内部累积的字符串
}  // make_session_id 结束


// 功能：解析日志根目录。
// 思路：优先读取环境变量 ROBOGO_LOG_ROOT，未设置或为空则回退默认 "logs"。
std::filesystem::path resolve_log_root_directory() {  // 允许部署时用环境变量重定向日志位置
    // 优先读取环境变量 ROBOGO_LOG_ROOT，若设置且非空则返回其值且[0] != '\0'(非空字符串)
    if (const char *env_value = std::getenv("ROBOGO_LOG_ROOT"); env_value != nullptr && env_value[0] != '\0') {  // C++17 if 初始化语句同时完成取值与判空
        // 返回环境变量值
        return std::filesystem::path(env_value);  // 使用环境变量指定的日志根目录
    }  // 结束环境变量分支
    // 回退默认 "logs"
    return std::filesystem::path(kDefaultLogRootDirectory);  // 回退到默认 "logs" 目录
}  // resolve_log_root_directory 结束

// 功能：把传输类型字符串编码为单字节枚举值。
std::uint8_t encode_transport(std::string_view transport) {  // 字符串→编码，便于紧凑落盘
    if (transport == "mqtt") {  // MQTT 传输类型
        return kTransportMqtt;  // 编码 1
    }  // 结束 mqtt 分支
    if (transport == "udp-video") {  // UDP 视频传输类型
        return kTransportUdpVideo;  // 编码 2
    }  // 结束 udp-video 分支
    return kTransportUnknown;  // 未知传输类型
}  // encode_transport 结束

// 功能：把方向字符串编码为单字节枚举值。
std::uint8_t encode_direction(std::string_view direction) {  // 字符串→编码
    if (direction == "rx") {  // 接收方向
        return kDirectionRx;  // 编码 1
    }  // 结束 rx 分支
    if (direction == "tx") {  // 发送方向
        return kDirectionTx;  // 编码 2
    }  // 结束 tx 分支
    return kDirectionUnknown;  // 未知方向
}  // encode_direction 结束

// 功能：以“小端”字节序把整数追加到字节缓冲。
// 思路：逐字节取低 8 位并右移，依次 push，实现与平台无关的小端序列化。
template<typename Integer>  // 模板：支持任意整型宽度(这里用到 uint16/uint32/uint64)
void append_little_endian(std::vector<std::uint8_t> &buffer, Integer value) {  // 把 value 追加到 buffer 尾部
    static_assert(std::is_integral_v<Integer>, "Integer type required");  // 编译期断言：只接受整型，防止误传浮点/指针

    using UnsignedInteger = std::make_unsigned_t<Integer>;  // 转成无符号类型，右移才是逻辑移位
    UnsignedInteger unsigned_value = static_cast<UnsignedInteger>(value);  // 保留原始位模式
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {  // 逐字节输出，低位先出
        buffer.push_back(static_cast<std::uint8_t>((unsigned_value >> (index * 8)) & 0xFF));  // 取第 index 个字节
    }  // 结束逐字节循环
}  // append_little_endian 结束

// 功能：以“小端”把整数直接写入文件流。
// 思路：先序列化到临时缓冲，再整体 write，减少多次 IO 调用。
template<typename Integer>  // 模板同 append_little_endian
void write_little_endian(std::ofstream &stream, Integer value) {  // 直接落盘，无需调用方拼缓冲
    std::vector<std::uint8_t> buffer;  // 临时字节缓冲
    buffer.reserve(sizeof(Integer));  // 预留固定字节数，避免扩容
    append_little_endian(buffer, value);  // 复用上面的序列化逻辑
    stream.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));  // ofstream::write 只接受 char*，故重新解释
}  // write_little_endian 结束

// 功能：计算数据的 CRC32（IEEE 802.3 多项式，查表法）。
// 思路：首次调用惰性构建 256 项反射表；随后逐字节查表异或，结果取反。
std::uint32_t crc32(std::span<const std::uint8_t> data) {  // 入参为待校验的字节视图
    // 静态局部变量：仅初始化一次的生成多项式表（反射 0xEDB88320）
    static std::array<std::uint32_t, 256> table = [] {  // 立即执行的 lambda，首次进入时构建
        std::array<std::uint32_t, 256> generated{};  // 结果表，值初始化全 0
        for (std::uint32_t index = 0; index < generated.size(); ++index) {  // 对 0~255 每个字节值建表
            std::uint32_t value = index;  // 以该字节值作为初始余数
            for (int bit = 0; bit < 8; ++bit) {  // 逐位做除法模拟
                if ((value & 1U) != 0U) {  // 最低位为 1 才需要异或多项式
                    value = 0xEDB88320U ^ (value >> 1U);  // 反射多项式 0xEDB88320 即标准 0x04C11DB7 的位反转
                } else {  // 最低位为 0
                    value >>= 1U;  // 仅右移一位
                }  // 结束单位处理
            }  // 结束 8 位循环
            generated[index] = value;  // 保存该字节对应的表项
        }  // 结束建表循环
        return generated;  // 返回构建完成的表
    }();  // 末尾 () 立即调用 lambda

    std::uint32_t value = 0xFFFFFFFFU;  // CRC 初值全 1
    for (const std::uint8_t byte : data) {  // 逐字节流式计算
        value = table[(value ^ byte) & 0xFFU] ^ (value >> 8U);  // 查表 + 右移 8 位
    }  // 结束逐字节循环
    return value ^ 0xFFFFFFFFU;  // 末尾取反，得到标准 CRC32
}  // crc32 结束

// 功能：将 NetworkCaptureRecord 抓包记录，序列化为二进制的 body 载荷（不含CRC，CRC由上层 write_record 后续追加）
// 协议排布规则（全部小端序）：
// [uint64 timestamp_us] [uint8 transport] [uint8 direction]
// [uint16 channel_len] [uint16 peer_len] [uint32 payload_len]
// [channel字符串字节] [peer字符串字节] [payload原始数据包字节]
//
// 变长字段说明：channel、peer、payload 长度不固定，所以先存【长度】，再存【内容】；
// 长度字段有上限约束，超出上限时直接截断内容，防止越界、溢出：
//   channel / peer 长度用 uint16 存储 → 最大 65535 字节
//   payload 长度用 uint32 存储 → 最大 4294967295 字节
// 所有多字节整数统一按小端写入，保证跨平台解析一致性（不受CPU本机大小端影响）
std::vector<std::uint8_t> build_record_body(const NetworkCaptureRecord &record) {  // 返回可变长字节缓冲
    // 截断 channel 字符串长度：不能超过 uint16 能表示的最大值，然后强转成 uint16
    const auto channel_length = static_cast<std::uint16_t>(std::min<std::size_t>(record.channel.size(), std::numeric_limits<std::uint16_t>::max()));  // min 保证不溢出
    // 截断 peer 字符串长度，同上，上限 uint16
    const auto peer_length = static_cast<std::uint16_t>(std::min<std::size_t>(record.peer.size(), std::numeric_limits<std::uint16_t>::max()));  // 超长部分丢弃
    // 截断 payload 裸数据长度，上限 uint32
    const auto payload_length = static_cast<std::uint32_t>(std::min<std::size_t>(record.payload.size(), std::numeric_limits<std::uint32_t>::max()));  // 4GB 上限

    std::vector<std::uint8_t> body;  // 输出缓冲
    // 预分配内存：提前算好总字节数，避免vector多次扩容拷贝，提升序列化性能
    body.reserve(sizeof(std::uint64_t) + sizeof(std::uint8_t) * 2 + sizeof(std::uint16_t) * 2 + sizeof(std::uint32_t)
                 + channel_length + peer_length + payload_length);  // 固定头 + 三个变长字段

    // 1. 写入 微秒级时间戳 uint64（小端）
    append_little_endian<std::uint64_t>(body, record.timestamp_us);  // 显式指定模板参数，确保 8 字节宽度
    // 2. 写入传输层类型（uint8，比如TCP/UDP）
    body.push_back(record.transport);  // 单字节直接追加
    // 3. 写入流向标记（uint8：入包/出包）
    body.push_back(record.direction);  // 单字节直接追加
    // 4. 写入 channel 长度 uint16（小端）
    append_little_endian<std::uint16_t>(body, channel_length);  // 长度前置，便于解析
    // 5. 写入 peer 长度 uint16（小端）
    append_little_endian<std::uint16_t>(body, peer_length);  // 长度前置
    // 6. 写入 payload 长度 uint32（小端）
    append_little_endian<std::uint32_t>(body, payload_length);  // 长度前置

    // 7. 拷贝 channel 的字节内容（只拷贝截断后的长度）
    body.insert(body.end(), record.channel.begin(), record.channel.begin() + static_cast<std::ptrdiff_t>(channel_length));  // 区间插入
    // 8. 拷贝 peer 的字节内容
    body.insert(body.end(), record.peer.begin(), record.peer.begin() + static_cast<std::ptrdiff_t>(peer_length));  // 只拷截断长度
    // 9. 拷贝抓包原始payload（UDP/TCP裸报文）
    body.insert(body.end(), record.payload.begin(), record.payload.begin() + static_cast<std::ptrdiff_t>(payload_length));  // 同样截断

    return body;  // 返回组装好的 body
}  // build_record_body 结束

// 功能：把一条抓包记录，按照自定义二进制文件格式写入输出流
// 文件单条记录整体格式（顺序全部小端）：
// [魔数 kRecordMagic] [uint32 body_size] [uint32 body_crc32] [body二进制数据]
// 思路：先调用 build_record_body 生成 body 二进制，算出body长度、CRC校验值，
//       按协议依次写入头部信息 + body；后续复盘读文件时，依靠魔数同步、CRC校验判断数据是否损坏
void write_record(std::ofstream &stream, const NetworkCaptureRecord &record) {  // 在写盘线程中调用
    // 序列化record，生成body二进制载荷（body内部格式见build_record_body）
    const std::vector<std::uint8_t> body = build_record_body(record);  // 生成 body
    // body字节长度，强转为uint32存入文件头
    const std::uint32_t body_size = static_cast<std::uint32_t>(body.size());  // 截断为 32 位长度
    // 对body完整数据计算crc32，用于回放解析时校验数据完整性
    const std::uint32_t body_crc = crc32(body);  // span 可由 vector 隐式构造

    // 写入魔数：用来解析的时候做帧同步，快速定位一条记录的起始位置，防止粘包/错位
    stream.write(kRecordMagic.data(), static_cast<std::streamsize>(kRecordMagic.size()));  // 4 字节记录魔数
    // 写入body长度（小端uint32），解析时用来读取后面body的字节数
    write_little_endian<std::uint32_t>(stream, body_size);  // 长度字段
    // 写入body的crc校验值（小端uint32）
    write_little_endian<std::uint32_t>(stream, body_crc);  // 校验字段
    // 写入body原始二进制；ofstream::write入参是const char*，所以用reinterpret_cast做二进制重解释转换
    stream.write(reinterpret_cast<const char *>(body.data()), static_cast<std::streamsize>(body.size()));  // 输出 body 本体
}  // write_record 结束

// 功能：按需把缓冲区数据刷入磁盘，落盘抓包文件
// 思路：磁盘IO很慢，不写一条刷一次；策略：强制刷新标志 或者 累计写满32条记录再flush
//       平衡磁盘性能（减少频繁系统调用）和崩溃丢数据风险，防止程序异常退出时数据还留在页缓存里没写到硬盘
void flush_capture_stream(bool force_flush) {  // force_flush 为 true 时无条件刷盘
    // 文件没有打开，直接返回
    if (!g_network_capture_stream.is_open()) {  // is_open 判断流是否关联了有效文件
        return;  // 未打开则无需刷新
    }  // 结束打开状态判断
    // 满足条件：主动强制刷新 或者 已经累计写了32条记录
    if (force_flush || g_records_since_flush >= 32) {  // 阈值 32 条为经验值
        g_network_capture_stream.flush();  // 把C++流缓冲区的数据提交给操作系统页缓存
        g_records_since_flush = 0;         // 清空计数，重新开始累计
    }  // 结束刷新条件判断
}  // flush_capture_stream 结束

// 功能：网络抓包后台写盘线程主循环
// 思路：阻塞式从无锁队列取抓包记录；收到停止信号并且队列已经空了，线程才退出。
//       每次取到一条后，一次性把队列里积压的全部记录批量写完，减少多次flush、减少线程切换开销；写完按阈值触发落盘
void network_capture_loop() {  // 该函数运行在独立线程中
    SPDLOG_INFO("网络抓包线程已启动");  // 记录线程生命周期，便于排查卡死

    NetworkCaptureRecord record;  // 临时变量，存放从队列取出的单条抓包记录
    for (;;) {  // 无限循环，靠停止标志 + break 退出
        // 带超时阻塞等待队列数据：最多阻塞等待 kNetworkCaptureWaitTimeout(1ms)
        // wait_dequeue_timed：超时没拿到数据返回false；拿到数据返回true
        if (!g_network_capture_queue.wait_dequeue_timed(record, kNetworkCaptureWaitTimeout)) {  // 超时返回 false
            // 超时的时候，检查停止标记（acquire内存序，保证能看到其他线程设置的停止信号）
            if (g_network_stop_requested.load(std::memory_order_acquire)) {  // acquire 保证读到最新写入
                break; // 收到停止请求，跳出循环准备退出
            }  // 结束停止标志判断
            continue; // 单纯超时、没数据，回到循环继续等待
        }  // 结束出队失败分支

        // 成功取出一条记录，写入文件，计数+1
        write_record(g_network_capture_stream, record);  // 序列化并写入流缓冲
        g_records_since_flush += 1;  // 累计待刷盘记录数

        // 【批量消费优化】：继续尝试非阻塞取队列剩下积压的所有记录，一次性写完
        // 避免队列短时间涌入大量包时，多次进入等待、多次触发flush，降低IO压力
        while (g_network_capture_queue.try_dequeue(record)) {  // try_dequeue 非阻塞，队列空立即返回 false
            write_record(g_network_capture_stream, record);  // 批量写盘
            g_records_since_flush += 1;  // 继续累计
        }  // 结束批量出队循环
        // 批量写完一批，尝试flush（非强制，满足32条阈值才真正刷盘）
        flush_capture_stream(false);  // 传 false 表示按阈值决定
    }  // 结束主循环

    // 线程退出前：强制刷盘，保证剩余缓存数据交给OS；关闭文件流
    flush_capture_stream(true);  // 退出前必须强制刷盘，防止丢数据
    if (g_network_capture_stream.is_open()) {  // 再次确认流有效
        g_network_capture_stream.close();  // 关闭文件，析构前显式关闭更清晰
    }  // 结束关闭判断
    SPDLOG_INFO("网络抓包线程已退出");  // 记录线程正常退出
}  // network_capture_loop 结束

}// namespace  // 匿名命名空间结束

// 功能：初始化日志与抓包子系统（仅执行一次）。
// 思路：解析根目录与生成会话 ID，建立会话目录；保存当前 proto 定义便于旧日志解析；配置 spdlog 异步双 sink；
//       打开抓包文件并写入文件头（魔数/版本/标志）；启动抓包后台线程。
void initialize(std::string_view application_name, const bool invert_udp_video) {  // 进程启动时调用
    //std::call_once表示调用一次，后续调用不会执行
    std::call_once(g_initialize_once, [application_name, invert_udp_video]() {  // 用 once_flag 保证多线程下也只执行一次
        // 解析日志根目录
        // 日志根目录：应用安装目录下的 log 子目录
        // 会话 ID：当前时间戳（毫秒级）
        g_log_root_directory = resolve_log_root_directory();  // 环境变量优先，否则 "logs"
        // 生成会话 ID
        // 会话 ID：当前时间戳（毫秒级）
        // 用于区分不同运行实例的日志文件
        g_session_id = make_session_id();  // 形如 2026-11-01_16-45-03.042
        // 会话目录：日志根目录下的会话 ID 子目录
        // 用于存储该会话的所有日志文件
        g_log_directory = g_log_root_directory / g_session_id;  // path 的 operator/ 拼接子路径

        // 创建根目录(就是项目根目录下的logs目录)
        // 用于存储该会话的所有日志文件
        std::filesystem::create_directories(g_log_root_directory);  // 递归创建，已存在不报错
        // 会话目录(会话 ID(当前时间戳（毫秒级）))
        std::filesystem::create_directories(g_log_directory);  // 递归创建会话目录

        const auto app_log_path = g_log_directory / "robogo.log";  // 应用文本日志文件
        g_network_capture_path = g_log_directory / "network_capture.bin";  // 二进制抓包文件

        // 保存当前 proto 定义到日志目录，用于后续日志分析
        // 即使 proto 文件后续变更，旧日志仍可基于保存的 proto 进行解析
        {  // 局部作用域：让 proto_stream 及时析构关闭文件
            const auto proto_path = g_log_directory / "messages.proto";  // 目标 proto 文件路径
            // std::ios::binary 以二进制模式打开入文件，std::ios::trunc 以截断模式打开文件，如果文件存在则先删除文件内容
            std::ofstream proto_stream(proto_path, std::ios::binary | std::ios::trunc);  // 按位或组合打开模式
            if (proto_stream.is_open()) {  // 打开成功
                // 把内嵌的 proto 文本原样写入，供离线解析历史日志
                // 内入二进制数据，避免被解释为文本
                // 例如：42 → 042
                // 123 → 0123
                // reinterpret_cast 将 void* 转换为 const char*，确保二进制数据不被解释为文本,粗暴二进制重新解释，完全不做类型检查，只改编译器对这块内存的解读方式，非常危险
                // static_cast 将 size_t 转换为 std::streamsize，确保与文件流类型兼容
                proto_stream.write(reinterpret_cast<const char *>(embedded_messages_proto_data),  // 内嵌 proto 文本首地址
                                   static_cast<std::streamsize>(embedded_messages_proto_data_len));  // 内嵌文本长度
                proto_stream.close();  // 立即关闭，确保内容落盘
                SPDLOG_INFO("已保存 proto 定义到日志目录 path={} size={}B",  // 记录保存结果
                            proto_path.string(), embedded_messages_proto_data_len);  // string() 把 path 转为本地编码字符串
            } else {  // 打开失败(权限/磁盘)
                SPDLOG_WARN("无法写入 proto 文件到日志目录 path={}", proto_path.string());  // 仅告警，不影响后续日志
            }  // 结束打开成功判断
        }  // 结束 proto 保存作用域(离开时 proto_stream 析构)

        // 应用日志文件 sink：纯文件、带级别着色模式
        auto app_file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(app_log_path.string(), true);  // _mt 表示多线程安全；true 表示追加写
        app_file_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");  // %e 毫秒，%^%l%$ 带色级别，%v 正文

        std::vector<spdlog::sink_ptr> sinks{app_file_sink};  // sink 列表，先放入文件 sink
        // 控制台 sink：带颜色，便于开发期观察
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();  // Windows 上会自动启用 ANSI 颜色转换
        console_sink->set_pattern("%Y-%m-%d %H:%M:%S.%e [%^%l%$] %v");  // 与文件 sink 保持一致的格式
        sinks.push_back(console_sink);  // 加入控制台 sink，形成双输出

        // 初始化 spdlog 异步线程池（单线程、队列 8192）
        spdlog::init_thread_pool(kSpdlogQueueSize, 1);  // 参数：队列容量、后台线程数
        // 构造异步 logger：block 策略在队列满时阻塞调用方，避免丢日志
        auto app_logger = std::make_shared<spdlog::async_logger>(  // 创建具名异步 logger
                "robogo",  // logger 名称
                sinks.begin(),  // sink 区间起点
                sinks.end(),  // sink 区间终点
                spdlog::thread_pool(),  // 复用刚初始化的线程池
                spdlog::async_overflow_policy::block);  // 队列满时阻塞而非丢弃
        app_logger->set_level(spdlog::level::trace);  // 记录全部级别
        app_logger->flush_on(spdlog::level::info);    // 遇到 info 及以上立即 flush
        spdlog::set_default_logger(app_logger);       // 设置为默认 logger，后续未指定 logger 的日志会使用该 logger

        // 打开二进制抓包文件（截断），失败则抛异常
        g_network_capture_stream.open(g_network_capture_path, std::ios::binary | std::ios::trunc);  // 二进制 + 截断
        if (!g_network_capture_stream.is_open()) {  // 打开失败说明目录/权限有问题
            throw std::runtime_error("无法打开网络抓包文件");  // 直接抛异常，避免"看似启用其实没抓包"
        }  // 结束打开失败分支
        // 写入文件头：魔数 + 格式版本 + 标志字
        g_network_capture_stream.write(kCaptureMagic.data(), static_cast<std::streamsize>(kCaptureMagic.size()));  // 8 字节文件签名
        write_little_endian<std::uint32_t>(g_network_capture_stream, kCaptureVersion);  // 写入抓包文件格式版本号(uint32 小端)
        const std::uint32_t capture_flags = BuildNetworkCaptureFlags(invert_udp_video);  // 由开关构造标志字
        write_little_endian<std::uint32_t>(g_network_capture_stream, capture_flags);  // 写入抓包标志字(uint32 小端)
        g_network_capture_stream.flush();  // 立即刷盘，保证文件头先落地

        // 复位状态并启动抓包线程
        g_network_stop_requested.store(false, std::memory_order_release);// 重置抓包线标志
        g_records_since_flush = 0;          // 重置抓包记录数
        g_network_capture_thread = std::thread(network_capture_loop);// 启动抓包线程
        g_is_initialized = true;            /// 标志已初始化

        SPDLOG_INFO("日志系统已初始化 application={} session={} log_root={} log_dir={}",  // 输出关键路径，便于运维定位
                    application_name,  // 应用名
                    g_session_id,  // 会话 ID
                    g_log_root_directory.string(),  // 根目录
                    g_log_directory.string());  // 本次会话目录
        SPDLOG_INFO("应用日志已切换为异步模式 thread_count=1 queue_size={}", kSpdlogQueueSize);  // 提示异步参数
        SPDLOG_INFO("原始网络抓包已启用 path={} format=binary-v2 flags={} invert_udp_video={}",  // 抓包启用信息
                    g_network_capture_path.string(),  // 抓包文件路径
                    capture_flags,  // 标志字数值
                    invert_udp_video);  // 视频翻转开关
    });  // call_once 的 lambda 结束
}  // initialize 结束

// 功能：关闭日志系统。
// 思路：未初始化直接返回；置停止标志并 join 抓包线程（其会落盘关文件），随后关闭 spdlog 并复位标志。
void shutdown() {  // 进程退出前调用
    if (!g_is_initialized) {  // 未初始化时无需关闭
        return;  // 直接返回
    }  // 结束初始化判断
    SPDLOG_INFO("日志系统准备关闭 session={}", g_session_id);  // 记录关闭开始
    g_network_stop_requested.store(true, std::memory_order_release);  // release 保证之前的写入对抓包线程可见
    if (g_network_capture_thread.joinable()) {  // joinable 判断线程是否还在运行
        g_network_capture_thread.join();  // 阻塞等待写线程把剩余记录落盘并退出
    }  // 结束 join 判断
    spdlog::shutdown();  // 关闭 spdlog 线程池与所有 logger，释放全局资源
    g_is_initialized = false;  // 复位标志，允许后续重新初始化
}  // shutdown 结束

// 功能：返回是否已初始化。
bool is_initialized() {  // 供其他模块判断能否写日志
    return g_is_initialized;  // 读取初始化标志
}  // is_initialized 结束

// 以下为全局路径/标识的访问器
const std::filesystem::path &log_root_directory() {  // 返回根目录引用，避免拷贝
    return g_log_root_directory;  // 直接返回内部静态变量
}  // log_root_directory 结束

// 功能：返回本次运行的日志目录（logs/ 下的会话子目录），供外部写附加文件。
const std::filesystem::path &log_directory() {  // 返回引用，调用方只读
    return g_log_directory;  // 会话目录
}  // log_directory 结束

// 功能：返回网络抓包文件路径（network_capture.bin 等），由网络模块在记录报文时使用。
const std::filesystem::path &network_capture_path() {  // 返回引用
    return g_network_capture_path;  // 抓包文件路径
}  // network_capture_path 结束

// 功能：返回本次运行的唯一会话 ID（用于日志目录命名与抓包文件关联）。
const std::string &session_id() {  // 返回引用
    return g_session_id;  // 会话 ID 字符串
}  // session_id 结束

// 功能：把文本截断到 max_length，超出追加 "...(truncated)"。
// 思路：长度不足直接返回副本；否则取前缀并加标记。
std::string truncate_text(std::string_view text, std::size_t max_length) {  // 默认上限 512，见头文件
    if (text.size() <= max_length) {  // 长度未超限
        return std::string(text);  // 由 string_view 构造独立副本
    }  // 结束未超限分支
    std::string truncated(text.substr(0, max_length));  // substr 取前缀，避免整串拷贝
    truncated += "...(truncated)";  // 追加截断标记，提示阅读者
    return truncated;  // 返回截断结果
}  // truncate_text 结束

// 功能：把一笔网络报文入队（供抓包线程写盘）。
// 思路：未初始化或已请求停止则跳过；填充记录各字段，尝试入队，队列满则告警丢弃（不阻塞采集方）。
void log_network_payload(std::string_view transport,  // 传输类型字符串
                         std::string_view direction,  // 方向字符串
                         std::string_view channel,  // 通道名
                         std::span<const std::uint8_t> payload,  // 原始字节
                         std::string_view peer) {  // 对端地址
    if (!g_is_initialized || g_network_stop_requested.load(std::memory_order_acquire)) {  // 未就绪或正在关闭则跳过
        return;  // 直接返回，避免向已关闭的文件写入
    }  // 结束可用性判断

    NetworkCaptureRecord record;  // 构造一条待入队记录
    const auto timestamp = std::chrono::system_clock::now();  // 取当前壁钟时刻
    // 记录微秒级时间戳
    record.timestamp_us = static_cast<std::uint64_t>(  // 转成 64 位无符号整数
            std::chrono::duration_cast<std::chrono::microseconds>(timestamp.time_since_epoch()).count());  // 自纪元起的微秒数
    record.transport = encode_transport(transport);  // 字符串→单字节编码
    record.direction = encode_direction(direction);  // 字符串→单字节编码
    record.channel.assign(channel);  // 拷贝通道名
    record.peer.assign(peer);  // 拷贝对端地址
    record.payload.assign(payload.begin(), payload.end());  // 拷贝原始载荷字节

    // 非阻塞入队；满则丢弃并告警，保护实时业务不被日志拖慢
    if (!g_network_capture_queue.emplace(std::move(record))) {  // emplace 失败表示队列已满
        SPDLOG_WARN("网络抓包队列已满，丢弃记录 channel={} peer={} bytes={}",  // 告警并给出丢弃规模
                    channel,  // 丢弃记录的通道
                    peer,  // 丢弃记录的对端
                    payload.size());  // 丢弃的字节数
    }  // 结束入队判断
}  // log_network_payload 结束

}// namespace RoboGo::logging  // 命名空间结束
