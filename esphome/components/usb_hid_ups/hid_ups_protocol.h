#pragma once

// ═══════════════════════════════════════════════════════════════
// USB HID Power Device Class — Report Descriptor Parser
//
// Implements parsing of the USB HID "Power Device" (0x84) and
// "Battery System" (0x85) Usage Pages as defined in:
//   - USB HID Usage Tables (HUT) 1.12, Chapter 4
//   - USB HID Power Device Class spec
//
// CyberPower USVs (VID 0x0764) follow this standard.
// ═══════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome {
namespace usb_hid_ups {

// ── HID Usage Pages ──────────────────────────────────────────
static constexpr uint16_t USAGE_PAGE_POWER_DEVICE  = 0x0084;
static constexpr uint16_t USAGE_PAGE_BATTERY       = 0x0085;
static constexpr uint16_t USAGE_PAGE_GENERIC       = 0x0001;

// ── Power Device Page (0x84) — collection usages ────────────
// These name COLLECTIONs, not values. They matter because a measurement
// usage on its own is ambiguous: Voltage (0x30) appears identically
// under Input, Output and PowerSummary, and only the enclosing
// collection says whether it is mains, output or battery voltage.
static constexpr uint16_t PD_COLL_UPS            = 0x0004;
static constexpr uint16_t PD_COLL_BATTERY_SYSTEM = 0x0010;
static constexpr uint16_t PD_COLL_POWER_CONVERTER= 0x0016;
static constexpr uint16_t PD_COLL_INPUT          = 0x001A;
static constexpr uint16_t PD_COLL_OUTPUT         = 0x001C;
static constexpr uint16_t PD_COLL_FLOW           = 0x001E;
static constexpr uint16_t PD_COLL_POWER_SUMMARY  = 0x0024;

// ── Power Device Page (0x84) Usages ─────────────────────────
static constexpr uint16_t PD_USAGE_UPS              = 0x0004;
static constexpr uint16_t PD_USAGE_INPUT            = 0x001A;
static constexpr uint16_t PD_USAGE_OUTPUT           = 0x001C;
static constexpr uint16_t PD_USAGE_PRESENT_STATUS   = 0x0002;
static constexpr uint16_t PD_USAGE_VOLTAGE          = 0x0030;
static constexpr uint16_t PD_USAGE_CURRENT          = 0x0031;
static constexpr uint16_t PD_USAGE_FREQUENCY        = 0x0032;
static constexpr uint16_t PD_USAGE_APPARENT_POWER   = 0x0033;
static constexpr uint16_t PD_USAGE_ACTIVE_POWER     = 0x0034;
static constexpr uint16_t PD_USAGE_PERCENT_LOAD     = 0x0035;
static constexpr uint16_t PD_USAGE_CONFIG_VOLTAGE   = 0x0040;
static constexpr uint16_t PD_USAGE_CONFIG_CURRENT   = 0x0041;
static constexpr uint16_t PD_USAGE_CONFIG_FREQUENCY = 0x0042;
static constexpr uint16_t PD_USAGE_CONFIG_APPARENT_POWER = 0x0043;
static constexpr uint16_t PD_USAGE_CONFIG_ACTIVE_POWER   = 0x0044;
static constexpr uint16_t PD_USAGE_SWITCHABLE       = 0x006B;

// ── Power Device Page (0x84) — writable command usages ──────
// These live in FEATURE reports and are written via SET_REPORT to
// trigger UPS actions. Usage numbers per HID Power Device Class spec.
static constexpr uint16_t PD_USAGE_DELAY_BEFORE_REBOOT   = 0x0055;
static constexpr uint16_t PD_USAGE_DELAY_BEFORE_STARTUP  = 0x0056;
static constexpr uint16_t PD_USAGE_DELAY_BEFORE_SHUTDOWN = 0x0057;
static constexpr uint16_t PD_USAGE_TEST_CMD              = 0x0058;  // battery test (1=quick,2=deep,3=abort)
static constexpr uint16_t PD_USAGE_AUDIBLE_ALARM_CTRL    = 0x005A;  // beeper (1=disable,2=enable,3=mute)

// ── Battery System Page (0x85) Usages ───────────────────────
static constexpr uint16_t BAT_USAGE_REMAINING_CAPACITY   = 0x0066;
static constexpr uint16_t BAT_USAGE_RUNTIME_TO_EMPTY     = 0x0068;
static constexpr uint16_t BAT_USAGE_DESIGN_CAPACITY      = 0x0083;
static constexpr uint16_t BAT_USAGE_FULL_CHARGE_CAPACITY = 0x0067;
static constexpr uint16_t BAT_USAGE_CHARGING             = 0x0044;
static constexpr uint16_t BAT_USAGE_DISCHARGING          = 0x0045;
static constexpr uint16_t BAT_USAGE_AC_PRESENT           = 0x00D0;
static constexpr uint16_t BAT_USAGE_BELOW_REMAINING_CAP  = 0x0042;
static constexpr uint16_t BAT_USAGE_SHUTDOWN_IMMINENT    = 0x00D3;
static constexpr uint16_t BAT_USAGE_REMAINING_TIME_LIMIT = 0x006A;
static constexpr uint16_t BAT_USAGE_CAPACITY_MODE        = 0x002C;
static constexpr uint16_t BAT_USAGE_BATTERY_PRESENT      = 0x00D2;
static constexpr uint16_t BAT_USAGE_OVERLOAD             = 0x0065;
static constexpr uint16_t BAT_USAGE_NEED_REPLACEMENT     = 0x006B;

// ── HID Report Descriptor item types ────────────────────────
enum class HidItemType : uint8_t {
  USAGE_PAGE      = 0x04,  // Global
  USAGE           = 0x08,  // Local
  USAGE_MINIMUM   = 0x18,  // Local
  USAGE_MAXIMUM   = 0x28,  // Local
  LOGICAL_MINIMUM = 0x14,  // Global
  LOGICAL_MAXIMUM = 0x24,  // Global
  REPORT_SIZE     = 0x74,  // Global
  REPORT_COUNT    = 0x94,  // Global
  REPORT_ID       = 0x84,  // Global
  COLLECTION      = 0xA0,  // Main
  END_COLLECTION  = 0xC0,  // Main
  INPUT           = 0x80,  // Main
  OUTPUT          = 0x90,  // Main
  FEATURE         = 0xB0,  // Main
  UNIT            = 0x64,  // Global
  UNIT_EXPONENT   = 0x54,  // Global
};

// Which report types carry this field?
enum class ReportType : uint8_t {
  INPUT   = 1,
  OUTPUT  = 2,
  FEATURE = 3,
};

// A single parsed field from the HID report descriptor
struct HidField {
  uint16_t usage_page;
  uint16_t usage;
  uint8_t  report_id;
  ReportType report_type;
  uint16_t bit_offset;   // offset within the report (after report_id byte)
  uint16_t bit_size;     // total bits for this field
  int32_t  logical_min;
  int32_t  logical_max;
  int8_t   unit_exponent;
  uint32_t unit;         // HID UNIT item (0x64); 0 = not declared
  uint16_t collection;   // innermost scoping COLLECTION usage; 0 = none
};

// Parsed report descriptor with all fields we care about
struct HidReportMap {
  std::vector<HidField> fields;

