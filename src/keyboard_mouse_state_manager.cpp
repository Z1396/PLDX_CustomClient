// 键鼠状态管理实现：采集全局键鼠输入并以约 75Hz 向裁判系统上报 KeyboardMouseControl，
// 同时维护比赛模式、鼠标灵敏度与地图标记模式等模块级状态。
#include "keyboard_mouse_state_manager.h"

// 模块级全局状态：累计鼠标位移、比赛/标记模式开关、灵敏度及发布节流计时。
static int32_t mouse_x = 0; // 累计 X 方向鼠标位移(像素，受灵敏度缩放)
static int32_t mouse_y = 0; // 累计 Y 方向鼠标位移
static int32_t mouse_z = 0; // 累计鼠标滚轮位移
static bool game_mode_enabled = false; // 比赛模式开关(决定是否采集并上报键鼠)
static float mouse_sensitivity = 1.0f; // 鼠标灵敏度系数
static bool marker_mode_enabled = false; // 地图点位标记模式开关
static bool marker_mode_include_sentry = true;// 标记模式是否包含哨兵，默认包含
static std::chrono::steady_clock::time_point last_publish_time; // 上次发布键鼠消息的时刻(用于节流)
static std::function<void(bool enabled)> game_input_capture_handler; // 平台层游戏输入捕获回调(进入/退出比赛模式时通知)
constexpr std::chrono::milliseconds publish_interval(13);// ~75Hz (1000/75 ≈ 13.33ms)

// 功能：查询当前是否处于比赛模式。
bool is_in_game_mode() {
    return game_mode_enabled;
}

// 功能：进入比赛模式，置位开关并记录日志。
void enter_game_mode() {
    SPDLOG_INFO("进入比赛模式");
    game_mode_enabled = true; // 置位比赛模式标志，后续开始采集并上报键鼠
}

// 功能：注册平台层的游戏输入捕获回调，进入/退出比赛模式时回调以独占输入。
// 参数 handler：回调函数(参数为是否启用捕获)。
void set_game_input_capture_handler(std::function<void(bool enabled)> handler) {
    game_input_capture_handler = std::move(handler); // 以 move 保存回调，避免拷贝
}

// 功能：获取当前鼠标灵敏度系数。
float get_mouse_sensitivity() {
    return mouse_sensitivity;
}

// 功能：更新鼠标灵敏度系数并记录日志。
// 参数 sensitivity：新的灵敏度值。
void set_mouse_sensitivity(float sensitivity) {
    SPDLOG_INFO("更新鼠标灵敏度 old={} new={}", mouse_sensitivity, sensitivity);
    mouse_sensitivity = sensitivity; // 更新灵敏度，影响后续位移累计
}

// 功能：查询当前是否处于地图点位标记模式。
bool is_marker_mode() {
    return marker_mode_enabled;
}

// 功能：查询地图标记是否包含哨兵。
bool is_marker_mode_include_sentry() {
    return marker_mode_include_sentry;
}

// 功能：设置地图标记是否包含哨兵并记录日志。
// 参数 include：true 表示群发给哨兵。
void set_marker_mode_include_sentry(bool include) {
    SPDLOG_INFO("更新地图标记范围 include_sentry={}", include);
    marker_mode_include_sentry = include; // 更新标记范围标志
}

// 功能：进入地图点位标记模式并记录日志。
void enter_marker_mode() {
    SPDLOG_INFO("进入地图点位标记模式");
    marker_mode_enabled = true; // 置位标记模式，等待用户在小地图点击
}

// 功能：退出地图点位标记模式并记录日志。
void exit_marker_mode() {
    SPDLOG_INFO("退出地图点位标记模式");
    marker_mode_enabled = false; // 清除标记模式
}

