#include "start_panel.h"
#include "app/config.h"   // 应用级配置：登录成功后把 MQTT 地址持久化，下次启动免手输
#include "keyboard_mouse_state_manager.h"   // 标记模式开关（is_marker_mode/enter_marker_mode 等）的声明
#include "logging/logging.h"   // spdlog 日志宏（SPDLOG_INFO/DEBUG/WARN/ERROR）
#include "panel_layout.h"   // 面板布局与样式工具（ScopedStyle、ShowPanelRow、ButtonStyle 等）

namespace {   // 匿名命名空间：以下常量与辅助函数仅本翻译单元可见

// 开局面板内部实现：包含样式常量、各子面板绘制与赛前指令发送逻辑。

using RoboGo::panel::ButtonStyle;                       // 按钮样式结构体（常态/悬停/按下三色 + 内边距 + 圆角）
using RoboGo::panel::DrawPanelTitle;                     // 绘制带颜色的面板标题（文字 + 分隔线 + 间距）
using RoboGo::panel::MakeButtonStyle;                    // 按 ButtonStyle 构造按钮样式
using RoboGo::panel::MakeInputStyle;                     // 构造输入框样式
using RoboGo::panel::MakeMouseSensitivitySliderStyle;    // 构造鼠标灵敏度滑块样式
using RoboGo::panel::MakeTextColorStyle;                 // 构造仅修改文字颜色的样式
using RoboGo::panel::MakeWindowStyle;                    // 构造窗口背景/圆角/内边距样式
using RoboGo::panel::ScopedChildWindow;                  // RAII 子窗口封装（构造 BeginChild、析构 EndChild）
using RoboGo::panel::ScopedStyle;                        // RAII 样式栈封装（析构统一 Pop）
using RoboGo::panel::ScopedTitledPanel;                  // 带标题的命名子面板
using RoboGo::panel::ShowPanelRow;                       // 一行多面板横向排布工具

// 登录面板可选身份（2027：重装 / 步兵3 / 步兵4 / 指挥手）。
// 最后一个槽位原先叫"飞镖"——飞镖不是机器人，而指挥手要把哨兵、飞镖、空中系统都收进一屏，
// 所以这一列改成"指挥手"，robot_id 仍沿用 8（飞镖系统的槽位），只是 UI 身份按指挥台渲染。
// 哨兵的整套操作（姿态/强化/补弹/回血/复活）已经并入指挥台，因此不再单独占一个登录身份。
constexpr std::array<const char *, 4> kLoginRobotNames = {"重装", "3号步兵", "4号步兵", "指挥手"}; // 登录面板可选身份（下标对应 kLoginRobotIds 的列）
constexpr int kCommanderLoginSlot = 3;   // "指挥手"在登录槽位中的下标（0 基）
constexpr float kLoginStatusGap = 4.0f;             // 连接状态行与下方内容之间的垂直间距
constexpr float kLoginSectionGap = 8.0f;            // 各区块（队伍/机器人）之间的垂直间距
constexpr float kLoginMqttInputTopMargin = 4.0f;    // MQTT 地址输入框顶部留白
constexpr float kPrimaryActionButtonHeight = 30.0f; // 底部主按钮（登录/进入比赛）的高度
constexpr float kLoginTeamButtonHeight = kPrimaryActionButtonHeight;  // 队伍按钮高度对齐主按钮
constexpr float kLoginRobotButtonHeight = kPrimaryActionButtonHeight; // 机器人按钮高度对齐主按钮
constexpr float kMouseSensitivityTopGap = 8.0f;     // 鼠标灵敏度行顶部留白
constexpr float kLoginPanelBottomPadding = 12.0f;   // 登录面板底部内边距
constexpr std::string_view kDefaultMqttPort = "3333"; // 地址未显式填写端口时使用的默认 MQTT 端口

// 登录面板机器人 ID 表：[队伍][机器人序号] -> 实际 robot_id（蓝队为 100+）。
constexpr std::array<std::array<int, 4>, 2> kLoginRobotIds = {{
        {1, 3, 4, 8},   // 红队：1 重装 / 3、4 步兵 / 8 指挥手（沿用飞镖槽位连接）
        {101, 103, 104, 108},   // 蓝队：同上，robot_id 统一加 100
}};   // 结束 [队伍][序号] 的 ID 表

// 飞镖可打击目标名（与 target_id 1~5 一一对应）。
constexpr std::array<const char *, 5> kDartTargetNames = {
        "前哨站",   // target_id 1：前哨站
        "基地固定目标",   // target_id 2：基地固定目标
        "基地随机固定目标",   // target_id 3：基地随机固定目标
        "基地随机移动目标",   // target_id 4：基地随机移动目标
        "基地末端移动目标",   // target_id 5：基地末端移动目标
};   // 结束飞镖目标名表

// 补给区 Buff 的 buff_type 编号（弹药兑换需处于该 Buff 内）。
constexpr uint32_t kAmmoExchangeBuffType = 6;
// 本地下发性能选择后，忽略远端同步的时长，避免界面抖动。
constexpr auto kPerformanceSelectionRemoteSyncIgnoreDuration = std::chrono::seconds(5);

// 功能：通用的 protobuf 控制消息发布器。
// 思路：序列化消息后通过 MQTT 发布；序列化失败记错误日志。
// 注意：这里**不再**顺手调用 enter_game_mode()。原实现让"点任意指令按钮"都等于进入比赛模式，
//       后果是开局面板被准星 HUD 取代、鼠标被平台层捕获，操作手会突然发现"界面没了、鼠标也没了"，
//       而且会在准备阶段就开始向机器人发键鼠包。进入比赛只保留显式的"进入比赛"按钮。
template<typename Message>
void PublishProto(RoboGo::io::MqttClient &mqtt_client, const char *topic, const Message &message) {   // 模板参数 Message 为任意 protobuf 消息类型（CommonCommand/DartCommand 等）
    std::string serialized;                                          // 存放序列化后的字节串
    if (message.SerializeToString(&serialized)) {                     // 尝试把 protobuf 消息序列化为字节串
        SPDLOG_INFO("发布控制消息 topic={} bytes={}", // 记录发布摘要便于排查
                    topic,   // 日志参数：MQTT 话题名
                    serialized.size());   // 日志参数：序列化后的字节数
        mqtt_client.publish(topic, serialized);                      // 通过 MQTT 发布序列化后的消息
    } else {   // 序列化失败：不发布
        SPDLOG_ERROR("控制消息序列化失败 topic={}", topic);          // 序列化失败则记录错误
    }   // 结束序列化结果判断
}   // 结束 PublishProto

// 功能：获取当前已登录机器人的 robot_id，未登录返回 0。
uint32_t GetCurrentRobotId(const State &state) {
    // 已登录返回 robot_id，否则返回 0（表示未知/未登录）。
    return state.robot_static_status.has_value() ? state.robot_static_status->robot_id() : 0;
}   // 结束 GetCurrentRobotId

// 功能：判断 robot_id 是否属于给定候选集合之一（可变参数折叠）。
template<typename... Ids>
bool IsOneOf(uint32_t robot_id, Ids... ids) {   // 可变参数模板：接受任意个候选 robot_id
    // 折叠表达式：robot_id 是否等于任一候选 id。
    return ((robot_id == static_cast<uint32_t>(ids)) || ...);
}   // 结束 IsOneOf

// 功能：判断当前机器人当前是否处于指定类型的激活 Buff 中（用于按钮可用性判断）。
bool HasActiveBuff(const State &state, uint32_t buff_type) {
    uint32_t current_robot_id = GetCurrentRobotId(state);             // 取当前 robot_id
    if (current_robot_id == 0) {                                      // 未登录（无 id）
        return false;                                                 // 无法判断，视为不在 Buff
    }   // 结束未登录判断（id 为 0 时无法判断 Buff 归属）
    // 遍历当前生效的 Buff 列表，是否存在"属于我方且该类型"的激活 Buff。
    return std::ranges::any_of(state.active_buffs, [current_robot_id, buff_type](const BuffWithTimestamp &buff_entry) {
        return buff_entry.buff.robot_id() == current_robot_id && buff_entry.buff.buff_type() == buff_type;   // 命中条件：Buff 归属当前机器人且类型匹配
    });   // 结束 any_of 遍历
}   // 结束 HasActiveBuff

// 功能：带标签的纯数字输入框，并同步解析出整数值与合法性标志。
// 思路：先渲染标签与输入框，再用 strtoll 解析整串；末尾指针指向 '\0' 表示整串均为数字则合法。
void IntegerInputText(const char *label, std::string *str, bool *is_valid, long long *value) {
    ImGui::AlignTextToFramePadding();                                // 文本垂直对齐到输入框内边距，视觉更整齐
    ImGui::Text("%s", label);                                        // 渲染标签文字
    ImGui::SameLine();                                               // 让输入框紧跟在标签之后（同一行）
    ImGui::InputText("", str, ImGuiInputTextFlags_CharsDecimal);     // 仅允许十进制字符的输入框，绑定到 str
    char *endPtr;                                                    // strtoll 解析结束后的尾指针
    *value = std::strtoll(str->c_str(), &endPtr, 10);                // 以十进制解析整串为 long long
    *is_valid = *endPtr == '\0';                                     // 尾指针指向 '\0' 表示整串全是数字，合法
}   // 结束 IntegerInputText

// 功能：渲染带禁用状态与悬停提示的按钮。
// 思路：当 disable_reason 非空时禁用按钮；悬停时若有禁用原因则显示原因，否则显示有效提示文案；返回是否被点击。
bool ButtonWithTooltip(const char *label, const std::string &valid_tooltip, const char *disable_reason) {
    ImGui::BeginDisabled(disable_reason != nullptr);                  // 存在禁用原因时禁用按钮（置灰不可点）
    bool is_clicked = ImGui::Button(label);                          // 渲染按钮并记录本次点击
    ImGui::EndDisabled();                                            // 结束禁用作用域
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {        // 鼠标悬停在该按钮上
        if (disable_reason != nullptr) {                             // 禁用态：显示被禁用的原因
            ImGui::SetTooltip("%s", disable_reason);   // 禁用态：显示不可用的原因
        } else if (!valid_tooltip.empty()) {                         // 可用态：显示有效提示文案
            ImGui::SetTooltip("%s", valid_tooltip.c_str());   // 可用态：显示有效提示文案
        }   // 结束 else if 分支
    }   // 结束悬停判断
    return is_clicked;                                               // 返回该按钮本次是否被点击
}   // 结束 ButtonWithTooltip（返回本次是否点击）


// 开局面板中各子面板的可见性集合，由当前 robot_id 决定哪些机器人可发送哪些指令。
struct StartPanelVisibility {
    bool show_ammo_panel = false;// 弹药兑换面板。
    bool show_recover_panel = false;// 远程回血面板。
    bool show_revive_panel = false;// 复活控制面板。
    bool show_assembly_panel = false;// 装配控制面板（2027 重装专用）。
    bool show_marker_panel = false;// 地图标记面板（哨兵/飞镖）。
    bool show_performance_panel = false;// 性能选择面板。
    bool show_dart_panel = false;// 飞镖控制面板（飞镖系统 8 号）。
    bool show_sentry_panel = false;// 哨兵控制面板（SentryCtrlCommand）。
    bool is_hero = false;// 当前是否为重装机器人（沿用旧变量名，用于 42mm 计价）。
    bool show_robot_performance_options = false;// 是否显示机器人性能选项（发射机构/底盘）。
    bool show_sentry_performance_option = false;// 是否显示哨兵性能选项。
};   // 结束可见性集合

// 按钮的反馈信息：有效时的提示文案，以及被禁用时的原因（为空表示可用）。
struct ButtonFeedback {
    std::string valid_tooltip;             // 按钮可用时鼠标悬停显示的说明文案
    const char *disabled_reason = nullptr; // 按钮被禁用的原因文案，nullptr 表示当前可用
};   // 结束按钮反馈结构

// 登录面板选中的队伍与机器人序号（全局持久状态）。
struct TeamRobotSelectionState {
    int selected_team = 0;// 0=红队，1=蓝队。
    int selected_robot = 0;// 对应 kLoginRobotNames 下标。
};   // 结束队伍/机器人选择状态

// MQTT 服务器地址输入状态（全局持久，含输入串、错误提示与初始化标记）。
struct MqttServerAddressState {
    std::string input = "127.0.0.1:3333"; // 输入框中的服务器地址（host 或 host:port 形式）
    std::string error_message;            // 地址格式校验失败时的提示文案
    bool initialized = false;             // 是否已用默认值初始化过（避免覆盖用户已输入的内容）
};   // 结束 MQTT 地址输入状态

// 弹药兑换面板输入状态（默认允许发弹量 10）。
struct AmmoPanelState {
    std::string ammo_str = "10"; // 输入框中的允许发弹量（字符串形式，便于 ImGui 输入控件绑定）
};   // 结束弹药面板状态

// 装配控制面板输入状态（默认难度 3）。
struct AssemblyPanelState {
    std::string difficulty_str = "3"; // 输入框中的装配难度等级（1~4 级）
};   // 结束装配面板状态

// 飞镖控制面板状态（目标 ID 与闸门开关）。
struct DartPanelState {
    uint32_t target_id = 1; // 飞镖打击目标编号（1=前哨站，2=基地固定目标，3/4/5=随机或移动目标）
    bool open = false;      // 闸门开关意图：true 表示请求开启飞镖闸门
};   // 结束飞镖面板状态

// 性能选择面板状态：缓存本地下发的发射机构/底盘/哨兵模式，并跟踪远端同步时机。
struct PerformancePanelState {
    uint32_t shooter = std::numeric_limits<uint32_t>::max();   // 发射机构模式；max 表示尚未选择/尚未同步
    uint32_t chassis = std::numeric_limits<uint32_t>::max();   // 底盘模式；max 表示尚未选择
    uint32_t sentry_control = std::numeric_limits<uint32_t>::max();   // 哨兵控制模式；max 表示尚未选择
    std::chrono::steady_clock::time_point last_handled_remote_sync = std::chrono::steady_clock::time_point::min();   // 已处理过的最新远端同步时间戳，用于去重重复消息
    std::chrono::steady_clock::time_point ignore_remote_sync_until = std::chrono::steady_clock::time_point::min();   // 在该时间点之前忽略远端同步（本地下发后的静默期）
};   // 结束性能选择面板状态

const ButtonStyle kPrimaryButtonStyle = { // 主操作按钮（登录/进入比赛）样式：蓝色系
        ImVec4(0.2f, 0.6f, 1.0f, 0.8f),   // 常态背景色
        ImVec4(0.3f, 0.7f, 1.0f, 1.0f),   // 悬停背景色
        ImVec4(0.1f, 0.5f, 0.9f, 1.0f),   // 按下背景色
};   // 结束主按钮样式（内边距/圆角用 ButtonStyle 默认值）

const ButtonStyle kCompactOptionButtonStyle = { // 紧凑选项按钮默认样式：深蓝灰
        ImVec4(0.16f, 0.2f, 0.26f, 0.95f),     // 常态背景色
        ImVec4(0.2f, 0.24f, 0.31f, 1.0f),      // 悬停背景色
        ImVec4(0.12f, 0.16f, 0.22f, 1.0f),     // 按下背景色
        ImVec2(10.0f, 6.0f),                    // 内边距（横向、纵向）
        7.0f,                                   // 圆角半径
};   // 结束紧凑选项按钮样式（三色+内边距+圆角齐全）

const ButtonStyle kSelectedRedRobotButtonStyle = { // 红队机器人被选中时的高亮样式
        ImVec4(0.84f, 0.26f, 0.26f, 0.9f),       // 常态背景色（红）
        ImVec4(0.92f, 0.34f, 0.34f, 1.0f),       // 悬停背景色
        ImVec4(0.74f, 0.18f, 0.18f, 1.0f),       // 按下背景色
        ImVec2(10.0f, 6.0f),                      // 内边距
        7.0f,                                     // 圆角半径
};   // 结束红队机器人选中样式

const ButtonStyle kSelectedBlueRobotButtonStyle = { // 蓝队机器人被选中时的高亮样式
        ImVec4(0.2f, 0.6f, 1.0f, 0.9f),           // 常态背景色（蓝）
        ImVec4(0.28f, 0.68f, 1.0f, 1.0f),         // 悬停背景色
        ImVec4(0.14f, 0.5f, 0.92f, 1.0f),         // 按下背景色
        ImVec2(10.0f, 6.0f),                       // 内边距
        7.0f,                                      // 圆角半径
};   // 结束蓝队机器人选中样式

const ButtonStyle kSelectedRedTeamButtonStyle = { // 红队被选中时的高亮样式
        ImVec4(0.88f, 0.28f, 0.28f, 0.92f),       // 常态背景色（红）
        ImVec4(0.95f, 0.35f, 0.35f, 1.0f),        // 悬停背景色
        ImVec4(0.78f, 0.2f, 0.2f, 1.0f),          // 按下背景色
        ImVec2(10.0f, 6.0f),                       // 内边距
        7.0f,                                      // 圆角半径
};   // 结束红队选中样式

const ButtonStyle kSelectedBlueTeamButtonStyle = { // 蓝队被选中时的高亮样式
        ImVec4(0.18f, 0.52f, 0.98f, 0.92f),       // 常态背景色（蓝）
        ImVec4(0.26f, 0.6f, 1.0f, 1.0f),          // 悬停背景色
        ImVec4(0.12f, 0.42f, 0.9f, 1.0f),         // 按下背景色
        ImVec2(10.0f, 6.0f),                       // 内边距
        7.0f,                                      // 圆角半径
};   // 结束蓝队选中样式

const ButtonStyle kAssemblyButtonStyle = { // 装配控制按钮样式：青蓝色
        ImVec4(0.3f, 0.8f, 1.0f, 0.8f),     // 常态背景色
        ImVec4(0.4f, 0.9f, 1.0f, 1.0f),     // 悬停背景色
        ImVec4(0.2f, 0.7f, 0.9f, 1.0f),     // 按下背景色
};   // 结束装配按钮样式


const ButtonStyle kRecoverButtonStyle = { // 远程回血按钮样式：红色
        ImVec4(1.0f, 0.3f, 0.3f, 0.8f),     // 常态背景色
        ImVec4(1.0f, 0.4f, 0.4f, 1.0f),     // 悬停背景色
        ImVec4(0.9f, 0.2f, 0.2f, 1.0f),     // 按下背景色
};   // 结束回血按钮样式

const ButtonStyle kReviveButtonStyle = { // 复活控制按钮样式：黄色
        ImVec4(1.0f, 1.0f, 0.3f, 0.8f),     // 常态背景色
        ImVec4(1.0f, 1.0f, 0.4f, 1.0f),     // 悬停背景色
        ImVec4(0.9f, 0.9f, 0.2f, 1.0f),     // 按下背景色
};   // 结束复活按钮样式

const ButtonStyle kMarkerButtonStyle = { // 地图标记按钮样式：橙色
        ImVec4(1.0f, 0.6f, 0.2f, 0.8f),     // 常态背景色
        ImVec4(1.0f, 0.7f, 0.3f, 1.0f),     // 悬停背景色
        ImVec4(0.9f, 0.5f, 0.1f, 1.0f),     // 按下背景色
};   // 结束标记按钮样式

const ButtonStyle kDartButtonStyle = { // 飞镖控制按钮样式：粉色
        ImVec4(1.0f, 0.4f, 0.8f, 0.8f),     // 常态背景色
        ImVec4(1.0f, 0.5f, 0.9f, 1.0f),     // 悬停背景色
        ImVec4(0.9f, 0.3f, 0.7f, 1.0f),     // 按下背景色
};   // 结束飞镖按钮样式


const ButtonStyle kEnterGameButtonStyle = { // 进入比赛/登录主按钮样式：绿色描边
        ImVec4(0.0f, 0.8f, 0.4f, 0.9f),     // 常态背景色
        ImVec4(0.0f, 1.0f, 0.5f, 1.0f),     // 悬停背景色
        ImVec4(0.0f, 0.6f, 0.3f, 1.0f),     // 按下背景色
        ImVec2(0.0f, 0.0f),                  // 内边距（此处为 0，由按钮尺寸决定）
        8.0f,                                // 圆角半径
};   // 结束进入比赛按钮样式

// 功能：构造一个"因某原因被禁用"的按钮反馈。
ButtonFeedback MakeDisabledFeedback(const char *reason) {
    ButtonFeedback feedback;                                         // 默认构造（valid_tooltip 为空）
    feedback.disabled_reason = reason;                               // 记录禁用原因
    return feedback;                                                 // 返回仅含禁用原因的反馈
}   // 结束 MakeDisabledFeedback

// 功能：构造一个提示"将消耗 N 金币"的有效按钮反馈。
ButtonFeedback MakePriceFeedback(uint32_t price) {
    ButtonFeedback feedback;                                         // 默认构造（disabled_reason 为空=可用）
    feedback.valid_tooltip = "将消耗" + std::to_string(price) + "金币"; // 拼接"将消耗 N 金币"提示文案
    return feedback;                                                 // 返回含有效提示的反馈
}   // 结束 MakePriceFeedback

// 功能：根据当前经济判断是否买得起指定价格的指令，返回相应反馈。
// 思路：无经济信息则禁用；剩余经济不足则禁用；否则给出消耗金币提示。
ButtonFeedback GetEconomyFeedback(const State &state, uint32_t price) {
    if (!state.global_logistics_status.has_value()) {                // 尚未收到全局后勤（经济）状态
        return MakeDisabledFeedback("无法获取当前经济");             // 无数据则禁用
    }   // 结束无经济数据判断
    if (state.global_logistics_status->remaining_economy() < price) { // 剩余经济不足以支付该指令价格
        return MakeDisabledFeedback("当前经济不足");                 // 经济不足则禁用
    }   // 结束经济不足判断
    return MakePriceFeedback(price);                                 // 买得起：返回消耗金币提示
}   // 结束 GetEconomyFeedback

// 功能：计算"兑换允许发弹量"按钮的可用性反馈。
// 思路：必须在补给区 Buff 内、输入合法；重装按 10 金币/发计价，其他机器人要求发量为 10 的倍数再计价。
ButtonFeedback GetAmmoExchangeFeedback(const State &state, bool is_valid_ammo, long long ammo, bool is_hero) {
    if (!HasActiveBuff(state, kAmmoExchangeBuffType)) {              // 必须处于补给区 Buff 内
        return MakeDisabledFeedback("不在补给区内");                 // 不在补给区则禁用
    }   // 结束补给区判断
    if (!is_valid_ammo) {                                           // 输入的发弹量含非法字符
        return MakeDisabledFeedback("输入含非法字符");         // 非法输入则禁用
    }   // 结束输入合法性判断
    if (is_hero) {                                                  // 重装按 10 金币/发计价
        return GetEconomyFeedback(state, static_cast<uint32_t>(ammo * 10));   // 重装按 10 金币/发换算总价后走经济判断
    }   // 结束重装分支
    if (ammo % 10 != 0) {                                           // 非重装要求发量为 10 的倍数
        return MakeDisabledFeedback("必须是 10 的倍数");   // 非重装且不是 10 的倍数则禁用
    }   // 结束倍数判断
    return GetEconomyFeedback(state, static_cast<uint32_t>(ammo));  // 普通机器人按 1 金币/发计价
}   // 结束 GetAmmoExchangeFeedback

// 功能：计算"远程兑换"按钮的可用性反馈。
// 思路：输入合法前提下，英雄要求发量为 10 倍数且按 15 金币/发计价，其他机器人要求 100 倍数并按 1.5 倍计价。
ButtonFeedback GetRemoteAmmoExchangeFeedback(const State &state, bool is_valid_ammo, long long ammo, bool is_hero) {
    if (!is_valid_ammo) {                                           // 输入的发弹量含非法字符
        return MakeDisabledFeedback("输入含非法字符");         // 非法输入则禁用
    }   // 结束输入合法性判断
    if (is_hero) {                                                  // 重装：发量 10 倍数，按 15 金币/发
        if (ammo % 10 != 0) {   // 英雄同样要求发量为 10 的倍数
            return MakeDisabledFeedback("必须是 10 的倍数");   // 非 10 倍数则禁用
        }   // 结束倍数判断
        return GetEconomyFeedback(state, static_cast<uint32_t>(ammo * 15));   // 英雄按 15 金币/发换算总价
    }   // 结束重装分支
    if (ammo % 100 != 0) {                                          // 非英雄要求发量为 100 的倍数
        return MakeDisabledFeedback("必须是 100 的倍数");   // 非英雄要求 100 的倍数
    }   // 结束倍数判断
    return GetEconomyFeedback(state, static_cast<uint32_t>(ammo * 1.5)); // 普通机器人按 1.5 倍计价
}   // 结束 GetRemoteAmmoExchangeFeedback

// 功能：计算"远程回血"按钮的可用性反馈。
// 思路：依据剩余比赛时间动态计算回血价格（越接近结束越便宜），再走经济判断。
ButtonFeedback GetRecoverFeedback(const State &state) {
    if (!state.game_status.has_value()) {                           // 尚未收到比赛状态
        return MakeDisabledFeedback("无法获取比赛状态");            // 无数据则禁用
    }   // 结束比赛状态判断
    // 依据剩余比赛时间动态定价：越接近结束越便宜（420 为基准，按每分钟 20 递减）。
    uint32_t price = static_cast<uint32_t>(50 + std::ceil(420 - state.game_status->stage_countdown_sec() / 60 * 20));
    return GetEconomyFeedback(state, price);                         // 走经济判断返回反馈
}   // 结束 GetRecoverFeedback

// 功能：计算"确认复活"按钮的可用性反馈。
// 思路：需处于复活待定状态、读条已完成且可免费复活才允许。
ButtonFeedback GetConfirmReviveFeedback(const State &state) {
    if (!state.robot_respawn_status.has_value()) {                   // 尚未收到复活状态
        return MakeDisabledFeedback("无法获取复活状态");            // 无数据则禁用
    }   // 结束复活状态判断
    const auto &respawn_status = *state.robot_respawn_status;        // 取复活状态引用
    if (!respawn_status.is_pending_respawn()) {                      // 机器人当前不在复活待定状态
        return MakeDisabledFeedback("机器人不在复活状态");   // 不在复活待定状态则禁用
    }   // 结束待定判断
    if (respawn_status.total_respawn_progress() > 0 &&               // 存在读条进度且尚未读满
        respawn_status.current_respawn_progress() < respawn_status.total_respawn_progress()) {   // 读条未满：当前进度小于总进度
        return MakeDisabledFeedback("复活读条未完成");              // 读条未完成则禁用
    }   // 结束读条判断
    if (!respawn_status.can_free_respawn()) {                        // 不可免费复活
        return MakeDisabledFeedback("无法免费复活");   // 不可免费复活则禁用
    }   // 结束免费复活判断
    return {};                                                       // 满足条件：可用（无提示）
}   // 结束 GetConfirmReviveFeedback

// 功能：计算"兑换立即复活"按钮的可用性反馈。
// 思路：需可花金币复活，再按复活所需金币走经济判断。
ButtonFeedback GetInstantReviveFeedback(const State &state) {
    if (!state.robot_respawn_status.has_value()) {                   // 尚未收到复活状态
        return MakeDisabledFeedback("无法获取复活状态");            // 无数据则禁用
    }   // 结束复活状态判断
    const auto &respawn_status = *state.robot_respawn_status;        // 取复活状态引用
    if (!respawn_status.can_pay_for_respawn()) {                     // 不支持花金币复活
        return MakeDisabledFeedback("无法花费金币复活");   // 不支持花金币复活则禁用
    }   // 结束金币复活判断
    uint32_t price = respawn_status.gold_cost_for_respawn();         // 取复活所需金币
    return GetEconomyFeedback(state, price);                         // 走经济判断返回反馈
}   // 结束 GetInstantReviveFeedback

// 功能：计算"确认发射（飞镖）"按钮的可用性反馈。
// 思路：需能获取飞镖状态，且闸门已开启（open==2）。
ButtonFeedback GetDartLaunchFeedback(const State &state) {
    if (!state.dart_select_target_status_sync.has_value()) {         // 尚未收到飞镖同步状态
        return MakeDisabledFeedback("无法获取飞镖状态");            // 无数据则禁用
    }   // 结束飞镖状态判断
    if (state.dart_select_target_status_sync->open() != 2) {         // 闸门未开启（open==2 表示已开）
        return MakeDisabledFeedback("闸门未开启");   // 闸门未开启（open 不等于 2）则禁用
    }   // 结束闸门判断
    return {};                                                       // 闸门已开：可用
}   // 结束 GetDartLaunchFeedback

// 思路：需未在进行中且有可用免费时长。

// 思路：需未在进行中；若仍有免费时长提示优先使用免费，否则提示发起付费。


// 思路：需当前发射机构处于"可解除锁定"状态（shooter_status==2）才允许。

// 功能：构造"进入比赛/登录"主按钮的样式（绿色描边突出）。
ScopedStyle MakeEnterGameButtonStyle() {
    ScopedStyle style = MakeButtonStyle(kEnterGameButtonStyle);       // 基础绿色按钮样式
    style.PushVar(ImGuiStyleVar_FrameBorderSize, 2.0f);               // 加 2px 边框突出主按钮
    style.PushColor(ImGuiCol_Border, ImVec4(0.3f, 1.0f, 0.6f, 0.8f)); // 绿色边框
    style.PushColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));  // 白色文字
    return style;   // 返回样式对象（析构时自动弹栈）
}   // 结束 MakeEnterGameButtonStyle

