// application.cpp：应用程序核心——解析启动参数、搭建后台服务(MQTT / UDP 视频流 / 字节块解码线程)，驱动主渲染循环、视频纹理上传与键鼠上报。
#include "application.h"

#include "keyboard_mouse_state_manager.h"
#include "main_window.h"
#include "video/custom_byte_block_video_decoder_worker.h"
#include "video/rgba_image_transform.h"
#include "video/video_stream_manager.h"

// 应用核心实现：解析启动参数、建立后台服务(IO 线程/MQTT/UDP 视频流/自定义字节块解码 worker)、
// 运行主渲染循环、将视频帧上传为 GPU 纹理，并按需上报键鼠控制与自定义控制载荷。
namespace {

// 引入本文件所需的 RoboGo::app 类型别名，简化后续书写。
using RoboGo::app::LaunchOptions;
using RoboGo::app::TextureSlot;
using RoboGo::app::TextureView;
using RoboGo::app::UiLogMessage;
using RoboGo::app::VideoFrame;

constexpr bool kDefaultInvertUdpVideo = false; // UDP 视频是否默认做 180 度旋转

// 功能：返回默认的 MQTT 服务器地址。
// 思路：以 RoboSouls 本地 broker 为默认值，另保留正式比赛与自部署地址作为注释备用。
std::string DefaultMqttServer() {
    return "tcp://127.0.0.1:3333";// RoboSouls 本地 broker（联调默认）
    // return "tcp://192.168.12.1:3333";// 正式比赛裁判系统
    // return "tcp://192.168.1.2:3333";// 自己部署的裁判系统
}

// 功能：从原始 MQTT 服务器字符串与视频端口字符串中解析出可用的网络端点。
// 思路：先给出主机/端口默认值，若服务器地址以 "tcp://" 开头则剥离协议头并按 ':' 拆分
//       主机与端口；最后将视频端口字符串转为整数。
// 参数 options：启动选项(读入 mqtt_server/video_port，写出 mqtt_host/mqtt_port/parsed_video_port)。
void ParseNetworkEndpoints(LaunchOptions &options) {
    options.mqtt_host = "127.0.0.1"; // 主机默认值(本地回环)
    options.mqtt_port = "3333"; // 端口默认值
    if (options.mqtt_server.starts_with("tcp://")) { // 仅处理 tcp 协议形式的地址
        std::string address = options.mqtt_server.substr(6); // 剥离 "tcp://" 前缀
        const size_t colon_pos = address.find(':'); // 查找主机与端口的分隔符
        if (colon_pos != std::string::npos) {
            options.mqtt_host = address.substr(0, colon_pos); // 取 ':' 之前为主机
            options.mqtt_port = address.substr(colon_pos + 1); // 取 ':' 之后为端口
        }
    }
    //std::stoi表示将字符串转换为整数，将options.video_port转换为整数
    options.parsed_video_port = std::stoi(options.video_port); // 视频端口字符串转整数
}

// AvFrameRgbaConverter —— 将 FFmpeg 解码得到的 AVFrame(YUV) 转换为 RGBA 像素缓冲，
// 并按需做 180 度旋转；内部复用 sws 缩放/色彩转换上下文以避免每帧重建。
class AvFrameRgbaConverter {
public:
    // 功能：构造转换器并设定初始是否做 180 度旋转。
    // 思路：保存翻转开关，sws 上下文延迟到首次 convert 时按帧尺寸惰性创建并复用。
    explicit AvFrameRgbaConverter(const bool invert_udp_video)
        : invert_udp_video_(invert_udp_video) {
    }

    // 功能：析构时释放 sws 上下文，防止内存泄漏。
    ~AvFrameRgbaConverter() {
        if (sws_context_ != nullptr) { // 仅当上下文已创建时才释放
            sws_freeContext(sws_context_);
            sws_context_ = nullptr; // 置空，避免悬空指针
        }
    }

