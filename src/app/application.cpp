// application.cpp：应用程序核心——解析启动参数、搭建后台服务(MQTT / UDP 视频流 / 字节块解码线程)，驱动主渲染循环、视频纹理上传与键鼠上报。
#include "application.h"  // 本编译单元的主头文件(声明 ParseLaunchOptions / RunApplication)

#include "config.h"  // 应用级配置：MQTT 服务端地址的持久化读写
#include "keyboard_mouse_state_manager.h"  // 键鼠状态采集与管理(供 update_keyboard_mouse_state 使用)
#include "main_window.h"  // 主窗口/主界面绘制(ShowMainWindow)
#include "video/custom_byte_block_video_decoder_worker.h"  // 自定义字节块解码 worker(后台线程)
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

// 功能：去掉字符串首尾的 ASCII 空白（命令行/配置文件输入都可能带空格）。
std::string TrimAscii(std::string_view text) {
    const auto is_space = [](char character) {  // 只认空格/制表/回车/换行，避免 locale 依赖
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    };
    std::size_t begin = 0;  // 左边界
    std::size_t end = text.size();  // 右边界（开区间）
    while (begin < end && is_space(text[begin])) {  // 跳过左侧空白
        ++begin;  // 右移
    }  // 结束左侧裁剪
    while (end > begin && is_space(text[end - 1])) {  // 跳过右侧空白
        --end;  // 左移
    }  // 结束右侧裁剪
    return std::string(text.substr(begin, end - begin));  // 返回裁剪后的副本
}  // TrimAscii 结束

// 功能：返回默认的 MQTT 服务器地址。
// 思路：优先用上次成功登录后保存到用户配置里的地址；没有配置时用 2027 赛季的裁判系统地址。
//       联调本地 mock 时，在登录面板把地址改成 127.0.0.1:3333 即可，登录成功后会覆盖配置。
std::string DefaultMqttServer() {  // 返回 std::string 而非 string_view：调用方要存进 LaunchOptions 持有
    const std::string configured = RoboGo::app::LoadConfiguredMqttServer();  // 读取持久化配置（可能为空）
    if (!configured.empty()) {  // 有历史配置
        // 注意：本函数在日志系统初始化之前被调用，这里不能写日志（会落到 spdlog 默认 logger 上、且进不了日志文件）。
        // 实际使用的地址由 RunApplication 的"解析启动参数"一行统一打印。
        return "tcp://" + configured;  // 统一补上 tcp:// 前缀，交给后面的解析
    }  // 结束历史配置分支
    return "tcp://192.168.12.1:3333";  // 2027 赛季裁判系统固定地址（自定义终端 IP 为 192.168.12.2）
}  // DefaultMqttServer 结束

// 功能：从原始 MQTT 服务器字符串与视频端口字符串中解析出可用的网络端点。
// 思路：兼容 "tcp://host:port" / "host:port" / "host"（端口缺省 3333）三种写法；
//       原先只认带 tcp:// 的形式，写成 "192.168.12.1:3333" 会静默回落到 127.0.0.1，现场极难排查。
// 参数 options：启动选项(读入 mqtt_server/video_port，写出 mqtt_host/mqtt_port/parsed_video_port)。
void ParseNetworkEndpoints(LaunchOptions &options) {
    options.mqtt_host = "192.168.12.1";  // 主机默认值（2027 裁判系统）
    options.mqtt_port = "3333";  // 端口默认值
    std::string address = TrimAscii(options.mqtt_server);  // 去掉首尾空白
    if (address.starts_with("tcp://")) {  // 剥离可选的协议前缀
        address = TrimAscii(address.substr(6));  // 去掉 "tcp://" 并再次裁剪
    }  // 结束前缀剥离
    if (address.empty()) {  // 地址为空（理论上不会，默认值非空）
        // 保留默认端点；解析结果由 RunApplication 在日志初始化后统一打印，这里不写日志。
    } else {  // 地址非空
        const std::size_t colon_pos = address.rfind(':');  // 用最后一个冒号分隔端口（兼容 IPv6 的 [::1]:3333 写法）
        if (colon_pos != std::string::npos) {  // 找到冒号才拆分
            const std::string host_part = TrimAscii(address.substr(0, colon_pos));  // 冒号之前为主机
            const std::string port_part = TrimAscii(address.substr(colon_pos + 1));  // 冒号之后为端口
            if (!host_part.empty()) {  // 主机非空才采用
                options.mqtt_host = host_part;  // 写入主机
            }  // 结束主机非空判断
            if (!port_part.empty()) {  // 端口非空才采用
                options.mqtt_port = port_part;  // 写入端口
            }  // 结束端口非空判断
        } else {  // 没有冒号：整串视为主机，端口用默认值
            options.mqtt_host = address;  // 只改主机
        }  // 结束拆分分支
    }  // 结束地址非空分支

    // 视频端口：非法输入不再让程序抛异常退出，回落默认 3334。
    try {  // std::stoi 在非法输入时会抛 invalid_argument/out_of_range
        options.parsed_video_port = std::stoi(options.video_port);  // 视频端口字符串转整数
    } catch (const std::exception &) {  // 非数字/溢出
        options.parsed_video_port = 3334;  // 使用协议默认端口（解析结果稍后由启动日志打印）
    }  // 结束异常处理
    if (options.parsed_video_port <= 0 || options.parsed_video_port > 65535) {  // 端口越界
        options.parsed_video_port = 3334;  // 使用协议默认端口
    }  // 结束越界判断
}  // ParseNetworkEndpoints 结束