// 功能：构造开局面板窗口的半透明背景样式。
ScopedStyle MakeStartPanelWindowStyle() {
    // 半透明深色背景 + 8px 圆角 + 15px 内边距的开局面板窗口样式。
    return MakeWindowStyle(ImVec4(0.1f, 0.15f, 0.2f, 0.5f), 8.0f, ImVec2(15.0f, 15.0f));
}   // 结束 MakeStartPanelWindowStyle

// 功能：把 MQTT 连接状态枚举翻译为中文文案。
const char *GetLoginConnectionStateText(RoboGo::io::MqttClient::ConnectionState connection_state) {
    switch (connection_state) {   // 按 MQTT 连接状态分支
        case RoboGo::io::MqttClient::ConnectionState::Idle:   // 空闲：从未发起连接
            return "未登录";                                        // 未发起连接
        case RoboGo::io::MqttClient::ConnectionState::Connecting:   // 连接中
            return "连接中";                                        // 正在连接服务端
        case RoboGo::io::MqttClient::ConnectionState::Connected:   // 已连接
            return "已登录";                                        // 已成功连接
        case RoboGo::io::MqttClient::ConnectionState::Failed:   // 失败并在重试
            return "连接失败，重试中";                              // 连接失败正在重试
        default:   // 未定义的枚举值兜底
            return "未知状态";                                      // 未定义的枚举值
    }   // 结束状态映射 switch
}   // 结束 GetLoginConnectionStateText