    // 功能：把一帧 YUV AVFrame 转换为 RGBA 的 VideoFrame。
    // 思路：校验入参 -> 准备 sws 上下文 -> 分配 RGBA 缓冲 -> sws_scale 转换 -> 按需 180 度旋转。
    // 返回：成功返回像素数据，失败/格式不支持返回 std::nullopt。
    std::optional<VideoFrame> convert(const AVFrame *frame) {
        if (frame == nullptr) { // 空帧直接拒绝
            return std::nullopt;
        }

        // 仅支持 NV12 / YUV420P 两种常见图传像素格式，其它格式视为不可处理。
        if (frame->format != AV_PIX_FMT_NV12 && frame->format != AV_PIX_FMT_YUV420P) {
            return std::nullopt;
        }

        // 获取或复用 sws 上下文：源为当前 YUV 格式，目标为同尺寸 RGBA。
        // 使用 SWS_FAST_BILINEAR 保证实时性；sws_getCachedContext 在参数不变时会直接复用旧上下文。
        sws_context_ = sws_getCachedContext(
                sws_context_,
                frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (sws_context_ == nullptr) { // 上下文创建失败(如内存不足)
            return std::nullopt;
        }

        VideoFrame video_frame;
        // 按 宽*高*4 字节(RGBA 每像素 4 字节)分配输出缓冲。
        video_frame.buffer.resize(static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height) * 4u);
        video_frame.width = frame->width; // 记录输出宽度
        video_frame.height = frame->height; // 记录输出高度
        uint8_t *dest_data[1] = {video_frame.buffer.data()}; // sws_scale 需要目标平面指针数组
        int dest_linesize[1] = {frame->width * 4}; // 目标每行字节数(紧密排列，无 padding)
        // 执行 YUV -> RGBA 的实际转换；返回转换出的行数。
        const int scaled_height = sws_scale(sws_context_,
                                            frame->data, frame->linesize,
                                            0, frame->height,
                                            dest_data, dest_linesize);
        if (scaled_height != frame->height) { // 行数不符说明转换异常
            return std::nullopt;
        }
        // 按开关决定是否对 RGBA 缓冲做 180 度旋转(图传画面方向修正)。
        if (!RoboGo::video::ApplyRgba180Rotation(video_frame.buffer,
                                                 video_frame.width,
                                                 video_frame.height,
                                                 invert_udp_video_.load(std::memory_order_relaxed))) {
            return std::nullopt;
        }
        return video_frame; // 返回转换完成的帧
    }

    // 功能：运行时切换是否做 180 度旋转。
    void SetInvertUdpVideo(const bool enabled) {
        invert_udp_video_.store(enabled, std::memory_order_relaxed); // 原子写入，relaxed 足够(仅传递开关)
    }