// AvFrameRgbaConverter —— 将 FFmpeg 解码得到的 AVFrame(YUV) 转换为 RGBA 像素缓冲，
// 并按需做 180 度旋转；内部复用 sws 缩放/色彩转换上下文以避免每帧重建。
class AvFrameRgbaConverter {  // 只在 IO 线程的帧回调里使用，故内部不加锁(单个原子开关除外)
public:  // 对外只暴露"转换"与"翻转开关"
    // 功能：构造转换器并设定初始是否做 180 度旋转。
    // 思路：保存翻转开关，sws 上下文延迟到首次 convert 时按帧尺寸惰性创建并复用。
    // 参数 invert_udp_video(原子bool)：是否对 UDP 视频做 180 度旋转。
    explicit AvFrameRgbaConverter(const bool invert_udp_video)  // explicit：避免 bool 被隐式转换为转换器
        : invert_udp_video_(invert_udp_video) {  // 直接初始化原子成员(原子的拷贝构造被删除，只能用初始化列表)
    }  // 构造函数结束(空函数体：sws 上下文留待首次 convert 创建)

    // 功能：析构时释放 sws 上下文，防止内存泄漏。
    ~AvFrameRgbaConverter() {  // 与 sws_getCachedContext 配对释放
        // SwsContext sws_context_：FFmpeg libswscale 库的核心上下文结构体，
        // 本质上是不透明句柄，封装了图像缩放、像素格式转换与色彩空间转换所需的全部参数和状态。
        if (sws_context_ != nullptr) { // 仅当上下文已创建时才释放
            // sws_freeContext：释放 SwsContext 所占资源，与 sws_getContext 配对。
            sws_freeContext(sws_context_);  // 释放内部查找表与滤镜参数，不做帧数据拷贝
            sws_context_ = nullptr; // 置空，避免悬空指针
        }  // if 结束
    }  // 析构函数结束

    // 功能：把一帧 YUV AVFrame 转换为 RGBA 的 VideoFrame。
    // 思路：校验入参 -> 准备 sws 上下文 -> 分配 RGBA 缓冲 -> sws_scale 转换 -> 按需 180 度旋转。
    // 返回：成功返回像素数据，失败/格式不支持返回 std::nullopt。
    // AVFrame（libavutil）：存放【解码之后、未压缩的原始画面/音频】的容器，即 YUV 帧载体，与 AVPacket 成对。
    //   AVPacket：压缩数据（H.264 码流包，输入给解码器）
    //   AVFrame：解码出来的裸数据（YUV/RGB，解码器输出）
    std::optional<VideoFrame> convert(const AVFrame *frame) {  // 用 optional 表达"可能失败"，避免异常穿越 FFmpeg 回调
        if (frame == nullptr) { // 空帧直接拒绝
            return std::nullopt;  // 上游解码失败时可能给出空指针
        }  // if 结束

        // 仅支持 NV12（semi-planar 半平面,） / YUV420P（I420，planar 三平面） 两种常见图传像素格式，其它格式视为不可处理。
        if (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_YUV420P) {  // 两种 4:2:0 布局，覆盖绝大多数图传编码输出
            return std::nullopt;  // 不支持则直接放弃该帧
        }  // if 结束

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
            return std::nullopt;  // 放弃该帧，下帧会再次尝试创建
        }  // if 结束