// 功能：根据连接状态返回对应的提示颜色（灰/黄/绿/红）。
ImVec4 GetLoginConnectionStateColor(RoboGo::io::MqttClient::ConnectionState connection_state) {
    switch (connection_state) {   // 按连接状态选择提示色
        case RoboGo::io::MqttClient::ConnectionState::Idle:   // 空闲：中性灰
            return ImVec4(0.7f, 0.7f, 0.75f, 1.0f);                 // 灰色
        case RoboGo::io::MqttClient::ConnectionState::Connecting:   // 连接中：黄色（进行中）
            return ImVec4(1.0f, 0.8f, 0.35f, 1.0f);                 // 黄色（进行中）
        case RoboGo::io::MqttClient::ConnectionState::Connected:   // 已连接：绿色（成功）
            return ImVec4(0.35f, 1.0f, 0.55f, 1.0f);                // 绿色（成功）
        case RoboGo::io::MqttClient::ConnectionState::Failed:   // 失败：红色
            return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);                // 红色（失败）
        default:   // 兜底白色
            return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);                  // 白色兜底
    }   // 结束颜色 switch
}   // 结束 GetLoginConnectionStateColor

// 功能：去除字符串首尾的 ASCII 空白字符（空格/制表/回车/换行）。
// 思路：双指针分别从两端向中间收缩，跳过空白后取子串。
std::string TrimAsciiWhitespace(std::string_view text) {
    auto is_space = [](char ch) {                                   // 判定 ASCII 空白字符
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';   // 空白判定：空格/制表/回车/换行
    };   // 结束 is_space lambda

    std::size_t start = 0;                                          // 从头扫描
    while (start < text.size() && is_space(text[start])) {   // 从前向后跳过前导空白
        start += 1;                                                 // 跳过前导空白
    }   // 结束前导跳过
    std::size_t end = text.size();                                  // 从尾扫描
    while (end > start && is_space(text[end - 1])) {   // 从后向前跳过尾部空白
        end -= 1;                                                   // 跳过尾部空白
    }   // 结束尾部跳过
    return std::string(text.substr(start, end - start));            // 截取去空白后的子串
}   // 结束 TrimAsciiWhitespace

// 功能：解析用户输入的 MQTT 服务端地址（支持 "host:port" / "tcp://host:port" / IPv6 "[host]:port"），拆分出 host 与 port。
// 思路：先去除空白、剥离可选 "tcp://" 前缀；按首 '[' 判定 IPv6，否则按冒号位置拆分；port 缺省填 3333 并校验数字范围 1-65535。
bool ParseMqttServerEndpoint(const std::string &input, std::string &host, std::string &port, std::string &error_message) {
    std::string normalized = TrimAsciiWhitespace(input);              // 先去除首尾空白
    if (normalized.empty()) {                                         // 去除后仍为空
        error_message = "请输入 MQTT 服务端地址";                     // 提示需输入
        return false;   // 空地址直接判为非法
    }   // 结束空串判断
    if (normalized.starts_with("tcp://")) {                           // 剥离可选的 tcp:// 前缀
        normalized = normalized.substr(6);   // 去掉开头 6 个字符的 tcp 协议前缀
    }   // 结束前缀剥离
    normalized = TrimAsciiWhitespace(normalized);                     // 再次去空白（前缀后可能残留）
    if (normalized.empty()) {   // 剥掉前缀后可能变成空串
        error_message = "服务端地址不能为空";   // 提示地址不能为空
        return false;   // 解析失败
    }   // 结束二次空串判断

    std::string host_part;                                            // 待解析的主机部分
    std::string port_part;                                            // 待解析的端口部分
    if (normalized.front() == '[') {                                  // 以 '[' 开头 => 可能是 IPv6
        const std::size_t close_bracket = normalized.find(']');       // 找配对的 ']'
        if (close_bracket == std::string::npos) {                     // 缺少 ']'
            error_message = "IPv6 地址请使用 [地址]:端口 格式";       // 提示正确格式
            return false;   // 缺少 ']' 无法确定 IPv6 主机边界
        }   // 结束括号闭合判断
        host_part = normalized.substr(1, close_bracket - 1);          // 取 '[' 与 ']' 之间的主机
        if (close_bracket + 1 < normalized.size()) {                  // ']' 后还有内容
            if (normalized[close_bracket + 1] != ':') {               // 必须是 ':端口'
                error_message = "IPv6 地址后只能跟端口";             // 否则报错
                return false;   // ']' 之后不是 ':' 则格式非法
            }   // 结束字符判断
            port_part = normalized.substr(close_bracket + 2);         // 取 ':' 之后的端口
        }   // 结束 ']' 之后内容判断
    } else {                                                          // 普通 host:port
        const std::size_t first_colon = normalized.find(':');         // 第一个冒号
        const std::size_t last_colon = normalized.rfind(':');         // 最后一个冒号
        if (first_colon == std::string::npos) {                       // 没有冒号 => 仅主机
            host_part = normalized;   // 没有冒号：整串都视为主机（端口留空走默认值）
        } else if (first_colon == last_colon) {                       // 只有一个冒号 => host:port
            host_part = normalized.substr(0, first_colon);           // 冒号前为主机
            port_part = normalized.substr(first_colon + 1);          // 冒号后为端口
        } else {                                                      // 多个冒号且非 IPv6 => 非法
            error_message = "检测到多个冒号，请使用 [IPv6]:端口 格式";   // 多个冒号且非 IPv6 形式，无法确定主机/端口边界
            return false;   // 解析失败
        }   // 结束普通 host:port 拆分
    }   // 结束 IPv6/普通地址分支

    host_part = TrimAsciiWhitespace(host_part);                       // 去主机部分空白
    port_part = TrimAsciiWhitespace(port_part);                       // 去端口部分空白
    if (host_part.empty()) {                                          // 主机为空
        error_message = "服务端主机不能为空";   // 主机为空同样非法
        return false;   // 解析失败
    }   // 结束主机判空
    if (port_part.empty()) {                                          // 端口缺省
        port_part = std::string(kDefaultMqttPort);                    // 使用默认端口 3333
    }   // 结束端口缺省判断
    if (!std::ranges::all_of(port_part, [](char ch) { return ch >= '0' && ch <= '9'; })) { // 校验端口全为数字
        error_message = "端口必须为数字";   // 端口含非数字字符
        return false;   // 解析失败
    }   // 结束数字校验

    const auto parsed_port = std::strtoll(port_part.c_str(), nullptr, 10); // 字符串端口转整数
    if (parsed_port <= 0 || parsed_port > 65535) {                      // 校验端口范围 1~65535
        error_message = "端口范围应为 1 - 65535";   // 端口超出 1~65535 合法区间
        return false;   // 解析失败
    }   // 结束端口范围校验

    host = host_part;                                                 // 输出主机
    port = std::to_string(parsed_port);                               // 输出端口（规范化为数字串）
    error_message.clear();                                            // 解析成功，清空错误
    return true;   // 解析成功
}   // 结束 ParseMqttServerEndpoint

// 功能：返回全局唯一的队伍/机器人选择状态（静态单例）。
TeamRobotSelectionState &GetTeamRobotSelectionState() {
    static TeamRobotSelectionState selection_state;                  // 全局唯一选择状态（静态单例）
    return selection_state;                                          // 返回引用，使各子面板共享同一状态
}   // 结束 GetTeamRobotSelectionState

// 功能：返回全局唯一的 MQTT 地址输入状态；首次访问时从已配置的服务端端点预填输入框。
MqttServerAddressState &GetMqttServerAddressState(RoboGo::io::MqttClient &mqtt_client) {
    static MqttServerAddressState mqtt_server_state;                  // 全局唯一地址状态（静态单例）
    if (!mqtt_server_state.initialized) {                             // 首次访问：尝试预填默认值
        const auto [host, port] = mqtt_client.get_server_endpoint();  // 取已配置的服务端端点
        if (!host.empty() && !port.empty()) {                         // 端点有效
            mqtt_server_state.input = host + ":" + port;              // 预填 "host:port"
        }   // 结束端点有效判断
        mqtt_server_state.initialized = true;                         // 标记已初始化，避免覆盖用户后续输入
    }   // 结束首次初始化判断
    return mqtt_server_state;                                         // 返回全局状态引用
}   // 结束 GetMqttServerAddressState

// 功能：返回全局唯一的弹药兑换面板状态。
AmmoPanelState &GetAmmoPanelState() {
    static AmmoPanelState ammo_panel_state;                          // 全局唯一弹药面板状态
    return ammo_panel_state;                                         // 返回引用
}   // 结束 GetAmmoPanelState

// 功能：返回全局唯一的装配面板状态。
AssemblyPanelState &GetAssemblyPanelState() {
    static AssemblyPanelState assembly_panel_state;                  // 全局唯一装配面板状态
    return assembly_panel_state;                                     // 返回引用
}   // 结束 GetAssemblyPanelState

// 功能：返回全局唯一的飞镖面板状态，并实时用远端同步的目标/闸门状态覆盖本地。
DartPanelState &GetDartPanelState(const State &state) {
    static DartPanelState dart_panel_state;                           // 全局唯一飞镖面板状态
    if (state.dart_select_target_status_sync.has_value()) {           // 收到远端飞镖同步
        // 本地与远端不一致时记调试日志，便于排查。
        if (dart_panel_state.target_id != state.dart_select_target_status_sync->target_id() ||
            dart_panel_state.open != (state.dart_select_target_status_sync->open() != 0)) {   // 本地目标或闸门开关与远端不一致时进入差异日志
            SPDLOG_DEBUG("同步飞镖状态 target_id={} open={}",   // 日志：远端下发的目标编号
                         state.dart_select_target_status_sync->target_id(),   // 日志参数占位（前半段格式串已写出）
                         state.dart_select_target_status_sync->open() != 0);   // 日志参数：远端闸门状态（非 0 即开）
        }   // 结束差异日志判断
        dart_panel_state.target_id = state.dart_select_target_status_sync->target_id(); // 用远端目标覆盖本地
        dart_panel_state.open = state.dart_select_target_status_sync->open() != 0;      // 用远端闸门状态覆盖本地（非 0 即开）
    }   // 结束远端同步覆盖
    return dart_panel_state;                                          // 返回（可能已同步的）状态引用
}   // 结束 GetDartPanelState

// 功能：返回全局唯一的性能选择面板状态，并在收到远端同步时更新本地选项。
// 思路：用 last_handled_remote_sync 去重已处理消息；若落在"本地下发后忽略窗口"内则跳过，避免刚下发又被远端覆盖。
PerformancePanelState &GetPerformancePanelState(const State &state) {
    static PerformancePanelState performance_panel_state;             // 全局唯一性能面板状态
    // 仅在收到"比上次更新"的远端同步时处理（用 last_handled 去重重复消息）。
    if (state.robot_performance_selection_sync.has_value() &&
        performance_panel_state.last_handled_remote_sync < state.last_received_robot_performance_selection_sync) {   // 只在收到「比上次处理更晚」的同步时才覆盖（时间戳去重）
        performance_panel_state.last_handled_remote_sync = state.last_received_robot_performance_selection_sync; // 更新去重标记
        // 若落在"本地下发后忽略窗口"内，跳过远端覆盖，避免刚下发又被旧同步覆盖。
        if (state.last_received_robot_performance_selection_sync < performance_panel_state.ignore_remote_sync_until) {
            SPDLOG_DEBUG("忽略性能选择远端同步，等待本地下发结果稳定");   // 处于本地静默期：只记调试日志，不覆盖本地选择
        } else {   // 不在静默期：采用远端值
            performance_panel_state.shooter = state.robot_performance_selection_sync->shooter();       // 同步发射机构选项
            performance_panel_state.chassis = state.robot_performance_selection_sync->chassis();       // 同步底盘选项
            performance_panel_state.sentry_control = state.robot_performance_selection_sync->sentry_control(); // 同步哨兵选项
        }   // 结束静默期分支
    }   // 结束远端同步处理
    return performance_panel_state;                                   // 返回（可能已同步的）状态引用
}   // 结束 GetPerformancePanelState

