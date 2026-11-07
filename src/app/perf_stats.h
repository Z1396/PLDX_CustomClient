// perf_stats.h：运行期性能指标采集（解码/转换/上传/渲染耗时、队列深度、丢帧数）。
// 设计原则：写入方是各后台线程与渲染线程，读取方只有 UI 线程，全部用原子变量 + 指数滑动平均，
// 不加锁、不分配内存，单次记录开销在纳秒级，可以安全地放在每帧热路径上。
#pragma once

// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include "imgui.h"
// [/pch-split]
#include <atomic>
#include <chrono>
#include <cstdint>

namespace RoboGo::app::perf {  // 应用层性能统计

// 一次快照：供性能面板一次性读取，避免面板绘制期间数值跳变。
struct Metrics {
    double decode_ms = 0.0;      // HEVC 解码耗时（解码线程）
    double convert_ms = 0.0;     // YUV->RGBA 转换耗时（解码线程）
    double upload_ms = 0.0;      // 纹理上传耗时（渲染线程，含提交与等待）
    double ui_ms = 0.0;          // 界面构建耗时（渲染线程）
    double frame_ms = 0.0;       // 整帧耗时（事件泵到 Present 返回）
    float fps = 0.0f;            // 平滑后的帧率
    int udp_queue = 0;           // UDP 视频帧队列积压
    uint64_t dropped_frames = 0; // 累计因队列满而丢弃的帧数（历史总量，不代表当前延迟）
    int recent_drops = 0;        // 最近 10 秒内的丢帧数（判断"现在还在不在丢"）
    int video_width = 0;         // 当前图传帧宽度（像素）
    int video_height = 0;        // 当前图传帧高度（像素）
    bool visible = false;        // 面板是否显示
};

// 各阶段耗时记录（毫秒）。内部做指数滑动平均，单次抖动不会让面板数字乱跳。
void RecordDecode(double milliseconds);
void RecordConvert(double milliseconds);
void RecordUpload(double milliseconds);
void RecordUi(double milliseconds);
void RecordFrame(double milliseconds);
// 记录当前图传帧尺寸（分辨率变化时也要更新）。
void RecordVideoSize(int width, int height);
// 记录队列深度（每帧调用一次）。
void RecordQueues(int udp_queue_depth);
// 累加一帧丢弃计数。
void AddDroppedFrame();
// 切换/查询面板显示状态（F12）。
void ToggleVisible();
bool IsVisible();
// 取当前快照。
Metrics Snapshot();

// RAII 计时器：构造记起点、析构把耗时上报给指定记录函数。
// 用法：{ ScopedTimer timer(&RecordUpload); ... }  提前 return 也能正确上报。
class ScopedTimer {
public:
    explicit ScopedTimer(void (*sink)(double)) : sink_(sink), start_(std::chrono::steady_clock::now()) {}
    ScopedTimer(const ScopedTimer &) = delete;             // 不允许拷贝：计时器是作用域独占的
    ScopedTimer &operator=(const ScopedTimer &) = delete;  // 同上
    ~ScopedTimer();                                        // 析构时上报耗时

private:
    void (*sink_)(double);                              // 上报目标
    std::chrono::steady_clock::time_point start_;       // 计时起点
};

}  // namespace RoboGo::app::perf