        VideoFrame video_frame;  // 输出帧：填充缓冲与宽高
        // 按 宽*高*4 字节(RGBA 每像素 4 字节)分配输出缓冲。
        video_frame.buffer.resize(static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4u);  // 先转 size_t 再乘，避免 int 溢出
        video_frame.width = frame->width; // 记录输出宽度
        video_frame.height = frame->height; // 记录输出高度
        uint8_t *dest_data[1] = {video_frame.buffer.data()}; // sws_scale 需要目标平面指针数组
        int dest_linesize[1] = {frame->width * 4}; // 目标每行字节数(紧密排列，无 padding)
        // 执行 YUV -> RGBA 的实际转换；返回转换出的行数。
        const int scaled_height = sws_scale(sws_context_,  // 转换上下文
                                            frame->data, frame->linesize,  // 源平面指针与每行字节数(YUV 各平面 stride)
                                            0, frame->height,  // 从第 0 行开始，转换整帧高度
                                            dest_data, dest_linesize);  // 目标平面指针与 stride
        // 行数不符说明转换异常
        if (scaled_height != frame->height) {  // 正常情况下应等于输入高度
            return std::nullopt;  // 转换不完整则丢弃该帧
        }  // if 结束
        // 按开关决定是否对 RGBA 缓冲做 180 度旋转(图传画面方向修正)。
        if (!RoboGo::video::ApplyRgba180Rotation(video_frame.buffer,  // 就地旋转 RGBA 缓冲
                                                 video_frame.width,  // 宽(像素)
                                                 video_frame.height,  // 高(像素)
                                                 invert_udp_video_.load(std::memory_order_relaxed))) {  // relaxed：仅读一个开关，无跨变量同步需求
            return std::nullopt;  // 旋转失败(如尺寸与缓冲不匹配)同样丢弃
        }  // if 结束
        return video_frame; // 返回转换完成的帧
    }  // convert 结束

    // 功能：运行时切换是否做 180 度旋转。
    void SetInvertUdpVideo(const bool enabled) {  // UI 线程调用，IO 线程读取
        invert_udp_video_.store(enabled, std::memory_order_relaxed); // 原子写入，relaxed 足够(仅传递开关)
    }  // SetInvertUdpVideo 结束

    // 功能：查询当前是否启用 180 度旋转。
    [[nodiscard]] bool IsInvertUdpVideoEnabled() const {  // nodiscard：返回值有意义，不应被忽略
        return invert_udp_video_.load(std::memory_order_relaxed); // 原子读取当前开关值
    }  // IsInvertUdpVideoEnabled 结束

private:  // 内部状态
    SwsContext *sws_context_ = nullptr; // sws 上下文(惰性创建并复用)
    std::atomic<bool> invert_udp_video_ = false; // 180 度旋转开关(跨线程读写)
};  // AvFrameRgbaConverter 类定义结束

// BackgroundServices —— 汇总所有后台线程与网络/视频服务：
// IO 线程(Asio)、MQTT 客户端、UDP 视频流管理器、自定义字节块解码 worker，
// 以及它们与主线程之间的无锁队列；主线程通过本类取出消息与最新视频帧。
class BackgroundServices {  // 生命周期与 RunApplication 中的作用域块绑定，退出时析构即停服
public:  // 构造初始化各组件，之后由主循环调用 Process*/Take* 接口
    // 功能：构造后台服务，按启动选项初始化各组件并启动 IO 线程。
    // 思路：成员初始化列表中依次构造转换器、解码 worker、MQTT 客户端、视频流管理器，
    // 最后创建一个专用线程运行 Asio io_context。
    explicit BackgroundServices(const LaunchOptions &options)  // explicit：防止 LaunchOptions 被隐式转换
        :   udp_frame_converter_(options.invert_udp_video), // AvFrameRgbaConverterUDP udp_frame_converter_帧转换器(带初始旋转开关)
            custom_byte_block_decoder_worker_(  // 自定义字节块解码 worker：内部自带线程
                  // 解码 worker 的帧回调：把解出的 RGBA 帧包装成 VideoFrame 放入队列。
                  [this](RoboGo::video::CustomByteBlockDecodedFrame &&decoded_frame) {  // 右值引用接收，便于直接窃取像素缓冲
                      VideoFrame custom_video_frame;// 自定义视频帧
                      custom_video_frame.buffer = std::move(decoded_frame.rgba); // 转移像素缓冲所有权
                      custom_video_frame.width = decoded_frame.width; // 记录输出宽度
                      custom_video_frame.height = decoded_frame.height; // 记录输出高度
                      custom_video_frame_queue_.emplace(std::move(custom_video_frame)); // 无锁入队最新帧
                  },  // 帧回调结束
                  // 解码 worker 的日志回调：包装成 UI 日志消息入队，交由主线程消费。
                  [this](const int type, const std::string &text) {  // 在 worker 线程执行
                      UiLogMessage log_message;// 日志消息
                      log_message.type = type; // 日志类型
                      log_message.text = text; // 日志文本
                      custom_video_log_queue_.emplace(std::move(log_message)); // 日志入队
                  }),  // 日志回调结束，worker 构造参数结束

