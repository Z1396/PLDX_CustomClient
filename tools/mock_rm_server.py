#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""RoboGo 本地假数据源 / 模拟裁判系统（2027 协议全量）。

用途
    RoboGo 是 MQTT 客户端，需要连一个 broker。本工具自带**最小 MQTT 3.1.1 broker**，
    并完整模拟裁判系统的两半：

    1) 下行（Package 2，23 条消息全量）：按协议频率下发
         50Hz CustomByteBlock / 10Hz RobotDynamicStatus / 5Hz GameStatus /
         1Hz 其余定频消息 / 触发式 Event、PenaltyInfo、MapClickInfo、RadarInfoToClient、
         RobotPathPlanInfo、SentryCtrlResult、Buff。
    2) 上行（Package 1，9 类指令闭环）：解析客户端发来的 PUBLISH，按语义改变比赛状态，
         并立刻回推对应下行消息与回执。例如
         CommonCommand(兑换发弹量) -> 扣金币/加弹药 -> GlobalLogisticsStatus；
         AssemblyCommand -> 推进装配流程 -> TechCoreMotionStateSync + Event15/16；
         DartCommand(确认发射) -> 飞镖命中 Event9 + 对方机器人扣血；
         SentryCtrlCommand -> 改哨兵姿态 + SentryCtrlResult 回执；
         MapClickCmd -> 原样回推 MapClickInfo。

    不需要任何第三方库：protobuf 编码/解码、MQTT broker 全部手写。

用法
    python tools/mock_rm_server.py                    # 默认 127.0.0.1:3333、3 号步兵
    python tools/mock_rm_server.py --robot-id 1       # 以重装身份登录
    python tools/mock_rm_server.py --port 3334        # 换端口（记得在客户端改地址）
    python tools/mock_rm_server.py --no-custom-byte   # 关闭 50Hz 自定义数据流（调试视频时）
    python tools/mock_rm_server.py --selftest         # 自检：握手/订阅/全量编码/指令闭环

    RoboGo 侧：服务器地址填 127.0.0.1:3333，选任意机器人登录即可连接。
    界面上的按钮会真的改变这个"裁判系统"的状态（金币、装配、飞镖、哨兵…），
    按 l 可查看收到过的上行指令，按 h 打印当前比赛状态。

交互键（在运行本工具的终端里按，输入后回车）
    b / n   己方 / 对方基地 -1000 血
    d       飞镖命中事件            k  击杀事件        a  装配成功事件
    e       轮换 Event 1~16        s  轮换阵地控制状态
    p / r / o  黄牌 / 红牌 / 超热量判罚
    c       己方控制度 +300        u  加一个攻击 Buff  g  加"补给区" Buff(type=6)
    j       切换战亡待复活
    v       触发全局特殊机制      w  激活能量机关    x  轮换哨兵姿态
    z       切换飞镖闸门          m  下发地图标记
    h       打印当前状态          l  打印上行指令日志
    1~9     手动模拟一条上行指令（1键鼠 2自定义 3地图 4装配 5性能 6常用 7机关 8飞镖 9哨兵），
             不需要启动客户端也能验证指令闭环
    q       退出