    // 功能：查询当前是否启用 180 度旋转。
    [[nodiscard]] bool IsInvertUdpVideoEnabled() const {
        return invert_udp_video_.load(std::memory_order_relaxed); // 原子读取当前开关值
    }

private:
    SwsContext *sws_context_ = nullptr; // sws 上下文(惰性创建并复用)
    std::atomic<bool> invert_udp_video_ = false; // 180 度旋转开关(跨线程读写)
};

// BackgroundServices —— 汇总所有后台线程与网络/视频服务：
// IO 线程(Asio)、MQTT 客户端、UDP 视频流管理器、自定义字节块解码 worker，
// 以及它们与主线程之间的无锁队列；主线程通过本类取出消息与最新视频帧。
class BackgroundServices {
public:
    // 功能：构造后台服务，按启动选项初始化各组件并启动 IO 线程。
    // 思路：成员初始化列表中依次构造转换器、解码 worker、MQTT 客户端、视频流管理器，
    //       最后创建一个专用线程运行 Asio io_context。
    explicit BackgroundServices(const LaunchOptions &options)
        : udp_frame_converter_(options.invert_udp_video), // UDP 帧转换器(带初始旋转开关)
          custom_byte_block_decoder_worker_(
                  // 解码 worker 的帧回调：把解出的 RGBA 帧包装成 VideoFrame 放入队列。
                  [this](RoboGo::video::CustomByteBlockDecodedFrame &&decoded_frame) {
                      VideoFrame custom_video_frame;
                      custom_video_frame.buffer = std::move(decoded_frame.rgba); // 转移像素缓冲所有权
                      custom_video_frame.width = decoded_frame.width;
                      custom_video_frame.height = decoded_frame.height;
                      custom_video_frame_queue_.emplace(std::move(custom_video_frame)); // 无锁入队最新帧
                  },
                  // 解码 worker 的日志回调：包装成 UI 日志消息入队，交由主线程消费。
                  [this](const int type, const std::string &text) {
                      UiLogMessage log_message;
                      log_message.type = type;
                      log_message.text = text;
                      custom_video_log_queue_.emplace(std::move(log_message)); // 日志入队
                  }),
          // MQTT 客户端：连接裁判系统；收到消息时投递到主线程消息队列。
          mqtt_client_(io_context_, options.mqtt_host, options.mqtt_port, [this](const std::string &topic, const std::string &payload) {
              message_queue_.emplace(topic, payload); // (topic, payload) 原样入队
          }),
          // UDP 视频流管理器：绑定 0.0.0.0:video_port 接收图传；每解出一帧调用回调。
          video_stream_manager_(io_context_, boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), options.parsed_video_port), [this](const AVFrame *frame) {
              const auto video_frame = udp_frame_converter_.convert(frame); // YUV -> RGBA(+旋转)
              if (video_frame.has_value()) {
                  video_frame_queue_.emplace(video_frame.value()); // 转换成功则入队
              } else if (frame == nullptr) {
                  SPDLOG_ERROR("收到空视频帧指针，已丢弃"); // 空帧
              } else {
                  SPDLOG_ERROR("UDP 视频帧转换或方向处理失败 format={} width={} height={}",
                               frame->format,
                               frame->width,
                               frame->height); // 格式/转换失败
              }
          }),
          // IO 线程：运行 Asio 事件循环，承接 MQTT 与 UDP 的异步操作。
          io_thread_([this]() {
              SPDLOG_INFO("IO线程已启动");
              io_context_.run(); // 阻塞运行，直到 io_context_.stop()
              SPDLOG_INFO("IO线程已退出");
          }) {
    }

    // 功能：析构时确保后台服务全部停止、线程回收。
    ~BackgroundServices() {
        Stop();
    }

    // 禁止拷贝：本类持有线程与 socket 等不可复制资源。
    BackgroundServices(const BackgroundServices &) = delete;
    BackgroundServices &operator=(const BackgroundServices &) = delete;

    // 功能：暴露 MQTT 客户端引用，供主循环发送消息。
    RoboGo::io::MqttClient &mqtt_client() {
        return mqtt_client_;
    }

    // 功能：设置 UDP 视频是否 180 度旋转。
    void SetUdpVideoInverted(const bool enabled) {
        udp_frame_converter_.SetInvertUdpVideo(enabled);
    }

    // 功能：查询 UDP 视频是否 180 度旋转。
    [[nodiscard]] bool IsUdpVideoInverted() const {
        return udp_frame_converter_.IsInvertUdpVideoEnabled();
    }

    // 功能：在主线程中消费所有已到达的 MQTT 消息。
    // 思路：循环从无锁队列取出消息交给 HandleMqttMessage 分发；
    //       若消息携带自定义字节块数据，则转交解码 worker。
    void ProcessMessages(State &state) {
        std::pair<std::string, std::string> message;
        while (message_queue_.try_dequeue(message)) { // 直到队列取空
            HandleMqttMessage(state, mqtt_client_, message.first, message.second, [this](std::span<const uint8_t> packet_bytes) {
                custom_byte_block_decoder_worker_.enqueue_packet_bytes(packet_bytes); // 字节块包交给解码 worker
            });
        }
    }

    // 功能：把解码 worker 产生的日志搬进 UI 日志列表。
    void DrainCustomVideoLogs(State &state) {
        UiLogMessage log_message;
        while (custom_video_log_queue_.try_dequeue(log_message)) { // 排空日志队列
            AppendVehicleLog(state, log_message.type, log_message.text); // 追加到车辆日志
        }
    }

    // 功能：取出最新的 UDP 视频帧(丢弃积压旧帧，只保留最后一帧)。
    std::optional<VideoFrame> TakeLatestVideoFrame() {
        return TakeLatestFrame(video_frame_queue_);
    }

    // 功能：取出最新的自定义字节块视频帧。
    std::optional<VideoFrame> TakeLatestCustomVideoFrame() {
        return TakeLatestFrame(custom_video_frame_queue_);
    }

    // 功能：按顺序停止各服务并回收 IO 线程。
    // 思路：置停止标志 -> 停 MQTT/视频流 -> stop io_context -> join IO 线程 -> 停解码 worker。
    void Stop() {
        if (stopped_) { // 保证只执行一次
            return;
        }
        stopped_ = true;
        mqtt_client_.stop(); // 停止 MQTT
        video_stream_manager_.stop(); // 停止 UDP 视频接收
        io_context_.stop(); // 让 io_context.run() 返回
        if (io_thread_.joinable()) {
            io_thread_.join(); // 等待 IO 线程退出
        }
        custom_byte_block_decoder_worker_.stop(); // 最后停解码 worker
    }

