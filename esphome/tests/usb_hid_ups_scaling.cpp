// Regression cases for the SMART1500LCDXL descriptor observed on 2026-09-06.
#include <cassert>
#include <cmath>
#include "../components/usb_hid_ups/hid_ups_protocol.h"
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
}