            // MQTT 客户端：连接裁判系统；收到消息时投递到主线程消息队列。
            mqtt_client_(io_context_,  // 共用本类的 io_context
                        options.mqtt_host,  // 解析后的 broker 主机
                        options.mqtt_port,  // 解析后的 broker 端口
                        [this](const std::string &topic, const std::string &payload) {  // 下行报文回调(在 IO 线程执行)
                            message_queue_.emplace(topic, payload); // (topic, payload) 原样入队
                        }),  // 回调结束

            // UDP 视频流管理器：绑定 0.0.0.0:video_port 接收图传；每解出一帧调用回调。
            video_stream_manager_(io_context_,  // 同一个执行器，避免多线程竞争
                                    boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), options.parsed_video_port),  // 监听所有网卡的指定端口
                                    [this](const AVFrame *frame)  // 每解出一帧(裸 AVFrame)调用一次(IO 线程)
            {  // 回调体
                const auto video_frame = udp_frame_converter_.convert(frame); // YUV -> RGBA(+旋转)
                if (video_frame.has_value()) {  // 转换成功
                    video_frame_queue_.emplace(video_frame.value()); // 转换成功则入队
                } else if (frame == nullptr) {  // 空帧指针
                    SPDLOG_ERROR("收到空视频帧指针，已丢弃"); // 空帧
                } else {  // 有帧但转换失败
                    SPDLOG_ERROR("UDP 视频帧转换或方向处理失败 format={} width={} height={}",  // 记录格式与尺寸，便于定位不支持的编码
                                frame->format,  // 像素格式枚举值
                                frame->width,  // 帧宽
                                frame->height); // 格式/转换失败
                }  // if-else 链结束
            }),  // 帧回调结束

            // IO 线程：运行 Asio 事件循环，承接 MQTT 与 UDP 的异步操作。
            io_thread_([this]() {  // 新线程入口：整个 IO 线程都跑在 io_context_.run() 里
                SPDLOG_INFO("IO线程已启动");  // 标记后台线程就绪
                io_context_.run(); // 阻塞运行，直到 io_context_.stop()
                SPDLOG_INFO("IO线程已退出");  // 说明 stop() 已生效，线程即将结束
            }) {  // 初始化列表结束，进入构造函数体
    }  // 构造函数结束(全部工作在初始化列表中完成)

    // 功能：析构时确保后台服务全部停止、线程回收。
    ~BackgroundServices() {  // 保证异常/提前 return 时也能停服，不残留线程
        Stop();// 确保后台服务停止
    }  // 析构函数结束

    // 禁止拷贝：本类持有线程与 socket 等不可复制资源。
    BackgroundServices(const BackgroundServices &) = delete;  // 删除拷贝构造
    BackgroundServices &operator=(const BackgroundServices &) = delete;  // 删除拷贝赋值

    // 功能：暴露 MQTT 客户端引用，供主循环发送消息。
    RoboGo::io::MqttClient &mqtt_client() {  // 返回引用，避免拷贝且允许外部调用 publish()
        return mqtt_client_;  // 成员长期存活于本对象
    }  // mqtt_client 结束

    // 功能：设置 UDP 视频是否 180 度旋转。
    void SetUdpVideoInverted(const bool enabled) {  // 转调转换器内部的原子开关
        udp_frame_converter_.SetInvertUdpVideo(enabled);  // IO 线程下一帧起生效
    }  // SetUdpVideoInverted 结束

    // 功能：查询 UDP 视频是否 180 度旋转。
    [[nodiscard]] bool IsUdpVideoInverted() const {  // const：查询不改状态
        return udp_frame_converter_.IsInvertUdpVideoEnabled();  // 读取原子开关
    }  // IsUdpVideoInverted 结束

    // 功能：在主线程中消费所有已到达的 MQTT 消息。
    // 思路：循环从无锁队列取出消息交给 HandleMqttMessage 分发；
    //       若消息携带自定义字节块数据，则转交解码 worker。
    void ProcessMessages(State &state) {  // 每帧调用一次，必须取空以免积压
        std::pair<std::string, std::string> message;  // 复用的出参，避免每条消息都构造新对象
        while (message_queue_.try_dequeue(message)) { // 直到队列取空
            HandleMqttMessage(state,               // 状态引用，避免拷贝
                            mqtt_client_,             // 客户端引用，避免拷贝
                            message.first,             // 话题，避免拷贝
                            message.second,            // 消息，引用，避免拷贝
                            [this](std::span<const uint8_t> packet_bytes) {  // 按 topic 分发；自定义字节块走最后一个回调
                                custom_byte_block_decoder_worker_.enqueue_packet_bytes(packet_bytes); // 字节块包交给解码 worker
                });  // HandleMqttMessage 调用结束
        }  // while 结束
    }  // ProcessMessages 结束

    // 功能：把解码 worker 产生的日志搬进 UI 日志列表。
    void DrainCustomVideoLogs(State &state) {  // 每帧调用一次
        UiLogMessage log_message;  // 复用出参
        while (custom_video_log_queue_.try_dequeue(log_message)) { // 排空日志队列
            AppendVehicleLog(state, log_message.type, log_message.text); // 追加到车辆日志
        }  // while 结束
    }  // DrainCustomVideoLogs 结束

    // 功能：取出最新的 UDP 视频帧(丢弃积压旧帧，只保留最后一帧)。
    std::optional<VideoFrame> TakeLatestVideoFrame() {  // 无新帧时返回 nullopt
        return TakeLatestFrame(video_frame_queue_);  // 复用统一的"取最新"逻辑
    }  // TakeLatestVideoFrame 结束

    // 功能：取出最新的自定义字节块视频帧。
    std::optional<VideoFrame> TakeLatestCustomVideoFrame() {  // 与 UDP 帧队列相互独立
        return TakeLatestFrame(custom_video_frame_queue_);  // 同样只保留最新一帧
    }  // TakeLatestCustomVideoFrame 结束

    // 功能：按顺序停止各服务并回收 IO 线程。
    // 思路：置停止标志 -> 停 MQTT/视频流 -> stop io_context -> join IO 线程 -> 停解码 worker。
    void Stop() {  // 幂等：析构与显式调用都可安全触发
        if (stopped_) { // 保证只执行一次
            return;  // 已停过则直接返回
        }  // if 结束
        stopped_ = true;// 标记已停止
        mqtt_client_.stop(); // 停止 MQTT
        video_stream_manager_.stop(); // 停止 UDP 视频接收
        io_context_.stop(); // 让 io_context.run() 返回
        if (io_thread_.joinable()) {  // 线程可能已被回收
            io_thread_.join(); // 等待 IO 线程退出
        }  // if 结束
        custom_byte_block_decoder_worker_.stop(); // 最后停解码 worker
    }  // Stop 结束