// 功能：在地图标记模式下，向裁判系统发送一次地图点击标记指令(MapClickCmd)。
// 思路：校验标记模式 -> 填充 Protobuf 字段 -> 序列化并发布 -> 发送后自动退出标记模式。
// 参数 mqtt_client：MQTT 客户端；map_x/map_y：地图上的归一化坐标。
void send_map_marker(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y) {
    if (!marker_mode_enabled) {
        SPDLOG_WARN("忽略地图标记请求，因为当前不在标记模式");
        return; // 非标记模式直接忽略，避免误发
    }

    MapClickCmd map_click_cmd; // 构造地图点击指令 Protobuf 消息
    map_click_cmd.set_is_send_all(marker_mode_include_sentry ? 2 : 1); // 2 表示群发给哨兵，1 表示仅普通机器人
    std::string robot_id(7, 0x00); // 固定 7 字节 robot_id，先以全 0 占位
    map_click_cmd.set_robot_id(robot_id); // 设置目标机器人 ID(此处为占位)
    map_click_cmd.set_mode(1); // 设置指令模式
    map_click_cmd.set_enemy_id(0); // 敌方 ID(0 表示不适用)
    map_click_cmd.set_ascii(0); // ASCII 字段(0 表示不适用)
    map_click_cmd.set_type(1); // 指令类型
    map_click_cmd.set_map_x(map_x); // 设置地图 X 坐标
    map_click_cmd.set_map_y(map_y); // 设置地图 Y 坐标

    std::string serialized;
    if (map_click_cmd.SerializeToString(&serialized)) {
        SPDLOG_INFO("发送地图标记 map_x={} map_y={} include_sentry={}",
                    map_x,
                    map_y,
                    marker_mode_include_sentry);
        mqtt_client.publish("MapClickCmd", serialized); // 发布到 MapClickCmd 主题
    } else {
        SPDLOG_ERROR("地图标记序列化失败 map_x={} map_y={}", map_x, map_y);
    }

    marker_mode_enabled = false; // 发送完成后自动退出标记模式
    SPDLOG_INFO("地图点位标记模式已自动退出");
}

