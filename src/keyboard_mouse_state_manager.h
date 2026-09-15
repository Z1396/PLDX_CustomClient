#pragma once

#include <pch.h>

#include "io/mqtt_client.h"

bool is_in_game_mode();

void enter_game_mode();

void set_game_input_capture_handler(std::function<void(bool enabled)> handler);

void update_keyboard_mouse_state(RoboGo::io::MqttClient &mqtt_client);

float get_mouse_sensitivity();

void set_mouse_sensitivity(float sensitivity);

bool is_marker_mode();

bool is_marker_mode_include_sentry();

void set_marker_mode_include_sentry(bool include);

void enter_marker_mode();

void exit_marker_mode();

void send_map_marker(RoboGo::io::MqttClient &mqtt_client, float map_x, float map_y);