// 功能：发送通用指令（cmd_type 区分回血/复活/弹药兑换等，param 为附带参数）。
// 思路：填充 cmd_type 与 param 后发布到 "CommonCommand" 话题，并附带进入比赛标记。
void SendCommonCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t cmd_type, uint32_t param = 0) {
    CommonCommand common_command;                                     // 构造通用指令消息
    common_command.set_cmd_type(cmd_type);                            // 填入指令类型（区分回血/复活/弹药等）
    common_command.set_param(param);                                  // 填入附带参数（如发弹量）
    PublishProto(mqtt_client, "CommonCommand", common_command);  // 发布到 "CommonCommand"（不再顺带进入比赛模式）
}   // 结束 SendCommonCommand

// 功能：发送哨兵控制指令（2027：1补弹/3远程补弹/4远程回血/5确认复活/6花金币复活/7-12切换姿态）。
// 思路：填充 command_id 后发布到 "SentryCtrlCommand" 话题。
void SendSentryCtrlCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t command_id) {
    SentryCtrlCommand sentry_ctrl_command;                            // 构造哨兵控制指令
    sentry_ctrl_command.set_command_id(command_id);                   // 填入指令编号
    PublishProto(mqtt_client, "SentryCtrlCommand", sentry_ctrl_command); // 发布哨兵控制指令
}   // 结束 SendSentryCtrlCommand

// 功能：发送飞镖指令（目标、闸门开关、是否确认发射）。
// 思路：填充字段后发布到 "DartCommand" 话题。飞镖本身不需要切到比赛模式（操作手可能还在开局面板）。
void SendDartCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t target_id, bool open, bool launch_confirm) {
    DartCommand dart_command;                                         // 构造飞镖指令消息
    dart_command.set_target_id(target_id);                           // 目标编号
    dart_command.set_open(open);                                     // 闸门开关意图
    dart_command.set_launch_confirm(launch_confirm);                 // 是否确认发射
    PublishProto(mqtt_client, "DartCommand", dart_command);          // 发布飞镖指令
}   // 结束 SendDartCommand

// 功能：发送装配指令（difficulty 难度，operation 操作：0 开始兑换 / 1 确认装配 / 2 取消装配）。
// 思路：填充字段后发布到 "AssemblyCommand" 话题，并附带进入比赛标记。
void SendAssemblyCommand(RoboGo::io::MqttClient &mqtt_client, uint32_t difficulty, uint32_t operation) {
    AssemblyCommand assembly_command;                                 // 构造装配指令消息
    assembly_command.set_difficulty(difficulty);                      // 装配难度等级
    assembly_command.set_operation(operation);                        // 操作：0 开始兑换 / 1 确认装配 / 2 取消装配（2027 协议 2.2.21）
    PublishProto(mqtt_client, "AssemblyCommand", assembly_command); // 发布装配指令
}   // 结束 SendAssemblyCommand


// 功能：发送机器人性能选择指令，并标记一段"忽略远端同步"窗口。
// 思路：先把 ignore_remote_sync_until 设为当前+5s，再把本地发射机构/底盘/哨兵选项下发到 "RobotPerformanceSelectionCommand"。
void SendPerformanceSelection(RoboGo::io::MqttClient &mqtt_client, PerformancePanelState &performance_state) {
    // 先标记一段"忽略远端同步"窗口，避免下发后立刻被旧的远端同步覆盖本地选择。
    performance_state.ignore_remote_sync_until = std::chrono::steady_clock::now() + kPerformanceSelectionRemoteSyncIgnoreDuration;
    RobotPerformanceSelectionCommand performance_command;             // 构造性能选择指令
    performance_command.set_shooter(performance_state.shooter);       // 填入发射机构选项
    performance_command.set_chassis(performance_state.chassis);       // 填入底盘选项
    performance_command.set_sentry_control(performance_state.sentry_control); // 填入哨兵选项
    PublishProto(mqtt_client, "RobotPerformanceSelectionCommand", performance_command); // 发布（不进入比赛）
}   // 结束 SendPerformanceSelection

// 功能：渲染"标签 + 两个单选按钮"的一行，返回是否发生改变。
// 思路：用 SameLine 把两个 RadioButton 对齐到指定 x 偏移；选中则更新 selected_value 并标记 changed。
bool ShowLabeledRadioPair(const char *label,
                          float first_option_x,   // 参数：第一个选项按钮相对行首的 X 偏移
                          float second_option_x,   // 参数：第二个选项按钮相对行首的 X 偏移
                          const char *first_option_label,   // 参数：第一个选项的显示文案（含 ## 后的唯一 ID）
                          uint32_t first_value,   // 参数：第一个选项对应的枚举值
                          const char *second_option_label,   // 参数：第二个选项的显示文案
                          uint32_t second_value,   // 参数：第二个选项对应的枚举值
                          uint32_t &selected_value) {   // 参数：引用传入的当前选中值，本函数会按点击结果改写
    bool changed = false;                                             // 是否发生选项改变
    ImGui::Text("%s", label);                                         // 渲染标签
    ImGui::SameLine(first_option_x);                                  // 第一个选项对齐到指定 X
    if (ImGui::RadioButton(first_option_label, selected_value == first_value)) { // 是否选中第一项
        selected_value = first_value;                                  // 更新选中值
        changed = true;                                               // 标记改变
    }   // 结束第一项点击判断
    ImGui::SameLine(second_option_x);                                 // 第二个选项对齐到指定 X
    if (ImGui::RadioButton(second_option_label, selected_value == second_value)) { // 是否选中第二项
        selected_value = second_value;                                 // 更新选中值
        changed = true;                                               // 标记改变
    }   // 结束第二项点击判断
    return changed;                                                   // 返回是否改变（供调用方下发）
}   // 结束 ShowLabeledRadioPair

// 功能：渲染一个带选中态高亮的紧凑选项按钮。
// 思路：选中时套用 selected_style（颜色突出），否则用默认紧凑样式；返回是否被点击。
bool ShowCompactSelectionButton(const char *label, bool is_selected, const ButtonStyle &selected_style, const ImVec2 &size) {
    ScopedStyle style = MakeButtonStyle(is_selected ? selected_style : kCompactOptionButtonStyle); // 选中用高亮样式，否则默认紧凑样式
    return ImGui::Button(label, size);                                       // 渲染按钮并返回是否被点击
}   // 结束 ShowCompactSelectionButton

// 功能：根据所选队伍返回机器人按钮对应的高亮样式（红队/蓝队色）。
const ButtonStyle &GetSelectedRobotButtonStyle(const TeamRobotSelectionState &selection_state) {
    // 根据所选队伍返回对应的机器人高亮样式（红队红 / 蓝队蓝）。
    return selection_state.selected_team == 0 ? kSelectedRedRobotButtonStyle : kSelectedBlueRobotButtonStyle;
}   // 结束 GetSelectedRobotButtonStyle

// 功能：处理"登录"按钮：校验地址合法性后把服务端端点与客户 ID 写入 MQTT 客户端。
// 思路：先解析地址；失败则将错误原因回填到输入框状态并返回；成功则设置端点与 client_id 为当前 robot_id。
bool SubmitLoginSelection(RoboGo::io::MqttClient &mqtt_client) {
    MqttServerAddressState &mqtt_server_state = GetMqttServerAddressState(mqtt_client); // 取全局地址输入状态
    std::string mqtt_host;                                             // 解析出的主机
    std::string mqtt_port;                                             // 解析出的端口
    std::string parse_error;                                           // 解析错误信息
    if (!ParseMqttServerEndpoint(mqtt_server_state.input, mqtt_host, mqtt_port, parse_error)) { // 校验并拆分地址
        mqtt_server_state.error_message = parse_error;                 // 把错误回填到输入框状态
        SPDLOG_WARN("登录按钮点击失败，MQTT服务端地址非法 input={} reason={}", // 记录警告日志
                    mqtt_server_state.input,   // 日志参数：用户输入的原始地址串
                    parse_error);   // 日志参数：解析失败的具体原因
        return false;                                                  // 解析失败，阻止登录
    }   // 结束解析失败分支（阻止登录）
    mqtt_server_state.error_message.clear();                           // 解析成功，清空错误提示

    TeamRobotSelectionState &selection_state = GetTeamRobotSelectionState(); // 取全局队伍/机器人选择
    int current_robot_id = kLoginRobotIds[selection_state.selected_team][selection_state.selected_robot]; // 计算实际 robot_id
    SPDLOG_INFO("登录按钮点击 selected_team={} selected_robot={} client_id={} mqtt_server={}:{}", // 记录登录信息
                selection_state.selected_team,   // 日志参数：选中的队伍下标（0 红 / 1 蓝）
                selection_state.selected_robot,   // 日志参数：选中的机器人序号
                current_robot_id,   // 日志参数：实际 robot_id，同时用作 MQTT client_id
                mqtt_host,   // 日志参数：解析出的主机
                mqtt_port);   // 日志参数：解析出的端口
    // 登录成功即把"归一化后的 host:port"写进用户配置：下次启动自动预填，避免现场反复手输（输错一位就要排查半天）。
    // 必须在 move 之前保存：下面 set_server_endpoint 会把 mqtt_host/mqtt_port 移走。
    RoboGo::app::SaveConfiguredMqttServer(mqtt_host + ":" + mqtt_port); // 只存 host:port，前缀由启动时统一补
    mqtt_client.set_server_endpoint(std::move(mqtt_host), std::move(mqtt_port)); // 设置 MQTT 服务端端点
    mqtt_client.set_client_id(std::to_string(current_robot_id));      // 以 robot_id 作为 client_id 连接
    // 记录"这次登录用的是不是指挥手槽位"：指挥手没有自己的 robot_id，后续只能用这个标志区分身份。
    set_commander_role(selection_state.selected_robot == kCommanderLoginSlot);
    return true;                                                      // 登录参数已写入，返回成功
}   // 结束 SubmitLoginSelection

// 功能：根据连接状态计算登录/队伍选择子面板所需高度（已登录时仅显示退出按钮，更矮）。
// 思路：按各文本行/按钮高度与间距累加；已连接分支走精简布局。
float GetLoginPanelHeight(RoboGo::io::MqttClient::ConnectionState connection_state) {
    const ImGuiStyle &style = ImGui::GetStyle();                      // 取全局 ImGui 样式（含间距/内边距）
    const float scale = ImGui::GetScale();                            // DPI 缩放比例
    const float text_line_height = ImGui::GetTextLineHeight();        // 单行文本高度
    const float frame_height = ImGui::GetFrameHeight();               // 控件（按钮/输入框）高度
    const float team_button_height = std::max(frame_height, kLoginTeamButtonHeight * scale);   // 队伍按钮高度（不小于框高）
    const float robot_button_height = std::max(frame_height, kLoginRobotButtonHeight * scale); // 机器人按钮高度

    if (connection_state == RoboGo::io::MqttClient::ConnectionState::Connected) { // 已连接：仅显示退出按钮，更矮
        const float logout_button_height = std::max(frame_height, text_line_height + kRecoverButtonStyle.padding.y * 2.0f); // 退出按钮高度
        const float content_height = text_line_height +                          // 状态文本行
                                     style.SeparatorSize +                       // 分隔线
                                     style.ItemSpacing.y * 2.0f +                // 两段间距
                                     logout_button_height;                        // 退出按钮
        return content_height + style.WindowPadding.y * 2.0f;                     // 加上窗口上下内边距
    }   // 结束已连接分支（精简布局）

    // 未连接：累计各文本行/按钮/间距得到内容高度。
    const float content_height = text_line_height +
                                 kLoginStatusGap * scale +   // 状态文本行下的间距
                                 text_line_height +   // 队伍标题行
                                 team_button_height +   // 队伍按钮高度
                                 kLoginSectionGap * scale +   // 区块间距
                                 text_line_height +   // 机器人标题行
                                 robot_button_height +   // 机器人按钮高度
                                 kLoginSectionGap * scale +   // 区块间距
                                 text_line_height +   // MQTT 标题行
                                 frame_height +   // MQTT 输入框高度
                                 text_line_height +   // 错误提示占位行
                                 text_line_height;   // 预留一行余量，避免文本换行时被裁切

    return content_height + style.WindowPadding.y * 2.0f + kLoginPanelBottomPadding * scale; // 加上内边距与底部留白
}   // 结束 GetLoginPanelHeight

// 功能：渲染"连接状态"一行（带颜色）。
void ShowLoginStatus(RoboGo::io::MqttClient::ConnectionState connection_state) {
    ImGui::AlignTextToFramePadding();                                // 文本对齐到控件内边距
    ImGui::TextDisabled("连接状态");                                 // 渲染"连接状态"标签（灰色）
    ImGui::SameLine(0.0f, 8.0f);                                     // 与标签同行，间隔 8px
    ScopedStyle status_style = MakeTextColorStyle(GetLoginConnectionStateColor(connection_state)); // 按状态取颜色样式
    ImGui::TextUnformatted(GetLoginConnectionStateText(connection_state)); // 显示状态中文文案
}   // 结束 ShowLoginStatus

// 功能：渲染队伍选择（红队/蓝队两个并排按钮），点击更新选中队伍。
void ShowLoginTeamSelection(TeamRobotSelectionState &selection_state) {
    const float button_gap = ImGui::GetStyle().ItemSpacing.x;        // 相邻控件水平间距
    const float button_width = (ImGui::GetContentRegionAvail().x - button_gap) * 0.5f; // 两按钮均分可用宽度
    const float button_height = kLoginTeamButtonHeight * ImGui::GetScale(); // 按钮高度（随缩放）

    ImGui::TextDisabled("队伍");                                      // 灰色标题"队伍"
    if (ShowCompactSelectionButton("红队##team0",                      // 红队按钮（## 后用于区分 ID）
                                   selection_state.selected_team == 0,   // 选中态：当前队伍就是红队
                                   kSelectedRedTeamButtonStyle,   // 红队选中高亮样式
                                   ImVec2(button_width, button_height))) {   // 按钮尺寸：均分宽度 × 固定高度
        selection_state.selected_team = 0;                           // 选中红队
        SPDLOG_INFO("登录面板选择队伍=红队");   // 记录队伍选择结果
    }   // 结束红队按钮判断
    ImGui::SameLine();                                                // 蓝队与红队同行排列
    if (ShowCompactSelectionButton("蓝队##team1",   // 蓝队按钮（## 后为 ImGui 内部唯一 ID）
                                   selection_state.selected_team == 1,   // 选中态：当前队伍是蓝队
                                   kSelectedBlueTeamButtonStyle,   // 蓝队选中高亮样式
                                   ImVec2(button_width, button_height))) {   // 按钮尺寸
        selection_state.selected_team = 1;                           // 选中蓝队
        SPDLOG_INFO("登录面板选择队伍=蓝队");   // 记录队伍选择结果
    }   // 结束蓝队按钮判断
}   // 结束 ShowLoginTeamSelection

