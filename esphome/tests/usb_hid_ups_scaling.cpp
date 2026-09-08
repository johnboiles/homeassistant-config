// Regression cases for the SMART1500LCDXL descriptor observed on 2026-09-06.
#include <cassert>
#include <cmath>
#include "../components/usb_hid_ups/hid_ups_protocol.h"
#include "../components/usb_hid_ups/ups_data.h"
#include "tripplite_2012_descriptor.h"
using namespace esphome::usb_hid_ups;
int main() {
  HidField input{};
  input.unit = 0;
  input.unit_exponent = -1;
  // The actual unitless report 24 declares tenths of volts.
  assert(std::fabs(scale_field_value(input, 1190) - 119.0f) < 0.001f);
  // Preserve standard HID voltage scaling for other UPS descriptors.
  input.unit = 0x00F0D121;
  input.unit_exponent = 7;
  assert(scale_field_value(input, 119) == 119.0f);
  input.unit_exponent = 6;
  assert(std::fabs(scale_field_value(input, 1190) - 119.0f) < 0.001f);
  HidField runtime{};
  runtime.unit = 0x00001001;
  assert(scale_field_value(runtime, 2310) == 2310.0f);

  // Manufacturer's complete descriptor: measurements and warnings must
  // resolve to the right collection, report, and bit (not merely exist).
  HidReportMap map;
  assert(parse_report_descriptor(TRIPPLITE_2012_DESCRIPTOR, sizeof(TRIPPLITE_2012_DESCRIPTOR), map));
  assert(map.fields.size() == 58);
  auto check = [&](uint16_t page, uint16_t usage, uint16_t coll, uint8_t report, uint16_t bit) {
    const auto *f = map.find(page, usage, coll);
    assert(f && f->report_type == ReportType::FEATURE);
    assert(f->report_id == report && f->bit_offset == bit);
    return f;
  };
  const auto *output = check(0x84, PD_USAGE_VOLTAGE, PD_COLL_OUTPUT, 27, 0);
  assert(std::fabs(scale_field_value(*output, 1184) - 118.4f) < 0.001f);
  const auto *frequency = check(0x84, PD_USAGE_FREQUENCY, PD_COLL_INPUT, 25, 0);
  assert(std::fabs(scale_field_value(*frequency, 598) - 59.8f) < 0.001f);
  const auto *power = check(0x84, PD_USAGE_ACTIVE_POWER, PD_COLL_OUTPUT, 71, 0);
  assert(power->unit == 0x0000D121 && power->unit_exponent == 0);
  assert(scale_field_value(*power, 120) < 0.001f);  // The original defect.
  assert(fix_tripplite_power_units(map) == 1);
  assert(scale_field_value(*power, 120) == 120.0f);
  assert(scale_field_value(*power, 520) == 520.0f);
  assert(fix_tripplite_power_units(map) == 0);  // Idempotent.
  assert(std::fabs(scale_field_value(*output, 1184) - 118.4f) < 0.001f);
  assert(std::fabs(scale_field_value(*frequency, 598) - 59.8f) < 0.001f);
  HidReportMap other;
  for (int variant = 0; variant < 5; ++variant) {
    auto f = *power;
    if (variant == 0) f.unit_exponent = 6;  // Conformant tenths of watts.
    if (variant == 1) { f.unit = 0; f.unit_exponent = 0; }
    if (variant == 2) { f.usage = PD_USAGE_APPARENT_POWER; f.unit_exponent = 0; }
    if (variant == 3) { f.usage = PD_USAGE_VOLTAGE; f.unit_exponent = 0; }
    if (variant == 4) { f.usage_page = 0xffff; f.unit_exponent = 0; }
    other.fields.push_back(f);
  }
  assert(fix_tripplite_power_units(other) == 0);
  assert(std::fabs(scale_field_value(other.fields[0], 120) - 12.0f) < 0.001f);
  const auto *ac = check(0x85, BAT_USAGE_AC_PRESENT, 0, 50, 1);
  const auto *charging = check(0x85, BAT_USAGE_CHARGING, 0, 50, 2);
  const auto *discharging = check(0x85, BAT_USAGE_DISCHARGING, 0, 50, 3);
  const auto *replace = check(0x85, BAT_USAGE_NEED_REPLACEMENT, 0, 50, 4);
  const auto *low = check(0x85, BAT_USAGE_BELOW_REMAINING_CAP, 0, 50, 5);
  check(0x85, BAT_USAGE_FULLY_CHARGED, 0, 50, 6);
  check(0x85, BAT_USAGE_FULLY_DISCHARGED, 0, 50, 7);
  const auto *shutdown = check(0x84, PD_USAGE_SHUTDOWN_IMMINENT, 0, 50, 0);
  check(0x84, PD_USAGE_VOLTAGE_OUT_OF_RANGE, 0, 34, 0);
  check(0x84, PD_USAGE_BUCK, 0, 34, 1);
  check(0x84, PD_USAGE_BOOST, 0, 34, 2);
  const auto *overload = check(0x84, PD_USAGE_OVERLOAD, 0, 34, 4);
  check(0x84, PD_USAGE_OVER_TEMPERATURE, 0, 34, 6);
  check(0x84, PD_USAGE_INTERNAL_FAILURE, 0, 34, 7);
  check(0x84, PD_USAGE_AWAITING_POWER, 0, 34, 14);
  check(0x84, PD_USAGE_TEST_CMD, PD_COLL_BATTERY_SYSTEM, 16, 0);
  check(0x84, PD_USAGE_AUDIBLE_ALARM_CTRL, 0, 17, 0);

  uint8_t status[] = {0x46};  // Manufacturer capture: AC + charging + full.
  assert(extract_field_value(status, *ac) == 1);
  assert(extract_field_value(status, *charging) == 1);
  assert(extract_field_value(status, *discharging) == 0);
  assert(extract_field_value(status, *replace) == 0);
  // Exercise each warning independently so adjacent flags cannot mask a bug.
  for (const auto *f : {shutdown, replace, low}) {
    status[0] = 1u << f->bit_offset;
    assert(extract_field_value(status, *f) == 1);
    assert(extract_field_value(status, *ac) == 0);
  }
  uint8_t converter[] = {0x10, 0};
  assert(extract_field_value(converter, *overload) == 1);

  // Shared reports are sampled once, including failures. A subsequent
  // cycle must retry and recover; a different report/type must read afresh.
  HidReportCache cache;
  int calls = 0, value = -1;
  bool success = true;
  auto reader = [&](uint8_t id, ReportType, uint8_t *buf, size_t len) {
    ++calls;
    assert(len >= 2);
    buf[0] = id; buf[1] = 0x46;
    return success;
  };
  assert(cache.read_field(map, ac, value, reader) && value == 1);
  assert(cache.read_field(map, charging, value, reader) && value == 1 && calls == 1);
  assert(cache.read_field(map, overload, value, reader) && calls == 2);
  const auto *ac_input = map.find(0x85, BAT_USAGE_AC_PRESENT, 0, ReportType::INPUT);
  assert(cache.read_field(map, ac_input, value, reader) && calls == 3);
  cache.clear(); success = false;
  assert(!cache.read_field(map, ac, value, reader));
  assert(!cache.read_field(map, charging, value, reader) && calls == 4);
  cache.clear(); success = true;
  assert(cache.read_field(map, ac, value, reader) && value == 1 && calls == 5);

  // Fault data must become unknown on failure, disconnect, or stale polling.
  UpsData d;
  d.connected = true; d.last_poll_ms = 100;
  d.input_frequency = 60; d.output_power = 120; d.replace_battery_valid = true;
  d.shutdown_imminent_valid = d.battery_low_flag_valid = true;
  d.beeper_status = 2; d.self_test_result = 1;
  d.expire_readings(200);
  assert(d.replace_battery_valid && d.input_frequency == 60);
  d.expire_readings(20101);
  assert(!d.replace_battery_valid && !d.shutdown_imminent_valid && !d.battery_low_flag_valid);
  assert(std::isnan(d.input_frequency) && d.self_test_result == -1 && d.beeper_status == -1);
  assert(std::isnan(d.output_power));
  d.connected = false; d.overload_valid = true;
  d.expire_readings(101);
  assert(!d.overload_valid);
  d.connected = true; d.last_poll_ms = UINT32_MAX - 100;
  d.avr_boost_valid = true;
  d.expire_readings(100);  // millis wrap-around, only 201 ms old
  assert(d.avr_boost_valid);
  d.invalidate_readings();
  assert(!d.avr_boost_valid);
  assert(self_test_result_text(0) == "Unknown (0)");
  assert(self_test_result_text(1) == "Passed");
  assert(self_test_result_text(3) == "Failed");
  assert(self_test_result_text(5) == "In progress");
  assert(beeper_status_text(2) == "Enabled");
  assert(beeper_status_text(-1) == "Unavailable");
}
