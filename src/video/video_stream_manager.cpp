// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <boost/asio.hpp>
extern "C" {  // FFmpeg 为 C 接口，需按 C 链接方式包含
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
// [/pch-split]
#include "video_stream_manager.h" // 引入本文件的类声明

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

    // 功能：构造管理器，搭建「UDP 接收 → 分片重组 → 解码」链路并启动解码线程。
    // 思路：UDP 收到分片直接入队；分片管理器重组出完整帧后，先判断是否为有效起始帧(IDR/参数集)，
    //       待流就绪再交给解码器；最后单独起一个线程循环调用 handle_once 驱动重组与解码。
    VideoStreamManager::VideoStreamManager(boost::asio::io_context &io_context, // Asio 事件循环，接收器在其上异步读
                                           boost::asio::ip::udp::endpoint endpoint, // 监听的 UDP 地址与端口
                                           const std::function<void(const AVFrame *frame)> &frame_callback) // 解出帧后的回调
        : video_stream_receiver(io_context, endpoint, [this](const uint8_t *data, size_t size) { // 用事件循环+端点构造接收器，并注册收包回调
              // 接收到数据包之后送入分片管理
              video_shards_manager.on_receive_shard(data, size); // UDP 收包即转交分片管理器入队
          }),
          video_decoder(frame_callback) { // 用帧回调构造 HEVC 解码器
        SPDLOG_INFO("创建视频流管理器 endpoint={}:{}", // 记录监听端点，便于排查连接问题
                    endpoint.address().to_string(), // 端点 IP 字符串
                    endpoint.port()); // 端点端口号
        // 解码器不可用时只告警一次：后续帧会被静默丢弃，界面显示"等待图传"，但其它面板不受影响。
        // 这样现场就算显卡驱动/FFmpeg 构建有问题，客户端也不会启动即退出。
        if (!video_decoder.is_available()) { // 解码链路初始化失败
            SPDLOG_ERROR("HEVC 解码器不可用：本次运行将没有图传画面，其余界面功能正常"); // 给出明确结论，便于现场快速判断
        }
        // 接收到完整的视频帧之后去解码
        video_shards_manager.set_frame_callback([this](const std::vector<uint8_t> &frame) { // 注册「重组出完整帧」回调
            // 流未就绪时，需要先拿到首个有效关键帧(参数集或 IDR)才能开始解码，避免起始花屏
            if (!stream_ready) { // 流尚未就绪时先等待首个关键帧
                int nal_type = VideoDecoder::get_nal_type(frame); // 识别该帧 NAL 类型
                if (nal_type == -1) { // 无法识别则跳过
                    SPDLOG_DEBUG("尚未拿到可解码视频帧，忽略无效NAL frame_bytes={}", frame.size()); // 记录并丢弃无效帧
                    return; // 忽略无效帧
                }
                // HEVC 参数集 NAL 类型：VPS(32)/SPS(33)/PPS(34)
                bool is_param = (nal_type == 32 || nal_type == 33 || nal_type == 34); // 是否为参数集(VPS/SPS/PPS)
                // HEVC IDR 帧 NAL 类型：IDR_W_DLP(19)/IDR_N_LP(20)/CRA(21)
                bool is_idr = (nal_type == 19 || nal_type == 20 || nal_type == 21); // 是否为 IDR/CRA 关键帧

                // 遇到IDR帧或已有参数集时开始流
                if (is_idr || is_param) { // 遇到关键帧或参数集即认为流就绪
                    stream_ready = true; // 标记流已就绪
                    SPDLOG_INFO("视频流已就绪 nal_type={} frame_bytes={}", nal_type, frame.size()); // 记录流就绪事件
                }
            }

            // 解码
            if (stream_ready) { // 流已就绪才解码
                video_decoder.decode_frame(frame); // 把完整帧交给解码器（解码耗时由 decoder 内部只统计 send/receive）
            } else { // 否则丢弃
                SPDLOG_DEBUG("视频流尚未就绪，丢弃帧 bytes={}", frame.size()); // 记录丢弃（未就绪阶段）
            }
        });
        // 为视频解码单独创建一个线程
        decode_thread = std::thread([this]() { // 启动独立解码线程
            SPDLOG_INFO("视频解码线程已启动"); // 记录线程启动
            // 循环驱动分片重组与解码，直到被要求停止；handle_once 内部阻塞等待分片，不再忙等
            while (is_running.load(std::memory_order_relaxed)) { // 运行标志为真则持续驱动
                if (!video_shards_manager.handle_once()) { // 返回 false 表示收到唤醒哨兵
                    break; // 退出循环（stop() 已经置位 is_running，这里只是加速）
                }
            }
            SPDLOG_INFO("视频解码线程已退出"); // 记录线程退出
        }); // 解码线程 lambda 结束
    }

    // 功能：析构时确保解码线程已回收（防止忘记调用 stop() 时 std::thread 析构直接 terminate）。
    VideoStreamManager::~VideoStreamManager() {
        stop(); // 幂等停止
    }

    // 功能：停止视频流管线，回收线程资源。
    void VideoStreamManager::stop() { // 停止入口（可由外部主动调用）
        SPDLOG_INFO("收到视频流管理器停止请求"); // 记录停止请求
        video_stream_receiver.stop(); // 停止 UDP 接收
        is_running.store(false, std::memory_order_relaxed); // 通知解码线程退出循环
        video_shards_manager.wake(); // 投递唤醒哨兵：解码线程可能正阻塞在 wait_dequeue 上
        if (decode_thread.joinable()) { // 线程可连接才需 join
            decode_thread.join(); // 等待解码线程结束，确保资源安全回收
        }
        SPDLOG_INFO("视频流管理器已停止"); // 记录停止完成
    }

}  // namespace RoboGo::video