// 功能：每帧采集全局键鼠输入，按约 75Hz 向裁判系统上报 KeyboardMouseControl 消息。
// 思路：Esc 切换比赛模式 -> 在模式边沿通知平台捕获输入 -> 累计鼠标位移 -> 节流发布 ->
//       将位移/按键/鼠标键打包为 Protobuf 并序列化发布，发布成功后清零累计位移。
// 参数 mqtt_client：用于发布消息的 MQTT 客户端。
void update_keyboard_mouse_state(RoboGo::io::MqttClient &mqtt_client) {
    ImGuiIO &io = ImGui::GetIO(); // 获取 ImGui IO，用于读取鼠标/键盘输入
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { // 按下 Esc 切换比赛模式(进入/退出)
        game_mode_enabled = !game_mode_enabled;
        SPDLOG_INFO("比赛模式切换 enabled={}", game_mode_enabled);
    }

    static bool was_in_game_mode = false; // 记录上一帧的比赛模式状态，用于检测进入/退出边沿
    // 在比赛模式进入或退出的边沿，通知平台层捕获或释放游戏输入(独占键鼠)
    if (game_mode_enabled) {
        if (!was_in_game_mode) {
            if (game_input_capture_handler) {
                game_input_capture_handler(true); // 进入比赛模式：请求平台捕获输入
            }
        }
    } else if (was_in_game_mode) {
        if (game_input_capture_handler) {
            game_input_capture_handler(false); // 退出比赛模式：释放输入捕获
        }
    }
    was_in_game_mode = game_mode_enabled; // 更新边沿检测用的上一帧状态

    if (!game_mode_enabled) { // 非比赛模式：清零累计位移并跳过上报
        mouse_x = 0;
        mouse_y = 0;
        mouse_z = 0;
        return;
    }
    // 按灵敏度累计本帧鼠标位移(像素)与滚轮变化
    mouse_x += static_cast<int32_t>(io.MouseDelta.x * mouse_sensitivity);
    mouse_y += static_cast<int32_t>(io.MouseDelta.y * mouse_sensitivity);
    mouse_z += static_cast<int32_t>(io.MouseWheel);

    auto now = std::chrono::steady_clock::now();
    if (now - last_publish_time < publish_interval) { // 节流：未到发布间隔(约 75Hz)则跳过
        return;
    }
    last_publish_time = now; // 更新上次发布时刻

    // 将各按键状态按位打包进 32 位键盘值(低 16 位对应 W/S/A/D/Shift/Ctrl/Q~B 等键)
    std::bitset<32> keyboard_value;
    keyboard_value[0] = ImGui::IsKeyPressed(ImGuiKey_W);
    keyboard_value[1] = ImGui::IsKeyPressed(ImGuiKey_S);
    keyboard_value[2] = ImGui::IsKeyPressed(ImGuiKey_A);
    keyboard_value[3] = ImGui::IsKeyPressed(ImGuiKey_D);
    keyboard_value[4] = ImGui::IsKeyPressed(ImGuiKey_LeftShift) || ImGui::IsKeyPressed(ImGuiKey_RightShift);
    keyboard_value[5] = ImGui::IsKeyPressed(ImGuiKey_LeftCtrl) || ImGui::IsKeyPressed(ImGuiKey_RightCtrl);
    keyboard_value[6] = ImGui::IsKeyPressed(ImGuiKey_Q);
    keyboard_value[7] = ImGui::IsKeyPressed(ImGuiKey_E);
    keyboard_value[8] = ImGui::IsKeyPressed(ImGuiKey_R);
    keyboard_value[9] = ImGui::IsKeyPressed(ImGuiKey_F);
    keyboard_value[10] = ImGui::IsKeyPressed(ImGuiKey_G);
    keyboard_value[11] = ImGui::IsKeyPressed(ImGuiKey_Z);
    keyboard_value[12] = ImGui::IsKeyPressed(ImGuiKey_X);
    keyboard_value[13] = ImGui::IsKeyPressed(ImGuiKey_C);
    keyboard_value[14] = ImGui::IsKeyPressed(ImGuiKey_V);
    keyboard_value[15] = ImGui::IsKeyPressed(ImGuiKey_B);
    KeyboardMouseControl keyboard_mouse_control; // 构造键鼠控制 Protobuf 消息
    keyboard_mouse_control.set_mouse_x(mouse_x);
    keyboard_mouse_control.set_mouse_y(-mouse_y); // Y 轴取反以匹配裁判系统坐标系
    keyboard_mouse_control.set_mouse_z(mouse_z);
    keyboard_mouse_control.set_left_button_down(io.MouseDown[0]); // 左键按下状态
    keyboard_mouse_control.set_right_button_down(io.MouseDown[1]); // 右键按下状态
    keyboard_mouse_control.set_keyboard_value(keyboard_value.to_ulong()); // 打包后的 32 位键盘值
    keyboard_mouse_control.set_mid_button_down(io.MouseDown[2]); // 中键按下状态
    std::string serialized;
    if (keyboard_mouse_control.SerializeToString(&serialized)) { // 序列化为字节串
        SPDLOG_DEBUG("发送键鼠控制 mouse=({}, {}, {}) buttons=({},{},{}) keyboard=0x{:X}",
                     mouse_x,
                     mouse_y,
                     mouse_z,
                     io.MouseDown[0],
                     io.MouseDown[1],
                     io.MouseDown[2],
                     keyboard_value.to_ulong());
        mqtt_client.publish("KeyboardMouseControl", serialized); // 发布到 KeyboardMouseControl 主题
        mouse_x = 0; // 上报成功后清零累计位移
        mouse_y = 0;
        mouse_z = 0;
    } else {
        SPDLOG_ERROR("键鼠控制序列化失败");
    }
}
