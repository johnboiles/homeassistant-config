# USB HID UPS prototype

Based on ESDN83/esp-cyberpower-ups commit
`50a6fd822cc2a4d99712ecba2b8547ecee7fc942` (MIT; see LICENSE).
Upstream: https://github.com/ESDN83/esp-cyberpower-ups

Local changes:

- Generic component name `usb_hid_ups` and P4-compatible ESPHome code generation.
- No standalone upstream HTTP UI; management uses ESPHome API and OTA.
- No UPS command buttons are exposed by the prototype configuration.
- Honor the declared decimal exponent for unitless HID fields (Tripp Lite input
  voltage report 24 uses exponent -1).
- For `09ae:2012`, read battery voltage from the Battery collection (report 32)
  with NUT's 0.1 correction, instead of PowerSummary.Voltage (a mains reading).
- Reject short/wrong-ID reports. Clear failed measurements and expire samples
  after 20 seconds rather than publishing a previous reading indefinitely.
- Poll every 500 ms between complete read cycles, instead of the upstream 5 s.
  This follows the cadence described by Tripp Lite support for PowerAlert:
  https://alioth-lists.debian.net/pipermail/nut-upsdev/2019-June/007444.html
- Keep a HID interrupt-IN request queued as a normal HID host does. Defer
  interface cleanup on disconnect until its pending transfer has completed.
  Feature polling alone (even at 500 ms) produced USB reconnects about every
  15 seconds on the connected SMART1500LCDXL; interrupt polling eliminated
  that cycle during the short bench check. Long-term reliability is unproven.

Hardware under evaluation: ESP32-P4 Function EV Board v1.4, P4 silicon revision
1.0, 16 MB flash; Tripp Lite SMART1500LCDXL (`09ae:2012`, 662-byte descriptor,
58 parsed fields). Basic live readings have been verified through the encrypted
ESPHome API: battery 100%, load 14%, runtime 2310 s, input 119 V, battery 27.5 V,
power source Mains. A 120-second encrypted API subscription after the interrupt
polling fix stayed connected with live measurements and no USB disconnect
transitions. Ethernet OTA was also verified. This is a prototype, not a
replacement for a tested NAS
shutdown policy. Physical outage/restoration and long-term reliability still
need testing. The UPS USB product string is just `Tripp Lite UPS`.

The upstream USB stack runs independently; do not also enable ESPHome's
`usb_host` component with this component.

## Additional telemetry

The September 7 telemetry update adds 20 entities to the original 10:

- Output voltage, input frequency, and output power in watts (5-second publication).
- Mains present, on battery, charging, fully charged, fully discharged, low
  battery, replace battery, and shutdown imminent.
- AVR boost, AVR buck, input voltage out of range, overload, over temperature,
  internal fault, and awaiting power.
- Self-test result and beeper status, read without sending any commands.

The main device card keeps seven primary entities: Battery Charge, Battery
Runtime, Output Power, Load, Power Source, Battery Low, and Shutdown Imminent.
The other 23 monitoring entities use `entity_category: diagnostic` so detailed
readings and health information appear in Diagnostics. Entity IDs, state
publication, and existing automation references are preserved.

The 15 binary flags publish from one snapshot every second. A missing/failed
read invalidates the affected flags rather than retaining a healthy state.
All dynamic readings expire after 20 seconds without a completed poll or on
USB disconnect. Numeric values become NaN; text values say `Unavailable`.
An unrecognized self-test code is shown verbatim as `Unknown (code)`; zero
does not mean a test passed. The unit can report charging and fully charged
simultaneously, as in Tripp Lite's own capture; this is a status flag, not a
measurement of charge current.

The original mappings for overload, replacement, and imminent shutdown were
incorrect. Correct HID usages are `0084/0065`, `0085/004B`, and `0084/0069`.
The shared status reports are now read once per poll, including on failures,
so flags from one report agree and do not require separate USB transactions.
Command read-modify-write operations never reuse that poll cache.