// 功能：渲染可选身份按钮（一行等宽排列），点击更新选中身份。
void ShowLoginRobotSelection(TeamRobotSelectionState &selection_state) {
    const int robot_count = static_cast<int>(kLoginRobotNames.size());   // 可选身份数量（重装 / 3 / 4 / 指挥手）
    const float button_gap = ImGui::GetStyle().ItemSpacing.x;        // 相邻控件水平间距
    const float button_width = (ImGui::GetContentRegionAvail().x - button_gap * static_cast<float>(robot_count - 1)) /
                               static_cast<float>(robot_count);      // 按数量等分宽度（间隙数为数量减一）
    const float button_height = kLoginRobotButtonHeight * ImGui::GetScale(); // 按钮高度（随缩放）

    ImGui::TextDisabled("机器人");                                    // 灰色标题"机器人"
    for (int i = 0; i < robot_count; i++) { // 遍历全部可选身份
        if (i > 0) {   // 首个按钮独占行首，其余需要 SameLine
            ImGui::SameLine();                                        // 除首个外均同行排列
        }   // 结束同行判断
        if (ShowCompactSelectionButton(kLoginRobotNames[i],           // 按钮标签（兵种名）
                                       selection_state.selected_robot == i,   // 选中态：当前序号就是 i
                                       GetSelectedRobotButtonStyle(selection_state), // 按队伍取高亮样式
                                       ImVec2(button_width, button_height))) {   // 按钮尺寸（等宽等高等分）
            selection_state.selected_robot = i;                       // 选中第 i 个机器人
            SPDLOG_INFO("登录面板选择机器人 label={} robot_id={}",    // 记录选择
                        kLoginRobotNames[i],   // 日志参数：被点击的兵种名
                        kLoginRobotIds[selection_state.selected_team][selection_state.selected_robot]);   // 日志参数：选择后对应的实际 robot_id
        }   // 结束单个机器人按钮判断
    }   // 结束机器人按钮遍历
}   // 结束 ShowLoginRobotSelection

// 功能：渲染 MQTT 服务端地址输入框，并在解析错误/连接失败时给出提示。
// 设计说明：2027 正式地址是 192.168.12.1，但本地跑 tools/mock_rm_server.py 时监听的是 127.0.0.1，
//           两者只差一个 IP，现场/开发切换时极容易只改一半。这里放一个"本地"快捷按钮 + 失败提示，
//           点一下就能把地址切成 127.0.0.1:3333，不用手敲。
void ShowMqttServerEndpointInput(RoboGo::io::MqttClient &mqtt_client) {
    MqttServerAddressState &mqtt_server_state = GetMqttServerAddressState(mqtt_client); // 取全局地址输入状态
    ImGui::TextDisabled("MQTT 服务端");                               // 灰色标题
    ImGui::Dummy(ImVec2(0.0f, kLoginMqttInputTopMargin * ImGui::GetScale())); // 顶部留白
    {   // 局部作用域：输入框样式只在此块内生效
        ScopedStyle input_style = MakeInputStyle();                   // 套用输入框样式
        const float local_button_width = ImGui::CalcTextSize("本地").x + ImGui::GetStyle().FramePadding.x * 2.0f; // "本地"按钮宽度
        const float input_width = ImGui::GetContentRegionAvail().x - local_button_width - ImGui::GetStyle().ItemSpacing.x; // 输入框让出按钮宽度
        ImGui::SetNextItemWidth(input_width > 80.0f ? input_width : ImGui::GetContentRegionAvail().x); // 输入框宽度（极窄窗口下退回整行）
        if (ImGui::InputText("##mqtt_server_address", &mqtt_server_state.input)) { // 绑定到输入串
            mqtt_server_state.error_message.clear();                  // 用户编辑即清除旧错误提示
        }   // 结束用户编辑处理
        ImGui::SameLine();                                            // "本地"按钮与输入框同行
        if (ImGui::Button("本地")) {                                   // 一键切到本地模拟器地址
            mqtt_server_state.input = "127.0.0.1:3333";               // mock_rm_server.py 的默认监听地址
            mqtt_server_state.error_message.clear();                  // 清掉旧的解析错误
            SPDLOG_INFO("MQTT 地址已切换为本地模拟器 127.0.0.1:3333");   // 记录操作，便于复盘
        }   // 结束本地按钮判断
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {      // 悬停说明
            ImGui::SetTooltip("填入本地模拟器地址 127.0.0.1:3333\n比赛时用 192.168.12.1:3333"); // 两种地址都写清楚
        }   // 结束悬停提示
    }   // 结束作用域（ScopedStyle 析构自动弹样式）
    if (!mqtt_server_state.error_message.empty()) {                   // 存在解析错误
        ScopedStyle error_style = MakeTextColorStyle(ImVec4(1.0f, 0.45f, 0.45f, 1.0f)); // 红色文本样式
        ImGui::TextWrapped("%s", mqtt_server_state.error_message.c_str()); // 自动换行显示错误
    } else if (mqtt_client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Failed) { // 地址合法但连不上
        ScopedStyle hint_style = MakeTextColorStyle(ImVec4(1.0f, 0.72f, 0.3f, 1.0f)); // 橙色提示
        ImGui::TextUnformatted("连不上：本地模拟器用 127.0.0.1:3333");  // 最常见的错配原因，直接写在这里
    } else {   // 无解析错误
        ImGui::TextDisabled(" ");                                     // 无错误时用空格占位保持布局
    }   // 结束错误提示分支
}   // 结束 ShowMqttServerEndpointInput

// 功能：渲染已登录时的"退出登录"面板（置空 client_id 触发断开）。
void ShowLogoutPanel(RoboGo::io::MqttClient &mqtt_client) {
    ScopedTitledPanel logout_panel("LogoutPanel", ImGui::GetContentRegionAvail().x, "登录控制", ImVec4(1.0f, 0.3f, 0.3f, 1.0f)); // 红色标题子面板
    ScopedStyle logout_button_style = MakeButtonStyle(kRecoverButtonStyle); // 红色按钮样式
    if (ImGui::Button("退出登录")) {                                   // 点击退出登录
        SPDLOG_INFO("退出登录按钮点击 client_id={}", mqtt_client.get_client_id()); // 记录当前 client_id
        mqtt_client.set_client_id("");                                // 置空 client_id 触发断开
    }   // 结束退出登录点击判断
}   // 结束 ShowLogoutPanel

// 功能：登录/队伍选择区域的容器：已连接时显示退出登录面板，否则显示完整登录表单。
void ShowTeamRobotSelection(RoboGo::io::MqttClient &mqtt_client) {
    TeamRobotSelectionState &selection_state = GetTeamRobotSelectionState(); // 取全局选择状态
    const auto connection_state = mqtt_client.get_connection_state();        // 取当前连接状态
    const float selection_panel_height = GetLoginPanelHeight(connection_state); // 计算登录区所需高度
    ScopedChildWindow selection_panel("TeamRobotSelection",                  // 开一个不可滚动的子窗口
                                      ImVec2(0, selection_panel_height),   // 宽度 0 表示撑满可用宽度，高度由 GetLoginPanelHeight 算出
                                      false,   // 无边框，融入父面板背景
                                      ImGuiWindowFlags_NoScrollbar);   // 禁止滚动条，避免出现多余滚动条占位

    if (connection_state == RoboGo::io::MqttClient::ConnectionState::Connected) { // 已登录
        ShowLogoutPanel(mqtt_client);                                 // 仅显示退出面板
        return;                                                       // 提前结束，不再显示表单
    }   // 结束已登录分支

    ScopedStyle compact_spacing_style;                                 // 收紧子面板内间距
    compact_spacing_style.PushVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));   // 把控件行/列间距收紧为 (8,4)，让表单更紧凑

    ShowLoginStatus(connection_state);                                // 连接状态行
    ImGui::Dummy(ImVec2(0.0f, kLoginSectionGap * ImGui::GetScale())); // 区块间距
    ShowMqttServerEndpointInput(mqtt_client);                         // 地址输入框
    ImGui::Dummy(ImVec2(0.0f, kLoginStatusGap * ImGui::GetScale()));  // 状态与队伍间间距
    ShowLoginTeamSelection(selection_state);                          // 队伍选择
    ImGui::Dummy(ImVec2(0.0f, kLoginSectionGap * ImGui::GetScale())); // 区块间距
    ShowLoginRobotSelection(selection_state);                         // 机器人选择
}   // 结束 ShowTeamRobotSelection

// 功能：依据当前 robot_id 计算开局面板各子面板的可见性（不同兵种可用指令不同）。
// 思路：除兵种专属面板外，还要避免"同一功能在多个面板里重复出现"导致版面溢出——
//       哨兵的补弹/回血/复活都走 SentryCtrlCommand，已在"哨兵控制"面板里，就不再显示通用的弹药/回血/复活面板。
StartPanelVisibility GetStartPanelVisibility(const State &state) {
    uint32_t current_robot_id = GetCurrentRobotId(state);            // 取当前已登录 robot_id
    StartPanelVisibility visibility;                                  // 待填充的可见性集合
    if (is_commander_role()) {                                        // 指挥手不是机器人
        return visibility;                                            // 全 false：弹药/回血/复活/哨兵/飞镖都在指挥台里
    }                                                                 // 结束指挥手分支
    const bool is_sentry = IsOneOf(current_robot_id, 7, 107);          // 是否为哨兵（功能最全，面板最容易挤爆）
    visibility.show_sentry_panel = is_sentry;                          // 哨兵控制面板（7/107）
    visibility.show_ammo_panel = !is_sentry && IsOneOf(current_robot_id, 1, 3, 4, 5, 101, 103, 104, 105);        // 重装/步兵可兑换弹药（哨兵见哨兵控制面板）
    visibility.show_recover_panel = !is_sentry && IsOneOf(current_robot_id, 1, 3, 4, 5, 101, 103, 104, 105);  // 同上可远程回血
    visibility.show_revive_panel = !is_sentry && IsOneOf(current_robot_id, 1, 3, 4, 5, 101, 103, 104, 105); // 同上可复活
    visibility.show_assembly_panel = IsOneOf(current_robot_id, 1, 101); // 2027 重装专用装配（模块化 42mm）
    visibility.show_marker_panel = IsOneOf(current_robot_id, 7, 8, 107, 108);  // 哨兵/飞镖可地图标记
    visibility.show_robot_performance_options = IsOneOf(current_robot_id, 1, 3, 4, 5, 101, 103, 104, 105); // 重装/步兵性能选项
    visibility.show_sentry_performance_option = is_sentry;             // 哨兵控制方式（自动/半自动）
    visibility.show_performance_panel = visibility.show_robot_performance_options || visibility.show_sentry_performance_option; // 任一存在则显示性能面板
    visibility.show_dart_panel = IsOneOf(current_robot_id, 8, 108);   // 飞镖为独立系统（8/108）
    visibility.is_hero = IsOneOf(current_robot_id, 1, 101);           // 重装（沿用 is_hero 变量名）
    return visibility;   // 返回填充好的可见性集合
}   // 结束 GetStartPanelVisibility

// 功能：渲染飞镖控制面板（目标选择 + 闸门开关 + 确认发射）。
// 思路：5 个目标单选按钮排成两行；切换目标即时下发；开/关闸门与确认发射分别发送 DartCommand，发射附带进入比赛标记。
void ShowDartPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel dart_panel("DartPanel", panel_width, "飞镖控制", ImVec4(1.0f, 0.4f, 0.8f, 1.0f)); // 粉色标题子面板
    ImGui::AlignTextToFramePadding();                                // 文本对齐到内边距

    DartPanelState &dart_panel_state = GetDartPanelState(state);      // 取（并同步远端）飞镖面板状态

    float offset_from_start_x = ImGui::CalcTextSize("目标:").x + 10 * ImGui::GetScale(); // "目标:" 标签宽度 + 偏移
    ImGui::Text("目标:");                                            // 渲染标签
    ImGui::SameLine(offset_from_start_x);                            // 单选按钮从标签右侧开始
    float target_row_start_x = ImGui::GetCursorPosX();                // 记录首行单选按钮起始 X
    float target_row_start_y = ImGui::GetCursorPosY();                // 记录首行起始 Y
    float compact_row_gap = 2.0f * ImGui::GetScale();                // 两行之间的小间距

    for (uint32_t i = 0; i < kDartTargetNames.size(); i++) {         // 遍历 5 个目标
        if (i == 3) {                                                // 第 4 个起换到第二行
            ImGui::SetCursorPosY(target_row_start_y + ImGui::GetFrameHeight() + compact_row_gap); // 下移一行
            ImGui::SetCursorPosX(target_row_start_x);                // 回到行首 X
        } else if (i != 0) {                                         // 同一行内后续项
            ImGui::SameLine(0, 10);                                  // 与上一个间隔 10px
        }   // 结束同行/换行分支
        if (ImGui::RadioButton(kDartTargetNames[i], dart_panel_state.target_id == (i + 1))) { // 单选：是否选中该目标
            dart_panel_state.target_id = i + 1;                      // 更新本地目标（1 基）
            SendDartCommand(mqtt_client, dart_panel_state.target_id, dart_panel_state.open, false); // 切换目标即时下发
        }   // 结束单个目标单选判断
    }   // 结束 5 个目标的遍历

    ImGui::Spacing();                                                // 与目标行留白

    ScopedStyle dart_button_style = MakeButtonStyle(kDartButtonStyle); // 飞镖按钮样式

    if (ImGui::Button(dart_panel_state.open ? "关闭闸门" : "开启闸门")) { // 按钮文案随闸门状态切换
        dart_panel_state.open = !dart_panel_state.open;              // 本地翻转闸门意图
        SendDartCommand(mqtt_client, dart_panel_state.target_id, dart_panel_state.open, false); // 下发闸门开关
    }   // 结束闸门开关按钮判断

    ImGui::SameLine();                                               // 确认发射与闸门按钮同行

    ButtonFeedback launch_feedback = GetDartLaunchFeedback(state);    // 计算确认发射可用性反馈
    if (ButtonWithTooltip("确认发射", launch_feedback.valid_tooltip, launch_feedback.disabled_reason)) { // 悬停提示/禁用
        SendDartCommand(mqtt_client, dart_panel_state.target_id, dart_panel_state.open, true); // 确认发射（不再顺带切比赛模式）
    }   // 结束确认发射按钮判断
}   // 结束 ShowDartPanel