private:
    // 功能：从队列中取出最后一帧(排空队列，仅保留最新)。
    static std::optional<VideoFrame> TakeLatestFrame(moodycamel::ReaderWriterQueue<VideoFrame> &queue) {
        std::optional<VideoFrame> latest_frame;
        VideoFrame video_frame;
        while (queue.try_dequeue(video_frame)) { // 持续取，直到取空
            latest_frame = std::move(video_frame); // 始终用更新的帧覆盖
        }
        return latest_frame;
    }

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
};

// 功能：取出两路视频各自最新的帧并上传为 GPU 纹理，同时记录最近出帧时间。
// 思路：UDP 帧与自定义字节块帧分别处理；成功上传则刷新对应的时间戳，
//       供后续判断画面是否"仍在刷新"(超时回退逻辑使用)。
void UploadLatestVideoFrames(RoboGo::platform::PlatformBackend &platform_backend,
                             BackgroundServices &background_services,
                             std::chrono::steady_clock::time_point &last_udp_frame_time,
                             std::chrono::steady_clock::time_point &last_custom_frame_time) {
    const auto video_frame = background_services.TakeLatestVideoFrame(); // 取最新 UDP 帧
    if (video_frame.has_value()) {
        last_udp_frame_time = std::chrono::steady_clock::now(); // 刷新 UDP 出帧时间
        if (!platform_backend.UploadTexture(TextureSlot::UdpVideo, *video_frame)) { // 上传到 UDP 视频纹理槽
            SPDLOG_ERROR("创建UDP视频纹理失败 width={} height={}", video_frame->width, video_frame->height);
        }
    }

    const auto custom_video_frame = background_services.TakeLatestCustomVideoFrame(); // 取最新自定义视频帧
    if (custom_video_frame.has_value()) {
        last_custom_frame_time = std::chrono::steady_clock::now(); // 刷新自定义视频出帧时间
        if (!platform_backend.UploadTexture(TextureSlot::CustomVideo, *custom_video_frame)) { // 上传到自定义视频纹理槽
            SPDLOG_ERROR("创建 CustomByteBlock 视频纹理失败 width={} height={}",
                         custom_video_frame->width,
                         custom_video_frame->height);
        }
    }
}

// 功能：判断某一路视频当前是否可正常显示。
// 思路：纹理已就绪、曾收到过帧、且距最近一帧未超过超时阈值，三者同时满足才算"就绪"。
[[nodiscard]] bool IsVideoDisplayReady(const TextureView &video_texture,
                                       const std::chrono::steady_clock::time_point last_frame_time,
                                       const std::chrono::steady_clock::duration fallback_timeout,
                                       const std::chrono::steady_clock::time_point now) {
    return video_texture.IsReady() // 纹理句柄与尺寸有效
           && last_frame_time != std::chrono::steady_clock::time_point::min() // 至少收到过一帧
           && (now - last_frame_time) <= fallback_timeout; // 未超过无帧超时
}

