#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace esphome {
namespace usb_hid_ups {

// ── Power Event States ──────────────────────────────────────
enum class PowerState : uint8_t {
  NORMAL = 0,
  POWER_FAIL_GRACE,   // AC lost, waiting grace period
  BATTERY_LOW,        // Below runtime/capacity threshold
  SHUTDOWN_IMMINENT,  // UPS reports shutdown imminent
};

static const char *power_state_str(PowerState s) {
  switch (s) {
    case PowerState::NORMAL:            return "Normal";
    case PowerState::POWER_FAIL_GRACE:  return "Power Failure";
    case PowerState::BATTERY_LOW:       return "Battery Low";
    case PowerState::SHUTDOWN_IMMINENT: return "Shutdown Imminent";
    default: return "Unknown";
  }
}

// ── UPS Data (shared between tasks, protected by mutex) ─────
struct UpsData {
  // Sensor values
  float utility_voltage = NAN;
  float output_voltage = NAN;
  float input_frequency = NAN;
  float output_power = NAN;
  float battery_voltage = NAN;   // Battery pack voltage
  float battery_capacity = NAN;
  float remaining_runtime_sec = NAN;
  float load_percent = NAN;
  float rating_voltage = NAN;    // nominal mains voltage
  float rating_power_va = NAN;
  float rating_power_w = NAN;    // nameplate active power, NAN if not reported

  // Binary status
  bool ac_present = true;
  bool on_battery = false;
  bool charging = false;
  bool charging_valid = false;
  bool overload = false;
  bool overload_valid = false;
  bool battery_low_flag = false;
  bool battery_low_flag_valid = false;
  bool replace_battery = false;
  bool replace_battery_valid = false;
  bool shutdown_imminent = false;
  bool shutdown_imminent_valid = false;
  bool fully_charged = false;
  bool fully_charged_valid = false;
  bool fully_discharged = false;
  bool fully_discharged_valid = false;
  bool avr_boost = false;
  bool avr_boost_valid = false;
  bool avr_buck = false;
  bool avr_buck_valid = false;
  bool voltage_out_of_range = false;
  bool voltage_out_of_range_valid = false;
  bool over_temperature = false;
  bool over_temperature_valid = false;
  bool internal_failure = false;
  bool internal_failure_valid = false;
  bool awaiting_power = false;
  bool awaiting_power_valid = false;

  // -1 means a failed or unsupported read; other unknown codes remain visible.
  int32_t self_test_result = -1;
  int32_t beeper_status = -1;

  // Device info
  char model[64] = {};
  char serial[64] = {};
  bool connected = false;
  bool ac_present_valid = false;
  bool on_battery_valid = false;
  uint32_t last_poll_ms = 0;

  // State machine
  PowerState power_state = PowerState::NORMAL;
  uint32_t power_fail_start_ms = 0;   // millis() when AC was lost
  bool power_fail_event_sent = false; // set after grace expires to prevent re-firing
  char last_event[64] = "None";
  uint32_t last_event_time = 0;

  void invalidate_readings() {
    utility_voltage = output_voltage = battery_voltage = input_frequency = NAN;
    output_power = NAN;
    battery_capacity = remaining_runtime_sec = load_percent = NAN;
    ac_present_valid = on_battery_valid = false;
    charging_valid = false;
    overload_valid = false;
    battery_low_flag_valid = false;
    replace_battery_valid = false;
    shutdown_imminent_valid = false;
    fully_charged_valid = false;
    fully_discharged_valid = false;
    avr_boost_valid = false;
    avr_buck_valid = false;
    voltage_out_of_range_valid = false;
    over_temperature_valid = false;
    internal_failure_valid = false;
    awaiting_power_valid = false;
    self_test_result = beeper_status = -1;
  }

  void expire_readings(uint32_t now_ms) {
    if (!connected || last_poll_ms == 0 || now_ms - last_poll_ms > 20000)
      invalidate_readings();
  }
};

// Read results use different codes from SET_REPORT test commands. HID PDC / NUT
// test_read_info: zero is unspecified, not proof that a test passed.
static std::string self_test_result_text(int32_t code) {
  switch (code) {
    case -1: return "Unavailable";
    case 1: return "Passed";
    case 2: return "Warning";
    case 3: return "Failed";
    case 4: return "Aborted";
    case 5: return "In progress";
    case 6: return "No test initiated";
    case 7: return "Scheduled";
    default: return "Unknown (" + std::to_string(code) + ")";
  }
}

static std::string beeper_status_text(int32_t code) {
  switch (code) {
    case -1: return "Unavailable";
    case 1: return "Disabled";
    case 2: return "Enabled";
    case 3: return "Muted";
    default: return "Unknown (" + std::to_string(code) + ")";
  }
}

}  // namespace usb_hid_ups
}  // namespace esphome
