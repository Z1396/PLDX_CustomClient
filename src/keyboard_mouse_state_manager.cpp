#include "keyboard_mouse_state_manager.h"

static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static int32_t mouse_z = 0;
static bool game_mode_enabled = false;
static float mouse_sensitivity = 1.0f;
static bool marker_mode_enabled = false;
static bool marker_mode_include_sentry = true;// 标记模式是否包含哨兵，默认包含
static std::chrono::steady_clock::time_point last_publish_time;
static std::function<void(bool enabled)> game_input_capture_handler;
constexpr std::chrono::milliseconds publish_interval(13);// ~75Hz (1000/75 ≈ 13.33ms)

bool is_in_game_mode() {
    return game_mode_enabled;
}

void enter_game_mode() {
    SPDLOG_INFO("进入比赛模式");
    game_mode_enabled = true;
}

void set_game_input_capture_handler(std::function<void(bool enabled)> handler) {
    game_input_capture_handler = std::move(handler);
}

float get_mouse_sensitivity() {
    return mouse_sensitivity;
}

void set_mouse_sensitivity(float sensitivity) {
    SPDLOG_INFO("更新鼠标灵敏度 old={} new={}", mouse_sensitivity, sensitivity);
    mouse_sensitivity = sensitivity;
}

bool is_marker_mode() {
    return marker_mode_enabled;
}

bool is_marker_mode_include_sentry() {
    return marker_mode_include_sentry;
}

void set_marker_mode_include_sentry(bool include) {
    SPDLOG_INFO("更新地图标记范围 include_sentry={}", include);
    marker_mode_include_sentry = include;
}

void enter_marker_mode() {
    SPDLOG_INFO("进入地图点位标记模式");
    marker_mode_enabled = true;
}

void exit_marker_mode() {
    SPDLOG_INFO("退出地图点位标记模式");
    marker_mode_enabled = false;
}

void send_map_marker(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y) {
    if (!marker_mode_enabled) {
        SPDLOG_WARN("忽略地图标记请求，因为当前不在标记模式");
        return;
    }

    MapClickCmd map_click_cmd;
    map_click_cmd.set_is_send_all(marker_mode_include_sentry ? 2 : 1);
    std::string robot_id(7, 0x00);
    map_click_cmd.set_robot_id(robot_id);
    map_click_cmd.set_mode(1);
    map_click_cmd.set_enemy_id(0);
    map_click_cmd.set_ascii(0);
    map_click_cmd.set_type(1);
    map_click_cmd.set_map_x(map_x);
    map_click_cmd.set_map_y(map_y);

    std::string serialized;
    if (map_click_cmd.SerializeToString(&serialized)) {
        SPDLOG_INFO("发送地图标记 map_x={} map_y={} include_sentry={}",
                    map_x,
                    map_y,
                    marker_mode_include_sentry);
        mqtt_client.publish("MapClickCmd", serialized);
    } else {
        SPDLOG_ERROR("地图标记序列化失败 map_x={} map_y={}", map_x, map_y);
    }

    marker_mode_enabled = false;
    SPDLOG_INFO("地图点位标记模式已自动退出");
}

void update_keyboard_mouse_state(RoboGo::io::MqttClient &mqtt_client) {
    ImGuiIO &io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        game_mode_enabled = !game_mode_enabled;
        SPDLOG_INFO("比赛模式切换 enabled={}", game_mode_enabled);
    }

    static bool was_in_game_mode = false;
    if (game_mode_enabled) {
        if (!was_in_game_mode) {
            if (game_input_capture_handler) {
                game_input_capture_handler(true);
            }
        }
    } else if (was_in_game_mode) {
        if (game_input_capture_handler) {
            game_input_capture_handler(false);
        }
    }
    was_in_game_mode = game_mode_enabled;

    if (!game_mode_enabled) {
        mouse_x = 0;
        mouse_y = 0;
        mouse_z = 0;
        return;
    }
    mouse_x += static_cast<int32_t>(io.MouseDelta.x * mouse_sensitivity);
    mouse_y += static_cast<int32_t>(io.MouseDelta.y * mouse_sensitivity);
    mouse_z += static_cast<int32_t>(io.MouseWheel);

    auto now = std::chrono::steady_clock::now();
    if (now - last_publish_time < publish_interval) {
        return;
    }
    last_publish_time = now;

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
    KeyboardMouseControl keyboard_mouse_control;
    keyboard_mouse_control.set_mouse_x(mouse_x);
    keyboard_mouse_control.set_mouse_y(-mouse_y);
    keyboard_mouse_control.set_mouse_z(mouse_z);
    keyboard_mouse_control.set_left_button_down(io.MouseDown[0]);
    keyboard_mouse_control.set_right_button_down(io.MouseDown[1]);
    keyboard_mouse_control.set_keyboard_value(keyboard_value.to_ulong());
    keyboard_mouse_control.set_mid_button_down(io.MouseDown[2]);
    std::string serialized;
    if (keyboard_mouse_control.SerializeToString(&serialized)) {
        SPDLOG_DEBUG("发送键鼠控制 mouse=({}, {}, {}) buttons=({},{},{}) keyboard=0x{:X}",
                     mouse_x,
                     mouse_y,
                     mouse_z,
                     io.MouseDown[0],
                     io.MouseDown[1],
                     io.MouseDown[2],
                     keyboard_value.to_ulong());
        mqtt_client.publish("KeyboardMouseControl", serialized);
        mouse_x = 0;
        mouse_y = 0;
        mouse_z = 0;
    } else {
        SPDLOG_ERROR("键鼠控制序列化失败");
    }
}