// 功能：维护"哪一路视频作为主画面"的偏好与自动回退逻辑。
// 思路：按 H 键手动切换偏好；再结合两路视频是否就绪，决定实际主画面——
//       优先 UDP；UDP 无数据而自定义视频就绪时自动切到自定义视频，UDP 恢复后再切回。
void UpdateVideoLayoutPreference(State &state,
                                 const TextureView &udp_video_texture,
                                 const TextureView &custom_video_texture,
                                 std::chrono::steady_clock::time_point last_udp_frame_time,
                                 std::chrono::steady_clock::time_point last_custom_frame_time,
                                 bool &prefer_custom_video_as_main,
                                 bool &using_custom_video_as_main) {
    const auto now = std::chrono::steady_clock::now();
    // 分别计算两路视频当前是否可显示。
    const bool udp_video_display_ready = IsVideoDisplayReady(
            udp_video_texture,
            last_udp_frame_time,
            kUdpVideoFallbackTimeout,
            now);
    const bool custom_video_display_ready = IsVideoDisplayReady(
            custom_video_texture,
            last_custom_frame_time,
            kCustomVideoFallbackTimeout,
            now);

    bool handled_manual_video_layout_toggle = false; // 本帧是否已处理手动切换
    // 按 H 键(且未在输入文本)手动翻转主画面偏好。
    if (ImGui::IsKeyPressed(ImGuiKey_H, false) && !ImGui::GetIO().WantTextInput) {
        prefer_custom_video_as_main = !prefer_custom_video_as_main; // 翻转偏好
        handled_manual_video_layout_toggle = true;

        // 根据偏好与当前两路可用性，输出对应的提示日志。
        if (prefer_custom_video_as_main) {
            if (custom_video_display_ready) {
                AppendSystemLog(state, 0, "主画面偏好已切换到 CustomByteBlock 视频流。");
            } else if (udp_video_display_ready) {
                AppendSystemLog(state, 1, "主画面偏好已切换到 CustomByteBlock 视频流，等待 CustomByteBlock 视频流恢复后自动切换。");
            } else {
                AppendSystemLog(state, 1, "主画面偏好已切换到 CustomByteBlock 视频流，当前两路视频都不可用。");
            }
        } else {
            if (udp_video_display_ready) {
                AppendSystemLog(state, 0, "主画面偏好已切换到 UDP 视频流。");
            } else if (custom_video_display_ready) {
                AppendSystemLog(state, 1, "主画面偏好已切换到 UDP 视频流，UDP 视频流恢复后会自动切回。");
            } else {
                AppendSystemLog(state, 1, "主画面偏好已切换到 UDP 视频流，当前两路视频都不可用。");
            }
        }
    }

    // 实际是否使用自定义视频作为主画面：自定义视频就绪，且(用户偏好它 或 UDP 不可用)。
    const bool should_use_custom_video_as_main = custom_video_display_ready
                                                 && (prefer_custom_video_as_main || !udp_video_display_ready);
    if (!handled_manual_video_layout_toggle && should_use_custom_video_as_main != using_custom_video_as_main) {
        // 非手动切换且实际主画面发生变化时，更新状态并记录日志。
        using_custom_video_as_main = should_use_custom_video_as_main;
        if (using_custom_video_as_main) {
            AppendSystemLog(state, 1, "UDP 视频流无数据，主画面已切换到 CustomByteBlock 视频流");
        } else if (udp_video_display_ready) {
            AppendSystemLog(state, 0, "UDP 视频流恢复，主画面已切回 UDP 视频流");
        }
    } else {
        using_custom_video_as_main = should_use_custom_video_as_main; // 同步状态
    }
}

// 功能：切换 UDP 视频的 180 度旋转开关，并记录日志。
void ToggleUdpVideoInversion(State &state, BackgroundServices &background_services) {
    const bool udp_video_inverted = !background_services.IsUdpVideoInverted(); // 取反当前开关
    background_services.SetUdpVideoInverted(udp_video_inverted); // 应用新开关
    AppendSystemLog(state,
                    0,
                    udp_video_inverted
                            ? "UDP 视频方向已切换为 180 度旋转。" // 开启提示
                            : "UDP 视频方向已切换为正常方向。"); // 关闭提示
}

}// namespace

