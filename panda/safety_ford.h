#pragma once

#include "safety_declarations.h"

// --- System Forward Declarations ---
void can_send(CANPacket_t *to_send, uint8_t bus_number, bool skip_tx_hook);

// --- Ford Specific Constants ---
const struct lookup_t FORD_LOOKUP_ANGLE_RATE_UP = {{2.0, 17.0, 32.0}, {7.5, 1.0, 0.1}};
const struct lookup_t FORD_LOOKUP_ANGLE_RATE_DOWN = {{2.0, 17.0, 32.0}, {7.5, 2.0, 0.2}};
const int FORD_DEG_TO_CAN = 10;

#define FORD_EngBrakeData          0x165
#define FORD_BrakeSysFeatures      0x415
#define FORD_EngVehicleSpThrottle2 0x202
#define FORD_Yaw_Data_FD1          0x91
#define FORD_LateralMotionControl  0x3D3
#define FORD_MAIN_BUS 0U
#define FORD_CAM_BUS  2U

// --- Local Helpers ---
static bool ford_max_limit_check(int val, int upper, int lower) {
  return (val > upper) || (val < lower);
}

static float ford_interpolate(struct lookup_t lookup, float x) {
  float ret = lookup.y[0];
  int n = 3; 
  if (x <= lookup.x[0]) {
    ret = lookup.y[0];
  } else if (x >= lookup.x[n-1]) {
    ret = lookup.y[n-1];
  } else {
    for (int i=0; i < (n-1); i++) {
      if (x < lookup.x[i+1]) {
        float x0 = lookup.x[i];
        float y0 = lookup.y[i];
        float dx = lookup.x[i+1] - x0;
        float dy = lookup.y[i+1] - y0;
        ret = y0 + (dy * (x - x0) / dx);
        break;
      }
    }
  }
  return ret;
}

static uint8_t ford_checksum_simple(uint8_t cnt) {
  return (uint8_t)(255U - cnt - 3U);
}

// --- Hooks ---
static void ford_rx_hook(const CANPacket_t *to_push) {
  int bus = GET_BUS(to_push);
  int addr = GET_ADDR(to_push);

  if (bus == (int)FORD_MAIN_BUS) {
    if (addr == FORD_BrakeSysFeatures) {
      float speed = (float)((GET_BYTE(to_push, 0) << 8) | GET_BYTE(to_push, 1)) * 0.01f / 3.6f;
      // CRITICAL: Ensure vehicle_speed is initialized in init()!
      update_sample(&vehicle_speed, (int)(speed * VEHICLE_SPEED_FACTOR));

      if (controls_allowed) {
        CANPacket_t to_send = *to_push;
        uint8_t cnt = (GET_BYTE(to_push, 2) & 0x3CU) >> 2U;
        to_send.data[0] = 0x00;
        to_send.data[1] = 0x00;
        to_send.data[3] = ford_checksum_simple(cnt);
        can_send(&to_send, FORD_CAM_BUS, true);
      }
    }

    if ((addr == FORD_EngVehicleSpThrottle2) && controls_allowed) {
      CANPacket_t to_send = *to_push;
      uint8_t cnt = (GET_BYTE(to_push, 2) & 0x78U) >> 3U;
      to_send.data[1] = ford_checksum_simple(cnt);
      to_send.data[6] = 0x00;
      to_send.data[7] = 0x00;
      can_send(&to_send, FORD_CAM_BUS, true);
    }
    
    if (addr == FORD_EngBrakeData) {
      int cruise_state = (GET_BYTE(to_push, 1) & 0x7);
      bool cruise_engaged = ((cruise_state == 4) || (cruise_state == 5));
      if (cruise_engaged && !cruise_engaged_prev) { controls_allowed = true; }
      if (!cruise_engaged) { controls_allowed = false; }
      cruise_engaged_prev = cruise_engaged;
    }
  }
}

static bool ford_tx_hook(const CANPacket_t *to_send) {
  bool tx = true;
  int addr = GET_ADDR(to_send);
  bool violation = false;

  if (relay_malfunction) { tx = false; }

  if (addr == 0x3A8 || addr == FORD_LateralMotionControl) {
    int raw_angle_can = (((GET_BYTE(to_send, 2) & 0x7F) << 8) | GET_BYTE(to_send, 3));
    int desired_angle = raw_angle_can - 10000;
    bool steer_enabled = (GET_BYTE(to_send, 2) >> 7) & 1U;

    if (controls_allowed && steer_enabled) {
      float current_speed = (float)vehicle_speed.values[0] / (float)VEHICLE_SPEED_FACTOR;
      float up = ford_interpolate(FORD_LOOKUP_ANGLE_RATE_UP, current_speed) * (float)FORD_DEG_TO_CAN;
      float down = ford_interpolate(FORD_LOOKUP_ANGLE_RATE_DOWN, current_speed) * (float)FORD_DEG_TO_CAN;
      int delta_up = (int)up + 1;
      int delta_down = (int)down + 1;
      int highest = desired_angle_last + ((desired_angle_last > 0) ? delta_up : delta_down);
      int lowest = desired_angle_last - ((desired_angle_last >= 0) ? delta_down : delta_up);
      violation |= ford_max_limit_check(desired_angle, highest, lowest);
    }
    desired_angle_last = desired_angle;
    if (!controls_allowed && steer_enabled) { violation = true; }
  }

  if (violation) { tx = false; controls_allowed = false; }
  return tx;
}

static int ford_fwd_hook(int bus_num, int addr) {
  int bus_fwd = -1;
  if (!relay_malfunction) {
    if (bus_num == (int)FORD_MAIN_BUS) {
      bool is_speed_msg = (addr == FORD_BrakeSysFeatures) || (addr == FORD_EngVehicleSpThrottle2);
      if (!(is_speed_msg && controls_allowed) && (addr != 0x3A8) && (addr != FORD_LateralMotionControl)) {
        bus_fwd = (int)FORD_CAM_BUS;
      }
    } else if (bus_num == (int)FORD_CAM_BUS) {
      bus_fwd = (int)FORD_MAIN_BUS;
    }
  }
  return bus_fwd;
}

// --- Init Configuration ---
static RxCheck ford_rx_checks[] = {
  {.msg = {{FORD_BrakeSysFeatures, FORD_MAIN_BUS, 8}, {0}, {0}}},
  {.msg = {{FORD_EngBrakeData, FORD_MAIN_BUS, 8}, {0}, {0}}},
};

static safety_config ford_init(uint16_t param) {
  UNUSED(param);
  controls_allowed = false;
  
  // MANDATORY: Initialize vehicle_speed struct to prevent F4 crash
  for (int i = 0; i < 6; i++) { vehicle_speed.values[i] = 0; }
  
  return (safety_config){
    .rx_checks = ford_rx_checks,
    .rx_checks_len = sizeof(ford_rx_checks) / sizeof(ford_rx_checks[0]),
  };
}

const safety_hooks ford_hooks = {
  .init = ford_init,
  .rx = ford_rx_hook,
  .tx = ford_tx_hook,
  .fwd = ford_fwd_hook,
};

