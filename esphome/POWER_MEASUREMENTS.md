# Tripp Lite UPS monitor: power measurements

Measured 2026-09-07 on the ESP32-P4 Function EV Board v1.4 (P4 revision 1.0),
with the SMART1500LCDXL connected by USB and IP101 Ethernet linked at 100 Mbps.
ESPHome 2026.5.0, ESP-IDF 5.5.4. These are measurements of this assembled board,
including its USB host supply; they are not estimates from chip datasheets.

## Deployed configuration

The final firmware uses dynamic scaling and holds the unused C6 in reset. A
separate measurement in Nordic Power Profiler 4.3.1 gave **62.28 mA / 0.311 W**
over a settled 60-second window: about **38% below the original 0.499 W**.
The exported CSV contained 600,001 finite measurements, uniformly spaced
100 microseconds apart (inclusive endpoints). Per-second means had a standard
deviation of 0.14 mA. This build removes the temporary PM profiling and tuning
actions; the slightly lower result than the experimental build is expected
to depend on that build difference and normal measurement variation.

Only standard ESPHome configuration is needed in `tripplite-ups.yaml`:

- `CONFIG_PM_ENABLE: "y"` and `CONFIG_PM_DFS_INIT_AUTO: "y"`.
- A GPIO output on GPIO54, which initializes low and holds C6_EN low.
- Keep the normal 360 MHz startup/maximum frequency and leave automatic light
  sleep disabled. No MIPI changes or new external components are needed.

The final YAML was copied to Home Assistant after confirming its earlier copy
had not changed. The two copies have SHA-256
`76d040b4afd128a7e28c939bfd96f8212300b6697d62c104efcac74bb091bbe6`.
The previous live YAML is backed up at
`/share/ha-tidy-backups/20260907-ups-power/tripplite-ups.yaml`.
Home Assistant's configuration check passed with no errors or warnings, and
its temporary profiling services were removed on reconnect.

The final image uploaded successfully over Ethernet while the old firmware
was running at a verified 40 MHz (4.90 seconds for the transfer). The final
firmware also recovered after a full PPK2 power-off: Ethernet and USB returned,
with fresh UPS measurements. Occasional individual USB control-transfer
errors were observed at both fixed 180 MHz and with the final configuration;
polling recovered, with no USB disconnect or invalid published values in the
90-second cold-boot verification. A subsequent 180-second check had one
control-transfer error and one briefly unavailable battery-voltage sample,
which recovered at the next five-second publication. All other published
measurements remained finite and the USB connection stayed up. This work
leaves the USB driver's error handling unchanged; it does not establish that
the power settings caused these errors. Real outage/restoration and
longer-term reliability remain untested.

The saved Nordic session is
`esphome/scratch/power-profiling/production-dfs-c6-off.ppk2`, with the settled
minute in `production-dfs-c6-off-final-minute.csv` and its statistics in the
adjacent JSON file. Power Profiler remains connected at 5 V with output on;
recording is stopped. The temporary Python instrument process was closed.

## Results

Each comparison below used a settled 30-second PPK2 capture. The experimental
build enabled PM profiling and tickless idle, and exposed temporary API actions
to change frequency and C6 reset without rebooting. The same firmware was used
for all rows in this table. C6 "off" means its enable pin was held low.

| CPU configuration | C6 | Light sleep requested | Mean at 5 V | Nominal power |
| --- | --- | --- | ---: | ---: |
| Fixed 360 MHz, initial control | On | No | 101.1 mA | 0.505 W |
| Fixed 180 MHz | On | No | 97.1 mA | 0.486 W |
| Fixed 90 MHz | On | No | 90.3 mA | 0.451 W |
| Dynamic 40–360 MHz | On | No | 90.8 mA | 0.454 W |
| Fixed 360 MHz | Off | No | 73.6 mA | 0.368 W |
| Dynamic 40–360 MHz | Off | No | 63.4 mA | 0.317 W |
| Dynamic 40–360 MHz | Off | Yes | 63.4 mA | 0.317 W |
| Fixed 90 MHz | Off | No | 63.0 mA | 0.315 W |
| Fixed 40 MHz | Off | No | 58.6 mA | 0.293 W |
| Fixed 360 MHz, closing control | On | No | 100.9 mA | 0.504 W |