namespace RoboGo::app {

// 功能：解析命令行参数，生成结构化的启动选项。
// 思路：先填入默认值(默认 MQTT 地址、默认视频端口、默认旋转)，再识别
//       --mqtt_server 与 --video_port 两个参数覆盖默认值，最后解析网络端点。
LaunchOptions ParseLaunchOptions(const std::vector<std::string> &args) {
    LaunchOptions options;  // 初始化选项结构体
    options.mqtt_server = DefaultMqttServer(); // 默认 MQTT 服务器地址
    options.video_port = "3334"; // 默认 UDP 视频端口
    options.invert_udp_video = kDefaultInvertUdpVideo; // 默认是否旋转

    // 遍历命令行参数，识别需要携带一个值的启动选项。
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--mqtt_server" && i + 1 < args.size()) { // --mqtt_server <addr>
            options.mqtt_server = args[i + 1]; // 取下一个参数作为地址
            ++i; // 跳过已消费的值
        } else if (args[i] == "--video_port" && i + 1 < args.size()) { // --video_port <port>
            options.video_port = args[i + 1]; // 取下一个参数作为端口
            ++i; // 跳过已消费的值
        }
    }

    ParseNetworkEndpoints(options); // 将字符串端点解析为 host/port
    return options; // 返回最终选项
}

