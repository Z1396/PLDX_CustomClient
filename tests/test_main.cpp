// 不引入第三方测试框架，用三个手写断言函数累计 checks/failed，
// 失败打印明细并以非零码退出，供 CTest 判定。
// 覆盖范围刻意只挑「改错了不报错、只会静默出错」的纯逻辑：
// 1) CustomControl 的 13 字节布局 —— 与电控的私有约定，偏移错了下位机直接读错位；
// 2) 发布节流边界 —— 100ms 周期，早了刷包、晚了掉帧；
// 3) Event → 中文文案 —— 协议码与文案错位，现场看到的就是错误战况；
// 4) 顶部提示定级 —— 危急事件漏提示比误提示更危险；
// 5) robot_id → 阵营/兵种名 —— 敌我判定错一次就是战术事故；
// 6) 无异常整数解析 —— 空串/非法字符/溢出/尾随字符四类失败原因必须可区分。
// 设计要点：所有被测函数都不依赖 GUI、网络或硬件，纯函数可直接链接进测试可执行；
// 断言失败不中断，跑完全部用例后统一汇报，便于一次看到所有回归。
// 依赖：custom_control_payload.h、mqtt/event_formatter.h、app/number_parser.h、
// app/robot_registry.h、app/team_util.h。
// 注意：新增纯逻辑回归时直接加 Test*() 并在 main() 里调用即可，不需要改 CMakeLists。
// 轻量单元测试：不引入第三方框架，失败即打印并以非零码退出（供 CTest 判定）。
// 覆盖范围刻意只挑「改错了不会报错、只会静默出错」的纯逻辑：
// 1) CustomControl 的 13 字节布局 —— 这是与机器人电控的私有约定，偏移错了下位机直接读错位；
// 2) 发布节流边界 —— 100ms 周期，早了会刷包、晚了会掉帧；
// 3) Event → 中文文案 —— 协议码与文案的映射一旦错位，现场看到的就是错误战况；
// 4) robot_id → 阵营/兵种名 —— 敌我判定错一次就是战术事故。
#include <cstdio>
#include <optional>
#include <string>

#include "app/number_parser.h"
#include "app/robot_registry.h"
#include "app/team_util.h"
#include "custom_control_payload.h"
#include "mqtt/event_formatter.h"

namespace {

// 全局计数：断言总数与失败数。单线程执行，无需原子量。
int g_failed = 0;
int g_checks = 0;

/**
 * @brief 布尔断言。
 * @note 思路：无论成功与否都累加 g_checks，失败时打印 FAIL 与用例名并累加 g_failed，不中断后续用例，便于一次跑完看到所有回归。
 */
void Check(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL  %s\n", what);
    }
}

/**
 * @brief 整数断言，失败时同时打印期望值与实际值。
 * @note 参数：统一收窄成 long long，避免 uint16/int/uint64 混用时的重载歧义。
 */
void CheckEq(long long actual, long long expected, const char *what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failed;
        std::printf("FAIL  %s (expected %lld, got %lld)\n", what, expected, actual);
    }
}

/**
 * @brief 字符串断言，失败时分两行打印期望与实际（便于看中文文案差异）。
 */
void CheckStr(const std::string &actual, const std::string &expected, const char *what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failed;
        std::printf("FAIL  %s\n        expected: %s\n        actual  : %s\n", what, expected.c_str(), actual.c_str());
    }
}

/**
 * @brief 用例 1：逐字节锁死 CustomControl 的 13 字节私有载荷布局。
 * @note 契约：5 槽 uint16 小端 + 2 保留字节(0) + 1 调试计数；偏移由
 *       kCustomControl*Offset 常量给出，防止电控侧读错位。
 * @note 覆盖 BuildCustomControlPayload()：hp{1,258,400,65535,0} 的 8 个数据字节、
 *       两个保留字节与末字节计数器 0x7F，共 11 条断言。
 * @note 由 main() 第一个调用；CTest 的 robogo_unit 依据 main() 返回值判定成败。
 */
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

/**
 * @brief 用例 2：验证 CustomControl 100ms 发布节流的四个边界。
 * @note 边界：从未发布(nullopt) / 99ms / 100ms / 150ms，期望依次为 发 / 不发 / 发 / 发。
 * @note 用 steady_clock::time_point{} + seconds(1000) 构造固定时刻，不读真实时钟，
 *       因此结果与机器负载、运行时长无关，测试完全确定。
 * @note 被测函数 IsCustomControlPublishDue()，对应客户端 10Hz 的上报节流。
 */