private:  // 内部工具与成员
    // 功能：从队列中取出最后一帧(排空队列，仅保留最新)。
    static std::optional<VideoFrame> TakeLatestFrame(moodycamel::ReaderWriterQueue<VideoFrame> &queue) {  // static：不依赖实例状态
        std::optional<VideoFrame> latest_frame;  // 为空表示队列本来就空
        VideoFrame video_frame;  // 复用的中转对象
        // try_dequeue 会阻塞，直到有新帧可用或队列为空
        while (queue.try_dequeue(video_frame)) { // 持续取，直到取空
            latest_frame = std::move(video_frame); // 始终用更新的帧覆盖
        }  // while 结束
        return latest_frame;  // 返回最后一帧(可能为空)
    }  // TakeLatestFrame 结束

    boost::asio::io_context io_context_; // Asio 事件循环
    moodycamel::ReaderWriterQueue<std::pair<std::string, std::string>> message_queue_; // MQTT 消息队列
    moodycamel::ReaderWriterQueue<VideoFrame> video_frame_queue_; // UDP 视频帧队列
    moodycamel::ReaderWriterQueue<VideoFrame> custom_video_frame_queue_; // 自定义视频帧队列
    moodycamel::ReaderWriterQueue<UiLogMessage> custom_video_log_queue_; // 自定义视频日志队列
    AvFrameRgbaConverter udp_frame_converter_; // YUV->RGBA 转换器
    RoboGo::video::CustomByteBlockVideoDecoderWorker custom_byte_block_decoder_worker_; // 字节块解码线程
    RoboGo::io::MqttClient mqtt_client_; // MQTT 客户端
    RoboGo::video::VideoStreamManager video_stream_manager_; // UDP 视频流管理器
    std::thread io_thread_; // IO 线程
    bool stopped_ = false; // 是否已停止
};  // BackgroundServices 类定义结束

