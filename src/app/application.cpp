// application.cpp：应用程序核心——解析启动参数、搭建后台服务(MQTT / UDP 视频流 / 字节块解码线程)，驱动主渲染循环、视频纹理上传与键鼠上报。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <boost/asio.hpp>
extern "C" {  // FFmpeg 为 C 接口，需按 C 链接方式包含
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
#include <readerwriterqueue.h>
#include <messages.pb.h>
// [/pch-split]
#include "application.h"  // 本编译单元的主头文件(声明 ParseLaunchOptions / RunApplication)

#include <expected>  // std::expected / std::unexpected（C++23）
#include "app/number_parser.h"  // 无异常整数解析（from_chars + std::expected）
#include "resource/resources.h"  // 内嵌图片解码结果与错误原因

#include "config.h"  // 应用级配置：MQTT 服务端地址的持久化读写
#include "custom_control_publisher.h"  // CustomControl 周期下发（敌方血量透传给电控）
#include "keyboard_mouse_state_manager.h"  // 键鼠状态采集与管理(供 update_keyboard_mouse_state 使用)
#include "perf_stats.h"  // 性能指标采集(解码/转换/上传/渲染耗时、队列深度)
#include "main_window.h"  // 主窗口/主界面绘制(ShowMainWindow)
#include "video/rgba_image_transform.h"  // RGBA 缓冲变换工具(如 180 度旋转)
#include "video/video_stream_manager.h"  // UDP 视频流管理器：绑定端口并解出 AVFrame 回调

// 应用核心实现：解析启动参数、建立后台服务(IO 线程/MQTT/UDP 视频流/自定义字节块解码 worker)、
// 运行主渲染循环、将视频帧上传为 GPU 纹理，并按需上报键鼠控制与自定义控制载荷。
namespace {  // 匿名命名空间：本文件内部的辅助函数与类型，不对外暴露

// 引入本文件所需的 RoboGo::app 类型别名，简化后续书写。
using RoboGo::app::LaunchOptions;   // 启动选项结构体，包含 MQTT 服务器地址、视频端口、主机名等。
using RoboGo::app::TextureSlot;     // 纹理槽枚举，定义了视频纹理/自定义视频纹理/小地图纹理的槽位。
using RoboGo::app::TextureView;     // 纹理视图结构体，包含 GPU 句柄与尺寸，用于渲染。
using RoboGo::app::UiLogMessage;    // UI 日志消息结构体，包含日志级别、时间戳、消息内容等。
using RoboGo::app::VideoFrame;      // 视频帧结构体，包含视频数据、时间戳等。

constexpr bool kDefaultInvertUdpVideo = false; // UDP 视频是否默认做 180 度旋转

/**
 * @brief 去掉字符串首尾的 ASCII 空白（命令行/配置文件输入都可能带空格）。
 */
std::string TrimAscii(std::string_view text) {
    const auto is_space = [](char character) {  // 只认空格/制表/回车/换行，避免 locale 依赖
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    std::size_t begin = 0;  // 左边界
    std::size_t end = text.size();  // 右边界（开区间）
    while (begin < end && is_space(text[begin])) {  // 跳过左侧空白
        ++begin;  // 右移
    }
    while (end > begin && is_space(text[end - 1])) {  // 跳过右侧空白
        --end;  // 左移
    }
    return std::string(text.substr(begin, end - begin));  // 返回裁剪后的副本
}

/**
 * @brief 返回默认的 MQTT 服务器地址。
 * @note 思路：优先用上次成功登录后保存到用户配置里的地址；没有配置时用 2027 赛季的裁判系统地址。联调本地 mock 时，在登录面板把地址改成 127.0.0.1:3333 即可，
 *        登录成功后会覆盖配置。
 */
std::string DefaultMqttServer() {  // 返回 std::string 而非 string_view：调用方要存进 LaunchOptions 持有
    const std::string configured = RoboGo::app::LoadConfiguredMqttServer();  // 读取持久化配置（可能为空）
    if (!configured.empty()) {  // 有历史配置
        // 注意：本函数在日志系统初始化之前被调用，这里不能写日志（会落到 spdlog 默认 logger 上、且进不了日志文件）。
        // 实际使用的地址由 RunApplication 的"解析启动参数"一行统一打印。
        return "tcp://" + configured;  // 统一补上 tcp:// 前缀，交给后面的解析
    }
    return "tcp://192.168.12.1:3333";  // 2027 赛季裁判系统固定地址（自定义终端 IP 为 192.168.12.2）
}

/**
 * @brief 从原始 MQTT 服务器字符串与视频端口字符串中解析出可用的网络端点。
 * @note 思路：兼容 "tcp://host:port" / "host:port" / "host"（端口缺省 3333）三种写法；原先只认带 tcp:// 的形式，写成 "192.168.12.1:3333"
 *        会静默回落到 127.0.0.1，现场极难排查。参数 options：启动选项(读入 mqtt_server/video_port，写出 mqtt_host/mqtt_port/parsed_video_port)
 *        。
 */
void ParseNetworkEndpoints(LaunchOptions &options) {
    options.mqtt_host = "192.168.12.1";  // 主机默认值（2027 裁判系统）
    options.mqtt_port = "3333";  // 端口默认值
    std::string address = TrimAscii(options.mqtt_server);  // 去掉首尾空白
    if (address.starts_with("tcp://")) {  // 剥离可选的协议前缀
        address = TrimAscii(address.substr(6));  // 去掉 "tcp://" 并再次裁剪
    }
    if (address.empty()) {  // 地址为空（理论上不会，默认值非空）
        // 保留默认端点；解析结果由 RunApplication 在日志初始化后统一打印，这里不写日志。
    } else {  // 地址非空
        const std::size_t colon_pos = address.rfind(':');  // 用最后一个冒号分隔端口（兼容 IPv6 的 [::1]:3333 写法）
        if (colon_pos != std::string::npos) {  // 找到冒号才拆分
            const std::string host_part = TrimAscii(address.substr(0, colon_pos));  // 冒号之前为主机
            const std::string port_part = TrimAscii(address.substr(colon_pos + 1));  // 冒号之后为端口
            if (!host_part.empty()) {  // 主机非空才采用
                options.mqtt_host = host_part;  // 写入主机
            }
            if (!port_part.empty()) {  // 端口非空才采用
                options.mqtt_port = port_part;  // 写入端口
            }
        } else {  // 没有冒号：整串视为主机，端口用默认值
            options.mqtt_host = address;  // 只改主机
        }
    }

    // 视频端口：from_chars 不抛异常；非法、溢出、越界一律回落默认 3334。
    const auto parsed_port = RoboGo::app::ParseInteger<int>(options.video_port);
    options.parsed_video_port =
            (parsed_port.has_value() && *parsed_port > 0 && *parsed_port <= 65535)
                    ? *parsed_port
                    : 3334;  // 使用协议默认端口（解析结果稍后由启动日志打印）
}

// 视频帧转换失败的具体原因：调用方据此区分「上游空帧」「格式不支持」与「真正的转换故障」。
enum class VideoConvertError {
    NullFrame,
    UnsupportedPixelFormat,
    SwsContextFailed,
    IncompleteConversion,
    RotationFailed,
};

[[nodiscard]] const char *ToString(const VideoConvertError error) noexcept {
    switch (error) {
        case VideoConvertError::NullFrame:              return "null frame";
        case VideoConvertError::UnsupportedPixelFormat: return "unsupported pixel format";
        case VideoConvertError::SwsContextFailed:       return "sws context creation failed";
        case VideoConvertError::IncompleteConversion:   return "incomplete conversion";
        case VideoConvertError::RotationFailed:         return "rotation failed";
    }
    return "unknown";
}

// AvFrameRgbaConverter —— 将 FFmpeg 解码得到的 AVFrame(YUV) 转换为 RGBA 像素缓冲，
// 并按需做 180 度旋转；内部复用 sws 缩放/色彩转换上下文以避免每帧重建。
class AvFrameRgbaConverter {  // 只在 IO 线程的帧回调里使用，故内部不加锁(单个原子开关除外)
public:  // 对外只暴露"转换"与"翻转开关"
    /**
     * @brief 构造转换器并设定初始是否做 180 度旋转。
     * @note 思路：保存翻转开关，sws 上下文延迟到首次 convert 时按帧尺寸惰性创建并复用。参数 invert_udp_video(原子bool)：是否对 UDP 视频做 180
     *        度旋转。
     */
    explicit AvFrameRgbaConverter(const bool invert_udp_video)  // explicit：避免 bool 被隐式转换为转换器
        : invert_udp_video_(invert_udp_video) {  // 直接初始化原子成员(原子的拷贝构造被删除，只能用初始化列表)
    }  // 构造函数结束(空函数体：sws 上下文留待首次 convert 创建)

    /**
     * @brief 析构时释放 sws 上下文，防止内存泄漏。
     */
    ~AvFrameRgbaConverter() {  // 与 sws_getCachedContext 配对释放
        // SwsContext sws_context_：FFmpeg libswscale 库的核心上下文结构体，
        // 本质上是不透明句柄，封装了图像缩放、像素格式转换与色彩空间转换所需的全部参数和状态。
        if (sws_context_ != nullptr) { // 仅当上下文已创建时才释放
            // sws_freeContext：释放 SwsContext 所占资源，与 sws_getContext 配对。
            sws_freeContext(sws_context_);  // 释放内部查找表与滤镜参数，不做帧数据拷贝
            sws_context_ = nullptr; // 置空，避免悬空指针
        }
    }

    /**
     * @brief 从池中取一个可写的帧缓冲（尺寸不符才重新分配，同尺寸直接复用，不清零）。为什么要池：原来每帧都构造新的 VideoFrame，`resize(宽*高*4)` 会把 8.29MB
     *        全部填 0，而紧接着 sws_scale 又整帧覆盖它——实测这次清零要 0.71ms/帧（本机 1920x1080）。
     */
    [[nodiscard]] VideoFrame AcquireFrame(const int width, const int height) {
        std::vector<uint8_t> buffer;  // 待复用的像素缓冲
        {
            std::lock_guard<std::mutex> lock(frame_pool_mutex_);  // 解码线程取、渲染线程还，需要互斥
            if (!frame_pool_.empty()) {  // 池里有存货
                buffer = std::move(frame_pool_.back());  // 取最后一个（后进先出，缓存更热）
                frame_pool_.pop_back();  // 从池中移除
            }
        }

        const std::size_t needed = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        VideoFrame video_frame;  // 输出帧
        video_frame.buffer = std::move(buffer);  // 接管复用缓冲
        if (video_frame.buffer.size() != needed) {  // 首次分配或分辨率变化
            video_frame.buffer.resize(needed);  // 只有这一步会清零，且一生只发生几次
        }
        video_frame.width = width;  // 记录宽度
        video_frame.height = height;  // 记录高度
        return video_frame;  // 交给调用方写入
    }

    /**
     * @brief 把用完的帧缓冲归还池中（纹理上传完成后调用）。
     * @note 说明：池容量有上限，超出部分直接随帧销毁，避免异常情况下把显存/内存一直攥着。
     */
    void ReleaseFrame(VideoFrame &&frame) {
        std::lock_guard<std::mutex> lock(frame_pool_mutex_);  // 与 AcquireFrame 共用同一把锁
        if (frame_pool_.size() >= kMaxPooledFrames) {  // 池已满
            return;  // 直接丢弃（析构会释放这块内存）
        }
        frame_pool_.push_back(std::move(frame.buffer));  // 缓冲归池
    }

    /**
     * @brief 把一帧 YUV AVFrame 转换为 RGBA 的 VideoFrame。
     * @note 思路：校验入参 -> 准备 sws 上下文 -> 分配 RGBA 缓冲 -> sws_scale 转换 -> 按需 180 度旋转。
     * @return 成功返回像素数据，失败返回带具体原因的 unexpected。AVFrame（libavutil）：存放【解码之后、未压缩的原始画面/音频】的容器，即 YUV 帧载体，与 AVPacket
     *        成对。AVPacket：压缩数据（H.264 码流包，输入给解码器）AVFrame：解码出来的裸数据（YUV/RGB，解码器输出）
     */
    std::expected<VideoFrame, VideoConvertError> convert(const AVFrame *frame) {  // 失败带原因，且不抛异常穿越 FFmpeg 回调
        RoboGo::app::perf::ScopedTimer convert_timer(&RoboGo::app::perf::RecordConvert);  // 统计本帧 YUV->RGBA 耗时（析构时上报，提前 return 也会记）
        if (frame == nullptr) { // 空帧直接拒绝
            return std::unexpected(VideoConvertError::NullFrame);  // 上游解码失败时可能给出空指针
        }

        // 仅支持 NV12（semi-planar 半平面,） / YUV420P（I420，planar 三平面） 两种常见图传像素格式，其它格式视为不可处理。
        if (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_YUV420P) {  // 两种 4:2:0 布局，覆盖绝大多数图传编码输出
            return std::unexpected(VideoConvertError::UnsupportedPixelFormat);  // 不支持则直接放弃该帧
        }

        // SwsContext 是 FFmpeg swscale 模块的图像转换上下文句柄，专门做像素格式转换、缩放、裁剪。
        // 即 NV12 / YUV420P -> RGBA 的转换器。
        // 获取或复用 sws 上下文：源为当前 YUV 格式，目标为同尺寸 RGBA。
        // 使用 SWS_FAST_BILINEAR 保证实时性；sws_getCachedContext 在参数不变时会直接复用旧上下文。
        sws_context_ = sws_getCachedContext(  // 参数不变则返回原上下文，避免每帧重建开销
                sws_context_,  // 旧上下文；首次为 nullptr
                frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),  // 源：宽、高、像素格式(NV12/YUV420P)
                frame->width, frame->height, AV_PIX_FMT_RGBA,  // 目标：同宽同高，RGBA 交错格式(每像素 4 字节)
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);  // 快速双线性：质量够用且开销低；后三个为滤镜参数(不使用)
        if (sws_context_ == nullptr) { // 上下文创建失败(如内存不足)
            return std::unexpected(VideoConvertError::SwsContextFailed);  // 放弃该帧，下帧会再次尝试创建
        }

        VideoFrame video_frame = AcquireFrame(frame->width, frame->height);  // 从池中取缓冲（同尺寸不清零、不重新分配）
        uint8_t *dest_data[1] = {video_frame.buffer.data()}; // sws_scale 需要目标平面指针数组
        int dest_linesize[1] = {frame->width * 4}; // 目标每行字节数(紧密排列，无 padding)
        // 执行 YUV -> RGBA 的实际转换；返回转换出的行数。
        const int scaled_height = sws_scale(sws_context_,  // 转换上下文
                                            frame->data, frame->linesize,  // 源平面指针与每行字节数(YUV 各平面 stride)
                                            0, frame->height,  // 从第 0 行开始，转换整帧高度
                                            dest_data, dest_linesize);  // 目标平面指针与 stride
        // 行数不符说明转换异常
        if (scaled_height != frame->height) {  // 正常情况下应等于输入高度
            return std::unexpected(VideoConvertError::IncompleteConversion);  // 转换不完整则丢弃该帧
        }
        // 按开关决定是否对 RGBA 缓冲做 180 度旋转(图传画面方向修正)。
        if (!RoboGo::video::ApplyRgba180Rotation(video_frame.buffer,  // 就地旋转 RGBA 缓冲
                                                 video_frame.width,  // 宽(像素)
                                                 video_frame.height,  // 高(像素)
                                                 invert_udp_video_.load(std::memory_order_relaxed))) {  // relaxed：仅读一个开关，无跨变量同步需求
            return std::unexpected(VideoConvertError::RotationFailed);  // 旋转失败(如尺寸与缓冲不匹配)同样丢弃
        }
        return video_frame; // 返回转换完成的帧
    }

    /**
     * @brief 运行时切换是否做 180 度旋转。
     */
    void SetInvertUdpVideo(const bool enabled) {  // UI 线程调用，IO 线程读取
        invert_udp_video_.store(enabled, std::memory_order_relaxed); // 原子写入，relaxed 足够(仅传递开关)
    }

    /**
     * @brief 查询当前是否启用 180 度旋转。
     */
    [[nodiscard]] bool IsInvertUdpVideoEnabled() const {  // nodiscard：返回值有意义，不应被忽略
        return invert_udp_video_.load(std::memory_order_relaxed); // 原子读取当前开关值
    }

private:  // 内部状态
    static constexpr std::size_t kMaxPooledFrames = 4;  // 池容量：队列上限也是 4，刚好对应
    std::mutex frame_pool_mutex_;                       // 保护 frame_pool_ 的互斥量
    std::vector<std::vector<uint8_t>> frame_pool_;      // 空闲像素缓冲池（每个约 8MB）
    SwsContext *sws_context_ = nullptr; // sws 上下文(惰性创建并复用)
    std::atomic<bool> invert_udp_video_ = false; // 180 度旋转开关(跨线程读写)
};

// 视频帧队列上限：UI 线程每帧只取最新一帧，中间的全是垃圾。
// 留 4 帧是为了容忍渲染偶尔卡顿一两帧；再多就是纯粹的内存占用（每帧约 8MB）。
// 为什么必须有上限：拖窗口/切全屏时 Windows 会阻塞渲染循环好几秒，而解码线程仍在 60fps 产帧，
// 无界队列会以每秒 500MB 的速度涨上去（拖 3 秒就是 1.4GB）。
constexpr std::size_t kMaxQueuedVideoFrames = 4;

// BackgroundServices —— 汇总所有后台线程与网络/视频服务：
// IO 线程(Asio)、MQTT 客户端、UDP 视频流管理器、自定义字节块解码 worker，
// 以及它们与主线程之间的无锁队列；主线程通过本类取出消息与最新视频帧。
class BackgroundServices {  // 生命周期与 RunApplication 中的作用域块绑定，退出时析构即停服
public:  // 构造初始化各组件，之后由主循环调用 ProcessMessages / TakeLatestVideoFrame 等接口
    /**
     * @brief 构造后台服务，按启动选项初始化各组件并启动 IO 线程。
     * @note 思路：成员初始化列表中依次构造转换器、解码 worker、MQTT 客户端、视频流管理器，最后创建一个专用线程运行 Asio io_context。
     */
    explicit BackgroundServices(const LaunchOptions &options)  // explicit：防止 LaunchOptions 被隐式转换
        :   udp_frame_converter_(options.invert_udp_video), // AvFrameRgbaConverterUDP udp_frame_converter_帧转换器(带初始旋转开关)
        
            // MQTT 客户端：连接裁判系统；收到消息时投递到主线程消息队列。
            mqtt_client_(io_context_,  // 共用本类的 io_context
                        options.mqtt_host,  // 解析后的 broker 主机
                        options.mqtt_port,  // 解析后的 broker 端口
                        [this](const std::string &topic, const std::string &payload) {     // 下行报文回调(在 IO 线程执行)
                            message_queue_.emplace(topic, payload); }),                    // (topic, payload) 原样入队
                        
            // UDP 视频流管理器：绑定 0.0.0.0:video_port 接收图传；每解出一帧调用回调。
            video_stream_manager_(io_context_,  // 同一个执行器，避免多线程竞争
                                    boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), options.parsed_video_port),  // 监听所有网卡的指定端口
                                    [this](const AVFrame *frame)  // 每解出一帧(裸 AVFrame)调用一次(IO 线程)
            {  // 回调体
                auto video_frame = udp_frame_converter_.convert(frame); // YUV -> RGBA(+旋转)
                if (video_frame.has_value()) {  // 转换成功
                    PushUdpVideoFrame(std::move(*video_frame)); // 非 const 才能真移动，避免每帧 8MB 拷贝
                } else {  // 失败原因随日志带出，现场可区分空帧 / 格式不支持 / 转换故障
                    SPDLOG_ERROR("UDP 视频帧转换失败 reason={} format={} width={} height={}",
                                ToString(video_frame.error()),
                                frame != nullptr ? frame->format : -1,
                                frame != nullptr ? frame->width : 0,
                                frame != nullptr ? frame->height : 0);
                }
            }),

            // IO 线程：运行 Asio 事件循环，承接 MQTT 与 UDP 的异步操作。
            io_thread_([this]() {  // 新线程入口：整个 IO 线程都跑在 io_context_.run() 里
                SPDLOG_INFO("IO线程已启动");  // 标记后台线程就绪
                io_context_.run(); // 阻塞运行，直到 io_context_.stop()
                SPDLOG_INFO("IO线程已退出");  // 说明 stop() 已生效，线程即将结束
            }),  // IO 线程 lambda 结束

            video_test_enabled_(options.video_test) // 初始化列表最后一项：是否启用合成视频源
            {  
                if (video_test_enabled_) {  // 开发自检模式：没有图传时用合成画面验证整条链路
                    SPDLOG_INFO("合成视频源已启用 --video-test（仅用于自检，比赛请勿开启）");
                    video_test_thread_ = std::thread([this]() { RunVideoTestSource(); });  // 独立线程按 60fps 产帧
                }
            }  // 构造函数结束(全部工作在初始化列表中完成)

    /**
     * @brief 析构时确保后台服务全部停止、线程回收。
     */
    ~BackgroundServices() {  // 保证异常/提前 return 时也能停服，不残留线程
        Stop();  // 确保后台服务停止
    }

    // 禁止拷贝：本类持有线程与 socket 等不可复制资源。
    BackgroundServices(const BackgroundServices &) = delete;  // 删除拷贝构造
    BackgroundServices &operator=(const BackgroundServices &) = delete;  // 删除拷贝赋值

    /**
     * @brief 暴露 MQTT 客户端引用，供主循环发送消息。
     */
    RoboGo::io::MqttClient &mqtt_client() {  // 返回引用，避免拷贝且允许外部调用 publish()
        return mqtt_client_;  // 成员长期存活于本对象
    }

    /**
     * @brief 设置 UDP 视频是否 180 度旋转。
     */
    void SetUdpVideoInverted(const bool enabled) {  // 转调转换器内部的原子开关
        udp_frame_converter_.SetInvertUdpVideo(enabled);  // IO 线程下一帧起生效
    }

    /**
     * @brief 查询 UDP 视频是否 180 度旋转。
     */
    [[nodiscard]] bool IsUdpVideoInverted() const {  // const：查询不改状态
        return udp_frame_converter_.IsInvertUdpVideoEnabled();  // 读取原子开关
    }

    /**
     * @brief 在主线程中消费所有已到达的 MQTT 消息。
     * @note 思路：循环从无锁队列取出消息交给 HandleMqttMessage 分发；若消息携带自定义字节块数据，则转交解码 worker。
     */
    void ProcessMessages(State &state) {  // 每帧调用一次，必须取空以免积压
        std::pair<std::string, std::string> message;  // 复用的出参，避免每条消息都构造新对象
        while (message_queue_.try_dequeue(message)) { // 直到队列取空
            HandleMqttMessage(state,                        // 状态引用，避免拷贝
                            mqtt_client_,             // 客户端引用，避免拷贝
                            message.first,             // 话题，避免拷贝
                            message.second);           // 消息，引用，避免拷贝
        }
    }

    /**
     * @brief 取出最新的 UDP 视频帧(丢弃积压旧帧，只保留最后一帧)。
     */
    std::optional<VideoFrame> TakeLatestVideoFrame() {  // 无新帧时返回 nullopt
        return TakeLatestFrame(video_frame_queue_);  // 复用统一的"取最新"逻辑
    }

    /**
     * @brief 把一帧 UDP 视频放进队列；队列已满时丢掉最旧的一帧并把它的缓冲还回池。
     */
    void PushUdpVideoFrame(VideoFrame frame) {
        while (video_frame_queue_.size_approx() >= kMaxQueuedVideoFrames) {  // 到上限
            VideoFrame discarded;  // 承载被丢弃的旧帧
            if (!video_frame_queue_.try_dequeue(discarded)) {  // size_approx 只是近似，可能实际已空
                break;  // 避免死循环
            }
            RoboGo::app::perf::AddDroppedFrame();  // 计入丢帧统计
            udp_frame_converter_.ReleaseFrame(std::move(discarded));  // 缓冲回收，不浪费这次分配
        }
        video_frame_queue_.emplace(std::move(frame));  // 入队新帧
    }

    /**
     * @brief 把用完的 UDP 帧缓冲还给池（上传完成后调用）。
     */
    void ReleaseUdpVideoFrame(VideoFrame &&frame) { udp_frame_converter_.ReleaseFrame(std::move(frame)); }

    /**
     * @brief 查询 UDP 视频队列当前积压（供性能面板显示）。
     */
    [[nodiscard]] int UdpVideoQueueDepth() const { return video_frame_queue_.size_approx(); }

    /**
     * @brief 合成视频源——按 60fps 往 UDP 队列灌入渐变画面。为什么需要：没有图传/没有机器人时，解码、帧池、上传、渲染这几段代码根本走不到，改了视频链路却无法验证。加一个开关就能纯本地把整条管线跑起来。
     *        画面设计：横向彩虹渐变 + 纵向亮度渐变 + 每帧轻微色相偏移，肉眼即可判断是否在动、有没有撕裂。
     */
    void RunVideoTestSource() {
        constexpr int width = 1920;  // 与真实图传一致的分辨率
        constexpr int height = 1080;
        std::uint32_t frame_index = 0;  // 帧序号
        while (!video_test_stop_.load(std::memory_order_relaxed)) {  // 收到停止信号就退出
            VideoFrame frame = udp_frame_converter_.AcquireFrame(width, height);  // 从池里取缓冲（走真实路径）
            auto *pixels = frame.buffer.data();  // RGBA 起始地址
            const std::uint32_t hue_shift = frame_index * 5u;  // 每帧整体偏移，肉眼可见在动
            for (int y = 0; y < height; ++y) {  // 逐行填充
                auto *row = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
                const std::uint8_t green = static_cast<std::uint8_t>((y * 255) / height);  // 纵向亮度渐变
                for (int x = 0; x < width; ++x) {  // 逐像素填充
                    row[x * 4 + 0] = static_cast<std::uint8_t>(((x * 255) / width + hue_shift) & 0xFF);  // R 横向渐变
                    row[x * 4 + 1] = green;  // G 纵向渐变
                    row[x * 4 + 2] = static_cast<std::uint8_t>((frame_index * 3u) & 0xFF);  // B 随时间变化
                    row[x * 4 + 3] = 255;  // A 不透明
                }
            }
            PushUdpVideoFrame(std::move(frame));  // 走与真实图传完全相同的入队路径
            ++frame_index;  // 下一帧
            std::this_thread::sleep_for(std::chrono::milliseconds(16));  // 60fps
        }
    }

    /**
     * @brief 按顺序停止各服务并回收 IO 线程。
     * @note 思路：置停止标志 -> 停 MQTT/视频流 -> stop io_context -> join IO 线程 -> 停解码 worker。
     */
    void Stop() {  // 幂等：析构与显式调用都可安全触发
        if (stopped_) { // 保证只执行一次
            return;  // 已停过则直接返回
        }
        stopped_ = true;  // 标记已停止
        video_test_stop_.store(true, std::memory_order_relaxed);  // 先让合成视频源停下，避免它继续入队
        if (video_test_thread_.joinable()) {  // 自检线程可能从未启动
            video_test_thread_.join();  // 等它退出
        }  // 结束自检线程回收
        mqtt_client_.stop(); // 停止 MQTT
        video_stream_manager_.stop(); // 停止 UDP 视频接收
        io_context_.stop(); // 让 io_context.run() 返回
        if (io_thread_.joinable()) {  // 线程可能已被回收
            io_thread_.join(); // 等待 IO 线程退出
        }
    }

private:  // 内部工具与成员
    /**
     * @brief 从队列中取出最后一帧(排空队列，仅保留最新)。
     */
    static std::optional<VideoFrame> TakeLatestFrame(moodycamel::ReaderWriterQueue<VideoFrame> &queue) {  // static：不依赖实例状态
        std::optional<VideoFrame> latest_frame;  // 为空表示队列本来就空
        VideoFrame video_frame;  // 复用的中转对象
        // try_dequeue 会阻塞，直到有新帧可用或队列为空
        while (queue.try_dequeue(video_frame)) { // 持续取，直到取空
            latest_frame = std::move(video_frame); // 始终用更新的帧覆盖
        }
        return latest_frame;  // 返回最后一帧(可能为空)
    }

    boost::asio::io_context io_context_; // Asio 事件循环
    moodycamel::ReaderWriterQueue<std::pair<std::string, std::string>> message_queue_; // MQTT 消息队列
    moodycamel::ReaderWriterQueue<VideoFrame> video_frame_queue_; // UDP 视频帧队列
    AvFrameRgbaConverter udp_frame_converter_; // YUV->RGBA 转换器
    RoboGo::io::MqttClient mqtt_client_; // MQTT 客户端
    RoboGo::video::VideoStreamManager video_stream_manager_; // UDP 视频流管理器
    std::thread io_thread_; // IO 线程
    std::thread video_test_thread_;                 // 合成视频源线程（仅 --video-test 时启动）
    std::atomic<bool> video_test_stop_{false};      // 合成视频源停止标志
    const bool video_test_enabled_ = false;         // 是否启用合成视频源
    bool stopped_ = false; // 是否已停止
};