// 功能：应用主入口，初始化平台后端与后台服务后进入主渲染循环。
// 思路：初始化后端 -> 注册游戏输入捕获回调 -> 加载小地图纹理 -> 建立后台服务 ->
//       每帧：消费消息/更新状态/上报控制/上传视频纹理 -> 绘制界面 -> 提交渲染；
//       退出循环后清理图形与网络资源。
// 返回：0 表示正常退出，1 表示平台初始化失败。
int RunApplication(platform::PlatformBackend &platform_backend, const LaunchOptions &options) {
    const auto main_start = std::chrono::steady_clock::now(); // 记录启动时刻(用于统计耗时)
    SPDLOG_INFO("程序启动");
    // 打印解析后的启动参数，便于排查配置问题。
    SPDLOG_INFO("解析启动参数 mqtt_server={} video_port={} invert_udp_video={}",
                options.mqtt_server,// MQTT 服务器地址
                options.video_port,// UDP 视频端口
                options.invert_udp_video);// 是否旋转 UDP 视频
    SPDLOG_INFO("网络端点已解析 mqtt_host={} mqtt_port={} video_port={}",
                options.mqtt_host,// MQTT 服务器主机名
                options.mqtt_port,// MQTT 服务器端口
                options.parsed_video_port);// 解析后的 UDP 视频端口

    if (!platform_backend.Initialize()) { // 初始化窗口/图形设备/ImGui
        return 1; // 初始化失败直接退出
    }

    // 注册"游戏输入捕获"回调，使 UI 层可请求锁定/释放鼠标键盘。
    set_game_input_capture_handler([&platform_backend](const bool enabled) {
        // 调用平台后端设置游戏输入捕获状态
        platform_backend.SetGameInputCapture(enabled);
    });

    State state = CreateInitialState(); // 创建全局界面/数据状态

    // 加载内嵌小地图 PNG 并上传为纹理；失败时回退为纯色背景(不致命)。
    auto mini_map_image = RoboGo::resource::LoadEmbeddedMapImage();
    if (!mini_map_image.has_value()) {
        SPDLOG_WARN("未能加载内嵌小地图，将回退为纯色小地图背景");
    } else if 
    // 尝试上传小地图纹理
    // 如果上传成功，记录日志
    (platform_backend.UploadTexture(TextureSlot::MiniMap, *mini_map_image)) {
        SPDLOG_INFO("小地图纹理已上传 width={} height={}", mini_map_image->width, mini_map_image->height);
    } else {
        SPDLOG_ERROR("创建小地图纹理失败 width={} height={}", mini_map_image->width, mini_map_image->height);
    }

    { 
        // 用作用域包裹后台服务，保证退出时先析构(停止线程)再继续收尾
        BackgroundServices background_services(options); // 启动 IO 线程、MQTT、视频流等
        // 记录两路视频最近一次出帧的时间，初值 min() 表示"尚未收到帧"。
        std::chrono::steady_clock::time_point last_udp_frame_time = std::chrono::steady_clock::time_point::min();
        std::chrono::steady_clock::time_point last_custom_frame_time = std::chrono::steady_clock::time_point::min();
        bool prefer_custom_video_as_main = false; // 用户是否偏好自定义视频为主画面
        bool using_custom_video_as_main = false; // 当前实际是否以自定义视频为主画面

        // 统计从启动到后台服务就绪的耗时。
        const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - main_start)
                                        .count();
        SPDLOG_INFO("程序启动完成 startup_ms={}", startup_ms);

        // 主循环：ProcessEvents 返回 false(如窗口关闭)时退出。
        while (platform_backend.ProcessEvents()) {
            background_services.ProcessMessages(state); // 消费 MQTT 下行消息
            background_services.DrainCustomVideoLogs(state); // 搬运自定义视频日志
            UpdateState(state); // 按时间推进界面/业务状态
            PublishCustomControlIfDue(state, background_services.mqtt_client()); // 到点则上报自定义控制
            UploadLatestVideoFrames(platform_backend, // 上传两路视频的最新帧
                                    background_services,// 后台服务，用于获取视频帧
                                    last_udp_frame_time,// 最近一次 UDP 视频帧时间
                                    last_custom_frame_time// 最近一次自定义视频帧时间
                                );

            if (!platform_backend.BeginFrame()) { // 准备本帧(窗口最小化等可跳过)
                continue;
            }

            // 获取三个纹理槽的视图(句柄 + 尺寸)，交给界面绘制使用。
            const TextureView udp_video_texture = platform_backend.GetTextureView(TextureSlot::UdpVideo);
            const TextureView custom_video_texture = platform_backend.GetTextureView(TextureSlot::CustomVideo);
            const TextureView mini_map_texture = platform_backend.GetTextureView(TextureSlot::MiniMap);

            // 根据按键与两路视频可用性，更新主画面选择。
            UpdateVideoLayoutPreference(state,
                                        udp_video_texture,
                                        custom_video_texture,
                                        last_udp_frame_time,
                                        last_custom_frame_time,
                                        prefer_custom_video_as_main,
                                        using_custom_video_as_main);

            // 按 I 键(且非文本输入)切换 UDP 视频 180 度旋转。
            if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !ImGui::GetIO().WantTextInput) {
                ToggleUdpVideoInversion(state, background_services);
            }

            const auto now = std::chrono::steady_clock::now();
            // 为绘制重新计算两路视频的就绪状态(供界面决定占位/提示)。
            const bool udp_video_display_ready = IsVideoDisplayReady(
                    udp_video_texture,
                    last_udp_frame_time,
                    kUdpVideoFallbackTimeout,
                    now);
            const bool custom_video_display_ready = IsVideoDisplayReady(
                    custom_video_texture,
                    last_custom_frame_time,
                    kCustomVideoFallbackTimeout,
                    now);

            // 绘制整个主界面；返回值表示界面内是否请求切换视频方向。
            const bool toggle_udp_video_inversion_requested = ShowMainWindow(
                    state,
                    background_services.mqtt_client(),
                    udp_video_texture.texture_ref,
                    udp_video_texture.size,
                    custom_video_texture.texture_ref,
                    custom_video_texture.size,
                    using_custom_video_as_main,
                    prefer_custom_video_as_main,
                    udp_video_display_ready,
                    custom_video_display_ready,
                    background_services.IsUdpVideoInverted(),
                    mini_map_texture.texture_ref,
                    mini_map_texture.size);
            if (toggle_udp_video_inversion_requested) {
                ToggleUdpVideoInversion(state, background_services); // 界面请求切换方向
            }

            update_keyboard_mouse_state(background_services.mqtt_client()); // 采集并上报键鼠(75Hz 限频)

            ImGui::Render(); // 生成 ImGui 绘制数据
            platform_backend.RenderFrame(ImGui::GetDrawData()); // 提交到屏幕
        }

        SPDLOG_INFO("开始执行图形资源清理");
        platform_backend.Shutdown(); // 先释放图形资源(纹理/设备/窗口)

        SPDLOG_INFO("开始关闭网络与后台线程");
        background_services.Stop(); // 停止网络与后台线程
        background_services.DrainCustomVideoLogs(state); // 收尾：把剩余日志刷入界面
        SPDLOG_INFO("程序退出完成");
    }

    set_game_input_capture_handler({}); // 解除输入捕获回调
    return 0; // 正常退出
}

}// namespace RoboGo::app
