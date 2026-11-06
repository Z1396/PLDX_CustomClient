// MQTT 下行消息分发实现：静态分发表 + 二分查找。
//
// 为什么用表而不是 if-else 链：
//   1. 新增协议话题只需在表里加一行、在 handlers.h 加一个声明，不必在 500 多行的分支链里找位置；
//   2. 原来最坏要比较 22 次字符串、平均 11.5 次，二分后稳定 4~5 次，且 string_view 比较不做 strlen；
//   3. 「已支持哪些 topic」一眼可见，和协议文档能直接对照。
#include "mqtt/mqtt_router.h"

#include "mqtt/handlers.h"

namespace RoboGo::mqtt {
namespace {

    // 分发表：必须按 topic 字典序排列（ASCII 序），否则二分查找会漏 topic。
    // 下面的 static_assert 会在编译期拦住插错位置的改动。
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

    static_assert(std::is_sorted(kTopicEntries.begin(), kTopicEntries.end(),
                                 [](const TopicEntry &lhs, const TopicEntry &rhs) {
                                     return lhs.topic < rhs.topic;
                                 }),
                  "kTopicEntries 必须按 topic 字典序排列，否则二分查找会漏 topic");

}  // namespace

std::span<const TopicEntry> AllTopics() {
    return kTopicEntries;
}

void DispatchMqttMessage(State &state, const std::string &topic, const std::string &payload) {
    const auto it = std::lower_bound(kTopicEntries.begin(), kTopicEntries.end(), std::string_view(topic),
                                     [](const TopicEntry &entry, std::string_view key) {
                                         return entry.topic < key;
                                     });
    if (it == kTopicEntries.end() || it->topic != topic) {
        // 未匹配说明协议可能新增了话题：打出话题名与长度，是排查「界面不更新」的第一手线索。
        SPDLOG_WARN("收到未处理MQTT话题 topic={} bytes={}", topic, payload.size());
        return;
    }
    it->handle(state, it->topic, payload);
}

}  // namespace RoboGo::mqtt