/**
 * @brief 取出最新的 UDP 视频帧并上传为 GPU 纹理，同时记录最近出帧时间。
 * @note 思路：2027 起只剩 UDP 一路图传（CustomByteBlock 解码链路已移除，其产物本就未上屏），故只需处理 UDP 帧；成功上传则刷新时间戳，供超时回退逻辑判断画面是否仍在刷新。
 */
void UploadLatestVideoFrames(RoboGo::platform::PlatformBackend &platform_backend,  // 负责实际上传纹理到 GPU
                             BackgroundServices &background_services,  // 帧来源
                             std::chrono::steady_clock::time_point &last_udp_frame_time) {  // 出参：最近一次 UDP 出帧时刻
    auto video_frame = background_services.TakeLatestVideoFrame(); // 取最新 UDP 帧（非 const：上传完要把缓冲还回池）
    if (video_frame.has_value()) {  // 本帧有新画面才上传
        last_udp_frame_time = std::chrono::steady_clock::now(); // 刷新 UDP 出帧时间
        {
            RoboGo::app::perf::ScopedTimer upload_timer(&RoboGo::app::perf::RecordUpload);  // 统计纹理上传耗时（不再含 GPU 等待）
            if (!platform_backend.UploadTexture(TextureSlot::UdpVideo, *video_frame)) { // 上传到 UDP 视频纹理槽
                SPDLOG_ERROR("创建UDP视频纹理失败 width={} height={}", video_frame->width, video_frame->height);  // 上传失败记录尺寸，便于排查显存/尺寸问题
            }
        }
        background_services.ReleaseUdpVideoFrame(std::move(*video_frame));  // 像素缓冲归还帧池，下一帧直接复用
    }
}