// 功能：取出两路视频各自最新的帧并上传为 GPU 纹理，同时记录最近出帧时间。
// 思路：UDP 帧与自定义字节块帧分别处理；成功上传则刷新对应的时间戳，
//       供后续判断画面是否"仍在刷新"(超时回退逻辑使用)。
void UploadLatestVideoFrames(RoboGo::platform::PlatformBackend &platform_backend,  // 负责实际上传纹理到 GPU
                             BackgroundServices &background_services,  // 帧来源
                             std::chrono::steady_clock::time_point &last_udp_frame_time,  // 出参：最近一次 UDP 出帧时刻
                             std::chrono::steady_clock::time_point &last_custom_frame_time) {  // 出参：最近一次自定义视频出帧时刻
    const auto video_frame = background_services.TakeLatestVideoFrame(); // 取最新 UDP 帧
    if (video_frame.has_value()) {  // 本帧有新画面才上传
        last_udp_frame_time = std::chrono::steady_clock::now(); // 刷新 UDP 出帧时间
        if (!platform_backend.UploadTexture(TextureSlot::UdpVideo, *video_frame)) { // 上传到 UDP 视频纹理槽
            SPDLOG_ERROR("创建UDP视频纹理失败 width={} height={}", video_frame->width, video_frame->height);  // 上传失败记录尺寸，便于排查显存/尺寸问题
        }  // if 结束
    }  // if 结束

    const auto custom_video_frame = background_services.TakeLatestCustomVideoFrame(); // 取最新自定义视频帧
    if (custom_video_frame.has_value()) {  // 有解码成功的新帧
        last_custom_frame_time = std::chrono::steady_clock::now(); // 刷新自定义视频出帧时间
        if (!platform_backend.UploadTexture(TextureSlot::CustomVideo, *custom_video_frame)) { // 上传到自定义视频纹理槽
            SPDLOG_ERROR("创建 CustomByteBlock 视频纹理失败 width={} height={}",  // 记录失败尺寸
                         custom_video_frame->width,  // 帧宽
                         custom_video_frame->height);  // 帧高
        }  // if 结束
    }  // if 结束
}  // UploadLatestVideoFrames 结束

// 功能：判断某一路视频当前是否可正常显示。
// 思路：纹理已就绪、曾收到过帧、且距最近一帧未超过超时阈值，三者同时满足才算"就绪"。
[[nodiscard]] bool IsVideoDisplayReady(const TextureView &video_texture,  // 纹理句柄与尺寸快照
                                       const std::chrono::steady_clock::time_point last_frame_time,  // 最近出帧时刻(可能为 min() 表示从未收到)
                                       const std::chrono::steady_clock::duration fallback_timeout,  // 无帧多久后判定不可用
                                       const std::chrono::steady_clock::time_point now) {  // 本次判断的当前时刻
    return video_texture.IsReady() // 纹理句柄与尺寸有效
           && last_frame_time != std::chrono::steady_clock::time_point::min() // 至少收到过一帧
           && (now - last_frame_time) <= fallback_timeout; // 未超过无帧超时
}  // IsVideoDisplayReady 结束

// 功能：切换 UDP 视频的 180 度旋转开关，并记录日志。
void ToggleUdpVideoInversion(State &state, BackgroundServices &background_services) {  // 键鼠快捷键与界面按钮共用
    const bool udp_video_inverted = !background_services.IsUdpVideoInverted(); // 取反当前开关
    background_services.SetUdpVideoInverted(udp_video_inverted); // 应用新开关
    AppendSystemLog(state,  // 写系统日志
                    0,  // 0=普通信息
                    udp_video_inverted  // 按最终状态选择提示文案
                            ? "UDP 视频方向已切换为 180 度旋转。" // 开启提示
                            : "UDP 视频方向已切换为正常方向。"); // 关闭提示
}  // ToggleUdpVideoInversion 结束

}// namespace  // 匿名命名空间结束

namespace RoboGo::app {  // 进入应用层命名空间

// 功能：解析命令行参数，生成结构化的启动选项。
// 思路：先填入默认值(默认 MQTT 地址、默认视频端口、默认旋转)，再识别
//       --mqtt_server 与 --video_port 两个参数覆盖默认值，最后解析网络端点。
LaunchOptions ParseLaunchOptions(const std::vector<std::string> &args) {  // 只读入参，返回按值构造的选项
    LaunchOptions options;  // 初始化选项结构体
    options.mqtt_server = DefaultMqttServer(); // 默认 MQTT 服务器地址（配置优先，其次 192.168.12.1）
    options.video_port = "3334"; // 默认 UDP 视频端口
    options.invert_udp_video = kDefaultInvertUdpVideo; // 默认是否旋转
    options.capture_mode = "mqtt"; // 默认只抓 MQTT（图传全量抓包每场约 1GB，见 logging.h 的档位说明）

    // 抓包档位：环境变量先给一个默认值，命令行随后可覆盖（便于比赛现场临时切换）
    if (const char *capture_env = std::getenv("ROBOGO_CAPTURE"); capture_env != nullptr && capture_env[0] != '\0') {
        options.capture_mode = capture_env; // 读取环境变量档位
    } // 结束环境变量判断

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
        }  // if-else-if 结束(其它参数忽略)
    }  // for 结束

    ParseNetworkEndpoints(options); // 将字符串端点解析为 host/port
    return options; // 返回最终选项
}  // ParseLaunchOptions 结束