  // Find a field by usage page + usage, optionally restricted to the
  // collection that encloses it.
  //
  // Pass a collection whenever the usage is ambiguous. Voltage (0x30)
  // occurs once per Input / Output / PowerSummary collection, and
  // without the filter this returns whichever the descriptor happens to
  // declare first — on a CyberPower BR1200ELCD that is the PowerSummary
  // copy, i.e. the battery, not the mains.
  //
  // collection == 0 keeps the old any-collection behaviour, which is
  // correct for usages that occur exactly once.
  const HidField *find(uint16_t usage_page, uint16_t usage,
                       uint16_t collection = 0,
                       ReportType type = ReportType::FEATURE) const {
    for (auto &f : fields) {
      if (f.usage_page == usage_page && f.usage == usage &&
          f.report_type == type &&
          (collection == 0 || f.collection == collection))
        return &f;
    }
    // Fallback: try any report type
    if (type != ReportType::INPUT) {
      for (auto &f : fields) {
        if (f.usage_page == usage_page && f.usage == usage &&
            (collection == 0 || f.collection == collection))
          return &f;
      }
    }
    return nullptr;
  }

  // How many fields carry this page+usage (ignoring collection)?
  int count(uint16_t usage_page, uint16_t usage) const {
    int n = 0;
    for (auto &f : fields)
      if (f.usage_page == usage_page && f.usage == usage) n++;
    return n;
  }
};

// ── Report Descriptor Parser ────────────────────────────────
// Parses a raw HID report descriptor and extracts all fields
// with their usage, report ID, and bit position.
//
// This is a simplified parser that handles the subset needed
// for Power Device Class reports. It tracks:
//   - Global state: usage_page, report_id, report_size, report_count, logical_min/max
//   - Local state: usage (resets after each Main item)
//   - Bit offsets per report_id
// Pick the collection that gives a measurement its meaning.
//
// Walks the open collections from innermost outwards and returns the
// first one that scopes a measurement. A descriptor may nest a Flow
// collection inside Input; for our purposes that is still Input, so
// Flow is deliberately not in the list. Falls back to the innermost
// collection when nothing matches, and to 0 at the top level.
static uint16_t scoping_collection(const uint16_t *stack, size_t depth) {
  for (size_t i = depth; i-- > 0;) {
    switch (stack[i]) {
      case PD_COLL_INPUT:
      case PD_COLL_OUTPUT:
      case PD_COLL_POWER_SUMMARY:
      case PD_COLL_BATTERY_SYSTEM:
        return stack[i];
      default:
        break;
    }
  }
  return depth ? stack[depth - 1] : 0;
}

static bool parse_report_descriptor(const uint8_t *desc, size_t len, HidReportMap &map) {
  // Global state
  uint16_t usage_page = 0;
  uint8_t  report_id = 0;
  uint16_t report_size = 0;
  uint16_t report_count = 0;
  int32_t  logical_min = 0;
  int32_t  logical_max = 0;
  int8_t   unit_exponent = 0;
  uint32_t unit = 0;

  // Local state (usage stack for multi-usage fields)
  static constexpr size_t MAX_USAGES = 64;
  uint16_t usages[MAX_USAGES];
  size_t   usage_count = 0;

  // Open COLLECTIONs, innermost last. Depth 12 is far beyond anything a
  // Power Device descriptor nests; deeper input is clamped rather than
  // overflowing, and the extra levels simply do not scope anything.
  static constexpr size_t MAX_COLL_DEPTH = 12;
  uint16_t coll_stack[MAX_COLL_DEPTH];
  size_t   coll_depth = 0;

  // Track bit offset per report_id (separate for input/feature/output)
  // Key: (report_type << 8) | report_id → bit offset
  uint16_t bit_offsets[3][256] = {};

  size_t pos = 0;
  while (pos < len) {
    uint8_t prefix = desc[pos];

    // Long item? Skip
    if (prefix == 0xFE) {
      if (pos + 2 >= len) break;
      uint8_t data_size = desc[pos + 1];
      pos += 3 + data_size;
      continue;
    }

    // Short item
    uint8_t item_size = prefix & 0x03;
    if (item_size == 3) item_size = 4;  // size encoding: 0,1,2,3 → 0,1,2,4 bytes
    uint8_t item_tag = prefix & 0xFC;   // tag + type (upper 6 bits)

    if (pos + 1 + item_size > len) break;

    // Read data (little-endian, sign-extend for signed items)
    int32_t data_signed = 0;
    uint32_t data_unsigned = 0;
    if (item_size >= 1) data_unsigned = desc[pos + 1];
    if (item_size >= 2) data_unsigned |= (uint32_t)desc[pos + 2] << 8;
    if (item_size >= 3) data_unsigned |= (uint32_t)desc[pos + 3] << 16;
    if (item_size >= 4) data_unsigned |= (uint32_t)desc[pos + 4] << 24;

    // Sign-extend
    data_signed = (int32_t)data_unsigned;
    if (item_size == 1 && (data_unsigned & 0x80)) data_signed |= 0xFFFFFF00;
    if (item_size == 2 && (data_unsigned & 0x8000)) data_signed |= 0xFFFF0000;

    switch (item_tag) {
      // ── Global items ──
      case (uint8_t)HidItemType::USAGE_PAGE:
        usage_page = (uint16_t)data_unsigned;
        break;
      case (uint8_t)HidItemType::REPORT_ID:
        report_id = (uint8_t)data_unsigned;
        break;
      case (uint8_t)HidItemType::REPORT_SIZE:
        report_size = (uint16_t)data_unsigned;
        break;
      case (uint8_t)HidItemType::REPORT_COUNT:
        report_count = (uint16_t)data_unsigned;
        break;
      case (uint8_t)HidItemType::LOGICAL_MINIMUM:
        logical_min = data_signed;
        break;
      case (uint8_t)HidItemType::LOGICAL_MAXIMUM:
        logical_max = data_signed;
        break;
      case (uint8_t)HidItemType::UNIT_EXPONENT:
        unit_exponent = (int8_t)(data_unsigned & 0x0F);
        if (unit_exponent > 7) unit_exponent -= 16;  // nibble sign extension
        break;
      case (uint8_t)HidItemType::UNIT:
        // Needed to interpret UNIT_EXPONENT: the declared exponent is
        // relative to the unit's own built-in scale. See
        // hid_unit_exponent() at the bottom of this file.
        unit = data_unsigned;
        break;

      // ── Local items ──
      case (uint8_t)HidItemType::USAGE:
        if (usage_count < MAX_USAGES)
          usages[usage_count++] = (uint16_t)data_unsigned;
        break;

      // ── Main items (Input, Output, Feature) ──
      case (uint8_t)HidItemType::INPUT:
      case (uint8_t)HidItemType::OUTPUT:
      case (uint8_t)HidItemType::FEATURE: {
        ReportType rt;
        if (item_tag == (uint8_t)HidItemType::INPUT) rt = ReportType::INPUT;
        else if (item_tag == (uint8_t)HidItemType::OUTPUT) rt = ReportType::OUTPUT;
        else rt = ReportType::FEATURE;

        uint8_t rt_idx = (uint8_t)rt - 1;
        bool is_constant = (data_unsigned & 0x01);  // bit 0 = Constant

        for (uint16_t i = 0; i < report_count; i++) {
          if (!is_constant && i < usage_count) {
            HidField field;
            field.usage_page = usage_page;
            field.usage = usages[i];
            field.report_id = report_id;
            field.report_type = rt;
            field.bit_offset = bit_offsets[rt_idx][report_id];
            field.bit_size = report_size;
            field.logical_min = logical_min;
            field.logical_max = logical_max;
            field.unit_exponent = unit_exponent;
            field.unit = unit;
            field.collection = scoping_collection(coll_stack, coll_depth);
            map.fields.push_back(field);
          }
          bit_offsets[rt_idx][report_id] += report_size;
        }

        // Reset local state after Main item
        usage_count = 0;
        break;
      }

      case (uint8_t)HidItemType::COLLECTION:
        // The collection takes the first unused local Usage (HID 1.11,
        // 6.2.2.4). Keeping that on a stack is what lets an otherwise
        // ambiguous measurement usage be resolved later.
        if (coll_depth < MAX_COLL_DEPTH)
          coll_stack[coll_depth++] = (usage_count > 0) ? usages[0] : 0;
        usage_count = 0;
        break;

      case (uint8_t)HidItemType::END_COLLECTION:
        if (coll_depth > 0) coll_depth--;
        usage_count = 0;
        break;

      default:
        break;
    }

    pos += 1 + item_size;
  }

  return !map.fields.empty();
}

// ── Value extraction from raw report data ───────────────────
// Extracts a field value from a raw HID report buffer.
// The buffer should NOT include the report_id byte (already stripped).
static int32_t extract_field_value(const uint8_t *report_data, const HidField &field) {
  uint16_t bit_off = field.bit_offset;
  uint16_t bit_len = field.bit_size;

  if (bit_len == 0 || bit_len > 32) return 0;

  // Extract bits
  uint32_t value = 0;
  for (uint16_t i = 0; i < bit_len; i++) {
    uint16_t abs_bit = bit_off + i;
    uint16_t byte_idx = abs_bit / 8;
    uint8_t  bit_idx = abs_bit % 8;
    if (report_data[byte_idx] & (1 << bit_idx))
      value |= (1u << i);
  }

  // Sign-extend if logical_min is negative
  if (field.logical_min < 0 && bit_len < 32) {
    if (value & (1u << (bit_len - 1)))
      value |= ~((1u << bit_len) - 1);
  }

  return (int32_t)value;
}

// ── Value encoding into raw report data (inverse of extract) ──
// Writes `value` into the field's bit range within report_data.
// report_data must NOT include the report_id byte. Bits outside the
// field are left untouched (read-modify-write friendly).
static void encode_field_value(uint8_t *report_data, const HidField &field, int32_t value) {
  uint16_t bit_off = field.bit_offset;
  uint16_t bit_len = field.bit_size;
  if (bit_len == 0 || bit_len > 32) return;

  uint32_t uval = (uint32_t)value;
  for (uint16_t i = 0; i < bit_len; i++) {
    uint16_t abs_bit = bit_off + i;
    uint16_t byte_idx = abs_bit / 8;
    uint8_t  bit_idx = abs_bit % 8;
    if ((uval >> i) & 1u)
      report_data[byte_idx] |= (uint8_t)(1u << bit_idx);
    else
      report_data[byte_idx] &= (uint8_t)~(1u << bit_idx);
  }
}

// Apply unit exponent to get real-world value
static float apply_exponent(int32_t raw, int8_t exponent) {
  float val = (float)raw;
  if (exponent > 0) {
    for (int i = 0; i < exponent; i++) val *= 10.0f;
  } else if (exponent < 0) {
    for (int i = 0; i < -exponent; i++) val /= 10.0f;
  }
  return val;
}

// ── Unit exponent resolution ────────────────────────────────
// UNIT_EXPONENT is not an absolute power of ten. It is stated relative
// to the built-in scale of the UNIT the field carries: HID expresses
// voltage in units that already imply 10^7, so a descriptor declaring
// exponent 7 means volts and one declaring 6 means tenths of a volt.
// Applying the declared exponent on its own would be wrong by that
// built-in factor.
//
// Table and arithmetic follow NUT's HIDUnits[] and get_unit_expo() in
// drivers/libhid.c, the de-facto reference for HID Power Device parsing.
//
// NUT additionally maps logical to physical range via PHYSICAL_MINIMUM
// and PHYSICAL_MAXIMUM. This parser does not read those items, which is
// equivalent to them being absent — the case in which NUT also passes
// the logical value through unchanged. No Power Device descriptor seen
// so far declares them.
struct HidUnitScale {
  uint32_t unit;
  int8_t   expo;
};

static constexpr HidUnitScale HID_UNIT_SCALES[] = {
  { 0x00F0D121, 7 },  // volt
  { 0x00100001, 0 },  // ampere
  { 0x0000D121, 7 },  // volt-ampere / watt
  { 0x00001001, 0 },  // second
  { 0x00010001, 0 },  // kelvin
  { 0x0000F001, 0 },  // hertz
  { 0x00101001, 0 },  // ampere-second
};

// Effective power of ten for a raw field value.
//
// With no recognized UNIT, preserve the declared decimal exponent (as NUT does).
// Tripp Lite 09ae:2012 uses UNIT=0 and exponent=-1 for mains voltage.
static int8_t hid_unit_exponent(uint32_t unit, int8_t declared) {
  for (auto &u : HID_UNIT_SCALES) {
    if (u.unit == unit) return (int8_t)(declared - u.expo);
  }
  return declared;
}

// Raw extracted value -> physical value, honouring unit and exponent.
static float scale_field_value(const HidField &field, int32_t raw) {
  return apply_exponent(raw, hid_unit_exponent(field.unit, field.unit_exponent));
}

}  // namespace usb_hid_ups
}  // namespace esphome