Reference: [Tripp Lite support's SMART1500LCDXL debug capture](https://alioth-lists.debian.net/pipermail/nut-upsdev/2019-June/007443.html),
attachment `debuglogsmart1500NUT.log.gz`. Its 662-byte descriptor reproduces
the 58 fields parsed on this device and is retained as a regression fixture.
Status names and read-result meanings are checked against NUT's
[`libhid.c`](https://github.com/networkupstools/nut/blob/master/drivers/libhid.c)
and [`usbhid-ups.c`](https://github.com/networkupstools/nut/blob/master/drivers/usbhid-ups.c).

Output Power comes from the UPS's actual ActivePower report, not a load-percent
estimate. On `09ae:2012`, an ActivePower field declaring unit `0x0000D121`
with exponent 0 gets its exponent corrected to 7 before scaling. This is the
specific defect fixed in [NUT PR #3581](https://github.com/networkupstools/nut/pull/3581)
(merged August 23, 2026). Other fields, units, and valid exponents are unchanged.
Our first live probe returned raw 120–123 at 13% load; without the correction,
that became 0.000012–0.0000123 W. Numeric temperature is not available.

Home Assistant assigned the new entities IDs starting with
`sensor.basement_tripp_lite_ups_` / `binary_sensor.basement_tripp_lite_ups_`.
The original entities retain their existing `tripp_lite_ups_` IDs, preserving
the outage notification automation.

Validation on September 7, 2026: ESPHome 2026.5.0 build and Ethernet OTA passed.
All 30 entities appeared in HA. A 180-second encrypted API capture of the final
firmware recorded output power 118–125 W, frequency 59.8–59.9 Hz, output voltage
about 118 V, and no asserted fault flags. Self-test returned `Unknown (0)` and
the beeper was `Enabled`. Two transient USB control-transfer errors recovered;
there were no USB disconnects or unavailable published samples after startup.
The host regression test passes with AddressSanitizer and UndefinedBehaviorSanitizer,
covering report/bit selection, power scaling and its exclusions, shared report
caching/failure recovery, and expired/disconnected status invalidation. Fault
transitions are checked with recorded/synthetic bytes, not physical overloads,
battery depletion, or an outage. Power consumption was not remeasured after
this telemetry update.

## Board configuration and operation

The main configuration is `esphome/tripplite-ups.yaml`. Its local component path
is resolved relative to that YAML. Copy the component directory too when moving
the configuration to another ESPHome builder.

The board uses onboard IP101 Ethernet (MDC 31, MDIO 52, reset 51, reference clock
input 50, PHY address 1). Wi-Fi/ESP-Hosted was tested during bring-up but is not
needed in the final configuration. DHCP assigned `10.0.0.120` during testing;
use `tripplite-ups.local` or a DHCP reservation for ongoing access.

The Function EV Board configuration holds its unused C6 in reset through
GPIO54 and enables IDF dynamic frequency scaling. Ethernet keeps the idle
clock at 90 MHz; 360 MHz remains available for work. Automatic light sleep is
disabled. See [power measurements](../../POWER_MEASUREMENTS.md) for the PPK2
comparisons, wiring, and validation limits. This GPIO mapping is specific to
the Function EV Board v1.4 and must be checked when changing boards.

Plug the UPS data cable into USB-A. Use USB-to-UART for flashing and debug logs,
or USB Power-in for a dedicated battery-backed 5 V supply. Leave the shared OTG
USB-C port unused. Network equipment must also remain powered during outages.

Build/upload from the repository root with ESPHome 2026.5.0 (tested):

```sh
esphome compile esphome/tripplite-ups.yaml
esphome upload esphome/tripplite-ups.yaml --device tripplite-ups.local
```

For serial recovery, use `--device /dev/cu.usbserial-210 --upload_speed 115200`.
Higher serial speeds produced transport errors with this setup.

The existing `esphome_encryption_key` and `ota_password` secrets are reused.
Add the device through Home Assistant's ESPHome integration. The existing
`Notification: power is out` automation uses
`sensor.tripp_lite_ups_input_voltage`, preserving its below-60 V trigger and
critical notification actions. No NUT server or NAS shutdown automation is
included yet.

Regression check from the repository root:

```sh
c++ -std=c++17 esphome/tests/usb_hid_ups_scaling.cpp -o /tmp/test-ups-scaling
/tmp/test-ups-scaling
```