"""

import argparse
import random
import socket
import struct
import sys
import threading
import time

# ---------------------------------------------------------------------------
# 一、最小 protobuf 编码器
#    只需要 varint / 定长 32 位 / 长度前缀三种 wire type，足以覆盖本项目所有字段。
# ---------------------------------------------------------------------------


def _varint(value: int) -> bytes:
    """把整数编码成 protobuf varint（负数按 64 位补码处理）。"""
    if value < 0:
        value += 1 << 64
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def _tag(field: int, wire_type: int) -> bytes:
    """字段头：field_number << 3 | wire_type。"""
    return _varint((field << 3) | wire_type)


def pb_uint(field: int, value: int) -> bytes:
    """uint32/int32/bool 字段（wire type 0）。"""
    return _tag(field, 0) + _varint(int(value))


def pb_float(field: int, value: float) -> bytes:
    """float 字段（wire type 5，固定 4 字节小端）。"""
    return _tag(field, 5) + struct.pack("<f", float(value))


def pb_string(field: int, value: str) -> bytes:
    """string 字段（wire type 2）。"""
    raw = value.encode("utf-8")
    return _tag(field, 2) + _varint(len(raw)) + raw


def pb_packed_uint(field: int, values) -> bytes:
    """repeated uint32（默认 packed 编码）。"""
    body = b"".join(_varint(int(v)) for v in values)
    return _tag(field, 2) + _varint(len(body)) + body


def _concat(*parts) -> bytes:
    """把多个已编码字段拼成一条消息（空字段直接跳过）。"""
    return b"".join(p for p in parts if p)


def pb_bytes(field: int, value: bytes) -> bytes:
    """bytes 字段（wire type 2）。"""
    return _tag(field, 2) + _varint(len(value)) + bytes(value)


def pb_packed_int32(field: int, values) -> bytes:
    """repeated int32（packed）；负数按 64 位补码 varint，与 int32 编码一致。"""
    body = b"".join(_varint(int(v) & ((1 << 64) - 1)) for v in values)
    return _tag(field, 2) + _varint(len(body)) + body


def pb_message(field: int, payload: bytes) -> bytes:
    """嵌套 message 字段（wire type 2，内容即子消息的序列化字节）。"""
    return _tag(field, 2) + _varint(len(payload)) + payload


# ---------------------------------------------------------------------------
# 一(b)、最小 protobuf 解码器 —— 用于解析客户端发来的上行指令
# ---------------------------------------------------------------------------


def _read_varint(data: bytes, pos: int):
    """从 data[pos:] 读一个 varint，返回 (值, 新位置)。"""
    result = 0
    shift = 0
    while pos < len(data):
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        if not (byte & 0x80):
            return result, pos
        shift += 7
    raise ValueError("varint 被截断")


def parse_fields(data: bytes) -> dict:
    """把一条 protobuf 消息解成 {字段号: [值, ...]}。

    wire type 0 -> varint（bool/uint32/int32 统一按无符号读出，调用方按需转）；
    wire type 2 -> bytes（string / bytes / packed repeated / 嵌套 message 都先原样保留）；
    wire type 5 -> float（4 字节小端）；
    wire type 1 -> double（8 字节小端，本项目用不到但顺手支持）。
    """
    out = {}
    pos = 0
    while pos < len(data):
        tag, pos = _read_varint(data, pos)
        field, wire_type = tag >> 3, tag & 0x07
        if wire_type == 0:
            value, pos = _read_varint(data, pos)
        elif wire_type == 2:
            size, pos = _read_varint(data, pos)
            value = data[pos:pos + size]
            pos += size
        elif wire_type == 5:
            value = struct.unpack("<f", data[pos:pos + 4])[0]
            pos += 4
        elif wire_type == 1:
            value = struct.unpack("<d", data[pos:pos + 8])[0]
            pos += 8
        else:
            raise ValueError("不支持的 wire type %d" % wire_type)
        out.setdefault(field, []).append(value)
    return out


def field_u(fields: dict, number: int, default=0):
    """取标量字段（varint）；int32 负数会被读成大整数，这里转回有符号。"""
    values = fields.get(number)
    if not values:
        return default
    value = values[-1]
    if isinstance(value, int) and value >= (1 << 63):
        value -= 1 << 64
    return value


def field_b(fields: dict, number: int, default=b""):
    """取长度前缀字段（bytes / string / packed / 嵌套 message）。"""
    values = fields.get(number)
    return values[-1] if values else default


def field_f(fields: dict, number: int, default=0.0):
    """取 float 字段。"""
    values = fields.get(number)
    return values[-1] if values else default


def parse_packed_uint(data: bytes):
    """解析 packed repeated uint32/int32 的字节串。"""
    out = []
    pos = 0
    while pos < len(data):
        value, pos = _read_varint(data, pos)
        out.append(value)
    return out


# ---------------------------------------------------------------------------
# 二、各消息的构造
# ---------------------------------------------------------------------------


def msg_game_status(stage: int, countdown: int, elapsed: int, red: int, blue: int) -> bytes:
    """2.2.3 GameStatus：比赛阶段 / 倒计时 / 比分。"""
    return _concat(
        pb_uint(1, 1),          # current_round 当前局号
        pb_uint(2, 1),          # total_rounds 总局数
        pb_uint(3, red),        # red_score
        pb_uint(4, blue),       # blue_score
        pb_uint(5, stage),      # current_stage 4=比赛中
        pb_uint(6, countdown),  # stage_countdown_sec
        pb_uint(7, elapsed),    # stage_elapsed_sec
        pb_uint(8, 0),          # is_paused = false
        pb_uint(9, 255),        # game_result 非结算阶段为 255
        pb_uint(10, 255),       # end_reason 非结算阶段为 255
    )


def cp_rates(sim: dict) -> tuple:
    """按阵地控制状态推算每秒控制度增量：完全控制的前哨 +1/秒、基地 +3/秒。

    客户端只收到 total_CP 一个总数，前哨/基地的拆分是它自己按这个模型反推的；
    模拟器这边也用同一套模型产生总量，两边才对得上（否则分项之和会与总数打架）。
    """
    ally_outpost = (sim["outpost_r_status"] in (1, 2)) + (sim["outpost_b_status"] in (1, 2))
    ally_base = (sim["base_ally_ctrl"] == 1) + (sim["base_enemy_ctrl"] == 2)
    enemy_outpost = (sim["outpost_r_status"] in (3, 4)) + (sim["outpost_b_status"] in (3, 4))
    enemy_base = (sim["base_ally_ctrl"] == 2) + (sim["base_enemy_ctrl"] == 1)
    return ally_outpost * 1 + ally_base * 3, enemy_outpost * 1 + enemy_base * 3


def msg_global_unit_status(sim: dict) -> bytes:
    """2.2.4 GlobalUnitStatus：基地/前哨血量、阵地控制状态、8 台机器人血量、累计控制度与伤害。"""
    return _concat(
        pb_uint(1, sim["base_ally"]),            # 己方基地血量（2027 上限 10000）
        pb_uint(2, sim["base_ally_status"]),     # 己方基地状态：0 有 100% 防御增益
        pb_uint(3, sim["base_ally_ctrl"]),       # 己方基地阵地控制状态（1 己方完全 / 2 对方临时）
        pb_uint(4, sim["outpost_r"]),            # R 前哨站血量（2027 上限 1000）
        pb_uint(5, sim["outpost_r_status"]),     # R 前哨阵地控制状态
        pb_uint(6, sim["base_enemy"]),           # 对方基地血量
        pb_uint(7, sim["base_enemy_status"]),    # 对方基地状态
        pb_uint(8, sim["base_enemy_ctrl"]),      # 对方基地阵地控制状态（1 对方完全 / 2 己方临时）
        pb_uint(9, sim["outpost_b"]),            # B 前哨站血量
        pb_uint(10, sim["outpost_b_status"]),    # B 前哨阵地控制状态
        pb_packed_uint(11, sim["robot_health"]),  # 己方 1/3/4/7 + 对方 1/3/4/7
        pb_packed_uint(12, sim["robot_bullets"]),  # 己方各机器人剩余发弹量
        pb_uint(13, sim["damage_ally"]),         # 己方累计总伤害
        pb_uint(14, sim["damage_enemy"]),        # 对方累计总伤害
        pb_uint(15, sim["cp_ally"]),             # 己方累计控制度（2027 新增）
        pb_uint(16, sim["cp_enemy"]),            # 对方累计控制度（2027 新增）
    )


def msg_global_logistics(sim: dict) -> bytes:
    """2.2.5 GlobalLogisticsStatus：经济 / 科技等级 / 加密等级。"""
    return _concat(
        pb_uint(1, sim["economy"]),          # 剩余经济
        pb_uint(2, sim["economy_total"]),    # 累计总经济
        pb_uint(3, sim["tech_level"]),      # 科技等级（装配成功后提升）
        pb_uint(4, sim["encryption_level"]), # 加密等级
    )


def msg_robot_static(sim: dict) -> bytes:
    """2.2.10 RobotStaticStatus：本机编号/类型/最大血量等（header 靠 robot_id 判红蓝）。"""
    return _concat(
        pb_uint(1, 1),                        # connection_state 已连接
        pb_uint(2, 0),                        # field_state 已上场
        pb_uint(3, 1),                        # alive_state 存活
        pb_uint(4, sim["robot_id"]),          # robot_id（<100 红方，>=100 蓝方）
        pb_uint(5, sim["robot_type"]),        # robot_type
        pb_uint(6, sim["performance"]["shooter"]),   # 发射机构性能体系（响应上行指令）
        pb_uint(7, sim["performance"]["chassis"]),   # 底盘性能体系（响应上行指令）
        pb_uint(8, 3),                        # 等级
        pb_uint(9, sim["max_health"]),        # 最大血量
        pb_uint(10, 400),                     # 最大热量
        pb_float(11, 40.0),                   # 热量冷却速率
        pb_uint(12, 80),                      # 最大功率
        pb_uint(13, 60),                      # 最大缓冲能量
        pb_uint(14, 0),                       # 最大底盘能量（2027 保留）
    )


def msg_robot_dynamic(sim: dict) -> bytes:
    """2.2.11 RobotDynamicStatus：10Hz 实时血量/热量/经验/发弹量。"""
    return _concat(
        pb_uint(1, sim["health"]),
        pb_float(2, sim["heat"]),
        pb_float(3, 18.5),                    # 上次弹丸射速
        pb_uint(4, 0),                        # reserved（2027 保留位）
        pb_uint(5, sim["buffer_energy"]),
        pb_uint(6, 320),                      # 当前经验
        pb_uint(7, 500),                      # 升级所需经验
        pb_uint(8, sim["fired"]),
        pb_uint(9, sim["ammo"]),
        pb_uint(10, 0),                       # 是否脱战
        pb_uint(11, 0),                       # 脱战倒计时
        pb_uint(12, 1),                       # 可远程补血
        pb_uint(13, 1),                       # 可远程补弹
    )


def msg_robot_module(sim: dict) -> bytes:
    """2.2.12 RobotModuleStatus：11 个模块在线状态 + 模块化 42mm 装配权限。"""
    return _concat(
        pb_uint(1, 1),    # 电源管理
        pb_uint(2, 1),    # RFID
        pb_uint(3, 1),    # 灯条
        pb_uint(4, 1),    # 17mm 测速
        pb_uint(5, 0),    # 42mm 测速（重装没装时为 0）
        pb_uint(6, 1),    # 定位
        pb_uint(7, 2),    # 装甲：2 = 安装不规范（用来验证角标区分）
        pb_uint(8, 1),    # 图传
        pb_uint(9, 1),    # 电容
        pb_uint(10, 1),   # 主控
        pb_uint(11, sim["modular_42mm"]),  # 模块化 42mm 装配权限
    )


def msg_penalty(penalty_type: int, effect_sec: int, total_num: int = 1) -> bytes:
    """2.2.15 PenaltyInfo：判罚类型、持续秒数、累计判罚数。"""
    return _concat(
        pb_uint(1, penalty_type),
        pb_uint(2, effect_sec),
        pb_uint(3, total_num),
    )


def msg_event(event_id: int, param: str = "") -> bytes:
    """2.2.7 Event：事件号 + 参数串。"""
    return _concat(
        pb_uint(1, event_id),
        pb_string(2, param) if param else b"",
    )


def msg_tech_core(sim: dict) -> bytes:
    """2.2.22 TechCoreMotionStateSync：装配核心状态（用于验证装配面板的禁用逻辑与倒计时）。"""
    return _concat(
        pb_uint(1, 4),                       # 最高可选难度
        pb_uint(2, sim["core_state"]),       # 1 初始位 / 2 运动中 / 3 已到位
        pb_uint(3, sim["core_putin"]),
        pb_uint(4, sim["core_move"]),
        pb_uint(5, sim["core_rotate"]),
        pb_uint(6, 0),                       # 对方核心状态
        pb_uint(7, sim["core_remain_all"]),  # 四级装配总剩余
        pb_uint(8, sim["core_remain_step"]),
    )


# ---- 2027 补齐的下行消息（原版只实现了 9 条，下面 14 条覆盖协议 Package 2 全量）----


def msg_global_special_mechanism(sim: dict) -> bytes:
    """2.2.6 正在生效的全局特殊机制（1Hz）。"""
    mechanisms = sim.get("mechanisms", [])
    ids = [m[0] for m in mechanisms]
    times = [m[1] for m in mechanisms]
    return _concat(pb_packed_uint(1, ids), pb_packed_int32(2, times))


def msg_robot_injury_stat(sim: dict) -> bytes:
    """2.2.8 一次存活期间累计受伤统计（1Hz）。"""
    i = sim["injury"]
    return _concat(pb_uint(1, i["total"]), pb_uint(2, i["collision"]),
                   pb_uint(3, i["small"]), pb_uint(4, i["large"]),
                   pb_uint(5, i["dart"]), pb_uint(6, i["module"]),
                   pb_uint(7, i["offline"]), pb_uint(8, i["penalty"]),
                   pb_uint(9, i["server"]), pb_uint(10, i["killer_id"]))


def msg_robot_respawn_status(sim: dict) -> bytes:
    """2.2.9 复活状态（1Hz）。"""
    r = sim["respawn"]
    return _concat(pb_uint(1, 1 if r["pending"] else 0), pb_uint(2, r["total"]),
                   pb_uint(3, r["progress"]), pb_uint(4, 1 if r["free"] else 0),
                   pb_uint(5, r["cost"]), pb_uint(6, 1 if r["can_pay"] else 0))


def msg_robot_position(sim: dict) -> bytes:
    """2.2.13 机器人空间坐标和朝向（1Hz）。"""
    p = sim["position"]
    return _concat(pb_float(1, p["x"]), pb_float(2, p["y"]), pb_float(3, p["z"]),
                   pb_float(4, p["yaw"]), pb_uint(5, sim["robot_id"]))


def msg_buff(buff: tuple) -> bytes:
    """2.2.14 Buff（获得时触发，此后 1Hz 定频直到失效）。buff=(robot_id,type,level,max,left)。"""
    robot_id, buff_type, level, max_time, left_time = buff
    return _concat(pb_uint(1, robot_id), pb_uint(2, buff_type), pb_uint(3, level),
                   pb_uint(4, max_time), pb_uint(5, left_time))


def _sample_polyline(waypoints, points):
    """把折线航点按弧长等距采样成 points+1 个整数坐标点。"""
    import math
    cum = [0.0]                                   # 累计弧长表：cum[i] = 到第 i 个航点的距离
    for i in range(len(waypoints) - 1):
        cum.append(cum[-1] + math.hypot(waypoints[i + 1][0] - waypoints[i][0],
                                        waypoints[i + 1][1] - waypoints[i][1]))
    total = cum[-1]
    out = []
    for k in range(points + 1):
        d = total * k / points                    # 目标弧长位置
        seg = 0
        while seg < len(cum) - 2 and cum[seg + 1] < d:
            seg += 1
        seg_len = cum[seg + 1] - cum[seg]
        t = 0.0 if seg_len <= 0 else (d - cum[seg]) / seg_len
        x1, y1 = waypoints[seg]
        x2, y2 = waypoints[seg + 1]
        out.append((int(round(x1 + (x2 - x1) * t)), int(round(y1 + (y2 - y1) * t))))
    return out


def make_patrol_path(waypoints=None, points=50):
    """生成一条沿高地巡逻的哨兵轨迹（起点 + 49 段相对偏移）。

    之前这里是随机抖动（每段 ±20 的伪随机数），累加 49 次后所有点挤在起点附近，
    客户端画出来就是地图左上角一坨乱线；改成标准圆环又太像"UI 画错了个圈"。
    这里用一条带直道和拐角的折线（贴近真实下发的规划路线）：
    从高地下方出发 -> 沿左侧上 -> 顶边横移 -> 右侧斜下 -> 底边回 -> 闭合。
    返回 (start_x, start_y, offset_x, offset_y)，偏移按整数累计，末点回到起点。
    """
    if waypoints is None:
        waypoints = [(112, 104), (112, 52), (150, 40), (192, 66),
                     (168, 104), (112, 104)]
    positions = _sample_polyline(waypoints, points)
    start_x, start_y = positions[0]
    offset_x = [positions[i + 1][0] - positions[i][0] for i in range(points)]
    offset_y = [positions[i + 1][1] - positions[i][1] for i in range(points)]
    return start_x, start_y, offset_x, offset_y


_PATROL_START_X, _PATROL_START_Y, _PATROL_OFFSET_X, _PATROL_OFFSET_Y = make_patrol_path()


def msg_robot_path_plan_info(sim: dict) -> bytes:
    """2.2.16 哨兵轨迹规划（触发式）：49 段相对增量。"""
    p = sim["path_plan"]
    return _concat(pb_uint(1, p["intention"]), pb_uint(2, p["start_x"]),
                   pb_uint(3, p["start_y"]), pb_packed_int32(4, p["offset_x"]),
                   pb_packed_int32(5, p["offset_y"]), pb_uint(6, p["sender_id"]))


def msg_map_click_info(click: dict) -> bytes:
    """2.2.17 地图标记同步（触发式，由 MapClickCmd 原样回推）。"""
    return _concat(pb_uint(1, click["is_send_all"]), pb_bytes(2, click["robot_id"]),
                   pb_uint(3, click["mode"]), pb_uint(4, click["enemy_id"]),
                   pb_uint(5, click["ascii"]), pb_uint(6, click["type"]),
                   pb_float(7, click["map_x"]), pb_float(8, click["map_y"]))


def msg_radar_info_to_client(sim: dict) -> bytes:
    """2.2.19 雷达位置（触发式）：12 个嵌套 RadarSingleRobotInfo。"""
    parts = []
    for x, y, highlight in sim["radar"]:
        sub = _concat(pb_uint(1, x), pb_uint(2, y), pb_uint(3, highlight))
        parts.append(pb_message(1, sub))
    return _concat(*parts)


def msg_custom_byte_block(sim: dict) -> bytes:
    """2.2.20 机器人自定义数据流（50Hz，对应 0x0310，上限 300 字节）。"""
    counter = sim["custom_byte_counter"]
    payload = bytes([counter & 0xFF]) + b"MOCK-CUSTOM-BLOCK" + bytes([(counter >> 8) & 0xFF])
    return _concat(pb_bytes(1, payload))


def msg_robot_performance_selection_sync(sim: dict) -> bytes:
    """2.2.24 性能体系状态同步（1Hz）。"""
    p = sim["performance"]
    return _concat(pb_uint(1, p["shooter"]), pb_uint(2, p["chassis"]),
                   pb_uint(3, p["sentry_control"]))


def msg_rune_status_sync(sim: dict) -> bytes:
    """2.2.27 能量机关状态（1Hz）。"""
    r = sim["rune"]
    return _concat(pb_uint(1, r["status"]), pb_uint(2, r["arms"]),
                   pb_float(3, r["avg_rings"]))


def msg_sentry_status_sync(sim: dict) -> bytes:
    """2.2.28 哨兵姿态（1Hz）。"""
    t = sim["sentry"]
    return _concat(pb_uint(1, t["posture"]), pb_uint(2, 1 if t["weakened"] else 0),
                   pb_uint(3, 1 if t["powered"] else 0))


def msg_dart_select_target_status_sync(sim: dict) -> bytes:
    """2.2.30 飞镖目标选择状态（1Hz）。"""
    d = sim["dart"]
    return _concat(pb_uint(1, d["target"]), pb_uint(2, d["open"]))


def msg_sentry_ctrl_result(command_id: int, result_code: int) -> bytes:
    """2.2.32 哨兵控制指令回执（触发式）。"""
    return _concat(pb_uint(1, command_id), pb_uint(2, result_code))


# ---------------------------------------------------------------------------
# 三、最小 MQTT 3.1.1 broker
# ---------------------------------------------------------------------------


def _read_exact(sock: socket.socket, size: int) -> bytes:
    """从 socket 精确读取 size 字节；对端关闭则抛 ConnectionError。"""
    buf = bytearray()
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("peer closed")
        buf.extend(chunk)
    return bytes(buf)


def _read_remaining_length(sock: socket.socket) -> int:
    """读取 MQTT 可变长度字段（每字节 7 位，最高位为续读标志）。"""
    multiplier = 1
    value = 0
    while True:
        byte = _read_exact(sock, 1)[0]
        value += (byte & 0x7F) * multiplier
        if not byte & 0x80:
            return value
        multiplier *= 128
        if multiplier > 128 ** 3:
            raise ValueError("malformed remaining length")


def _encode_remaining_length(length: int) -> bytes:
    """把长度编码成 MQTT 可变长度字段。"""
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        if length:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


class MiniBroker:
    """够用就好的 MQTT broker：接受连接、回 CONNACK/SUBACK/PINGRESP、并向订阅者推送 PUBLISH。"""

    def __init__(self, host: str, port: int, on_uplink=None):
        self.host = host
        self.port = port
        self._clients = []          # 所有已连接客户端 socket
        self._lock = threading.Lock()
        self._stop = threading.Event()
        # 上行指令回调：收到客户端 PUBLISH 时调用 on_uplink(topic, payload)。
        # 由外层的 Scenario.handle_uplink 实现"裁判系统响应指令"的闭环。
        self.on_uplink = on_uplink

    # ---- 对外接口 ----

    def start(self):
        """启动监听线程（daemon，主线程退出即结束）。"""
        thread = threading.Thread(target=self._accept_loop, daemon=True)
        thread.start()

    def publish(self, topic: str, payload: bytes):
        """向所有已连接客户端推送一条 QoS0 PUBLISH。"""
        topic_bytes = topic.encode("utf-8")
        # MQTT 的 topic 长度是 2 字节大端（不是 varint），这里容易写错。
        body = struct.pack(">H", len(topic_bytes)) + topic_bytes + payload
        packet = b"\x30" + _encode_remaining_length(len(body)) + body
        with self._lock:
            clients = list(self._clients)
        for client in clients:
            try:
                client.sendall(packet)
            except OSError:
                self._drop(client)

    def stop(self):
        """停止 broker 并关闭所有连接。"""
        self._stop.set()

    # ---- 内部实现 ----

    def _drop(self, client: socket.socket):
        """把断开的客户端从列表里移除。"""
        with self._lock:
            if client in self._clients:
                self._clients.remove(client)
        try:
            client.close()
        except OSError:
            pass

    def _accept_loop(self):
        """接受连接；每个连接起一个线程处理 MQTT 控制包。"""
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # Windows 上 SO_REUSEADDR 会允许两个进程同时监听同一端口（先来的连接被谁接走不确定），
        # 必须改用 SO_EXCLUSIVEADDRUSE，端口占用时直接报错，避免"起了两个 broker 互相抢客户端"。
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            server.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        else:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            server.bind((self.host, self.port))
        except OSError as exc:
            print(f"[broker] 端口 {self.host}:{self.port} 绑定失败（{exc}）。"
                  f"可能已有另一个 mock 在跑，请先结束它。")
            raise
        server.listen(4)
        print(f"[broker] 监听 {self.host}:{self.port}，等 RoboGo 连接……")
        while not self._stop.is_set():
            try:
                client, addr = server.accept()
            except OSError:
                break
            print(f"[broker] 客户端接入 {addr[0]}:{addr[1]}")
            with self._lock:
                self._clients.append(client)
            threading.Thread(target=self._serve, args=(client,), daemon=True).start()
        server.close()

    def _serve(self, client: socket.socket):
        """处理单个客户端的控制包，直到断开。"""
        try:
            while not self._stop.is_set():
                header = _read_exact(client, 1)[0]
                packet_type = header >> 4
                length = _read_remaining_length(client)
                body = _read_exact(client, length) if length else b""
                if packet_type == 1:      # CONNECT
                    client.sendall(b"\x20\x02\x00\x00")   # CONNACK 接受
                    print("[broker] CONNECT -> CONNACK")
                elif packet_type == 8:    # SUBSCRIBE
                    packet_id = body[:2]
                    filters = []
                    pos = 2
                    while pos < len(body):
                        topic_len = struct.unpack(">H", body[pos:pos + 2])[0]
                        pos += 2
                        filters.append(body[pos:pos + topic_len].decode("utf-8", "replace"))
                        pos += topic_len + 1   # 跳过 topic 与 QoS 字节
                    suback = b"\x90" + _encode_remaining_length(2 + len(filters)) + packet_id + bytes(len(filters))
                    client.sendall(suback)    # SUBACK 全部授予 QoS0
                    print(f"[broker] SUBSCRIBE {len(filters)} 个主题 -> SUBACK")
                elif packet_type == 12:   # PINGREQ
                    client.sendall(b"\xd0\x00")
                elif packet_type == 3:    # 客户端上行 PUBLISH（9 类控制指令）
                    qos = (header >> 1) & 0x03
                    if len(body) >= 2:
                        topic_len = struct.unpack(">H", body[:2])[0]
                        topic = body[2:2 + topic_len].decode("utf-8", "replace")
                        pos = 2 + topic_len
                        if qos == 1 and len(body) >= pos + 2:
                            packet_id = body[pos:pos + 2]
                            pos += 2
                            client.sendall(b"\x40\x02" + packet_id)   # PUBACK
                        payload = body[pos:]
                        if self.on_uplink is not None and payload:
                            try:
                                self.on_uplink(topic, payload)
                            except Exception as exc:      # 单条指令解析失败不能拖垮 broker
                                print("[broker] 上行指令处理异常 topic=%s: %s" % (topic, exc))
                elif packet_type == 14:   # DISCONNECT
                    break
        except (ConnectionError, OSError, ValueError):
            pass
        finally:
            self._drop(client)


# ---------------------------------------------------------------------------
# 四、模拟数据与场景
# ---------------------------------------------------------------------------


def _new_sim(robot_id: int) -> dict:
    """初始化一份假比赛状态（默认值贴近 2027 规则：基地 10000、前哨 1000）。"""
    is_red = robot_id < 100
    return {
        "robot_id": robot_id,
        "robot_type": 1 if robot_id % 100 == 1 else (7 if robot_id % 100 == 7 else 3),
        "max_health": 400,
        "health": 400,
        "heat": 0.0,
        "ammo": 100,
        "fired": 0,
        "buffer_energy": 60,
        "economy": 1200,
        "economy_total": 2600,
        "base_ally": 10000,
        "base_enemy": 10000,
        "base_ally_status": 0,        # 0 = 拥有 100% 防御增益
        "base_enemy_status": 0,
        "base_ally_ctrl": 1,          # 1 = 己方完全控制
        "base_enemy_ctrl": 2,         # 2 = 己方临时控制对方基地（故意做不对称，方便看控制度拆分）
        "outpost_r": 1000,
        "outpost_b": 1000,
        "outpost_r_status": 1,        # 1 己方完全 / 2 己方临时 / 3 对方临时 / 4 对方完全
        "outpost_b_status": 4,
        "robot_health": [400, 400, 400, 400, 400, 400, 400, 400],   # 己方 1/3/4/7 + 对方 1/3/4/7
        "robot_bullets": [100, 100, 100, 100],
        "damage_ally": 0,
        "damage_enemy": 0,
        "cp_ally": 0,
        "cp_enemy": 0,
        "modular_42mm": 0,
        "core_state": 1,
        "core_putin": 0,
        "core_move": 0,
        "core_rotate": 0,
        "core_remain_all": 0,
        "core_remain_step": 0,
        "countdown": 420,
        "red_score": 0,
        "blue_score": 0,
        "_is_red": is_red,
        # ---- 以下为「全量数据 + 指令闭环」新增的状态 ----
        "tech_level": 0,
        "encryption_level": 0,
        "penalty_count": 0,
        "mechanisms": [],                       # [(机制ID, 剩余秒)]，2.2.6
        "injury": {"total": 0, "collision": 0, "small": 0, "large": 0, "dart": 0,
                   "module": 0, "offline": 0, "penalty": 0, "server": 0, "killer_id": 0},
        "respawn": {"pending": False, "total": 100, "progress": 0,
                    "free": True, "cost": 0, "can_pay": False},
        "position": {"x": 5.0, "y": 12.0, "z": 0.0, "yaw": 90.0},
        "buffs": [],                            # [(robot_id, type, level, max_time, left_time)]
        "path_plan": {"intention": 1, "start_x": _PATROL_START_X, "start_y": _PATROL_START_Y,
                      "offset_x": list(_PATROL_OFFSET_X), "offset_y": list(_PATROL_OFFSET_Y),
                      "sender_id": 7},
        "last_map_click": {"is_send_all": 2, "robot_id": bytes([1, 3, 4, 7, 0, 0, 0]),
                           "mode": 1, "enemy_id": 0, "ascii": 0, "type": 1,
                           "map_x": 12.5, "map_y": 8.0},
        # 12 槽位顺序：对方 1/2/3/4/6/7 + 己方 1/2/3/4/6/7；坐标单位是 dm（场地 280×150）。
        "radar": [(246, 116, 0), (238, 132, 0), (214, 96, 0), (198, 110, 0), (206, 78, 0), (188, 100, 0),
                  (34, 34, 0), (46, 58, 0), (62, 46, 0), (78, 72, 0), (52, 22, 0), (86, 40, 0)],
        "custom_byte_counter": 0,
        "performance": {"shooter": 1, "chassis": 1, "sentry_control": 0},
        "rune": {"status": 1, "arms": 0, "avg_rings": 0.0},
        "sentry": {"posture": 1, "weakened": False, "powered": False},
        "dart": {"target": 1, "open": 0},
        "assembly": {"active": False, "difficulty": 0, "elapsed": 0},
        "pending_messages": [],                 # 触发式消息待发队列 [(topic, bytes)]
        "uplink_log": [],                       # 最近收到的上行指令（最多 50 条）
    }


class Scenario:
    """按固定频率下发假数据，并响应键盘指令。"""

    def __init__(self, broker: MiniBroker, robot_id: int, send_custom_byte: bool = True):
        self.broker = broker
        self.sim = _new_sim(robot_id)
        self._stop = threading.Event()
        self.send_custom_byte = send_custom_byte
        # 上行回调运行在 broker 的客户端线程，主循环运行在场景线程；
        # 待发队列会被两边同时访问，必须加锁，否则 _flush_pending 换列表时会丢消息。
        self._queue_lock = threading.Lock()

    # ---- 主循环 ----

    def run(self):
        """主循环：按协议频率分档下发 —— 50Hz / 10Hz / 5Hz / 1Hz + 触发式队列。"""
        tick = 0
        last_1hz = time.time()
        last_5hz = time.time()
        last_10hz = time.time()
        last_50hz = time.time()
        while not self._stop.is_set():
            now = time.time()
            # 50Hz：机器人自定义数据流（可用 --no-custom-byte 关闭，避免干扰视频解码调试）
            if self.send_custom_byte and now - last_50hz >= 0.02:
                last_50hz = now
                self.sim["custom_byte_counter"] += 1
                self.broker.publish("CustomByteBlock", msg_custom_byte_block(self.sim))
            # 10Hz：机器人实时数据
            if now - last_10hz >= 0.1:
                last_10hz = now
                self.broker.publish("RobotDynamicStatus", msg_robot_dynamic(self.sim))
            # 5Hz：比赛全局状态
            if now - last_5hz >= 0.2:
                last_5hz = now
                self._publish_game_status()
            # 1Hz：其余全部定频下行 + 状态推进
            if now - last_1hz >= 1.0:
                last_1hz = now
                tick += 1
                self._advance_state(tick)
                self._publish_1hz(tick)
            # 触发式消息（上行指令回执、事件、判罚等）
            self._flush_pending()
            time.sleep(0.01)

    def _queue(self, topic: str, payload: bytes):
        """把触发式消息放进待发队列（由主循环统一发送，避免多线程直接碰 socket）。"""
        with self._queue_lock:
            self.sim["pending_messages"].append((topic, payload))

    def _flush_pending(self):
        """发送本帧累积的触发式消息。"""
        with self._queue_lock:
            pending = self.sim["pending_messages"]
            self.sim["pending_messages"] = []
        for topic, payload in pending:
            self.broker.publish(topic, payload)

    def _publish_game_status(self):
        """5Hz：GameStatus。"""
        sim = self.sim
        stage = 4 if sim["countdown"] > 0 else 5
        self.broker.publish("GameStatus",
                            msg_game_status(stage, sim["countdown"], 420 - sim["countdown"],
                                            sim["red_score"], sim["blue_score"]))

    def _publish_1hz(self, tick: int):
        """1Hz：下发协议里所有定频下行消息（补齐原版缺的 14 条）。"""
        sim = self.sim
        publish = self.broker.publish
        publish("GlobalUnitStatus", msg_global_unit_status(sim))
        publish("GlobalLogisticsStatus", msg_global_logistics(sim))
        publish("GlobalSpecialMechanism", msg_global_special_mechanism(sim))
        publish("RobotInjuryStat", msg_robot_injury_stat(sim))
        publish("RobotRespawnStatus", msg_robot_respawn_status(sim))
        publish("RobotStaticStatus", msg_robot_static(sim))
        publish("RobotModuleStatus", msg_robot_module(sim))
        publish("RobotPosition", msg_robot_position(sim))
        publish("TechCoreMotionStateSync", msg_tech_core(sim))
        publish("RobotPerformanceSelectionSync", msg_robot_performance_selection_sync(sim))
        publish("RuneStatusSync", msg_rune_status_sync(sim))
        publish("SentryStatusSync", msg_sentry_status_sync(sim))
        publish("DartSelectTargetStatusSync", msg_dart_select_target_status_sync(sim))
        # Buff：有激活增益时 1Hz 定频，直到全部失效
        for buff in list(sim["buffs"]):
            publish("Buff", msg_buff(buff))
        # 雷达 / 轨迹：协议是触发式，这里按固定间隔模拟触发
        if tick % 5 == 0:
            publish("RadarInfoToClient", msg_radar_info_to_client(sim))
        if tick % 10 == 0:
            publish("RobotPathPlanInfo", msg_robot_path_plan_info(sim))
        # 自动事件：每 25 秒一发飞镖命中，保证事件提示层有东西可看
        if tick % 25 == 5:
            self.trigger_event(9, "2,4")

    def _advance_state(self, tick: int):
        """每秒推进一次比赛状态（数值/倒计时/装配/复活/Buff/机关/位置）。"""
        sim = self.sim
        if sim["countdown"] > 0:
            sim["countdown"] -= 1
        # 控制度：由阵地控制状态实时推算（完全控制的前哨 +1/s、基地 +3/s），
        # 而不是写死 +1/+3，这样客户端把总数拆成"前哨贡献/基地贡献"时才对得上。
        ally_rate, enemy_rate = cp_rates(sim)
        sim["cp_ally"] += ally_rate
        sim["cp_enemy"] += enemy_rate
        sim["damage_ally"] += random.randint(60, 180)
        sim["damage_enemy"] += random.randint(40, 150)
        sim["heat"] = 0.0 if tick % 6 == 0 else min(400.0, sim["heat"] + 35.0)
        sim["fired"] += random.randint(0, 3)
        sim["ammo"] = max(0, 100 - sim["fired"] // 3)
        sim["robot_health"][0] = sim["health"]
        # 位置缓慢移动，让 RobotPosition / 小地图有变化
        sim["position"]["x"] = 5.0 + (tick % 20) * 0.8
        sim["position"]["y"] = 12.0 + (tick % 15) * 0.6
        sim["position"]["yaw"] = float((tick * 7) % 360)
        # 复活读条推进
        r = sim["respawn"]
        if r["pending"] and r["progress"] < r["total"]:
            r["progress"] = min(r["total"], r["progress"] + 10)
        # Buff 剩余时间递减，过期移除
        alive = []
        for robot_id, buff_type, level, max_time, left_time in sim["buffs"]:
            if left_time > 1:
                alive.append((robot_id, buff_type, level, max_time, left_time - 1))
        sim["buffs"] = alive
        # 全局特殊机制计时递减
        sim["mechanisms"] = [(mid, sec - 1) for mid, sec in sim["mechanisms"] if sec > 1]
        # 装配流程：开始兑换后逐步完成 放入 -> 平移 -> 旋转，最后核心到位
        asm = sim["assembly"]
        if asm["active"]:
            asm["elapsed"] += 1
            e = asm["elapsed"]
            sim["core_state"] = 2 if e < 4 else 3
            sim["core_putin"] = 1 if e >= 1 else 0
            sim["core_move"] = 1 if e >= 2 else 0
            sim["core_rotate"] = 1 if e >= 3 else 0
            sim["core_remain_all"] = max(0, 45 - e)
            sim["core_remain_step"] = max(0, 12 - (e % 12))
        # 能量机关：激活中(2) 若干秒后变已激活(3)
        rune = sim["rune"]
        if rune["status"] == 2:
            rune["arms"] = min(10, rune["arms"] + 2)
            rune["avg_rings"] = round(min(10.0, rune["avg_rings"] + 1.7), 1)
            if rune["arms"] >= 10:
                rune["status"] = 3
                self._queue("Event", msg_event(3, "10,%.1f" % rune["avg_rings"]))
                self._queue("Event", msg_event(4, "2"))

    # ---- 上行指令闭环：模拟裁判系统接收指令并改变比赛状态 ----

    def handle_uplink(self, topic: str, payload: bytes):
        """裁判系统侧入口：解析一条客户端上行 PUBLISH，改变状态并回推下行/回执。"""
        sim = self.sim
        try:
            fields = parse_fields(payload)
        except ValueError as exc:
            print("[uplink] %s 解析失败: %s" % (topic, exc))
            return
        sim["uplink_log"].append((time.strftime("%H:%M:%S"), topic, len(payload)))
        if len(sim["uplink_log"]) > 50:
            sim["uplink_log"].pop(0)
        handler = {
            "KeyboardMouseControl": self._on_keyboard_mouse,
            "CustomControl": self._on_custom_control,
            "MapClickCmd": self._on_map_click,
            "AssemblyCommand": self._on_assembly,
            "RobotPerformanceSelectionCommand": self._on_performance,
            "CommonCommand": self._on_common,
            "RuneActivateCommand": self._on_rune,
            "DartCommand": self._on_dart,
            "SentryCtrlCommand": self._on_sentry_ctrl,
        }.get(topic)
        if handler is None:
            return
        try:
            handler(fields)
        except (ValueError, KeyError, IndexError) as exc:
            print("[uplink] %s 处理出错: %s" % (topic, exc))

    def _reply_state(self, *topics):
        """指令生效后立刻回推一批下行消息，让客户端马上看到结果。"""
        builders = {
            "GlobalLogisticsStatus": msg_global_logistics,
            "GlobalUnitStatus": msg_global_unit_status,
            "RobotDynamicStatus": msg_robot_dynamic,
            "RobotStaticStatus": msg_robot_static,
            "RobotModuleStatus": msg_robot_module,
            "RobotRespawnStatus": msg_robot_respawn_status,
            "TechCoreMotionStateSync": msg_tech_core,
            "RobotPerformanceSelectionSync": msg_robot_performance_selection_sync,
            "RuneStatusSync": msg_rune_status_sync,
            "SentryStatusSync": msg_sentry_status_sync,
            "DartSelectTargetStatusSync": msg_dart_select_target_status_sync,
        }
        for topic in topics:
            self._queue(topic, builders[topic](self.sim))

    def _on_keyboard_mouse(self, fields):
        """75Hz 键鼠：裁判系统只做转发，这里仅统计，不改变比赛状态。"""
        self.sim["_km_count"] = self.sim.get("_km_count", 0) + 1

    def _on_custom_control(self, fields):
        """75Hz 自定义数据：仅统计长度。"""
        self.sim["_cc_bytes"] = self.sim.get("_cc_bytes", 0) + len(field_b(fields, 1))

    def _on_map_click(self, fields):
        """MapClickCmd -> 原样回推 MapClickInfo（协议 2.2.17）。"""
        click = {
            "is_send_all": field_u(fields, 1),
            "robot_id": field_b(fields, 2, b"\x00" * 7),
            "mode": field_u(fields, 3, 1),
            "enemy_id": field_u(fields, 4),
            "ascii": field_u(fields, 5),
            "type": field_u(fields, 6, 1),
            "map_x": field_f(fields, 7),
            "map_y": field_f(fields, 8),
        }
        self.sim["last_map_click"] = click
        self._queue("MapClickInfo", msg_map_click_info(click))
        print("[uplink] MapClickCmd -> MapClickInfo mode=%s pos=(%.1f,%.1f)"
              % (click["mode"], click["map_x"], click["map_y"]))

    def _on_assembly(self, fields):
        """AssemblyCommand：0 开始兑换 / 1 确认装配 / 2 取消装配。"""
        operation = field_u(fields, 1)
        difficulty = field_u(fields, 2)
        asm = self.sim["assembly"]
        if operation == 0:      # 开始兑换：启动装配流程，核心开始运动
            asm.update({"active": True, "difficulty": difficulty, "elapsed": 0})
            self.sim["core_state"] = 2
            self.sim["core_putin"] = self.sim["core_move"] = self.sim["core_rotate"] = 0
            self.sim["core_remain_all"] = 45
            self.sim["core_remain_step"] = 12
            print("[uplink] 装配开始兑换 难度=%u（重装进入无敌/断电流程）" % difficulty)
        elif operation == 1:    # 确认装配：完成，授予模块化 42mm 权限
            asm["active"] = False
            self.sim["core_state"] = 1
            self.sim["core_putin"] = self.sim["core_move"] = self.sim["core_rotate"] = 0
            self.sim["core_remain_all"] = self.sim["core_remain_step"] = 0
            self.sim["modular_42mm"] = 1
            self.sim["tech_level"] = max(self.sim["tech_level"], asm["difficulty"])
            self._queue("Event", msg_event(15, "0"))   # 装配成功
            self._queue("Event", msg_event(16, ""))    # 成功安装模块化 42mm
            print("[uplink] 装配确认完成 难度=%u -> 模块化42mm权限=1" % asm["difficulty"])
        elif operation == 2:    # 取消装配：回到初始位
            asm["active"] = False
            self.sim["core_state"] = 1
            self.sim["core_putin"] = self.sim["core_move"] = self.sim["core_rotate"] = 0
            self.sim["core_remain_all"] = self.sim["core_remain_step"] = 0
            self._queue("Event", msg_event(15, "6"))   # 主动退出
            print("[uplink] 装配已取消")
        self._reply_state("TechCoreMotionStateSync", "RobotModuleStatus", "GlobalLogisticsStatus")

    def _on_performance(self, fields):
        """RobotPerformanceSelectionCommand -> 同步性能体系。"""
        p = self.sim["performance"]
        p["shooter"] = field_u(fields, 1, p["shooter"])
        p["chassis"] = field_u(fields, 2, p["chassis"])
        p["sentry_control"] = field_u(fields, 3, p["sentry_control"])
        self._reply_state("RobotPerformanceSelectionSync", "RobotStaticStatus")
        print("[uplink] 性能体系 shooter=%u chassis=%u sentry=%u"
              % (p["shooter"], p["chassis"], p["sentry_control"]))

    def _on_common(self, fields):
        """CommonCommand：兑换发弹量 / 复活 / 远程补弹 / 远程回血。"""
        cmd = field_u(fields, 1)
        param = field_u(fields, 2)
        sim = self.sim
        # 简化价格表（仅用于演示闭环，不对应真实比赛经济数值）
        if cmd == 1:                       # 兑换 17mm，param 必须是 10 的倍数
            if param % 10 != 0:
                print("[uplink] 兑换17mm被拒：param=%u 不是10的倍数" % param)
                return
            price = param // 10 * 20
            if sim["economy"] < price:
                print("[uplink] 兑换17mm被拒：金币不足")
                return
            sim["economy"] -= price
            sim["ammo"] += param
            sim["robot_bullets"][0] += param
            print("[uplink] 兑换17mm +%u 发，-%u 金币" % (param, price))
        elif cmd == 2:                     # 兑换 42mm
            price = param * 30
            if sim["economy"] < price:
                print("[uplink] 兑换42mm被拒：金币不足")
                return
            sim["economy"] -= price
            sim["ammo"] += param
            print("[uplink] 兑换42mm +%u 发，-%u 金币" % (param, price))
        elif cmd == 3:                     # 确认复活
            r = sim["respawn"]
            if r["pending"] and r["progress"] >= r["total"]:
                r["pending"] = False
                r["progress"] = 0
                sim["health"] = sim["max_health"]
                print("[uplink] 确认复活 -> 已复活")
            else:
                print("[uplink] 确认复活被拒：读条未完成")
        elif cmd == 4:                     # 兑换立即复活
            sim["health"] = sim["max_health"]
            sim["respawn"]["pending"] = False
            sim["respawn"]["progress"] = 0
            sim["economy"] = max(0, sim["economy"] - 300)
            print("[uplink] 立即复活 -300 金币")
        elif cmd == 5:                     # 远程补弹
            sim["ammo"] += 100
            sim["economy"] = max(0, sim["economy"] - 100)
            print("[uplink] 远程补弹 +100 发 -100 金币")
        elif cmd == 6:                     # 远程回血
            sim["health"] = sim["max_health"]
            sim["economy"] = max(0, sim["economy"] - 200)
            print("[uplink] 远程回血 -200 金币")
        else:
            print("[uplink] 未知 CommonCommand cmd_type=%u" % cmd)
            return
        self._reply_state("GlobalLogisticsStatus", "RobotDynamicStatus", "GlobalUnitStatus",
                          "RobotRespawnStatus")

    def _on_rune(self, fields):
        """RuneActivateCommand -> 能量机关进入激活中。"""
        if field_u(fields, 1) == 1:
            self.sim["rune"].update({"status": 2, "arms": 0, "avg_rings": 0.0})
            self._reply_state("RuneStatusSync")
            print("[uplink] 能量机关开始激活")

    def _on_dart(self, fields):
        """DartCommand：切换目标/闸门；确认发射 -> 飞镖命中事件 + 扣血。"""
        target = field_u(fields, 1, 1)
        open_flag = field_u(fields, 2)
        launch = field_u(fields, 3)
        sim = self.sim
        sim["dart"]["target"] = target
        sim["dart"]["open"] = 2 if open_flag else 0
        if launch:
            hitter = 1 if sim["_is_red"] else 2
            self.trigger_event(9, "%d,%d" % (hitter, target))
            if target in (4, 5):
                # 规则：命中随机移动/末端移动目标 -> 对方全部存活地面机器人扣 20%/50% 上限血
                percent = 0.2 if target == 4 else 0.5
                for i in range(4, 8):
                    sim["robot_health"][i] = max(1, int(sim["robot_health"][i] * (1 - percent)))
                print("[uplink] 飞镖命中目标%u -> 对方地面机器人扣 %u%% 上限血"
                      % (target, int(percent * 100)))
            if target >= 2:
                sim["base_enemy"] = max(0, sim["base_enemy"] - 500)
            print("[uplink] 飞镖发射确认 目标=%u" % target)
        else:
            print("[uplink] 飞镖目标=%u 闸门=%s" % (target, "开" if open_flag else "关"))
        self._reply_state("DartSelectTargetStatusSync", "GlobalUnitStatus")

    def _on_sentry_ctrl(self, fields):
        """SentryCtrlCommand -> 改变哨兵姿态并回 SentryCtrlResult。"""
        cmd = field_u(fields, 1)
        t = self.sim["sentry"]
        posture_map = {7: 1, 8: 2, 9: 3, 10: 1, 11: 2, 12: 3}
        if cmd in posture_map:
            t["posture"] = posture_map[cmd]
        t["powered"] = cmd in (10, 11, 12)
        if cmd in (1, 3):      # 补弹
            self.sim["ammo"] += 50
        elif cmd in (4, 6):    # 回血 / 复活
            self.sim["health"] = self.sim["max_health"]
        self._queue("SentryCtrlResult", msg_sentry_ctrl_result(cmd, 0))   # 0 = 成功
        self._reply_state("SentryStatusSync", "RobotDynamicStatus")
        print("[uplink] 哨兵指令 %u -> 回执成功 姿态=%u 强化=%s"
              % (cmd, t["posture"], t["powered"]))

    # ---- 交互动作 ----

    def damage_base(self, amount: int, ally: bool):
        """基地扣血：用于观察 2027 的 λ 降采样角标与血条比例。"""
        key = "base_ally" if ally else "base_enemy"
        status_key = "base_ally_status" if ally else "base_enemy_status"
        self.sim[key] = max(0, self.sim[key] - amount)
        # 基地状态：掉血超过 1000 后置为"护甲展开(2)"，用来验证角标从"盾"变成"护甲"。
        self.sim[status_key] = 0 if 10000 - self.sim[key] < 1000 else 2
        # λ 只用己方基地计算（规则 5.6.1：己方后勤系数），所以这里始终打印己方 λ 供对照。
        lost_ally = 10000 - self.sim["base_ally"]
        print(f"[sim] {'己方' if ally else '对方'}基地 -{amount} -> {self.sim[key]}"
              f"（己方 λ={1 - lost_ally / 20000:.2f}）")

    def trigger_event(self, event_id: int, param: str = ""):
        """发一条 Event。"""
        self.broker.publish("Event", msg_event(event_id, param))
        print(f"[sim] Event {event_id} param={param!r}")

    def cycle_outpost_status(self):
        """轮换前哨阵地控制状态，观察角标从绿到红。"""
        self.sim["outpost_r_status"] = self.sim["outpost_r_status"] % 4 + 1
        self.sim["outpost_b_status"] = self.sim["outpost_b_status"] % 4 + 1
        print(f"[sim] 阵地控制状态 R={self.sim['outpost_r_status']} B={self.sim['outpost_b_status']}")

    def penalty(self, penalty_type: int, effect_sec: int):
        """发一条判罚，观察顶部横幅与倒计时。"""
        self.sim["penalty_count"] += 1
        self.broker.publish("PenaltyInfo",
                            msg_penalty(penalty_type, effect_sec, self.sim["penalty_count"]))
        print(f"[sim] 判罚 type={penalty_type} {effect_sec}s 累计={self.sim['penalty_count']}")

    def add_cp(self, amount: int):
        """手动加控制度。"""
        self.sim["cp_ally"] += amount
        print(f"[sim] 己方控制度 -> {self.sim['cp_ally']}")

    # ---- 新增的模拟动作（覆盖全量下行与上行闭环）----

    def add_buff(self, buff_type: int = 1):
        """给本机加一个 Buff，并立即触发一次 Buff 消息。"""
        buff = (self.sim["robot_id"], buff_type, 50, 30, 30)
        self.sim["buffs"] = [b for b in self.sim["buffs"] if b[1] != buff_type]
        self.sim["buffs"].append(buff)
        self.broker.publish("Buff", msg_buff(buff))
        print("[sim] Buff type=%u level=50 持续30s" % buff_type)

    def toggle_respawn(self):
        """切换"战亡待复活"状态，验证复活读条与远程复活指令。"""
        r = self.sim["respawn"]
        r["pending"] = not r["pending"]
        r["progress"] = 0
        self.sim["health"] = 0 if r["pending"] else self.sim["max_health"]
        self._reply_state("RobotRespawnStatus", "RobotDynamicStatus")
        print("[sim] 待复活=%s" % r["pending"])

    def trigger_mechanism(self, mechanism_id: int = 1):
        """触发一个全局特殊机制（堡垒占领计时）。"""
        self.sim["mechanisms"] = [(mid, sec) for mid, sec in self.sim["mechanisms"] if mid != mechanism_id]
        self.sim["mechanisms"].append((mechanism_id, 30))
        self._queue("GlobalSpecialMechanism", msg_global_special_mechanism(self.sim))
        print("[sim] 全局特殊机制 %u 启动 30s" % mechanism_id)

    def activate_rune(self):
        """触发能量机关激活流程。"""
        self._on_rune(parse_fields(pb_uint(1, 1)))

    def cycle_sentry_posture(self):
        """轮换哨兵姿态 进攻->防御->移动。"""
        t = self.sim["sentry"]
        t["posture"] = t["posture"] % 3 + 1
        self._reply_state("SentryStatusSync")
        print("[sim] 哨兵姿态 -> %u" % t["posture"])

    def cycle_dart_gate(self):
        """轮换飞镖闸门 关闭(0)->已开启(2)。"""
        d = self.sim["dart"]
        d["open"] = 2 if d["open"] != 2 else 0
        self._reply_state("DartSelectTargetStatusSync")
        print("[sim] 飞镖闸门 -> %u" % d["open"])

    def send_map_click(self):
        """主动下发一条地图标记（MapClickInfo）。"""
        self._queue("MapClickInfo", msg_map_click_info(self.sim["last_map_click"]))
        print("[sim] 地图标记已下发")

    def cycle_event(self):
        """轮换 Event 1~16，便于逐个观察事件提示层的文案与配色。"""
        eid = self.sim.get("_event_cursor", 0) % 16 + 1
        self.sim["_event_cursor"] = eid
        params = {1: "1,101", 2: "11", 3: "10,9.6", 4: "2", 5: "2", 6: "3",
                  9: "2,4", 10: "", 11: "", 12: "", 13: "", 14: "", 15: "0", 16: ""}
        self.trigger_event(eid, params.get(eid, ""))

    def show_status(self):
        """打印当前模拟裁判系统的关键状态。"""
        sim = self.sim
        print("--- 模拟裁判系统状态 ---")
        print("  机器人=%s 阵营=%s 阶段倒计时=%ss" %
              (sim["robot_id"], "红" if sim["_is_red"] else "蓝", sim["countdown"]))
        print("  己方基地=%s 对方基地=%s 前哨R/B=%s/%s" %
              (sim["base_ally"], sim["base_enemy"], sim["outpost_r"], sim["outpost_b"]))
        print("  金币=%s 弹药=%s 血量=%s 科技等级=%s 模块化42mm=%s" %
              (sim["economy"], sim["ammo"], sim["health"], sim["tech_level"], sim["modular_42mm"]))
        print("  控制度 %s:%s  总伤害 %s:%s" %
              (sim["cp_ally"], sim["cp_enemy"], sim["damage_ally"], sim["damage_enemy"]))
        print("  装配中=%s 核心状态=%s 哨兵姿态=%s 飞镖闸门=%s 能量机关=%s" %
              (sim["assembly"]["active"], sim["core_state"], sim["sentry"]["posture"],
               sim["dart"]["open"], sim["rune"]["status"]))
        print("  上行指令累计=%s 条" % len(sim["uplink_log"]))

    def show_uplink_log(self):
        """打印最近收到的上行指令。"""
        log = self.sim["uplink_log"]
        if not log:
            print("[sim] 尚未收到任何上行指令（启动 RoboGo 并点按钮，或按数字键 1~9 模拟）")
            return
        print("--- 最近上行指令（最多 50 条）---")
        for ts, topic, size in log[-15:]:
            print("  %s  %-36s %d bytes" % (ts, topic, size))

    def simulate_uplink(self, index: int):
        """手动模拟一条上行指令：没有客户端时也能验证裁判系统的响应闭环。"""
        sim = self.sim
        samples = {
            1: ("KeyboardMouseControl", _concat(pb_uint(1, 0), pb_uint(6, 0))),
            2: ("CustomControl", _concat(pb_bytes(1, b"MOCK-UPLINK"))),
            3: ("MapClickCmd", _concat(pb_uint(1, 2), pb_bytes(2, bytes([1, 3, 4, 7, 0, 0, 0])),
                                      pb_uint(3, 1), pb_uint(6, 1),
                                      pb_float(7, 12.5), pb_float(8, 8.0))),
            4: ("AssemblyCommand", _concat(pb_uint(1, 0), pb_uint(2, 2))),
            5: ("RobotPerformanceSelectionCommand", _concat(pb_uint(1, 2), pb_uint(2, 2), pb_uint(3, 1))),
            6: ("CommonCommand", _concat(pb_uint(1, 1), pb_uint(2, 50))),
            7: ("RuneActivateCommand", _concat(pb_uint(1, 1))),
            8: ("DartCommand", _concat(pb_uint(1, 4), pb_uint(2, 1), pb_uint(3, 1))),
            9: ("SentryCtrlCommand", _concat(pb_uint(1, 7))),
        }
        item = samples.get(index)
        if item is None:
            return
        topic, payload = item
        print("[sim] 模拟上行 %d -> %s" % (index, topic))
        self.handle_uplink(topic, payload)

    def stop(self):
        """停止场景循环。"""
        self._stop.set()


def _keyboard_loop(scenario: Scenario):
    """读标准输入，把按键映射成模拟动作。"""
    print(__doc__)
    for line in sys.stdin:
        key = line.strip()[:1].lower()
        if key == "b":
            scenario.damage_base(1000, ally=True)
        elif key == "n":
            scenario.damage_base(1000, ally=False)
        elif key == "d":
            scenario.trigger_event(9, "2,4")
        elif key == "k":
            scenario.trigger_event(1, "1,101")
        elif key == "a":
            scenario.trigger_event(15, "0")
        elif key == "s":
            scenario.cycle_outpost_status()
        elif key == "p":
            scenario.penalty(1, 30)
        elif key == "r":
            scenario.penalty(3, 60)
        elif key == "o":
            scenario.penalty(5, 15)
        elif key == "c":
            scenario.add_cp(300)
        elif key == "e":
            scenario.cycle_event()
        elif key == "u":
            scenario.add_buff()
        elif key == "g":
            # 补给区 Buff(type=6)：客户端"兑换允许发弹量"按钮要求本车处于该 Buff 内，不加这条按钮永远是灰的。
            scenario.add_buff(6)
        elif key == "j":
            scenario.toggle_respawn()
        elif key == "v":
            scenario.trigger_mechanism()
        elif key == "w":
            scenario.activate_rune()
        elif key == "x":
            scenario.cycle_sentry_posture()
        elif key == "z":
            scenario.cycle_dart_gate()
        elif key == "m":
            scenario.send_map_click()
        elif key == "h":
            scenario.show_status()
        elif key == "l":
            scenario.show_uplink_log()
        elif key.isdigit() and key != "0":
            scenario.simulate_uplink(int(key))
        elif key == "q":
            scenario.stop()
            return


# ---------------------------------------------------------------------------
# 五、自检：起 broker + 内置客户端，确认握手与推送都正常
# ---------------------------------------------------------------------------


def _selftest(host: str, port: int) -> int:
    """不依赖 RoboGo，验证 broker 能完成 CONNECT/SUBSCRIBE 并成功推送 PUBLISH。"""
    broker = MiniBroker(host, port)
    broker.start()
    time.sleep(0.3)

    received = []
    try:
        client = socket.create_connection((host, port), timeout=3)
        # CONNECT：client_id = "selftest"
        cid = b"selftest"
        variable_header = b"\x00\x04MQTT\x04\x02\x00\x3c"   # 协议名/级别/标志/keepalive
        payload = struct.pack(">H", len(cid)) + cid
        body = variable_header + payload
        client.sendall(b"\x10" + _encode_remaining_length(len(body)) + body)
        connack = _read_exact(client, 4)
        if connack != b"\x20\x02\x00\x00":
            print("[selftest] CONNACK 异常:", connack.hex())
            return 1
        # SUBSCRIBE：GameStatus + GlobalUnitStatus
        topics = [b"GameStatus", b"GlobalUnitStatus"]
        sub_body = b"\x00\x01" + b"".join(struct.pack(">H", len(t)) + t + b"\x00" for t in topics)
        client.sendall(b"\x82" + _encode_remaining_length(len(sub_body)) + sub_body)
        suback_header = _read_exact(client, 1)[0]          # 固定头首字节应为 0x90
        suback_length = _read_remaining_length(client)      # 后续长度
        suback_body = _read_exact(client, suback_length)    # 包标识 + 各主题授予的 QoS
        if suback_header != 0x90:
            print("[selftest] SUBACK 异常:", hex(suback_header))
            return 1
        # 推送两条消息，确认能收到并解析主题
        sim = _new_sim(3)
        broker.publish("GameStatus", msg_game_status(4, 300, 120, 1, 0))
        broker.publish("GlobalUnitStatus", msg_global_unit_status(sim))
        client.settimeout(3)
        for _ in range(2):
            header = _read_exact(client, 1)[0]
            length = _read_remaining_length(client)
            body = _read_exact(client, length)
            topic_len = struct.unpack(">H", body[:2])[0]
            topic = body[2:2 + topic_len].decode()
            received.append((topic, len(body) - 2 - topic_len))
        print("[selftest] 收到:", received)
        ok = [name for name, _ in received] == ["GameStatus", "GlobalUnitStatus"]
        if not ok:
            print("[selftest] 主题不符")
            return 1

        # ---- 全量下行消息编码自检：23 条都要能构造、且能被手写解码器读回 ----
        sim2 = _new_sim(3)
        click = sim2["last_map_click"]
        samples = [
            ("GameStatus", msg_game_status(4, 300, 120, 1, 0)),
            ("GlobalUnitStatus", msg_global_unit_status(sim2)),
            ("GlobalLogisticsStatus", msg_global_logistics(sim2)),
            ("GlobalSpecialMechanism", msg_global_special_mechanism(sim2)),
            ("Event", msg_event(9, "2,4")),
            ("RobotInjuryStat", msg_robot_injury_stat(sim2)),
            ("RobotRespawnStatus", msg_robot_respawn_status(sim2)),
            ("RobotStaticStatus", msg_robot_static(sim2)),
            ("RobotDynamicStatus", msg_robot_dynamic(sim2)),
            ("RobotModuleStatus", msg_robot_module(sim2)),
            ("RobotPosition", msg_robot_position(sim2)),
            ("Buff", msg_buff((3, 1, 50, 30, 30))),
            ("PenaltyInfo", msg_penalty(1, 30, 1)),
            ("RobotPathPlanInfo", msg_robot_path_plan_info(sim2)),
            ("MapClickInfo", msg_map_click_info(click)),
            ("RadarInfoToClient", msg_radar_info_to_client(sim2)),
            ("CustomByteBlock", msg_custom_byte_block(sim2)),
            ("TechCoreMotionStateSync", msg_tech_core(sim2)),
            ("RobotPerformanceSelectionSync", msg_robot_performance_selection_sync(sim2)),
            ("RuneStatusSync", msg_rune_status_sync(sim2)),
            ("SentryStatusSync", msg_sentry_status_sync(sim2)),
            ("DartSelectTargetStatusSync", msg_dart_select_target_status_sync(sim2)),
            ("SentryCtrlResult", msg_sentry_ctrl_result(7, 0)),
        ]
        for topic, data in samples:
            if not data:
                print("[selftest] %s 编码为空" % topic)
                return 1
            try:
                if not parse_fields(data):
                    print("[selftest] %s 解码无字段" % topic)
                    return 1
            except ValueError as exc:
                print("[selftest] %s 解码失败: %s" % (topic, exc))
                return 1
        print("[selftest] 全量下行消息 %d/%d 条编码+解码通过" % (len(samples), 23))

        # ---- 上行指令闭环自检 ----
        scenario = Scenario(broker, 3, send_custom_byte=False)
        broker.on_uplink = scenario.handle_uplink
        gold_before = scenario.sim["economy"]
        scenario.handle_uplink("CommonCommand", _concat(pb_uint(1, 1), pb_uint(2, 50)))
        if scenario.sim["economy"] >= gold_before or scenario.sim["ammo"] <= 100:
            print("[selftest] CommonCommand 闭环失败（金币/弹药未变化）")
            return 1
        scenario.handle_uplink("AssemblyCommand", _concat(pb_uint(1, 1), pb_uint(2, 2)))
        if scenario.sim["modular_42mm"] != 1:
            print("[selftest] AssemblyCommand 闭环失败（未授予模块化42mm权限）")
            return 1
        scenario.handle_uplink("SentryCtrlCommand", _concat(pb_uint(1, 7)))
        if not any(t == "SentryCtrlResult" for t, _ in scenario.sim["pending_messages"]):
            print("[selftest] SentryCtrlCommand 未产生回执")
            return 1
        print("[selftest] 上行指令闭环通过（兑换/装配/哨兵回执）")

        print("[selftest] 全部通过")
        return 0
    finally:
        broker.stop()


def main() -> int:
    # 日志重定向到文件时也逐行刷新，方便实时观察上行指令与模拟动作。
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass
    parser = argparse.ArgumentParser(description="RoboGo 2027 本地假数据源（内置最小 MQTT broker）")
    parser.add_argument("--host", default="127.0.0.1", help="监听地址，默认 127.0.0.1")
    parser.add_argument("--port", type=int, default=3333, help="监听端口，默认 3333（与客户端默认一致）")
    parser.add_argument("--robot-id", type=int, default=3, help="伪装的机器人编号，<100 红方、>=100 蓝方，默认 3")
    parser.add_argument("--selftest", action="store_true", help="只做 broker 自检，不进入模拟循环")
    parser.add_argument("--no-custom-byte", action="store_true",
                        help="关闭 50Hz 的 CustomByteBlock（调试视频解码时可减少干扰）")
    args = parser.parse_args()

    if args.selftest:
        return _selftest(args.host, args.port)

    broker = MiniBroker(args.host, args.port)
    scenario = Scenario(broker, args.robot_id, send_custom_byte=not args.no_custom_byte)
    broker.on_uplink = scenario.handle_uplink     # 裁判系统响应上行指令的闭环入口
    broker.start()
    threading.Thread(target=scenario.run, daemon=True).start()
    try:
        _keyboard_loop(scenario)
    except KeyboardInterrupt:
        pass
    finally:
        scenario.stop()
        broker.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
