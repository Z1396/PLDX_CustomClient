// 轻量单元测试：不引入第三方框架，失败即打印并以非零码退出（供 CTest 判定）。
//
// 覆盖范围刻意只挑「改错了不会报错、只会静默出错」的纯逻辑：
//   1) CustomControl 的 13 字节布局 —— 这是与机器人电控的私有约定，偏移错了下位机直接读错位；
//   2) 发布节流边界 —— 100ms 周期，早了会刷包、晚了会掉帧；
//   3) Event → 中文文案 —— 协议码与文案的映射一旦错位，现场看到的就是错误战况；
//   4) robot_id → 阵营/兵种名 —— 敌我判定错一次就是战术事故。
#include <cstdio>
#include <optional>
#include <string>

#include "app/robot_registry.h"
#include "app/team_util.h"
#include "custom_control_payload.h"
#include "mqtt/event_formatter.h"

namespace {

int g_failed = 0;
int g_checks = 0;

void Check(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL  %s\n", what);
    }
}

void CheckEq(long long actual, long long expected, const char *what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failed;
        std::printf("FAIL  %s (expected %lld, got %lld)\n", what, expected, actual);
    }
}

void CheckStr(const std::string &actual, const std::string &expected, const char *what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failed;
        std::printf("FAIL  %s\n        expected: %s\n        actual  : %s\n", what, expected.c_str(), actual.c_str());
    }
}

// ---------- 1. CustomControl 载荷布局 ----------
void TestPayloadLayout() {
    // 尺寸契约：5 槽 × uint16 + 2 保留字节 + 1 调试计数 = 13
    CheckEq(static_cast<long long>(kCustomControlPayloadSize), 13, "payload size is 13 bytes");
    CheckEq(static_cast<long long>(kCustomControlRobotCount), 5, "payload carries 5 health slots");

    const std::array<uint16_t, kCustomControlRobotCount> hp{1, 258, 400, 65535, 0};
    const auto data = BuildCustomControlPayload(hp, 0x7F);

    // 每个 uint16 都是小端：低字节在前
    CheckEq(data[0], 1, "slot0 low byte");
    CheckEq(data[1], 0, "slot0 high byte");
    CheckEq(data[2], 0x02, "slot1 low byte (258 = 0x0102)");
    CheckEq(data[3], 0x01, "slot1 high byte");
    CheckEq(data[4], 0x90, "slot2 low byte (400 = 0x0190)");
    CheckEq(data[5], 0x01, "slot2 high byte");
    CheckEq(data[6], 0xFF, "slot3 low byte (65535)");
    CheckEq(data[7], 0xFF, "slot3 high byte");
    CheckEq(data[8], 0, "slot4 low byte");
    CheckEq(data[9], 0, "slot4 high byte");

    // 2027 起两个保留字节固定为 0，偏移不变
    CheckEq(data[kCustomControlAirSupportCallOffset], 0, "reserved byte 0 is zero");
    CheckEq(data[kCustomControlCounterCooldownOffset], 0, "reserved byte 1 is zero");
    CheckEq(data[kCustomControlDebugCounterOffset], 0x7F, "debug counter at last byte");
}

// ---------- 2. 发布节流 ----------
void TestPublishThrottle() {
    using namespace std::chrono;
    const steady_clock::time_point t0 = steady_clock::time_point{} + seconds(1000);

    Check(IsCustomControlPublishDue(std::nullopt, t0), "never published -> due");
    Check(!IsCustomControlPublishDue(t0, t0 + milliseconds(99)), "99ms -> not due");
    Check(IsCustomControlPublishDue(t0, t0 + milliseconds(100)), "100ms -> due");
    Check(IsCustomControlPublishDue(t0, t0 + milliseconds(150)), "150ms -> due");
}

// ---------- 3. Event 文案 ----------
void TestEventText() {
    Event event;

    event.set_event_id(1);
    event.set_param("1,101");
    CheckStr(RoboGo::mqtt::FormatEventText(event), "红方重装 击杀了 蓝方重装", "event 1 kill text");

    event.set_event_id(2);
    event.set_param("11");
    CheckStr(RoboGo::mqtt::FormatEventText(event), "红方前哨站被摧毁", "event 2 red outpost");

    event.set_event_id(2);
    event.set_param("111");
    CheckStr(RoboGo::mqtt::FormatEventText(event), "蓝方前哨站被摧毁", "event 2 blue outpost");

    event.set_event_id(5);
    event.set_param("4");
    CheckStr(RoboGo::mqtt::FormatEventText(event), "R 前哨阵地控制状态变更: 蓝方完全控制", "event 5 control state");

    event.set_event_id(9);
    event.set_param("1,2");
    CheckStr(RoboGo::mqtt::FormatEventText(event), "红方飞镖命中基地固定目标", "event 9 dart hit");

    event.set_event_id(15);
    event.set_param("0");
    CheckStr(RoboGo::mqtt::FormatEventText(event), "装配结果: 成功", "event 15 assembly ok");

    event.set_event_id(15);
    event.set_param("4");
    CheckStr(RoboGo::mqtt::FormatEventText(event), "装配结果: 重装战亡", "event 15 assembly aborted");

    // 未识别的事件必须带上原始 ID 与参数，否则现场无法补全处理
    event.set_event_id(99);
    event.set_param("xyz");
    CheckStr(RoboGo::mqtt::FormatEventText(event), "未知事件: ID=99, 参数=xyz", "unknown event keeps raw payload");
}

// ---------- 4. 顶部提示定级 ----------
void TestToastLevels() {
    State state{};
    RoboGo::mqtt::PushEventToast(state, 2, "outpost destroyed");   // 危急，应入队
    CheckEq(static_cast<long long>(state.event_toasts.size()), 1, "critical event pushes a toast");
    CheckEq(state.event_toasts.back().level, 2, "critical event level is 2");

    RoboGo::mqtt::PushEventToast(state, 1, "kill");                // 普通，应入队
    CheckEq(static_cast<long long>(state.event_toasts.size()), 2, "normal event pushes a toast");
    CheckEq(state.event_toasts.back().level, 0, "normal event level is 0");

    RoboGo::mqtt::PushEventToast(state, 3, "rune activated");      // 纯日志事件，不应入队
    CheckEq(static_cast<long long>(state.event_toasts.size()), 2, "log-only event does not push a toast");
}

// ---------- 5. robot_id 映射 ----------
void TestRobotIdMapping() {
    using RoboGo::app::robot_registry::GetRobotName;

    CheckStr(GetRobotName("1"), "红方重装", "id 1");
    CheckStr(GetRobotName("3"), "红方步兵3", "id 3");
    CheckStr(GetRobotName("107"), "蓝方哨兵", "id 107");
    CheckStr(GetRobotName("11"), "红方前哨站", "id 11");
    CheckStr(GetRobotName("112"), "蓝方基地", "id 112");
    CheckStr(GetRobotName("abc"), "未知机器人", "non-numeric id");
    CheckStr(GetRobotName(""), "未知机器人", "empty id");

    Check(RoboGo::app::team_util::IsRedRobotId(1), "id 1 is red");
    Check(RoboGo::app::team_util::IsRedRobotId(99), "id 99 is red");
    Check(!RoboGo::app::team_util::IsRedRobotId(100), "id 100 is blue");
    Check(!RoboGo::app::team_util::IsRedRobotId(107), "id 107 is blue");
}

}  // namespace

int main() {
    TestPayloadLayout();
    TestPublishThrottle();
    TestEventText();
    TestToastLevels();
    TestRobotIdMapping();

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