// 功能：渲染飞镖面板（2027 起本行仅保留飞镖）。
void ShowDartRow(const State &state,
                 RoboGo::io::MqttClient &mqtt_client,   // MQTT 客户端，供子面板下发指令
                 bool show_dart_panel) {   // 飞镖面板是否可见
    ShowPanelRow("DartRow", 6.0f, {   // 行布局工具
            {.visible = show_dart_panel, .render = [&](float width) { ShowDartPanel(state, mqtt_client, width); }},   // 飞镖面板渲染回调
    });   // 结束 initializer_list
}   // 结束 ShowDartRow

// 功能：渲染哨兵控制面板（状态显示 + 补给/复活/姿态三组按钮）。
// 思路：原先 11 个按钮挤成"一行 5 个 + 一行 6 个"，在半个面板宽度下直接横向溢出、
//       还会压到窗口底部的"进入比赛"按钮上。现在按功能分两行、按钮宽度按可用宽度自适应均分。
void ShowSentryPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel sentry_panel("SentryPanel", panel_width, "哨兵控制", ImVec4(0.9f, 0.6f, 1.0f, 1.0f)); // 紫色标题子面板

    // 状态与回执压成一行：面板高度是按行数换来的，多一行就会把下面的按钮挤出可视区。
    std::string status_line;                                           // 待显示的状态文本
    if (state.sentry_status_sync.has_value()) {                        // 已收到哨兵状态
        const auto &sync = *state.sentry_status_sync;                  // 取状态引用
        const char *posture = sync.posture_id() == 1 ? "进攻" : sync.posture_id() == 2 ? "防御" : sync.posture_id() == 3 ? "移动" : "未知"; // 姿态中文名
        status_line = std::string("姿态: ") + posture +
                      std::string("  弱化: ") + (sync.is_weakened() ? "是" : "否") +
                      std::string("  强化: ") + (sync.is_powered() ? "是" : "否");   // 姿态/弱化/强化
    } else {                                                           // 尚未同步
        status_line = "哨兵状态: 等待同步";                            // 占位文案
    }   // 结束状态分支
    if (state.sentry_ctrl_result.has_value()) {                        // 有上一指令回执
        status_line += std::string("   上一条 ") + std::to_string(state.sentry_ctrl_result->command_id()) +
                       "→" + std::to_string(state.sentry_ctrl_result->result_code());   // 指令号→结果码
    }   // 结束回执分支
    if (state.sentry_status_sync.has_value()) {                        // 有真实状态时用普通色，否则灰色
        ImGui::Text("%s", status_line.c_str());                        // 状态行
    } else {                                                           // 无状态
        ImGui::TextDisabled("%s", status_line.c_str());                // 灰色占位
    }   // 结束状态配色判断

    ImGui::Spacing();                                                  // 与按钮区留白
    ScopedStyle sentry_style = MakeButtonStyle(kPrimaryButtonStyle);   // 复用主按钮样式

    const float item_spacing = ImGui::GetStyle().ItemSpacing.x;        // 按钮间距
    const float available_width = ImGui::GetContentRegionAvail().x;    // 本行可用宽度
    // 一行按 6 个按钮均分；窄面板下保底 72px（宁可换行也不把按钮压成点不动的细条）。
    const float button_width = std::max(72.0f * ImGui::GetScale(), (available_width - item_spacing * 5.0f) / 6.0f);
    const ImVec2 button_size(button_width, 0.0f);                      // 统一按钮尺寸（高度自适应）

    // 第一行：补给与复活（对应 SentryCtrlCommand 1 / 3 / 4 / 5 / 6）。
    if (ImGui::Button("补弹", button_size)) SendSentryCtrlCommand(mqtt_client, 1);      // 补血点补弹
    ImGui::SameLine();
    if (ImGui::Button("远程补弹", button_size)) SendSentryCtrlCommand(mqtt_client, 3);  // 远程补弹
    ImGui::SameLine();
    if (ImGui::Button("远程回血", button_size)) SendSentryCtrlCommand(mqtt_client, 4);  // 远程回血
    ImGui::SameLine();
    if (ImGui::Button("确认复活", button_size)) SendSentryCtrlCommand(mqtt_client, 5);  // 确认复活
    ImGui::SameLine();
    if (ImGui::Button("金币复活", button_size)) SendSentryCtrlCommand(mqtt_client, 6);  // 花金币立即复活

    // 第二行：姿态与强化姿态（对应 SentryCtrlCommand 7~12）。
    if (ImGui::Button("进攻", button_size)) SendSentryCtrlCommand(mqtt_client, 7);      // 切换进攻姿态
    ImGui::SameLine();
    if (ImGui::Button("防御", button_size)) SendSentryCtrlCommand(mqtt_client, 8);      // 切换防御姿态
    ImGui::SameLine();
    if (ImGui::Button("移动", button_size)) SendSentryCtrlCommand(mqtt_client, 9);      // 切换移动姿态
    ImGui::SameLine();
    if (ImGui::Button("强化进攻", button_size)) SendSentryCtrlCommand(mqtt_client, 10); // 强化进攻
    ImGui::SameLine();
    if (ImGui::Button("强化防御", button_size)) SendSentryCtrlCommand(mqtt_client, 11); // 强化防御
    ImGui::SameLine();
    if (ImGui::Button("强化移动", button_size)) SendSentryCtrlCommand(mqtt_client, 12); // 强化移动
}   // 结束 ShowSentryPanel

// 功能：把哨兵面板单独排成一行。
void ShowSentryRow(const State &state, RoboGo::io::MqttClient &mqtt_client, const StartPanelVisibility &visibility) {
    ShowPanelRow("SentryRow", 5.0f, {                                  // 行布局（标题 + 状态 + 两行按钮）
            {.visible = visibility.show_sentry_panel, .render = [&](float width) { ShowSentryPanel(state, mqtt_client, width); }}, // 哨兵面板回调
    });   // 结束 initializer_list
}   // 结束 ShowSentryRow

// 功能：渲染弹药兑换面板（输入允许发弹量 + 兑换/远程兑换按钮）。
// 思路：数字输入框解析发量；两按钮依据补给区/经济/英雄计价反馈决定是否可用，兑换发送 CommonCommand(cmd 1/2)，远程兑换发送 cmd 5。
void ShowAmmoPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width, bool is_hero) {
    ScopedTitledPanel ammo_panel("AmmoPanel", panel_width, "弹药兑换", ImVec4(0.3f, 1.0f, 0.3f, 1.0f)); // 绿色标题子面板

    AmmoPanelState &ammo_panel_state = GetAmmoPanelState();          // 取全局弹药面板状态
    bool is_valid_ammo;                                             // 输入是否合法
    long long ammo;                                                 // 解析出的发弹量
    {   // 作用域：输入框样式
        ScopedStyle input_style = MakeInputStyle();                  // 输入框样式
        IntegerInputText("请输入允许发弹量：", &ammo_panel_state.ammo_str, &is_valid_ammo, &ammo); // 渲染并解析输入
    }   // 结束作用域（样式自动弹出）

    ImGui::Spacing();                                               // 与按钮区留白

    ButtonFeedback ammo_feedback = GetAmmoExchangeFeedback(state, is_valid_ammo, ammo, is_hero); // 本地兑换反馈
    ButtonFeedback remote_ammo_feedback = GetRemoteAmmoExchangeFeedback(state, is_valid_ammo, ammo, is_hero); // 远程兑换反馈

    {   // 作用域：按钮样式
        ScopedStyle ammo_button_style = MakeButtonStyle(kPrimaryButtonStyle); // 主按钮样式
        if (ButtonWithTooltip("兑换允许发弹量", ammo_feedback.valid_tooltip, ammo_feedback.disabled_reason)) { // 本地兑换
            SendCommonCommand(mqtt_client, is_hero ? 2 : 1, static_cast<uint32_t>(ammo)); // 重装42mm用cmd2 / 步兵哨兵17mm用cmd1（2027修正）
        }   // 结束本地兑换判断
        ImGui::SameLine();                                           // 远程兑换同行
        if (ButtonWithTooltip("远程兑换", remote_ammo_feedback.valid_tooltip, remote_ammo_feedback.disabled_reason)) {   // 远程兑换按钮（提示/禁用由反馈决定）
            SendCommonCommand(mqtt_client, 5);                       // 远程兑换指令 cmd5
        }   // 结束远程兑换判断
    }   // 结束按钮作用域
    // 把禁用原因直接写在按钮下面：灰按钮点击没反应时，操作手不必悬停或翻文档就知道卡在哪。
    // 只占一行（优先报本地兑换的原因），面板行高按 5 行预留，正好容得下。
    if (ammo_feedback.disabled_reason != nullptr) {                   // 本地兑换不可用
        ImGui::TextDisabled("兑换允许发弹量不可用：%s", ammo_feedback.disabled_reason);   // 直接给出原因
    } else if (remote_ammo_feedback.disabled_reason != nullptr) {     // 本地可用但远程不可用
        ImGui::TextDisabled("远程兑换不可用：%s", remote_ammo_feedback.disabled_reason);   // 给出远程兑换的原因
    }   // 结束禁用原因判断
}   // 结束 ShowAmmoPanel

// 功能：渲染装配控制面板（输入难度 + 取消/确认装配按钮）。
// 思路：难度输入框；取消/确认按钮依据技术核心状态（basic_state）禁用；确认时把难度随 AssemblyCommand 下发。
void ShowAssemblyPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel assembly_panel("AssemblyPanel", panel_width, "装配控制", ImVec4(0.3f, 0.8f, 1.0f, 1.0f)); // 青蓝色标题子面板

    AssemblyPanelState &assembly_panel_state = GetAssemblyPanelState(); // 取全局装配面板状态
    bool is_valid_assembly_difficulty = false;                      // 难度输入是否合法
    long long assembly_difficulty;                                  // 解析出的难度等级
    {   // 作用域：输入框样式
        ScopedStyle input_style = MakeInputStyle();                  // 输入框样式
        IntegerInputText("请输入装配难度：", &assembly_panel_state.difficulty_str, &is_valid_assembly_difficulty, &assembly_difficulty); // 渲染并解析难度
    }   // 结束作用域（样式自动弹出）

    ImGui::Spacing();                                               // 与按钮区留白

    // 2027 协议 2.2.21 AssemblyCommand.operation：0=开始兑换、1=确认装配、2=取消装配（2026 的"0=取消"写法已废止）。
    // 2027 协议 2.2.22 TechCoreMotionStateSync.basic_state：1=初始位置(空闲)、2=运动中、3=已到达对应位姿。
    const bool has_core_state = state.tech_core_motion_state_sync.has_value();  // 是否已收到科技核心状态同步
    const uint32_t core_basic_state = has_core_state ? state.tech_core_motion_state_sync->basic_state() : 0;  // 0 表示未知
    const uint32_t max_difficulty = has_core_state ? state.tech_core_motion_state_sync->maximum_difficulty_level() : 0;  // 当前可选最高难度
    const bool core_idle = core_basic_state == 1;                                  // 核心在初始位：允许"开始兑换"
    const bool core_in_progress = core_basic_state == 2 || core_basic_state == 3;  // 流程进行中(运动中/已到位)：允许"取消装配"
    const bool core_ready = core_basic_state == 3;                                 // 核心已到位：允许"确认装配"
    // 难度合法性：解析成功、在 1~4 范围内，且不超过服务器给出的最高可选难度。
    const bool difficulty_in_range = is_valid_assembly_difficulty && assembly_difficulty >= 1 &&
                                     assembly_difficulty <= 4 &&
                                     (max_difficulty == 0 || assembly_difficulty <= static_cast<long long>(max_difficulty));
    {   // 作用域：核心状态提示
        const char *core_state_text = "未知";                                        // 默认文案
        switch (core_basic_state) {                                                 // 按协议枚举翻译
            case 1: core_state_text = "初始位"; break;                              // 1 = 初始位置
            case 2: core_state_text = "运动中"; break;                              // 2 = 运动中
            case 3: core_state_text = "已到位"; break;                              // 3 = 已到达位姿
            default: break;                                                         // 0/其它 = 未同步
        }                                                                           // 结束状态翻译
        // 状态/难度/剩余时长压在同一行：面板高度是按行数换来的，多占一行会把下面的三个按钮挤出可视区。
        std::string core_line = std::string("核心: ") + core_state_text + "   上限 " + std::to_string(max_difficulty);  // 基础部分
        if (has_core_state && state.tech_core_motion_state_sync->remain_time_all() > 0) {  // 四级装配期间服务器才给剩余时长
            core_line += "   剩余 " + std::to_string(state.tech_core_motion_state_sync->remain_time_all()) + "s";  // 追加总剩余
        }   // 结束剩余时长追加
        ImGui::TextUnformatted(core_line.c_str());   // 一行输出，避免额外占高
    }   // 结束核心状态提示

    ImGui::Spacing();                                               // 与按钮区留白

    {   // 作用域：装配按钮样式
        ScopedStyle assembly_button_style = MakeButtonStyle(kAssemblyButtonStyle); // 装配按钮样式

        // 开始兑换(operation=0)：核心空闲且难度合法才可点。注意这不是"取消"，点下去会启动装配流程。
        ImGui::BeginDisabled(!core_idle || !difficulty_in_range);    // 不满足条件时禁用
        if (ImGui::Button("开始兑换")) {                             // 开始兑换（operation 0）
            SendAssemblyCommand(mqtt_client, static_cast<uint32_t>(assembly_difficulty), 0);  // operation 0 = 开始兑换，带难度
        }   // 结束开始兑换判断
        ImGui::EndDisabled();                                        // 结束禁用作用域

        ImGui::SameLine();                                           // 确认装配同行

        // 确认装配(operation=1)：核心已到位才可点。
        ImGui::BeginDisabled(!core_ready || !difficulty_in_range);   // 不满足条件时禁用
        if (ImGui::Button("确认装配")) {                             // 确认装配（operation 1）
            SendAssemblyCommand(mqtt_client, static_cast<uint32_t>(assembly_difficulty), 1);  // operation 1 = 确认，携带输入解析出的难度
        }   // 结束确认装配判断
        ImGui::EndDisabled();                                        // 结束禁用作用域

        ImGui::SameLine();                                           // 取消装配同行

        // 取消装配(operation=2)：只有流程进行中才可点，难度参数无意义填 0。
        ImGui::BeginDisabled(!core_in_progress);                     // 未开始装配时禁用取消
        if (ImGui::Button("取消装配")) {                             // 取消装配（operation 2）
            SendAssemblyCommand(mqtt_client, 0, 2);                  // operation 2 = 取消装配
        }   // 结束取消装配判断
        ImGui::EndDisabled();                                        // 结束禁用作用域
    }   // 结束按钮作用域
}   // 结束 ShowAssemblyPanel