// 功能：应用主入口，初始化平台后端与后台服务后进入主渲染循环。
// 思路：初始化后端 -> 注册游戏输入捕获回调 -> 加载小地图纹理 -> 建立后台服务 ->
//       每帧：消费消息/更新状态/上报控制/上传视频纹理 -> 绘制界面 -> 提交渲染；
//       退出循环后清理图形与网络资源。
// 返回：0 表示正常退出，1 表示平台初始化失败。
int RunApplication(platform::PlatformBackend &platform_backend, const LaunchOptions &options) {  // 主线程：整个渲染循环都在此函数内
    const auto main_start = std::chrono::steady_clock::now(); // 记录启动时刻(用于统计耗时)
    SPDLOG_INFO("程序启动");  // 第一条日志，便于在日志里定位一次启动
    // 打印解析后的启动参数，便于排查配置问题。
    SPDLOG_INFO("解析启动参数 mqtt_server={} video_port={} invert_udp_video={} capture={}",  // 原始参数
                options.mqtt_server,// MQTT 服务器地址
                options.video_port,// UDP 视频端口
                options.invert_udp_video,// 是否旋转 UDP 视频
                options.capture_mode);// 网络抓包档位（off/mqtt/all）
    SPDLOG_INFO("网络端点已解析 mqtt_host={} mqtt_port={} video_port={}",  // 解析后的端点
                options.mqtt_host,// MQTT 服务器主机名
                options.mqtt_port,// MQTT 服务器端口
                options.parsed_video_port);// 解析后的 UDP 视频端口

    if (!platform_backend.Initialize()) { // 初始化窗口/图形设备/ImGui
        return 1; // 初始化失败直接退出
    }  // if 结束

    // 注册"游戏输入捕获"回调，使 UI 层可请求锁定/释放鼠标键盘。
    set_game_input_capture_handler([&platform_backend](const bool enabled) {  // 捕获后端引用：后端在主线程存活于整个函数作用域
        // 调用平台后端设置游戏输入捕获状态
        platform_backend.SetGameInputCapture(enabled);  // enabled=true 锁定输入给游戏，false 释放给 UI
    });  // 注册结束

    State state = CreateInitialState(); // 创建全局界面/数据状态

    // 加载内嵌小地图 PNG 并上传为纹理；失败时回退为纯色背景(不致命)。
    auto mini_map_image = RoboGo::resource::LoadEmbeddedMapImage();  // 返回 optional<VideoFrame>：内嵌资源解码结果
    if (!mini_map_image.has_value()) {  // 资源缺失/解码失败
        SPDLOG_WARN("未能加载内嵌小地图，将回退为纯色小地图背景");  // 仅告警，不影响主流程
    } else if 
    // 小地图加载成功：尝试上传纹理
    // 尝试上传小地图纹理
    // 如果上传成功，记录日志
    (platform_backend.UploadTexture(TextureSlot::MiniMap, *mini_map_image)) {
        SPDLOG_INFO("小地图纹理已上传 width={} height={}", mini_map_image->width, mini_map_image->height);  // 记录尺寸，便于确认资源正确
    } else {  // 加载成功但上传失败(显存/尺寸超限等)
        SPDLOG_ERROR("创建小地图纹理失败 width={} height={}", mini_map_image->width, mini_map_image->height);  // 记录失败原因相关尺寸
    }  // if-else-if 结束

    {   // 作用域块
        // 用作用域包裹后台服务，保证退出时先析构(停止线程)再继续收尾
        BackgroundServices background_services(options); // 启动 IO 线程、MQTT、视频流等

        // 键鼠上报线程：与渲染帧率解耦，固定周期发送 KeyboardMouseControl（2027 协议上限 75Hz）。
        // 必须在这里启动（早于主循环）且在 background_services.Stop() 之前停止，保证线程不会用到已析构的客户端。
        start_keyboard_mouse_publisher(background_services.mqtt_client());
        
        // 记录两路视频最近一次出帧的时间，初值 min() 表示"尚未收到帧"。
        std::chrono::steady_clock::time_point last_udp_frame_time = std::chrono::steady_clock::time_point::min();  // UDP 从未收到帧
        std::chrono::steady_clock::time_point last_custom_frame_time = std::chrono::steady_clock::time_point::min();  // 自定义视频从未收到帧

        // 统计从启动到后台服务就绪的耗时。
        const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(  // 转毫秒
                                        std::chrono::steady_clock::now() - main_start)  // 当前时刻减去启动时刻
                                        .count();  // 取整数值
        SPDLOG_INFO("程序启动完成 startup_ms={}", startup_ms);  // 记录启动耗时

        // 主循环：如果遇到窗口关闭消息（比如用户点击关闭按钮），标记应关闭，退出循环。 
        while (platform_backend.ProcessEvents()) {  // 泵一次平台事件(窗口消息/输入)
            background_services.ProcessMessages(state); // 消费 MQTT 下行消息
            background_services.DrainCustomVideoLogs(state); // 搬运自定义视频日志
            UpdateState(state); // 按时间推进界面/业务状态
            PublishCustomControlIfDue(state, background_services.mqtt_client()); // 到点则上报自定义控制
            UploadLatestVideoFrames(platform_backend, // 上传两路视频的最新帧
                                    background_services,// 后台服务，用于获取视频帧
                                    last_udp_frame_time,// 最近一次 UDP 视频帧时间
                                    last_custom_frame_time// 最近一次自定义视频帧时间
                                );  // 上传结束

            if (!platform_backend.BeginFrame()) { // 准备本帧(窗口最小化等可跳过)
                continue; // 跳过本帧渲染，下一轮继续
            }  // if 结束

            // 获取三个纹理槽的视图(句柄 + 尺寸)，交给界面绘制使用。
            const TextureView udp_video_texture = platform_backend.GetTextureView(TextureSlot::UdpVideo);  // UDP 视频纹理视图
            const TextureView custom_video_texture = platform_backend.GetTextureView(TextureSlot::CustomVideo);  // 自定义视频纹理视图
            const TextureView mini_map_texture = platform_backend.GetTextureView(TextureSlot::MiniMap);  // 小地图纹理视图

            // 【2027 变更】英雄部署模式已删除，不再有"第二路主画面"的概念：
            // 主画面恒为 UDP 图传，CustomByteBlock 视频流只作为左栏的次画面预览，不再参与切换。

            // 按 I 键(且非文本输入)切换 UDP 视频 180 度旋转。
            if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !ImGui::GetIO().WantTextInput) {  // 避免在输入框里打字时误触发
                ToggleUdpVideoInversion(state, background_services);  // 翻转并记录日志
            }  // if 结束

            const auto now = std::chrono::steady_clock::now();  // 本帧统一时间基准
            
            // 为绘制重新计算两路视频的就绪状态(供界面决定占位/提示)。
            const bool udp_video_display_ready = IsVideoDisplayReady(  // UDP 是否有画面
                    udp_video_texture,  // 纹理状态
                    last_udp_frame_time,  // 最近出帧时刻
                    kUdpVideoFallbackTimeout,  // 超时阈值
                    now);  // 当前时刻
            const bool custom_video_display_ready = IsVideoDisplayReady(  // 自定义视频是否有画面
                    custom_video_texture,  // 纹理状态
                    last_custom_frame_time,  // 最近出帧时刻
                    kCustomVideoFallbackTimeout,  // 超时阈值
                    now);  // 当前时刻

            // 绘制整个主界面；返回值表示界面内是否请求切换视频方向。
            const bool toggle_udp_video_inversion_requested = ShowMainWindow(  // 返回 true 表示界面按钮点了"切换方向"
                    state,  // 全局状态
                    background_services.mqtt_client(),  // 供界面直接发送消息
                    udp_video_texture.texture_ref,  // UDP 纹理句柄
                    udp_video_texture.size,  // UDP 纹理尺寸
                    custom_video_texture.texture_ref,  // 自定义纹理句柄
                    custom_video_texture.size,  // 自定义纹理尺寸
                    udp_video_display_ready,  // UDP 是否可显示
                    custom_video_display_ready,  // 自定义是否可显示
                    background_services.IsUdpVideoInverted(),  // 当前旋转开关
                    mini_map_texture.texture_ref,  // 小地图纹理句柄
                    mini_map_texture.size);  // 小地图纹理尺寸
            if (toggle_udp_video_inversion_requested) {  // 界面请求切换
                ToggleUdpVideoInversion(state, background_services); // 界面请求切换方向
            }  // if 结束

            update_keyboard_mouse_state(background_services.mqtt_client()); // 采集并上报键鼠(75Hz 限频)

            ImGui::Render(); // 生成 ImGui 绘制数据
            platform_backend.RenderFrame(ImGui::GetDrawData()); // 提交到屏幕
        }  // 主循环结束

        SPDLOG_INFO("开始执行图形资源清理");  // 进入收尾阶段
        // 先停键鼠上报线程：它每 14ms 会调用一次 mqtt_client.publish，必须早于 MQTT 停止/客户端析构。
        stop_keyboard_mouse_publisher();  // 幂等，重复调用安全
        platform_backend.Shutdown(); // 先释放图形资源(纹理/设备/窗口)

        SPDLOG_INFO("开始关闭网络与后台线程");  // 再停后台服务：此时纹理已释放，避免回调里再上传纹理
        background_services.Stop(); // 停止网络与后台线程
        background_services.DrainCustomVideoLogs(state); // 收尾：把剩余日志刷入界面
        SPDLOG_INFO("程序退出完成");  // 作用域即将结束，后台服务析构
    }  // 作用域结束(background_services 在此析构)

    set_game_input_capture_handler({}); // 解除输入捕获回调
    return 0; // 正常退出
}  // RunApplication 结束

}// namespace RoboGo::app  // 应用层命名空间结束
