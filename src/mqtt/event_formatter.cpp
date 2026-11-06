// Event 消息文案翻译与顶部提示入队实现。
#include "mqtt/event_formatter.h"

#include "app/robot_registry.h"   // GetRobotName：robot_id -> 中文名
#include "logging/logging.h"

namespace RoboGo::mqtt {

using RoboGo::app::robot_registry::GetRobotName;

namespace {

    // 把形如 "a,b" 的参数按首个逗号拆成两段；找不到逗号返回空。
    std::optional<std::pair<std::string, std::string>> SplitByComma(const std::string &param) {
        const size_t comma_pos = param.find(',');
        if (comma_pos == std::string::npos) {
            return std::nullopt;
        }
        return std::make_pair(param.substr(0, comma_pos), param.substr(comma_pos + 1));
    }

}  // namespace

std::string FormatEventText(const Event &event) {
    std::string text;
    switch (event.event_id()) {
        case 1: {  // 击杀事件（参数 1 为被击杀者，参数 2 为击杀者，如 "1,101"）
            const auto parts = SplitByComma(event.param());
            if (parts.has_value()) {
                text = GetRobotName(parts->first) + " 击杀了 " + GetRobotName(parts->second);
            } else {
                text = "击杀事件: " + event.param();  // 参数格式不符合预期（没有逗号）时原样附上
            }
            break;
        }
        case 2: {  // 前哨站被摧毁（红方前哨站为 11，蓝方前哨站为 111）
            const std::string &param = event.param();
            std::string target_name;
            if (param == "11") {
                target_name = "红方前哨站";
            } else if (param == "111") {
                target_name = "蓝方前哨站";
            } else {
                target_name = "前哨站" + param;  // 未知 ID 时附上原始值，便于排查
            }
            text = target_name + "被摧毁";
            break;
        }
        case 3: {  // 大能量机关激活成功（参数形如 "10,9.6"：激活臂数,平均环数）
            const auto parts = SplitByComma(event.param());
            if (parts.has_value()) {
                text = "大能量机关激活成功: " + parts->first + "个灯臂，平均环数" + parts->second;
            } else {
                text = "大能量机关激活: " + event.param();
            }
            break;
        }
        case 4: {  // 能量机关进入已激活状态（1 小能量机关，2 大能量机关）
            const std::string &param = event.param();
            if (param == "1") {
                text = "小能量机关已激活";
            } else if (param == "2") {
                text = "大能量机关已激活";
            } else {
                text = "能量机关已激活: " + param;
            }
            break;
        }
        case 5: {  // R 前哨阵地改变控制状态（1 红临时 / 2 红完全 / 3 蓝临时 / 4 蓝完全）
            const std::string &param = event.param();
            std::string state_name;
            if (param == "1") state_name = "红方临时控制";
            else if (param == "2") state_name = "红方完全控制";
            else if (param == "3") state_name = "蓝方临时控制";
            else if (param == "4") state_name = "蓝方完全控制";
            else state_name = "未知状态(" + param + ")";
            text = "R 前哨阵地控制状态变更: " + state_name;
            break;
        }
        case 6: {  // B 前哨阵地改变控制状态（参数含义与 case 5 相同）
            const std::string &param = event.param();
            std::string state_name;
            if (param == "1") state_name = "红方临时控制";
            else if (param == "2") state_name = "红方完全控制";
            else if (param == "3") state_name = "蓝方临时控制";
            else if (param == "4") state_name = "蓝方完全控制";
            else state_name = "未知状态(" + param + ")";
            text = "B 前哨阵地控制状态变更: " + state_name;
            break;
        }
        case 7: {  // R 基地阵地改变控制方（1 红完全 / 2 蓝临时）
            const std::string &param = event.param();
            if (param == "1") {
                text = "R 基地阵地控制方变更: 红方完全控制";
            } else if (param == "2") {
                text = "R 基地阵地控制方变更: 蓝方临时控制";
            } else {
                text = "R 基地阵地控制方变更: 未知状态(" + param + ")";
            }
            break;
        }
        case 8: {  // B 基地阵地改变控制方（1 蓝完全 / 2 红临时）
            const std::string &param = event.param();
            if (param == "1") {
                text = "B 基地阵地控制方变更: 蓝方完全控制";
            } else if (param == "2") {
                text = "B 基地阵地控制方变更: 红方临时控制";
            } else {
                text = "B 基地阵地控制方变更: 未知状态(" + param + ")";
            }
            break;
        }
        case 9: {  // 飞镖命中（第一个参数命中方：1 红 / 2 蓝；第二个参数命中目标）
            const auto parts = SplitByComma(event.param());
            if (parts.has_value()) {
                const std::string side_name = (parts->first == "1") ? "红方" : "蓝方";
                const std::string &target = parts->second;

                std::string target_name;
                if (target == "1") target_name = "前哨站";
                else if (target == "2") target_name = "基地固定目标";
                else if (target == "3") target_name = "基地随机固定目标";
                else if (target == "4") target_name = "基地随机移动目标";
                else if (target == "5") target_name = "基地末端移动目标";
                else target_name = "目标" + target;

                text = side_name + "飞镖命中" + target_name;
            } else {
                text = "飞镖命中: " + event.param();
            }
            break;
        }
        case 10:  // 对方飞镖闸门开启（无参数）
            text = "对方飞镖闸门开启";
            break;
        case 11:  // 基地遭到攻击（无参数，每次触发有 5s 内置冷却）
            text = "基地遭到攻击";
            break;
        case 12:  // 对方前哨站停转（无参数）
            text = "对方前哨站停转";
            break;
        case 13:  // 对方基地护甲展开（无参数）
            text = "对方基地护甲展开";
            break;
        case 14:  // 重装战亡临时激活，进入强制退出缓冲期（无参数）
            text = "重装战亡临时激活，进入强制退出缓冲期";
            break;
        case 15: {  // 装配结果（0 成功 ~ 8 缓冲期到期强制结束）
            const std::string &param = event.param();
            std::string result_name;
            if (param == "0") result_name = "成功";
            else if (param == "1") result_name = "拔出";
            else if (param == "2") result_name = "超时";
            else if (param == "3") result_name = "离开装配区过久";
            else if (param == "4") result_name = "重装战亡";
            else if (param == "5") result_name = "四级难度未满足完成协作时限";
            else if (param == "6") result_name = "主动退出";
            else if (param == "7") result_name = "完成装配但结算时未检测到有能量单元";
            else if (param == "8") result_name = "缓冲期到期，装配流程强制结束";
            else result_name = "未知结果(" + param + ")";
            text = "装配结果: " + result_name;
            break;
        }
        case 16:  // 重装成功安装模块化 42mm 发射机构（无参数）
            text = "重装机器人成功安装模块化42mm发射机构";
            break;
        default:  // 未识别的事件 ID：打印 ID 与原始参数，便于后续补全处理
            text = "未知事件: ID=" + std::to_string(event.event_id()) + ", 参数=" + event.param();
            break;
    }
    return text;
}

void PushEventToast(State &state, uint32_t event_id, const std::string &text) {
    int level = -1;  // -1 表示不提示（纯日志事件）
    switch (event_id) {
        case 1: level = 0; break;                             // 击杀：普通提示
        case 2: level = 2; break;                             // 前哨站被摧毁：危急
        case 5: case 6: case 7: case 8: level = 1; break;      // 阵地控制变更：重要
        case 9: level = 2; break;                             // 飞镖命中：危急（2027 命中会致盲并大幅扣血）
        case 10: level = 1; break;                            // 对方飞镖闸门开启：重要
        case 11: level = 2; break;                            // 基地遭到攻击：危急
        case 12: level = 1; break;                            // 对方前哨站停转：重要
        case 13: level = 1; break;                            // 对方基地护甲展开：重要
        case 14: level = 2; break;                            // 重装战亡进入缓冲期：危急
        case 15: level = 1; break;                            // 装配结果：重要
        case 16: level = 1; break;                            // 模块化 42mm 安装成功：重要
        default: break;                                       // 其余事件（能量机关等）只进日志，不弹提示
    }
    if (level < 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    // 先剔除超过 6 秒的旧提示（与渲染端淡出时长一致），避免队列越积越长。
    state.event_toasts.erase(
            std::remove_if(state.event_toasts.begin(), state.event_toasts.end(),
                           [&now](const EventToast &toast) {
                               return std::chrono::duration_cast<std::chrono::milliseconds>(now - toast.time).count() > 6000;
                           }),
            state.event_toasts.end());
    state.event_toasts.push_back(EventToast{text, now, level});
    if (state.event_toasts.size() > 6) {
        state.event_toasts.erase(state.event_toasts.begin());  // 同时最多保留 6 条，丢弃最旧
    }
}

}  // namespace RoboGo::mqtt