// 功能：渲染远程回血面板。
// 思路：依据经济反馈决定按钮可用性，点击发送 CommonCommand(cmd 6)。
void ShowRecoverPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel recover_panel("RecoverPanel", panel_width, "远程回血", ImVec4(1.0f, 0.3f, 0.3f, 1.0f)); // 红色标题子面板

    ButtonFeedback recover_feedback = GetRecoverFeedback(state);       // 计算回血可用性反馈
    {   // 作用域：回血按钮样式
        ScopedStyle recover_button_style = MakeButtonStyle(kRecoverButtonStyle); // 回血按钮样式
        if (ButtonWithTooltip("远程回血", recover_feedback.valid_tooltip, recover_feedback.disabled_reason)) { // 悬停提示/禁用
            SendCommonCommand(mqtt_client, 6);                        // 远程回血指令 cmd6
        }   // 结束远程回血判断
    }   // 结束作用域
}   // 结束 ShowRecoverPanel

// 功能：渲染复活控制面板（确认复活 / 兑换立即复活两个按钮）。
// 思路：两按钮依据复活状态反馈分别决定是否可用，点击发送 CommonCommand(cmd 3 / 4)。
void ShowRevivePanel(const State &state, RoboGo::io::MqttClient &mqtt_client, float panel_width) {
    ScopedTitledPanel revive_panel("RevivePanel", panel_width, "复活控制", ImVec4(1.0f, 1.0f, 0.3f, 1.0f)); // 黄色标题子面板

    ButtonFeedback confirm_feedback = GetConfirmReviveFeedback(state); // 确认复活反馈
    ButtonFeedback instant_feedback = GetInstantReviveFeedback(state); // 立即复活反馈
    {   // 作用域：复活按钮样式
        ScopedStyle revive_button_style = MakeButtonStyle(kReviveButtonStyle); // 复活按钮样式
        if (ButtonWithTooltip("确认复活", confirm_feedback.valid_tooltip, confirm_feedback.disabled_reason)) { // 免费确认复活
            SendCommonCommand(mqtt_client, 3);                        // 确认复活指令 cmd3
        }   // 结束确认复活判断

        ImGui::SameLine();                                            // 立即复活同行

        if (ButtonWithTooltip("兑换立即复活", instant_feedback.valid_tooltip, instant_feedback.disabled_reason)) { // 花金币立即复活
            SendCommonCommand(mqtt_client, 4);                        // 立即复活指令 cmd4
        }   // 结束立即复活判断
    }   // 结束作用域
}   // 结束 ShowRevivePanel

// 功能：渲染地图标记面板（进入/退出标记模式 + 是否包含哨兵）。
// 思路：依据全局 marker 模式状态切换按钮文案；提供"包含哨兵"开关写入全局设置。
void ShowMarkerPanel(float panel_width) {
    ScopedTitledPanel marker_panel("MarkerPanel", panel_width, "地图标记", ImVec4(1.0f, 0.6f, 0.2f, 1.0f)); // 橙色标题子面板

    {   // 作用域：标记按钮样式
        ScopedStyle marker_button_style = MakeButtonStyle(kMarkerButtonStyle); // 标记按钮样式
        if (is_marker_mode()) {                                       // 当前已在标记模式
            if (ImGui::Button("退出地图点位标记模式")) {               // 提供退出按钮
                exit_marker_mode();                                   // 退出标记模式
            }   // 结束退出标记模式判断
        } else {                                                      // 未在标记模式
            if (ImGui::Button("开始标记地图点位")) {                   // 提供进入按钮
                enter_marker_mode();                                  // 进入标记模式
            }   // 结束进入标记模式判断
        }   // 结束标记模式分支
    }   // 结束作用域

    bool include_sentry = is_marker_mode_include_sentry();            // 取"是否包含哨兵"开关
    if (is_marker_mode()) {                                          // 标记模式进行中：补一句操作说明
        ImGui::TextDisabled("在小地图上左键点击即可下发坐标");        // 明确下一步该点哪里
    }   // 结束说明分支
    ImGui::Spacing();                                                // 与上方留白
    if (ImGui::RadioButton("包含哨兵", include_sentry)) {             // 单选：包含哨兵
        set_marker_mode_include_sentry(true);                        // 写入全局设置
    }   // 结束包含哨兵判断
    ImGui::SameLine();                                                // 与下一选项同行
    if (ImGui::RadioButton("不包含哨兵", !include_sentry)) {          // 单选：不包含哨兵
        set_marker_mode_include_sentry(false);                       // 写入全局设置
    }   // 结束不包含哨兵判断

    ImGui::Spacing();                                                // 底部留白
}   // 结束 ShowMarkerPanel

// 功能：渲染性能选择面板（发射机构/底盘/哨兵的模式单选）。
// 思路：英雄与普通机器人选项文案不同；任一选项改变即下发 RobotPerformanceSelectionCommand。
void ShowPerformancePanel(const State &state,
                          RoboGo::io::MqttClient &mqtt_client,   // MQTT 客户端，用于下发性能选择
                          float panel_width,   // 本面板被行布局工具分配的宽度
                          bool is_hero,   // 是否英雄（决定选项文案与取值）
                          bool show_robot_performance_options,   // 是否显示发射机构/底盘选项
                          bool show_sentry_performance_option) {   // 是否显示哨兵控制选项
    ScopedTitledPanel performance_panel("PerformancePanel", panel_width, "性能选择", ImVec4(0.3f, 0.8f, 1.0f, 1.0f)); // 青蓝标题子面板
    ImGui::AlignTextToFramePadding();                                // 文本对齐内边距

    bool is_send = false;                                             // 是否发生了选项改变（需下发）
    PerformancePanelState &performance_panel_state = GetPerformancePanelState(state); // 取（同步远端）性能状态

    float offset_from_start_x1 = ImGui::CalcTextSize("发射机构:").x + 10 * ImGui::GetScale(); // 标签后偏移（选项起始 X）
    float offset_from_start_x2 = offset_from_start_x1 + 150 * ImGui::GetScale();              // 第二个选项偏移（右移 150px）
    if (show_robot_performance_options) {                             // 显示机器人性能选项
        if (is_hero) {                                               // 英雄：近战/远程优先
            is_send |= ShowLabeledRadioPair("发射机构:", offset_from_start_x1, offset_from_start_x2, "近战优先##shooter3", 3, "远程优先##shooter4", 4, performance_panel_state.shooter);   // 英雄发射机构：3=近战优先 / 4=远程优先（按位或累积是否改变）
        } else {                                                     // 普通战车：冷却/爆发优先
            is_send |= ShowLabeledRadioPair("发射机构:", offset_from_start_x1, offset_from_start_x2, "冷却优先##shooter1", 1, "爆发优先##shooter2", 2, performance_panel_state.shooter);   // 普通战车：1=冷却优先 / 2=爆发优先
        }   // 结束非英雄分支

        if (is_hero) {                                               // 英雄底盘选项
            is_send |= ShowLabeledRadioPair("底盘:", offset_from_start_x1, offset_from_start_x2, "近战优先##chassis3", 3, "远程优先##chassis4", 4, performance_panel_state.chassis);   // 英雄底盘：3=近战优先 / 4=远程优先
        } else {                                                     // 普通战车底盘选项
            is_send |= ShowLabeledRadioPair("底盘:", offset_from_start_x1, offset_from_start_x2, "血量优先##chassis1", 1, "功率优先##chassis2", 2, performance_panel_state.chassis);   // 普通战车底盘：1=血量优先 / 2=功率优先
        }   // 结束英雄/普通战车分支
    }   // 结束机器人性能选项块

    if (show_sentry_performance_option) {                            // 显示哨兵性能选项
        is_send |= ShowLabeledRadioPair("哨兵:", offset_from_start_x1, offset_from_start_x2, "自动##sentry0", 0, "半自动##sentry1", 1, performance_panel_state.sentry_control);   // 哨兵：0=自动 / 1=半自动
    }   // 结束哨兵选项块

    if (is_send) {                                                   // 任一选项改变
        SendPerformanceSelection(mqtt_client, performance_panel_state); // 下发性能选择并忽略短暂远端同步
    }   // 结束选项改变判断
}   // 结束 ShowPerformancePanel

// 功能：把弹药/装配两个面板排成一行（按可见性隐藏空位）。
void ShowAmmoAssemblyDeployRow(const State &state,
                               RoboGo::io::MqttClient &mqtt_client,   // MQTT 客户端
                               const StartPanelVisibility &visibility) {   // 可见性集合，决定哪些面板参与排布
    ShowPanelRow("AmmoAssemblyDeployRow", 5.0f, {   // 两个面板横向排一行（行高 5 倍：装配面板多了"核心状态"一行）
            {.visible = visibility.show_ammo_panel, .render = [&](float width) { ShowAmmoPanel(state, mqtt_client, width, visibility.is_hero); }},   // 弹药兑换面板回调（is_hero 影响计价方式）
            {.visible = visibility.show_assembly_panel, .render = [&](float width) { ShowAssemblyPanel(state, mqtt_client, width); }},   // 装配控制面板回调
    });   // 结束 initializer_list
}   // 结束 ShowAmmoAssemblyDeployRow

// 功能：把回血/复活两个面板排成一行（按可见性隐藏空位）。
void ShowRecoverReviveRow(const State &state,
                          RoboGo::io::MqttClient &mqtt_client,   // MQTT 客户端
                          const StartPanelVisibility &visibility) {   // 可见性集合
    ShowPanelRow("RecoverReviveRow", 3.0f, {   // 回血/复活两个面板排一行（行高 3 倍）
            {.visible = visibility.show_recover_panel, .render = [&](float width) { ShowRecoverPanel(state, mqtt_client, width); }},   // 远程回血面板回调
            {.visible = visibility.show_revive_panel, .render = [&](float width) { ShowRevivePanel(state, mqtt_client, width); }},   // 复活控制面板回调
    });   // 结束 initializer_list
}   // 结束 ShowRecoverReviveRow

// 功能：把地图标记/性能选择两个面板排成一行（按可见性隐藏空位）。
void ShowMarkerPerformanceRow(const State &state,
                              RoboGo::io::MqttClient &mqtt_client,   // MQTT 客户端
                              const StartPanelVisibility &visibility) {   // 可见性集合
    ShowPanelRow("MarkerPerformanceRow", 5.0f, {   // 标记/性能两个面板排一行（标记面板在标记模式下会多一行操作说明，按 5 行预留）
            {.visible = visibility.show_marker_panel, .render = [&](float width) { ShowMarkerPanel(width); }},   // 地图标记面板回调（不需要 state）
            {.visible = visibility.show_performance_panel, .render = [&](float width) {   // 性能选择面板回调开始
                 ShowPerformancePanel(state,   // 状态数据
                                      mqtt_client,   // MQTT 客户端
                                      width,   // 分配到的面板宽度
                                      visibility.is_hero,   // 是否英雄
                                      visibility.show_robot_performance_options,   // 是否显示发射机构/底盘选项
                                      visibility.show_sentry_performance_option);   // 是否显示哨兵选项
             }},   // 结束性能面板 lambda 体
    });   // 结束 initializer_list
}   // 结束 ShowMarkerPerformanceRow

// 功能：渲染鼠标灵敏度滑块行，并把改动实时写回全局设置。
void ShowMouseSensitivityRow() {
    ImGui::Dummy(ImVec2(0.0f, kMouseSensitivityTopGap * ImGui::GetScale())); // 顶部留白

    ScopedChildWindow mouse_sensitivity_row("MouseSensitivityRow",         // 不可滚动子窗口
                                            ImVec2(0, ImGui::GetFrameHeightWithSpacing()), // 单行高（之前给两行，多出的空白把下面的 UDP 行推远了）
                                            false,   // 无边框
                                            ImGuiWindowFlags_NoScrollbar);   // 禁止滚动条（只需两行固定高）

    {   // 作用域：金色标签样式
        ScopedStyle label_style = MakeTextColorStyle(ImVec4(1.0f, 0.8f, 0.3f, 1.0f)); // 金色标签样式
        ImGui::SetCursorPosX(0);                                        // 标签从行首开始
        ImGui::Text("鼠标灵敏度");                                      // 渲染标签
    }   // 结束标签作用域（样式自动弹出）

    ScopedStyle slider_style = MakeMouseSensitivitySliderStyle();        // 灵敏度滑块样式

    float mouse_sensitivity = get_mouse_sensitivity();                 // 读取全局灵敏度
    ImGui::SameLine(ImGui::CalcTextSize("鼠标灵敏度").x + 10 * ImGui::GetScale()); // 滑块从标签右侧开始
    ImGui::SetNextItemWidth(-10 * ImGui::GetScale());                  // 宽度撑满剩余（负值为填满）
    if (ImGui::SliderFloat("##mouse_sensitivity", &mouse_sensitivity, 0.1f, 5.0f, "%.2f")) { // 滑块范围 0.1~5.0
        set_mouse_sensitivity(mouse_sensitivity);                      // 改动实时写回全局
    }   // 结束滑块改动判断
}   // 结束 ShowMouseSensitivityRow

