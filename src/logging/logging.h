// logging.h：日志模块接口——对外暴露初始化/关闭/状态查询、日志目录与会话 ID 访问，以及网络报文抓取写入接口。
#pragma once  // 头文件包含守卫：防止重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含常用标准库与第三方库
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]

namespace RoboGo::logging {  // 进入日志命名空间

// 网络抓包档位：决定哪些流量真正落盘。
// 背景：图传是 60fps 的 HEVC，全量抓包每场要写约 1GB，既拖慢磁盘也容易把比赛本写满；
//       而复盘绝大多数问题只需要 MQTT 报文，所以默认只抓 MQTT。
enum class CaptureMode {
    Off,       // 完全关闭（连抓包文件都不创建）
    MqttOnly,  // 只抓 MQTT 上下行（默认）
    All,       // 全量：MQTT + UDP 图传原始分片
};

/**
 * @brief 把命令行/环境变量里的字符串解析成抓包档位。
 * @note 参数：取值 "off"/"mqtt"/"all"（大小写不敏感），无法识别时回退 MqttOnly。
 */
CaptureMode ParseCaptureMode(std::string_view text);

/**
 * @brief 查询当前生效的抓包档位（供日志与界面显示）。
 */
CaptureMode current_capture_mode();

/**
 * @brief 初始化日志系统（spdlog 异步 + 控制台/文件双 sink）并按档位启用原始网络抓包。
 * @note 参数：application_name 应用名（写入日志）；invert_udp_video 是否在抓包标志中记录“翻转 UDP 视频”选项；capture_mode 抓包档位（默认只抓
 *        MQTT）。
 */
void initialize(std::string_view application_name, bool invert_udp_video, CaptureMode capture_mode = CaptureMode::MqttOnly);  // 声明初始化函数：进程启动时调用一次，内部用 call_once 保证幂等

/**
 * @brief 关闭日志系统：停止抓包线程、等待其退出并关闭 spdlog。
 */
void shutdown();  // 声明关闭函数：抓包线程落盘并关闭文件后，再关闭 spdlog 全局资源

/**
 * @brief 查询日志系统是否已初始化。
 */
bool is_initialized();  // 返回内部初始化标志，供调用方判断能否安全写日志/抓包

// 以下为各全局路径/标识的访问器，返回内部存储的引用（只读使用，勿修改）。
const std::filesystem::path &log_root_directory();  // 日志根目录
const std::filesystem::path &log_directory();        // 本次会话日志目录（根目录/会话ID）
const std::filesystem::path &network_capture_path(); // 网络抓包二进制文件路径
const std::string &session_id();                     // 本次会话 ID（时间戳生成）

/**
 * @brief 把文本截断到 max_length，超出则追加 "...(truncated)" 标记。
 */
std::string truncate_text(std::string_view text, std::size_t max_length = 512);  // max_length 默认 512 字节，避免超长字段刷屏

/**
 * @brief 记录一笔网络报文到抓包队列（二进制格式，供复盘）。
 * @note 参数：transport 传输类型("mqtt"/"udp-video")；direction 方向("rx"/"tx")；channel 通道(如 topic)；payload
 *        原始载荷；peer 对端地址。本重载接受字节序列。
 */
void log_network_payload(std::string_view transport,  // 参数1：传输类型，取值 "mqtt" / "udp-video"
                         std::string_view direction,  // 参数2：报文方向，"rx" 接收 / "tx" 发送
                         std::string_view channel,  // 参数3：通道名，例如 MQTT topic 或 "video-stream"
                         std::span<const uint8_t> payload,  // 参数4：原始字节载荷(只读视图，不拷贝调用方内存)
                         std::string_view peer = {});  // 参数5：对端地址 "host:port"，缺省为空表示未知

// 便捷重载：直接接收字符串载荷（如 MQTT 文本负载），内部转为字节序列后调用上述版本。
inline void log_network_payload(std::string_view transport,  // 同上：传输类型
                                std::string_view direction,  // 同上：报文方向
                                std::string_view channel,  // 同上：通道名
                                std::string_view payload,  // 字符串载荷，内部按字节视图转发
                                std::string_view peer = {}) {  // 同上：对端地址，缺省为空
    const auto *begin = reinterpret_cast<const uint8_t *>(payload.data());  // 把 char 指针重新解释为字节指针，仅改变内存解读方式、不拷贝数据
    // clang 的 -Wunsafe-buffer-usage-in-container 无法跨调用证明「首指针 + 长度」同源；
    // 这里 data()/size() 都来自同一个 string_view，边界必然匹配，因此只在本处局部抑制。
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-container"
#endif
    log_network_payload(transport,  // 转发传输类型("mqtt"/"udp-video")
                        direction,  // 转发方向("rx"/"tx")
                        channel,  // 转发通道名(如 MQTT topic)
                        std::span<const uint8_t>(begin, payload.size()),  // 用首指针 + 字节数构造只读 span 视图
                        peer);  // 转发对端地址(默认空表示未知)
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
}

}  // namespace RoboGo::logging
