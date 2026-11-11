// MQTT 下行消息分发实现：静态分发表 + 二分查找。
//
// 为什么用表而不是 if-else 链：
//   1. 新增协议话题只需在表里加一行、在 handlers.h 加一个声明，不必在 500 多行的分支链里找位置；
//   2. 原来最坏要比较 22 次字符串、平均 11.5 次，二分后稳定 4~5 次，且 string_view 比较不做 strlen；
//   3. 「已支持哪些 topic」一眼可见，和协议文档能直接对照。
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "mqtt/mqtt_router.h"

#include "mqtt/handlers.h"

namespace RoboGo::mqtt {
namespace {

    // 分发表：必须按 topic 字典序排列（ASCII 序），否则二分查找会漏 topic。
    // 下面的 static_assert 会在编译期拦住插错位置的改动。
    // 新增协议话题时必须同步把模板参数 22 改成新数量，否则编译期就会因元素个数不符而失败。
    constexpr std::array<TopicEntry, 22> kTopicEntries{{
            {"Buff", &HandleBuff},
            {"DartSelectTargetStatusSync", &HandleDartSelectTargetStatusSync},
            {"Event", &HandleEvent},
            {"GameStatus", &HandleGameStatus},
            {"GlobalLogisticsStatus", &HandleGlobalLogisticsStatus},
            {"GlobalSpecialMechanism", &HandleGlobalSpecialMechanism},
            {"GlobalUnitStatus", &HandleGlobalUnitStatus},
            {"MapClickInfo", &HandleMapClickInfo},
            {"PenaltyInfo", &HandlePenaltyInfo},
            {"RadarInfoToClient", &HandleRadarInfoToClient},
            {"RobotDynamicStatus", &HandleRobotDynamicStatus},
            {"RobotInjuryStat", &HandleRobotInjuryStat},
            {"RobotModuleStatus", &HandleRobotModuleStatus},
            {"RobotPathPlanInfo", &HandleRobotPathPlanInfo},
            {"RobotPerformanceSelectionSync", &HandleRobotPerformanceSelectionSync},
            {"RobotPosition", &HandleRobotPosition},
            {"RobotRespawnStatus", &HandleRobotRespawnStatus},
            {"RobotStaticStatus", &HandleRobotStaticStatus},
            {"RuneStatusSync", &HandleRuneStatusSync},
            {"SentryCtrlResult", &HandleSentryCtrlResult},
            {"SentryStatusSync", &HandleSentryStatusSync},
            {"TechCoreMotionStateSync", &HandleTechCoreMotionStateSync},
    }};

        // 编译期护栏：表是 constexpr，编译器在编译阶段就逐项比较字典序；
    // 谁把新话题插错位置会直接编译失败，不会留到赛场上变成「界面不更新」。
    static_assert(std::is_sorted(kTopicEntries.begin(), kTopicEntries.end(),
                                 [](const TopicEntry &lhs, const TopicEntry &rhs) {
                                     return lhs.topic < rhs.topic;
                                 }),
                  "kTopicEntries 必须按 topic 字典序排列，否则二分查找会漏 topic");

}  // namespace

/**
 * @brief 返回整张分发表的只读视图，供 MQTT 客户端构造订阅包。
 * @return std::span<const TopicEntry>，指向静态 constexpr 数组，零拷贝且生命周期与程序等长。
 * @note 「订阅什么」与「能处理什么」共用这一份定义，新增话题只改表即可，不会出现表里有却漏订阅、
 *       消息永远收不到且界面不报错的情况。
 * @note 调用点在 mqtt_client.cpp 的 ConnectOnce() 订阅阶段，订阅 QoS0；上行发布另用 QoS1。
 */
std::span<const TopicEntry> AllTopics() {
    return kTopicEntries;
}

/**
 * @brief 按话题名把一条下行 protobuf 报文分发给对应的处理函数。
 * @param state 全局状态，由命中的 handler 写入；本函数自身不修改它。
 * @param topic 话题名 = 裁判系统消息类型（如 "GlobalUnitStatus"）。
 * @param payload 未解析的 protobuf 原始字节串。
 * @note 由主线程每帧 ProcessMessages() 取空消息队列后调用；IO 协程只负责入队，解析与写 State 全在主线程，
 *       因此 State 不需要加锁。
 * @note 未匹配时的 WARN 是发现「协议新增话题」的唯一线索，排查「界面不更新」时先搜这条日志。
 */
void DispatchMqttMessage(State &state, const std::string &topic, const std::string &payload) {
    // lower_bound 找「第一个不小于 topic」的表项：比较器只问 entry.topic < key，
    // 所以相等时走 else 分支、不会跳过目标；string_view 比较不做 strlen，22 项稳定 4~5 次比较。
    const auto it = std::lower_bound(kTopicEntries.begin(), kTopicEntries.end(), std::string_view(topic),
                                     [](const TopicEntry &entry, std::string_view key) {
                                         return entry.topic < key;
                                     });
    // 双条件缺一不可：前者拦「目标比所有表项都大」，后者拦「目标不存在但落在别的表项上」。
    if (it == kTopicEntries.end() || it->topic != topic) {
        // 未匹配说明协议可能新增了话题：打出话题名与长度，是排查「界面不更新」的第一手线索。
        SPDLOG_WARN("收到未处理MQTT话题 topic={} bytes={}", topic, payload.size());
        return;
    }
    // 函数指针跳转。第二实参传表里的 it->topic（静态字符串字面量）而不是本次收到的
    // 临时 std::string，handler 只用它打日志，不会留下悬空引用。
    it->handle(state, it->topic, payload);
}

}  // namespace RoboGo::mqtt