// 功能：渲染底部主操作按钮：已登录显示"进入比赛"，否则显示"登录"。
// 思路：按钮改为在正常布局流中居中（原来是 SetCursorPos 绝对定位到窗口底部），
//       这样"进入比赛"永远排在内容区之后，不会再被上面的功能面板压住。
void ShowPrimaryActionButton(RoboGo::io::MqttClient &mqtt_client) {
    float button_width = 200 * ImGui::GetScale();                     // 按钮宽度（随缩放）
    float button_height = kPrimaryActionButtonHeight * ImGui::GetScale(); // 按钮高度
    const auto connection_state = mqtt_client.get_connection_state(); // 取连接状态
    const bool is_logged_in = connection_state == RoboGo::io::MqttClient::ConnectionState::Connected; // 是否已登录

    // 水平居中：按当前行剩余宽度算出左侧偏移，剩余宽度为负（窗口极窄）时不偏移，保证按钮仍可点。
    const float center_offset = (ImGui::GetContentRegionAvail().x - button_width) * 0.5f; // 居中所需左偏移
    if (center_offset > 0.0f) {                                       // 只有放得下才偏移
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + center_offset); // 左移（在行首基础上加偏移）
    }   // 结束居中判断

    if (is_logged_in && is_commander_role()) {                       // 已登录且身份是指挥手
        ScopedStyle enter_game_button_style = MakeEnterGameButtonStyle(); // 绿色按钮样式
        if (ImGui::Button("进入指挥台", ImVec2(button_width, button_height))) { // 点击打开指挥台
            enter_commander_console();                                // 打开指挥台（不进入比赛模式，鼠标保持可见）
        }   // 结束进入指挥台点击判断
        return;                                                      // 已处理，结束
    }   // 结束指挥手分支

    if (is_logged_in) {                                              // 已登录：显示"进入比赛"
        ScopedStyle enter_game_button_style = MakeEnterGameButtonStyle(); // 绿色按钮样式
        if (ImGui::Button("进入比赛", ImVec2(button_width, button_height))) { // 点击进入比赛
            enter_game_mode();                                        // 切换到比赛模式
        }   // 结束进入比赛点击判断
        return;                                                      // 已处理，结束
    }   // 结束已登录分支

    ScopedStyle login_button_style = MakeEnterGameButtonStyle();      // 登录也用绿色样式
    if (ImGui::Button("登录", ImVec2(button_width, button_height))) { // 点击登录
        (void) SubmitLoginSelection(mqtt_client);                    // 校验地址并连接（忽略返回值）
    }   // 结束登录点击判断
}   // 结束 ShowPrimaryActionButton

// 功能：绘制「UDP 画面反转」开关行（标题 + 拨动开关 + 状态文字）。
// 思路：原来用一个 SmallButton 显示「开/关」，深色面板上几乎与背景同色，看不出是可点控件；
//       改为常见设置项的样式——金色标题右侧紧跟胶囊形拨动开关（轨道 + 圆形滑块，开启绿色靠右、
//       关闭深灰靠左），开关右边再用文字复述当前状态。标题+开关+状态整体作为点击热区，
//       悬停时该区域淡高亮并弹提示。控件跟随标题左对齐（而不是贴窗口右边缘）：登录控制面板
//       右下角会被小地图面板压住，右对齐会被盖掉。
// 返回：本帧是否点了开关（由调用方翻转实际方向并写日志）。
bool ShowVideoInvertRow(bool udp_video_inverted) {
    const float scale = ImGui::GetScale();                              // 全局缩放
    const float row_height = std::max(ImGui::GetFrameHeight(), 24.0f * scale); // 行高至少放得下开关
    const float track_width = 40.0f * scale;                            // 轨道宽度
    const float track_height = 20.0f * scale;                           // 轨道高度
    const float track_rounding = track_height * 0.5f;                   // 胶囊形圆角

    const char *title_text = "UDP 画面反转";                             // 左侧标题
    const char *state_text = udp_video_inverted ? "已开启" : "已关闭";    // 右侧状态复述
    const float title_width = ImGui::CalcTextSize(title_text).x;        // 标题宽度
    const float state_width = ImGui::CalcTextSize(state_text).x;        // 状态文字宽度
    const float title_gap = 14.0f * scale;                              // 标题与开关的间距
    const float state_gap = 10.0f * scale;                              // 开关与状态文字的间距
    const float control_width = title_width + title_gap + track_width + state_gap + state_width; // 整块控件宽度

    const ImVec2 row_pos = ImGui::GetCursorScreenPos();                 // 行左上角（屏幕坐标）
    // 只把"标题+开关+状态"这一块当热区：点标题也能切换，但不会把整行空白都变成按钮。
    const bool toggle_requested = ImGui::InvisibleButton("##udp_invert_toggle", ImVec2(control_width, row_height)); // 按下并抬起才算一次点击，不会拖拽误触
    const bool hovered = ImGui::IsItemHovered();                        // 鼠标是否落在控件块上

    ImDrawList *draw_list = ImGui::GetWindowDrawList();                 // 本窗口绘制列表
    if (hovered) {                                                      // 悬停：整块淡高亮，明确"这里能点"
        draw_list->AddRectFilled(row_pos,                               // 左上角
                                 ImVec2(row_pos.x + control_width, row_pos.y + row_height), // 右下角
                                 IM_COL32(255, 255, 255, 16),            // 极淡白
                                 5.0f * scale,                           // 圆角
                                 ImDrawFlags_None);                      // 无特殊标志
    }   // 结束悬停高亮

    const float text_y = row_pos.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f; // 文字垂直居中

    // 左侧标题
    draw_list->AddText(ImVec2(row_pos.x, text_y),                       // 行首
                       IM_COL32(255, 204, 77, 255),                      // 与灵敏度标签同款金色
                       title_text);                                      // 标题文字

    // 拨动开关：紧跟标题，垂直居中
    const ImVec2 track_min(row_pos.x + title_width + title_gap,          // 轨道左边界
                           row_pos.y + (row_height - track_height) * 0.5f); // 轨道上边界
    const ImVec2 track_max(track_min.x + track_width, track_min.y + track_height); // 轨道右/下边界
    const ImU32 track_color = udp_video_inverted                        // 开：绿色
                                      ? (hovered ? IM_COL32(58, 202, 120, 255) : IM_COL32(42, 176, 98, 255))
                                      : (hovered ? IM_COL32(62, 74, 92, 255) : IM_COL32(46, 56, 70, 255)); // 关：深灰蓝
    draw_list->AddRectFilled(track_min, track_max, track_color, track_rounding, ImDrawFlags_None); // 轨道底色
    draw_list->AddRect(track_min,                                       // 描边：深色底上勾出边界
                       track_max,
                       udp_video_inverted ? IM_COL32(102, 228, 158, 255) : IM_COL32(100, 114, 138, 255),
                       track_rounding,
                       1.0f * scale,
                       ImDrawFlags_None);

    const float knob_radius = track_height * 0.5f - 3.0f * scale;        // 滑块半径
    const float knob_center_x = udp_video_inverted                      // 开：贴右
                                        ? track_max.x - 3.0f * scale - knob_radius
                                        : track_min.x + 3.0f * scale + knob_radius; // 关：贴左
    draw_list->AddCircleFilled(ImVec2(knob_center_x, track_min.y + track_height * 0.5f), // 滑块圆心
                               knob_radius,                             // 半径
                               IM_COL32(238, 243, 250, 255),            // 近白
                               24);                                     // 分段数（够圆滑）

    // 状态文字：开关右侧，把"开/关"用文字再说一遍，单看滑块也不至于误读
    draw_list->AddText(ImVec2(track_max.x + state_gap, text_y),         // 紧接开关右边
                       udp_video_inverted ? IM_COL32(124, 222, 166, 255) // 开：浅绿
                                          : IM_COL32(148, 160, 180, 255), // 关：灰蓝
                       state_text);

    return toggle_requested;                                            // 返回本帧是否请求翻转
}   // 结束 ShowVideoInvertRow

}// namespace

// 功能：本地联调用的自动登录钩子——设置了环境变量 ROBOGO_DEV_ROBOT_ID 时，直接以该编号作为 client_id 连接。
// 思路：只在首次调用时执行一次；未设置环境变量则完全不影响原有登录流程（比赛现场不会设置它）。
//       配套 tools/mock_rm_server.py 使用，可跳过每次点选队伍/机器人，方便验证 HUD 排版。
void ApplyDevAutoLogin(RoboGo::io::MqttClient &mqtt_client) {
    static bool already_applied = false;                                  // 保证只执行一次
    if (already_applied) {                                                // 已执行过
        return;                                                           // 直接返回
    }                                                                     // 结束重复执行保护
    already_applied = true;                                               // 标记已执行
    const char *dev_robot_id = std::getenv("ROBOGO_DEV_ROBOT_ID");        // 读取调试用 robot_id
    if (dev_robot_id == nullptr || *dev_robot_id == '\0') {               // 未设置或为空
        return;                                                           // 走正常登录流程
    }                                                                     // 结束未设置分支
    const char *dev_host = std::getenv("ROBOGO_DEV_MQTT_HOST");           // 可选：覆盖 broker 地址
    const char *dev_port = std::getenv("ROBOGO_DEV_MQTT_PORT");           // 可选：覆盖 broker 端口
    mqtt_client.set_server_endpoint(dev_host != nullptr && *dev_host != '\0' ? dev_host : "127.0.0.1",  // 默认本地假数据源
                                    dev_port != nullptr && *dev_port != '\0' ? dev_port : "3333");      // 默认 3333
    mqtt_client.set_client_id(dev_robot_id);                              // 以指定编号连接，触发 MQTT 握手
    // 可选：把本次会话标记为"指挥手"，方便联调指挥台（ROBOGO_DEV_COMMANDER=1）。
    const char *dev_commander = std::getenv("ROBOGO_DEV_COMMANDER");      // 读取指挥手调试开关
    if (dev_commander != nullptr && dev_commander[0] == '1' && dev_commander[1] == '\0') {   // 严格等于 "1"
        set_commander_role(true);                                         // 以指挥手身份进入
    }                                                                     // 结束指挥手开关判断
    SPDLOG_INFO("调试自动登录已启用 client_id={} server={}:{}",            // 记录一条日志便于确认
                dev_robot_id,                                             // 机器人编号
                dev_host != nullptr ? dev_host : "127.0.0.1",             // 实际使用的主机
                dev_port != nullptr ? dev_port : "3333");                 // 实际使用的端口
}   // 结束 ApplyDevAutoLogin

// 功能：开局面板总入口。组织登录区、各赛前指令行、鼠标灵敏度与底部主按钮。仅当未进入比赛时由主窗口调用。
bool StartPanel(const State &state, RoboGo::io::MqttClient &mqtt_client, bool udp_video_inverted) {
    ApplyDevAutoLogin(mqtt_client);                                    // 联调时可自动登录（未设环境变量则无副作用）
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings; // 无导航/装饰/移动/保存
    ScopedStyle window_style = MakeStartPanelWindowStyle();            // 半透明背景样式
    if (!ImGui::Begin("StartPanel", nullptr, window_flags)) {          // 开窗口（被裁剪则跳过）
        ImGui::End();   // 内容区不可见时仍需配对 End
        return false;   // 本帧不渲染直接返回（无翻转请求）
    }   // 结束 Begin 失败分支

    const bool is_logged_in = mqtt_client.get_connection_state() == RoboGo::io::MqttClient::ConnectionState::Connected; // 是否已登录
    // 底部固定区（灵敏度 + UDP 反转 + 主按钮）占多高，直接决定内容区还剩多少。
    // 以前是拍一个公式估，估多了内容被白挤、估少了主按钮被窗口裁——改成用上一帧的实测值自校准（首帧才用保守估计）。
    static float measured_footer_height = 0.0f;                        // 上一帧底部固定区的实测高度
    const float footer_height_guess = ImGui::GetFrameHeightWithSpacing() * 3.0f + (kPrimaryActionButtonHeight + 26.0f) * ImGui::GetScale(); // 首帧兜底估计
    const float footer_height = measured_footer_height > 0.0f ? measured_footer_height : footer_height_guess; // 稳态用实测值

    {   // 作用域：可滚动的内容区（登录区 + 各兵种功能面板）
        const float available_height = ImGui::GetContentRegionAvail().y;          // 当前剩余高度
        const float content_height = std::max(available_height - footer_height,    // 扣掉底部固定区
                                              120.0f * ImGui::GetScale());        // 保底高度，避免窗口过小时内容区被压成 0
        ScopedChildWindow content_area("StartPanelContent", ImVec2(0, content_height), false, ImGuiWindowFlags_NoScrollbar); // 不画滚动条（滚轮仍可滚）：面板横向尚有富余，声明条只会弄脏界面

        // 注：这里以前有一句"小幅溢出就把滚动偏移清零"的钳制，本意是防顶部被顶出去，
        // 但它每帧都会把用户刚滚出的位移拽回去，表现为滚轮一滚界面就抽搐、内容还看不全。
        // 已删除——放得下时本就不会有滚动量，放不下时应当让用户正常滚。

        // 先渲染登录/队伍选择区域。
        ShowTeamRobotSelection(mqtt_client);

        // 已登录后按当前兵种可见性渲染各赛前指令行。
        if (is_logged_in) {
            const StartPanelVisibility visibility = GetStartPanelVisibility(state); // 计算当前兵种可见性
            ShowAmmoAssemblyDeployRow(state, mqtt_client, visibility);    // 弹药/装配行
            ShowRecoverReviveRow(state, mqtt_client, visibility);         // 回血/复活行
            ShowMarkerPerformanceRow(state, mqtt_client, visibility);     // 标记/性能行
            ShowSentryRow(state, mqtt_client, visibility);                // 哨兵控制行
            ShowDartRow(state, mqtt_client, visibility.show_dart_panel);   // 飞镖控制行（此前定义了却从未调用，飞镖号登录后看不到面板）
        }   // 结束已登录分支
    }   // 结束内容区作用域（子窗口析构即 EndChild）

    // 底部固定区：灵敏度、UDP 画面反转与"进入比赛/登录"主按钮，始终可见、不会被内容压住。
    const float footer_top_y = ImGui::GetCursorPosY();                 // 记录底部固定区起始位置（窗口内相对坐标）
    ShowMouseSensitivityRow();                                        // 灵敏度滑块行
    const bool video_invert_requested = ShowVideoInvertRow(udp_video_inverted); // UDP 反转开关行
    ShowPrimaryActionButton(mqtt_client);                             // 主操作按钮
    if (footer_top_y > 0.0f) {                                        // 首帧或异常时（起点为 0）不采样
        measured_footer_height = ImGui::GetCursorPosY() - footer_top_y; // 实测高度，供下一帧给内容区让位
    }                                                                 // 结束实测采样

    ImGui::End();                                                    // 结束窗口
    return video_invert_requested;                                    // 返回本帧是否请求翻转
}   // 结束 StartPanel 总入口