void TestPublishThrottle() {
    using namespace std::chrono;
    const steady_clock::time_point t0 = steady_clock::time_point{} + seconds(1000);

    Check(IsCustomControlPublishDue(std::nullopt, t0), "never published -> due");
    Check(!IsCustomControlPublishDue(t0, t0 + milliseconds(99)), "99ms -> not due");
    Check(IsCustomControlPublishDue(t0, t0 + milliseconds(100)), "100ms -> due");
    Check(IsCustomControlPublishDue(t0, t0 + milliseconds(150)), "150ms -> due");
}

/**
 * @brief 用例 3：校验 Event → 中文文案的映射与未知事件兜底。
 * @note 覆盖 event_id 1(击杀) / 2(前哨站摧毁，红蓝两分支) / 5(阵地控制) /
 *       9(飞镖命中) / 15(装配成功与战亡两种参数)。
 * @note 关键兜底：未识别 ID 必须输出「未知事件: ID=99, 参数=xyz」，保留原始
 *       ID 与参数，否则赛场新增事件时无法事后补全处理。
 * @note 被测函数 RoboGo::mqtt::FormatEventText()，顶部提示与系统日志都依赖它的文案。
 */
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

/**
 * @brief 用例 4：校验顶部提示 PushEventToast() 的事件定级与过滤。
 * @note 断言：level 2(危急) 入队且 level 保持 2；level 1(普通) 入队后 level 为 0；
 *       level 3(纯日志) 不入队，event_toasts.size() 保持 2。
 * @note 直接构造 State{} 调用被测函数，不经过 MQTT 与协程，属于纯逻辑单测。
 */
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

/**
 * @brief 用例 5：校验 robot_id → 中文兵种名与红蓝阵营判定。
 * @note 覆盖红方 1/3、蓝方 107、建筑 11/112、非法串 "abc" 与空串；
 *       边界规则为 id < 100 判红、>= 100 判蓝（用例含 99 红 / 100 蓝）。
 * @note 被测函数 GetRobotName() 与 IsRedRobotId()；UI 顶部胶囊与日志文案都走它们。
 */
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

/**
 * @brief 用例 6：校验 ParseInteger 的四类失败原因与正常值边界。
 * @note 正常值：3334 / 65535 / -1；失败原因必须可区分为 Empty、
 *       InvalidCharacter（含前导空格 " 12"）、OutOfRange、TrailingCharacters。
 * @note 带符号与无符号差异：uint32 收到 "-1" 判 OutOfRange；uint16 的 70000 判 OutOfRange。
 * @note 这是 C++23 std::expected 落地后的契约测试；ToString() 需保证错误原因可打印。
 */
void TestIntegerParsing() {
    using RoboGo::app::IntegerParseError;
    using RoboGo::app::ParseInteger;

    const auto ok = ParseInteger<int>("3334");
    Check(ok.has_value(), "parse 3334 succeeds");
    CheckEq(ok.value_or(-1), 3334, "parse 3334 value");
    CheckEq(ParseInteger<int>("65535").value_or(-1), 65535, "parse 65535");
    CheckEq(ParseInteger<int>("-1").value_or(0), -1, "parse negative");

    Check(!ParseInteger<int>("").has_value(), "empty string fails");
    CheckEq(static_cast<int>(ParseInteger<int>("").error()),
            static_cast<int>(IntegerParseError::Empty), "empty -> Empty");
    CheckEq(static_cast<int>(ParseInteger<int>("abc").error()),
            static_cast<int>(IntegerParseError::InvalidCharacter), "abc -> InvalidCharacter");
    CheckEq(static_cast<int>(ParseInteger<int>(" 12").error()),
            static_cast<int>(IntegerParseError::InvalidCharacter), "leading space -> InvalidCharacter");
    CheckEq(static_cast<int>(ParseInteger<int>("99999999999999999999999").error()),
            static_cast<int>(IntegerParseError::OutOfRange), "huge number -> OutOfRange");
    CheckEq(static_cast<int>(ParseInteger<int>("12abc").error()),
            static_cast<int>(IntegerParseError::TrailingCharacters), "12abc -> TrailingCharacters");
    CheckEq(static_cast<int>(ParseInteger<uint32_t>("-1").error()),
            static_cast<int>(IntegerParseError::OutOfRange), "negative for unsigned -> OutOfRange");
    CheckEq(static_cast<int>(ParseInteger<uint16_t>("70000").error()),
            static_cast<int>(IntegerParseError::OutOfRange), "70000 for uint16 -> OutOfRange");

    Check(std::string(RoboGo::app::ToString(IntegerParseError::Empty)).size() > 0,
          "error reason is printable");
}

}  // namespace

/**
 * @brief 按固定顺序跑完全部用例并汇报。
 * @return 0 表示全部通过，1 表示至少一条失败（CTest 据此判定）。
 */
int main() {
    TestPayloadLayout();
    TestPublishThrottle();
    TestEventText();
    TestToastLevels();
    TestRobotIdMapping();
    TestIntegerParsing();

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
