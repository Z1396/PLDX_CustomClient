// perf_stats.cpp：性能指标的原子存储与平滑实现。
#include "perf_stats.h"

#include <algorithm>

namespace RoboGo::app::perf {

namespace {  // 内部状态：全部原子，跨线程读写无需加锁

// 指数滑动平均系数：0.15 约等于"最近 6~7 次采样的加权平均"，
// 既能压掉单帧抖动，又能在 100ms 内跟上阶跃变化（比如分辨率切换）。
constexpr double kSmoothing = 0.15;

std::atomic<double> g_decode_ms{0.0};       // 解码耗时
std::atomic<double> g_convert_ms{0.0};      // YUV->RGBA 耗时
std::atomic<double> g_upload_ms{0.0};       // 纹理上传耗时
std::atomic<double> g_ui_ms{0.0};           // 界面构建耗时
std::atomic<double> g_frame_ms{0.0};        // 整帧耗时
std::atomic<int> g_udp_queue{0};            // UDP 队列深度
std::atomic<uint64_t> g_dropped{0};         // 累计丢帧
std::atomic<bool> g_visible{false};         // 面板可见性
std::atomic<int> g_video_width{0};          // 图传帧宽度
std::atomic<int> g_video_height{0};         // 图传帧高度

/**
 * @brief 把一个新采样值按滑动平均并入原子变量。
 * @note 说明：先用 relaxed 读旧值做平均，再存回；这是单写多读的统计量，允许极小概率的竞态。
 */
void Blend(std::atomic<double> &target, const double sample) {
    if (sample < 0.0) {  // 非法采样（时钟异常）
        return;  // 直接丢弃
    }
    const double previous = target.load(std::memory_order_relaxed);  // 读旧平均值
    const double blended = previous <= 0.0  // 首个采样
                           ? sample  // 直接采用，避免从 0 缓慢爬升
                           : previous * (1.0 - kSmoothing) + sample * kSmoothing;  // 否则做指数平滑
    target.store(blended, std::memory_order_relaxed);  // 写回
}

// 最近丢帧统计：按秒分桶，只保留最近 10 秒。
// 为什么要有这个：累计丢帧数只增不减，看久了会误以为"一直在丢、延迟一直在涨"。
// 真正要判断的是"现在还在不在丢"，所以额外维护一个滑动窗口。
constexpr int kDropWindowSeconds = 10;
std::array<std::uint32_t, kDropWindowSeconds> g_drop_ring{};   // 每秒一个桶，记录该秒的丢帧数
int g_drop_ring_index = 0;                                     // 当前写入哪个桶
std::uint64_t g_drop_last_total = 0;                           // 上次采样时的累计值
std::chrono::steady_clock::time_point g_drop_last_sample{};     // 上次采样时刻

}  // namespace

void RecordDecode(const double milliseconds) { Blend(g_decode_ms, milliseconds); }
void RecordConvert(const double milliseconds) { Blend(g_convert_ms, milliseconds); }
void RecordUpload(const double milliseconds) { Blend(g_upload_ms, milliseconds); }
void RecordUi(const double milliseconds) { Blend(g_ui_ms, milliseconds); }
void RecordFrame(const double milliseconds) { Blend(g_frame_ms, milliseconds); }

/**
 * @brief 更新性能统计原子量
 * @note 所有指标存在原子量里，跨线程读写无需加锁；耗时类指标走指数滑动平均（kSmoothing=0.15）。
 * @note 性能浮层由 F12 唤出，统计口径见 app/perf_stats.h 的 Metrics。
 */
void RecordVideoSize(const int width, const int height) {
    g_video_width.store(width, std::memory_order_relaxed);
    g_video_height.store(height, std::memory_order_relaxed);
}

/**
 * @brief 更新性能统计原子量
 * @note 所有指标存在原子量里，跨线程读写无需加锁；耗时类指标走指数滑动平均（kSmoothing=0.15）。
 * @note 性能浮层由 F12 唤出，统计口径见 app/perf_stats.h 的 Metrics。
 */
void RecordQueues(const int udp_queue_depth) {
    g_udp_queue.store(udp_queue_depth, std::memory_order_relaxed);  // 队列深度只关心瞬时值，不做平滑

    // 每秒采样一次累计丢帧，折算成"该秒丢了几帧"写进滑动窗口。
    // 这里由 UI 线程每帧调用一次，天然串行，不需要加锁。
    const auto now = std::chrono::steady_clock::now();
    if (g_drop_last_sample.time_since_epoch().count() == 0) {  // 首次调用
        g_drop_last_sample = now;                              // 记录基准时刻
        g_drop_last_total = g_dropped.load(std::memory_order_relaxed);  // 记录基准累计值
        return;                                                // 本轮没有可统计的区间
    }
    const std::chrono::duration<double> elapsed = now - g_drop_last_sample;  // 距上次采样多久
    if (elapsed.count() < 1.0) {  // 不到 1 秒
        return;  // 继续累积
    }
    const std::uint64_t total = g_dropped.load(std::memory_order_relaxed);  // 当前累计值
    const std::uint64_t delta = total - g_drop_last_total;                  // 这段区间内丢了多少
    const double seconds = std::max(elapsed.count(), 0.001);                // 防止除零
    // 折算到"每秒"，这样即使某一帧卡了很久、区间超过 1 秒，统计也不会失真。
    g_drop_ring[g_drop_ring_index] = static_cast<std::uint32_t>(delta / seconds);
    g_drop_ring_index = (g_drop_ring_index + 1) % kDropWindowSeconds;       // 环形推进
    g_drop_last_total = total;                                              // 更新基准
    g_drop_last_sample = now;                                               // 更新时刻
}

void AddDroppedFrame() { g_dropped.fetch_add(1, std::memory_order_relaxed); }  // 递增计数即可，读取端只看总量

/**
 * @brief 更新性能统计原子量
 * @note 所有指标存在原子量里，跨线程读写无需加锁；耗时类指标走指数滑动平均（kSmoothing=0.15）。
 * @note 性能浮层由 F12 唤出，统计口径见 app/perf_stats.h 的 Metrics。
 */
void ToggleVisible() {
    g_visible.store(!g_visible.load(std::memory_order_relaxed), std::memory_order_relaxed);  // 翻转显示状态
}

bool IsVisible() { return g_visible.load(std::memory_order_relaxed); }

/**
 * @brief 更新性能统计原子量
 * @note 所有指标存在原子量里，跨线程读写无需加锁；耗时类指标走指数滑动平均（kSmoothing=0.15）。
 * @note 性能浮层由 F12 唤出，统计口径见 app/perf_stats.h 的 Metrics。
 */
Metrics Snapshot() {
    Metrics metrics;  // 组装快照
    metrics.decode_ms = g_decode_ms.load(std::memory_order_relaxed);
    metrics.convert_ms = g_convert_ms.load(std::memory_order_relaxed);
    metrics.upload_ms = g_upload_ms.load(std::memory_order_relaxed);
    metrics.ui_ms = g_ui_ms.load(std::memory_order_relaxed);
    metrics.frame_ms = g_frame_ms.load(std::memory_order_relaxed);
    metrics.udp_queue = g_udp_queue.load(std::memory_order_relaxed);
    metrics.dropped_frames = g_dropped.load(std::memory_order_relaxed);
    std::uint32_t recent = 0;  // 最近 10 秒丢帧合计
    for (const std::uint32_t bucket: g_drop_ring) {  // 遍历所有秒桶
        recent += bucket;  // 累加
    }
    metrics.recent_drops = static_cast<int>(recent);  // 写入快照
    metrics.video_width = g_video_width.load(std::memory_order_relaxed);
    metrics.video_height = g_video_height.load(std::memory_order_relaxed);
    metrics.visible = g_visible.load(std::memory_order_relaxed);
    // 帧率由整帧耗时直接换算：比 ImGui 的 Framerate 更贴近"这条管线真正跑多快"。
    metrics.fps = metrics.frame_ms > 0.01 ? static_cast<float>(1000.0 / metrics.frame_ms) : 0.0f;
    return metrics;
}

ScopedTimer::~ScopedTimer() {
    if (sink_ == nullptr) {  // 没指定上报目标
        return;  // 什么都不做
    }
    const auto elapsed = std::chrono::steady_clock::now() - start_;  // 计算经过时间
    const double milliseconds = std::chrono::duration<double, std::milli>(elapsed).count();  // 转毫秒
    sink_(milliseconds);  // 上报
}

}  // namespace RoboGo::app::perf