/**
 * @brief 判断某一路视频当前是否可正常显示。
 * @note 思路：纹理已就绪、曾收到过帧、且距最近一帧未超过超时阈值，三者同时满足才算"就绪"。
 */
[[nodiscard]] bool IsVideoDisplayReady(const TextureView &video_texture,  // 纹理句柄与尺寸快照
                                       const std::chrono::steady_clock::time_point last_frame_time,  // 最近出帧时刻(可能为 min() 表示从未收到)
                                       const std::chrono::steady_clock::duration fallback_timeout,  // 无帧多久后判定不可用
                                       const std::chrono::steady_clock::time_point now) {  // 本次判断的当前时刻
    return video_texture.IsReady() // 纹理句柄与尺寸有效
           && last_frame_time != std::chrono::steady_clock::time_point::min() // 至少收到过一帧
           && (now - last_frame_time) <= fallback_timeout; // 未超过无帧超时
}

/**
 * @brief 切换 UDP 视频的 180 度旋转开关，并记录日志。
 */
void ToggleUdpVideoInversion(State &state, BackgroundServices &background_services) {  // 键鼠快捷键与界面按钮共用
    const bool udp_video_inverted = !background_services.IsUdpVideoInverted(); // 取反当前开关
    background_services.SetUdpVideoInverted(udp_video_inverted); // 应用新开关
    AppendSystemLog(state,  // 写系统日志
                    0,  // 0=普通信息
                    udp_video_inverted  // 按最终状态选择提示文案
                            ? "UDP 视频方向已切换为 180 度旋转。" // 开启提示
                            : "UDP 视频方向已切换为正常方向。"); // 关闭提示
}

}  // namespace