The original firmware, without PM instrumentation, measured 99.8–99.9 mA
(0.499 W) in two stable captures. The matched experimental controls differ by
only 0.22 mA between the beginning and end of the series. The C6 accounts for
about 27.3–27.5 mA (0.137 W) of avoidable draw. Scaling saves approximately
another 10.2 mA (0.051 W) with the C6 off.

Dynamic scaling requests a 40 MHz minimum and 360 MHz maximum. While Ethernet
is running, its APB-frequency lock keeps the idle CPU floor at 90 MHz. The
cores can still run at 360 MHz for work. Fixed 40 MHz saves another 24 mW in
this workload, at the cost of the higher clock rates for bursts of work.

**40 MHz configuration caveat:** the table's 40 MHz result was set and verified
at runtime through `esp_pm_configure()`. A separate build using ESPHome's
`cpu_frequency: 40MHz` accepted the YAML but generated an effective 360 MHz
SDK configuration. IDF 5.5.4 limits that boot-time choice to FPGA/bring-up
configurations. That misleading build was not flashed or counted as a 40 MHz
measurement. Do not assume the YAML setting alone selects 40 MHz on this
combination of ESPHome, IDF, and P4 revision.

## Light sleep and unused peripherals

Enabling automatic light sleep returned `ESP_OK`, but the PM diagnostics
reported **zero light-sleep entries**. The Ethernet driver held `APB_FREQ_MAX`
throughout the connected test, preventing the system from reaching sleep.
The 0.09 mW difference between the two dynamic-scaling captures is below the
variation in these measurements and is not evidence of a saving.

Register inspection found all seven inspected MIPI DSI/CSI clock enables
already zero, and LDO3 (the MIPI supply) disabled. No MIPI register writes were
needed. The schematic also shows the speaker amplifier enable pulled low.
The audio codec supply is always connected; its software power modes were not
measured in this experiment.

Do not force the Ethernet lock off to enter sleep. This IDF version's USB host
does not provide the later automatic USB suspend/light-sleep option, and USB
polling needs to keep working. The production configuration should leave
automatic light sleep disabled.

## Method and limits

- PPK2 source mode, set to 5000 mV, wired to J1 5V/GND. Board power switch OFF.
  The v1.4 schematic places that switch between USB power and the header's
  VCC_5V rail. USB-UART was connected for recovery; its CP2102N is powered
  upstream of the switch, so the measurements exclude its separate USB draw.
- The 5 V figures are source setpoints, not a separate measurement of voltage
  at the board. Watts are calculated as mean current × 5 V. They exclude AC
  adapter/UPS conversion losses and the rest of the protected equipment.
- Capture raw 100 ksample/s data first, then decode offline using the PPK2's
  own calibration coefficients. The wire format and conversion were checked
  against the installed Nordic Power Profiler 4.3.1 source. Reject invalid
  ranges and sample-counter gaps; omit three samples at each range transition.
  All retained comparison captures passed these checks. Per-second current
  standard deviations were approximately 0.09–0.34 mA.
- Keep the PPK2 session open between captures. Early short-lived capture
  attempts interrupted board power and are excluded. A live-decoding attempt
  also lost data and produced invalid spikes; those numbers are excluded.
  One C6 capture overlapped a settings change at its end and was repeated.
- Fresh voltage, charge, runtime, and load readings were verified through the
  encrypted API throughout each comparison. No USB-disconnect transitions
  occurred in the retained 33-second verification windows. This establishes
  short bench stability, not long-term or real-outage reliability.
- Raw captures, calibration metadata, temporary firmware, and verification
  logs are retained locally under the gitignored `esphome/scratch/power-profiling/`
  and `/tmp/ups-connector-research/`. The original firmware was backed up before
  flashing. None of the temporary tuning actions belong in the deployed API.

## References

- [Espressif v1.4 schematic](https://dl.espressif.com/dl/schematics/esp32-p4-function-ev-board-v1.4-schematics.pdf):
  P4 GPIO54 connects to C6_EN / C6_CHIP_PU; power switch and header routing.
- [ESP-IDF 5.5.4 P4 power management](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/system/power_management.html):
  dynamic frequency scaling, locks, and automatic sleep conditions.
- [ESP-IDF 5.5.4 USB host](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32p4/api-reference/peripherals/usb_host.html).
- [Nordic Power Profiler source](https://github.com/NordicSemiconductor/pc-nrfconnect-ppk):
  PPK2 framing and calibration formula, cross-checked against installed 4.3.1.