namespace RoboGo::app {  // 进入应用层命名空间

/**
 * @brief 解析命令行参数，生成结构化的启动选项。
 * @note 思路：先填入默认值(默认 MQTT 地址、默认视频端口、默认旋转)，再识别--mqtt_server 与 --video_port 两个参数覆盖默认值，最后解析网络端点。
 */
LaunchOptions ParseLaunchOptions(const std::vector<std::string> &args) {  // 只读入参，返回按值构造的选项
    LaunchOptions options;  // 初始化选项结构体
    options.mqtt_server = DefaultMqttServer(); // 默认 MQTT 服务器地址（配置优先，其次 192.168.12.1）
    options.video_port = "3334"; // 默认 UDP 视频端口
    options.invert_udp_video = kDefaultInvertUdpVideo; // 默认是否旋转
    options.capture_mode = "mqtt"; // 默认只抓 MQTT（图传全量抓包每场约 1GB，见 logging.h 的档位说明）

    // 抓包档位：环境变量先给一个默认值，命令行随后可覆盖（便于比赛现场临时切换）
    if (const char *capture_env = std::getenv("ROBOGO_CAPTURE"); capture_env != nullptr && capture_env[0] != '\0') {
        options.capture_mode = capture_env; // 读取环境变量档位
    }

    // --dev：本地联调快捷开关。客户端默认连 2027 裁判系统（192.168.12.1），
    // 而 tools/mock_rm_server.py 监听的是 127.0.0.1；加这个开关就不用每次手改地址。
    // 注意放在下面的参数循环之前：显式 --mqtt_server 仍然优先于 --dev。
    for (const auto &arg: args) {
        if (arg == "--dev") { // 命中开发模式开关
            options.mqtt_server = "tcp://127.0.0.1:3333"; // 指向本地模拟器
            break; // 只需要命中一次
        }
    }

    // 遍历命令行参数，识别需要携带一个值的启动选项。
    for (size_t i = 0; i < args.size(); ++i) {  // 顺序扫描，遇到带值选项时额外 ++i 跳过其值
        if (args[i] == "--mqtt_server" && i + 1 < args.size()) { // --mqtt_server <addr>
            options.mqtt_server = args[i + 1]; // 取下一个参数作为地址
            ++i; // 跳过已消费的值
        } else if (args[i] == "--video_port" && i + 1 < args.size()) { // --video_port <port>
            options.video_port = args[i + 1]; // 取下一个参数作为端口
            ++i; // 跳过已消费的值
        } else if (args[i] == "--capture" && i + 1 < args.size()) { // --capture off|mqtt|all
            options.capture_mode = args[i + 1]; // 取下一个参数作为抓包档位
            ++i; // 跳过已消费的值
        } else if (args[i] == "--video-test") { // --video-test：用合成画面替代图传
            options.video_test = true; // 打开合成视频源
        }  // if-else-if 结束(其它参数忽略)
    }

    ParseNetworkEndpoints(options); // 将字符串端点解析为 host/port
    return options; // 返回最终选项
}

/**
 * @brief 应用主入口，初始化平台后端与后台服务后进入主渲染循环。
 * @note 思路：初始化后端 -> 注册游戏输入捕获回调 -> 加载小地图纹理 -> 建立后台服务 ->每帧：消费消息/更新状态/上报控制/上传视频纹理 -> 绘制界面 -> 提交渲染；退出循环后清理图形与网络资源。
 * @return 0 表示正常退出，1 表示平台初始化失败。
 */
int RunApplication(platform::PlatformBackend &platform_backend, const LaunchOptions &options) {  // 主线程：整个渲染循环都在此函数内
    const auto main_start = std::chrono::steady_clock::now(); // 记录启动时刻(用于统计耗时)
    SPDLOG_INFO("程序启动");  // 第一条日志，便于在日志里定位一次启动
    // 打印解析后的启动参数，便于排查配置问题。
    SPDLOG_INFO("解析启动参数 mqtt_server={} video_port={} invert_udp_video={} capture={}",  // 原始参数
                options.mqtt_server,  // MQTT 服务器地址
                options.video_port,  // UDP 视频端口
                options.invert_udp_video,  // 是否旋转 UDP 视频
                options.capture_mode);  // 网络抓包档位（off/mqtt/all）
    SPDLOG_INFO("网络端点已解析 mqtt_host={} mqtt_port={} video_port={}",  // 解析后的端点
                options.mqtt_host,  // MQTT 服务器主机名
                options.mqtt_port,  // MQTT 服务器端口
                options.parsed_video_port);  // 解析后的 UDP 视频端口

    if (!platform_backend.Initialize()) { // 初始化窗口/图形设备/ImGui
        return 1; // 初始化失败直接退出
    }

    // 注册"游戏输入捕获"回调，使 UI 层可请求锁定/释放鼠标键盘。
    set_game_input_capture_handler([&platform_backend](const bool enabled) {  // 捕获后端引用：后端在主线程存活于整个函数作用域
        // 调用平台后端设置游戏输入捕获状态
        platform_backend.SetGameInputCapture(enabled);  // enabled=true 锁定输入给游戏，false 释放给 UI
    });

    State state = CreateInitialState(); // 创建全局界面/数据状态

    // 加载内嵌小地图 PNG 并上传为纹理；失败时回退为纯色背景(不致命)。
    auto mini_map_image = RoboGo::resource::LoadEmbeddedMapImage();  // expected<DecodedImage, ImageDecodeError>
    if (!mini_map_image.has_value()) {  // 资源缺失/解码失败
        SPDLOG_WARN("未能加载内嵌小地图，将回退为纯色小地图背景 reason={}",
                    RoboGo::resource::ToString(mini_map_image.error()));  // 仅告警，不影响主流程
    } else if
    // 小地图加载成功：尝试上传纹理
    // 尝试上传小地图纹理
    // 如果上传成功，记录日志
    (platform_backend.UploadTexture(TextureSlot::MiniMap, *mini_map_image)) {
        SPDLOG_INFO("小地图纹理已上传 width={} height={}", mini_map_image->width, mini_map_image->height);  // 记录尺寸，便于确认资源正确
    } else {  // 加载成功但上传失败(显存/尺寸超限等)
        SPDLOG_ERROR("创建小地图纹理失败 width={} height={}", mini_map_image->width, mini_map_image->height);  // 记录失败原因相关尺寸
    }

    // 地形分层叠加层：由 2027 场地高度掩膜上色而来（橙=高于 0.22m，蓝=0.15~0.22m，地面透明）。
    // 只是显示增强，加载失败不影响主流程（小地图退化为纯底图）。
    auto terrain_image = RoboGo::resource::LoadEmbeddedMapTerrainImage();  // 解码内嵌叠加层
    if (!terrain_image.has_value()) {  // 资源缺失/解码失败
        SPDLOG_WARN("未能加载地形分层叠加层，小地图只显示底图 reason={}",
                    RoboGo::resource::ToString(terrain_image.error()));  // 仅告警
    } else if (platform_backend.UploadTexture(TextureSlot::MiniMapTerrain, *terrain_image)) {  // 上传纹理
        SPDLOG_INFO("地形分层纹理已上传 width={} height={}", terrain_image->width, terrain_image->height);  // 记录尺寸
    } else {  // 上传失败
        SPDLOG_ERROR("创建地形分层纹理失败 width={} height={}", terrain_image->width, terrain_image->height);  // 记录失败
    }

    {   // 作用域块
        // 用作用域包裹后台服务，保证退出时先析构(停止线程)再继续收尾
        BackgroundServices background_services(options); // 启动 IO 线程、MQTT、视频流等

        // 键鼠上报线程：与渲染帧率解耦，固定周期发送 KeyboardMouseControl（2027 协议上限 75Hz）。
        // 必须在这里启动（早于主循环）且在 background_services.Stop() 之前停止，保证线程不会用到已析构的客户端。
        start_keyboard_mouse_publisher(background_services.mqtt_client());

        // 记录两路视频最近一次出帧的时间，初值 min() 表示"尚未收到帧"。
        std::chrono::steady_clock::time_point last_udp_frame_time = std::chrono::steady_clock::time_point::min();  // UDP 从未收到帧

        // 统计从启动到后台服务就绪的耗时。
        const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(  // 转毫秒
                                        std::chrono::steady_clock::now() - main_start)  // 当前时刻减去启动时刻
                                        .count();  // 取整数值
        SPDLOG_INFO("程序启动完成 startup_ms={}", startup_ms);  // 记录启动耗时

        // 主循环：如果遇到窗口关闭消息（比如用户点击关闭按钮），标记应关闭，退出循环。
        while (platform_backend.ProcessEvents()) {  
            const auto frame_start = std::chrono::steady_clock::now();  // 整帧计时起点
            // F12 开关性能浮层：只在非文本输入时响应，避免在输入框里打字误触。
            if (ImGui::IsKeyPressed(ImGuiKey_F12, false) && !ImGui::GetIO().WantTextInput) {
                perf::ToggleVisible();  // 翻转浮层显示状态
            }
            background_services.ProcessMessages(state); // 消费 MQTT 下行消息
            UpdateState(state); // 按时间推进界面/业务状态
            PublishCustomControlIfDue(state, background_services.mqtt_client()); // 到点则上报自定义控制
            UploadLatestVideoFrames(platform_backend, // 上传 UDP 视频的最新帧
                                    background_services,  // 后台服务，用于获取视频帧
                                    last_udp_frame_time);  // 最近一次 UDP 视频帧时间

            if (!platform_backend.BeginFrame()) { // 准备本帧(窗口最小化等可跳过)
                continue; // 跳过本帧渲染，下一轮继续
            }

            // 获取三个纹理槽的视图(句柄 + 尺寸)，交给界面绘制使用。
            const TextureView udp_video_texture = platform_backend.GetTextureView(TextureSlot::UdpVideo);  // UDP 视频纹理视图
            const TextureView mini_map_texture = platform_backend.GetTextureView(TextureSlot::MiniMap);  // 小地图纹理视图
            const TextureView mini_map_terrain_texture = platform_backend.GetTextureView(TextureSlot::MiniMapTerrain);  // 地形分层叠加层视图

            // 【2027 变更】英雄部署模式已删除，不再有"第二路主画面"的概念：主画面恒为 UDP 图传，
            // CustomByteBlock 解码链路已整体移除（产物本就未上屏），故现在只剩 UDP 一路。

            // 按 I 键(且非文本输入)切换 UDP 视频 180 度旋转。
            if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !ImGui::GetIO().WantTextInput) {  // 避免在输入框里打字时误触发
                ToggleUdpVideoInversion(state, background_services);  // 翻转并记录日志
            }
            // 按 M 键切换地图「地形分层」（小地图与指挥台共用）。M 不在键鼠上报的 16 个键里，不会误发给机器人。
            if (ImGui::IsKeyPressed(ImGuiKey_M, false) && !ImGui::GetIO().WantTextInput) {  // 输入框打字时不抢键
                toggle_terrain_overlay();  // 翻转并记录日志
            }

            const auto now = std::chrono::steady_clock::now();  // 本帧统一时间基准

            // 为绘制重新计算两路视频的就绪状态(供界面决定占位/提示)。
            const bool udp_video_display_ready = IsVideoDisplayReady(  // UDP 是否有画面
                    udp_video_texture,  // 纹理状态
                    last_udp_frame_time,  // 最近出帧时刻
                    kUdpVideoFallbackTimeout,  // 超时阈值
                    now);  // 当前时刻
            // 绘制整个主界面；返回值表示界面内是否请求切换视频方向。
            // 注意：计时器必须包在独立作用域里，只覆盖 ShowMainWindow 本身。
            // 如果让它活到循环末尾，就会把 ImGui::Render 和 Present 的垂直同步等待也算进来（实测会虚高到 16.6ms）。
            bool toggle_udp_video_inversion_requested = false;  // 界面是否请求切换视频方向
            {
                RoboGo::app::perf::ScopedTimer ui_timer(&RoboGo::app::perf::RecordUi);  // 统计界面构建耗时
                toggle_udp_video_inversion_requested = ShowMainWindow(  // 返回 true 表示界面按钮点了"切换方向"
                    state,  // 全局状态
                    background_services.mqtt_client(),  // 供界面直接发送消息
                    udp_video_texture.texture_ref,  // UDP 纹理句柄
                    udp_video_texture.size,  // UDP 纹理尺寸
                    udp_video_display_ready,  // UDP 是否可显示
                    background_services.IsUdpVideoInverted(),  // 当前旋转开关
                    mini_map_texture.texture_ref,  // 小地图纹理句柄
                    mini_map_texture.size,  // 小地图纹理尺寸
                    mini_map_terrain_texture.texture_ref,  // 地形分层叠加层句柄
                    mini_map_terrain_texture.size);  // 地形分层叠加层尺寸
            }
            if (toggle_udp_video_inversion_requested) {  // 界面请求切换
                ToggleUdpVideoInversion(state, background_services); // 界面请求切换方向
            }

            update_keyboard_mouse_state(background_services.mqtt_client()); // 采集并上报键鼠(75Hz 限频)

            ImGui::Render(); // 生成 ImGui 绘制数据
            platform_backend.RenderFrame(ImGui::GetDrawData()); // 提交到屏幕

            // 整帧耗时 = 事件泵 -> Present 返回。这就是操作手实际感受到的"一帧有多久"。
            perf::RecordFrame(std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - frame_start).count());
            // 队列积压：持续 >2 说明解码侧产得比渲染侧消耗得快，画面延迟会越积越大。
            perf::RecordQueues(background_services.UdpVideoQueueDepth());
        }  // 主循环结束

        SPDLOG_INFO("开始执行图形资源清理");  // 进入收尾阶段
        // 先停键鼠上报线程：它每 14ms 会调用一次 mqtt_client.publish，必须早于 MQTT 停止/客户端析构。
        stop_keyboard_mouse_publisher();  // 幂等，重复调用安全
        platform_backend.Shutdown(); // 先释放图形资源(纹理/设备/窗口)

        SPDLOG_INFO("开始关闭网络与后台线程");  // 再停后台服务：此时纹理已释放，避免回调里再上传纹理
        background_services.Stop(); // 停止网络与后台线程
        SPDLOG_INFO("程序退出完成");  // 作用域即将结束，后台服务析构
    }  // 作用域结束(background_services 在此析构)

    set_game_input_capture_handler({}); // 解除输入捕获回调
    return 0; // 正常退出
}

}  // namespace RoboGo::app
