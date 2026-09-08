#pragma once

// USB HID UPS monitor for ESPHome.
// Adapted from ESDN83/esp-cyberpower-ups; see README.md and LICENSE.
// Validated prototype: ESP32-P4 Function EV Board v1.4 and
// Tripp Lite SMART1500LCDXL (09ae:2012). The YAML exposes monitoring only.

#include "esphome/core/component.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "usb/usb_host.h"

#include "esp_event.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <atomic>
#include <cstring>
#include <cmath>

#include "hid_ups_protocol.h"
#include "ups_data.h"

namespace esphome {
namespace usb_hid_ups {

static const char *const TAG = "usb_hid_ups";
static const char *const FW_BUILD_ID = "Tripp Lite P4 prototype, upstream 50a6fd8";

// CyberPower USB identifiers
static constexpr uint16_t CYBERPOWER_VID = 0x0764;
static constexpr uint16_t CYBERPOWER_PID = 0x0501;  // Common PID; verify on real hardware
static constexpr uint16_t TRIPPLITE_VID = 0x09ae;
static constexpr uint16_t TRIPPLITE_PID = 0x2012;

// HID class requests
static constexpr uint8_t HID_REQ_GET_REPORT    = 0x01;
static constexpr uint8_t HID_REQ_SET_REPORT    = 0x09;
static constexpr uint8_t HID_REPORT_TYPE_INPUT  = 0x01;
static constexpr uint8_t HID_REPORT_TYPE_FEATURE = 0x03;

// HID descriptor type
static constexpr uint8_t USB_DT_HID        = 0x21;
static constexpr uint8_t USB_DT_HID_REPORT = 0x22;

// Polling interval
// Tripp Lite's PowerAlert polls USB every half second. Protocol 2012 devices
// have documented disconnect problems with infrequent host communication.
static constexpr uint32_t POLL_INTERVAL_MS = 500;

// ── UPS Commands (NUT-style instant commands) ───────────────
// Sent from any task via queue_command(); executed on the USB task
// as a SET_REPORT on the matching FEATURE report.
//   SAFE     : reversible, no impact on the protected load
//   DANGEROUS: switches the UPS output — can power off connected devices
enum class UpsCommand : uint8_t {
  // ── Safe ──
  BEEPER_MUTE = 0,       // AudibleAlarmControl = 3 (mute current alarm)
  BEEPER_ENABLE,         // AudibleAlarmControl = 2
  BEEPER_DISABLE,        // AudibleAlarmControl = 1
  TEST_BATTERY_START,    // Test = 1 (quick self-test)
  TEST_BATTERY_STOP,     // Test = 3 (abort test)
  SHUTDOWN_STOP,         // DelayBeforeShutdown = -1 (cancel a pending shutdown)
  // ── Dangerous (opt-in) ──
  SHUTDOWN_REBOOT,       // DelayBeforeReboot  = REBOOT_DELAY_S  (turn load off, then back on)
  LOAD_OFF_DELAY,        // DelayBeforeShutdown = LOAD_OFF_DELAY_S (turn load off, stay off)
};

// Delay values (seconds) for the load-switching commands.
static constexpr int32_t REBOOT_DELAY_S   = 10;
static constexpr int32_t LOAD_OFF_DELAY_S = 20;

// While a battery self-test runs, the UPS switches to battery on purpose.
// Suppress the power-fail/battery-low state machine for this long after a
// test is started so it does not trigger shutdown automations. A quick
// self-test lasts only a few seconds; this window is a safe upper bound.
static constexpr uint32_t TEST_SUPPRESS_MS = 45000;

// ── Ring buffer debug log ──────────────────────────────────
static constexpr size_t LOG_RING_SIZE = 8192;
static char log_ring_[LOG_RING_SIZE];
static size_t log_ring_head_ = 0;
static SemaphoreHandle_t log_ring_mutex_ = nullptr;

static void log_ring_init_() {
  if (!log_ring_mutex_) log_ring_mutex_ = xSemaphoreCreateMutex();
}

static void log_ring_append_(const char *msg) {
  if (!log_ring_mutex_) return;
  xSemaphoreTake(log_ring_mutex_, portMAX_DELAY);
  size_t len = strlen(msg);
  for (size_t i = 0; i < len; i++) {
    log_ring_[log_ring_head_] = msg[i];
    log_ring_head_ = (log_ring_head_ + 1) % LOG_RING_SIZE;
  }
  // Newline
  log_ring_[log_ring_head_] = '\n';
  log_ring_head_ = (log_ring_head_ + 1) % LOG_RING_SIZE;
  xSemaphoreGive(log_ring_mutex_);
}

// ═════════════════════════════════════════════════════════════
// Main Component
// ═════════════════════════════════════════════════════════════
class UsbHidUpsComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void setup() override;
  void loop() override;

  // ── Public accessors for Web UI ───────────────────────────
  UpsData get_data() {
    UpsData snapshot;
    if (!data_mutex_) return snapshot;  // Not yet initialized
    xSemaphoreTake(data_mutex_, portMAX_DELAY);
    snapshot = data_;
    xSemaphoreGive(data_mutex_);
    // A hung USB transaction must not keep publishing old measurements.
    snapshot.expire_readings((uint32_t)(esp_timer_get_time() / 1000));
    return snapshot;
  }

  // Config accessors
  uint32_t get_power_fail_delay() const { return power_fail_delay_s_; }
  uint32_t get_battery_low_runtime() const { return battery_low_runtime_s_; }
  uint32_t get_battery_low_capacity() const { return battery_low_capacity_pct_; }

  void set_power_fail_delay(uint32_t s) { power_fail_delay_s_ = s; save_config_(); }
  void set_battery_low_runtime(uint32_t s) { battery_low_runtime_s_ = s; save_config_(); }
  void set_battery_low_capacity(uint32_t pct) { battery_low_capacity_pct_ = pct; save_config_(); }

  // ── UPS command dispatch ──────────────────────────────────
  // Callable from any task (button lambdas run on the main loop).
  // The command is queued and executed on the USB task; it is a
  // no-op if the UPS does not expose the matching FEATURE report.
  void queue_command(UpsCommand cmd) {
    if (!cmd_queue_) return;
    uint8_t c = (uint8_t)cmd;
    if (xQueueSend(cmd_queue_, &c, 0) != pdTRUE)
      ESP_LOGW(TAG, "Command queue full, dropped command %d", c);
  }

  // Capability flags — true once the report descriptor is parsed and
  // the matching FEATURE report is present. Let the UI hide unsupported
  // buttons if desired.
  bool supports_beeper() const { return supports_beeper_; }
  bool supports_battery_test() const { return supports_battery_test_; }
  bool supports_load_control() const { return supports_load_control_; }

  // Web UI auth — empty string means no auth required.
  // User is always "admin"; only password is stored.
  const char *get_password() const { return password_; }
  bool has_password() const { return password_[0] != '\0'; }
  void set_password(const char *pw) {
    strncpy(password_, pw, sizeof(password_) - 1);
    password_[sizeof(password_) - 1] = '\0';
    save_password_();
  }

 private:
  SemaphoreHandle_t data_mutex_ = nullptr;
  SemaphoreHandle_t ctrl_sem_ = nullptr;
  QueueHandle_t cmd_queue_ = nullptr;
  UpsData data_;
  std::atomic<bool> publish_pending_{false};

  // Command capabilities (set once report descriptor is parsed)
  bool supports_beeper_ = false;
  bool supports_battery_test_ = false;
  bool supports_load_control_ = false;

  // Non-zero while a battery self-test is running (millis deadline).
  // Power-fail logic is suppressed until then. See TEST_SUPPRESS_MS.
  uint32_t test_active_until_ms_ = 0;

  // USB host state
  usb_host_client_handle_t client_hdl_ = nullptr;
  usb_device_handle_t dev_hdl_ = nullptr;
  uint8_t dev_addr_ = 0;
  uint8_t hid_iface_num_ = 0;
  uint16_t hid_report_desc_len_ = 0;
  HidReportMap report_map_;
  HidReportCache poll_cache_;
  bool poll_cache_active_ = false;
  bool device_open_ = false;
  bool tripplite_2012_ = false;
  bool device_gone_ = false;
  usb_transfer_t *interrupt_xfer_ = nullptr;
  uint8_t interrupt_ep_ = 0;
  uint16_t interrupt_mps_ = 0;
  bool interrupt_pending_ = false;

  // Control transfer buffer
  static constexpr size_t CTRL_BUF_SIZE = 1024;
  usb_transfer_t *ctrl_xfer_ = nullptr;

  // Configurable thresholds (stored in NVS)
  uint32_t power_fail_delay_s_ = 60;
  uint32_t battery_low_runtime_s_ = 300;
  uint32_t battery_low_capacity_pct_ = 35;
  char password_[64] = {};  // Web UI password (empty = no auth)

  // ── NVS Config ────────────────────────────────────────────
  void load_config_() {
    nvs_handle_t nvs;
    if (nvs_open("ups_config", NVS_READONLY, &nvs) == ESP_OK) {
      nvs_get_u32(nvs, "pf_delay", &power_fail_delay_s_);
      nvs_get_u32(nvs, "bl_runtime", &battery_low_runtime_s_);
      nvs_get_u32(nvs, "bl_capacity", &battery_low_capacity_pct_);
      size_t pw_len = sizeof(password_);
      nvs_get_str(nvs, "password", password_, &pw_len);
      nvs_close(nvs);
      ESP_LOGI(TAG, "Config loaded: pf_delay=%lus, bl_runtime=%lus, bl_cap=%lu%%, auth=%s",
               power_fail_delay_s_, battery_low_runtime_s_, battery_low_capacity_pct_,
               password_[0] ? "enabled" : "disabled");
    } else {
      ESP_LOGI(TAG, "No saved config, using defaults");
    }
  }

  void save_password_() {
    nvs_handle_t nvs;
    if (nvs_open("ups_config", NVS_READWRITE, &nvs) == ESP_OK) {
      nvs_set_str(nvs, "password", password_);
      nvs_commit(nvs);
      nvs_close(nvs);
    }
  }

  void save_config_() {
    nvs_handle_t nvs;
    if (nvs_open("ups_config", NVS_READWRITE, &nvs) == ESP_OK) {
      nvs_set_u32(nvs, "pf_delay", power_fail_delay_s_);
      nvs_set_u32(nvs, "bl_runtime", battery_low_runtime_s_);
      nvs_set_u32(nvs, "bl_capacity", battery_low_capacity_pct_);
      nvs_commit(nvs);
      nvs_close(nvs);
    }
  }

  // ── USB Enum Filter (MUST return true or devices are silently skipped!) ──
  static bool enum_filter_allow_all_(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue) {
    ESP_LOGI(TAG, "Enum filter: ALLOW VID=%04X PID=%04X", dev_desc->idVendor, dev_desc->idProduct);
    return true;
  }

  // ── USB Host Library Task ─────────────────────────────────
  static void usb_lib_task_entry_(void *arg) {
    auto *self = static_cast<UsbHidUpsComponent *>(arg);
    self->usb_lib_task_();
  }

  void usb_lib_task_() {
    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    #ifdef CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK
    host_config.enum_filter_cb = enum_filter_allow_all_;
    #endif

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(err));
      log_ring_append_("FATAL: USB host install failed");
      vTaskDelete(nullptr);
      return;
    }
    ESP_LOGI(TAG, "USB host library installed");
    log_ring_append_("USB host library installed");

    while (true) {
      usb_host_lib_handle_events(portMAX_DELAY, nullptr);
    }
  }

  // ── USB Monitor Task ──────────────────────────────────────
  static void usb_mon_task_entry_(void *arg) {
    auto *self = static_cast<UsbHidUpsComponent *>(arg);
    self->usb_mon_task_();
  }

  // Client event callback
  static void client_event_cb_(const usb_host_client_event_msg_t *msg, void *arg) {
    auto *self = static_cast<UsbHidUpsComponent *>(arg);
    switch (msg->event) {
      case USB_HOST_CLIENT_EVENT_NEW_DEV:
        self->dev_addr_ = msg->new_dev.address;
        ESP_LOGI(TAG, "New USB device at address %d", self->dev_addr_);
        break;
      case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB device disconnected");
        log_ring_append_("USB device disconnected");
        // Defer releasing USB handles until in-flight callbacks have drained.
        self->device_gone_ = true;
        xSemaphoreTake(self->data_mutex_, portMAX_DELAY);
        self->data_.connected = false;
        xSemaphoreGive(self->data_mutex_);
        break;
    }
  }

  void usb_mon_task_() {
    // Small delay to let USB host library fully initialize
    vTaskDelay(pdMS_TO_TICKS(500));

    // Register client
    usb_host_client_config_t client_config = {};
    client_config.is_synchronous = false;
    client_config.max_num_event_msg = 5;
    client_config.async.client_event_callback = client_event_cb_;
    client_config.async.callback_arg = this;

    esp_err_t err = usb_host_client_register(&client_config, &client_hdl_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Client register failed: %s", esp_err_to_name(err));
      log_ring_append_("FATAL: Client register failed");
      vTaskDelete(nullptr);
      return;
    }
    ESP_LOGI(TAG, "USB client registered successfully");
    log_ring_append_("USB client registered");

    // Allocate control transfer
    err = usb_host_transfer_alloc(CTRL_BUF_SIZE, 0, &ctrl_xfer_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Transfer alloc failed: %s", esp_err_to_name(err));
      log_ring_append_("FATAL: Transfer alloc failed");
      vTaskDelete(nullptr);
      return;
    }
    ctrl_xfer_->callback = ctrl_xfer_cb_;
    ctrl_xfer_->context = this;

    ESP_LOGI(TAG, "Waiting for USB devices... (hub support: %s)",
    #ifdef CONFIG_USB_HOST_HUBS_SUPPORTED
      "YES"
    #else
      "NO"
    #endif
    );
    log_ring_append_("Waiting for USB devices...");

    uint32_t heartbeat = 0;
    while (true) {
      // Pump client events
      usb_host_client_handle_events(client_hdl_, 200);

      if (device_gone_) {
        handle_disconnect_();
        continue;
      }

      // New device detected?
      if (dev_addr_ != 0 && !device_open_) {
        handle_new_device_();
      }

      // If connected, poll UPS data
      if (device_open_ && !device_gone_) {
        poll_ups_data_();
        // Wait out the poll interval in small slices so queued commands
        // (beeper, test, …) are dispatched within ~100ms instead of 5s.
        for (uint32_t waited = 0; waited < POLL_INTERVAL_MS && device_open_; waited += 100) {
          process_commands_();
          usb_host_client_handle_events(client_hdl_, pdMS_TO_TICKS(100));
          if (device_gone_) break;
        }
        process_commands_();
        heartbeat = 0;
      } else {
        // Heartbeat + active device scan every 10s when no device connected
        heartbeat++;
        if (heartbeat >= 50) {  // 50 * 200ms = 10s
          // Try to actively list devices known to the USB host library
          int num_devs = 0;
          uint8_t dev_addrs[8] = {};
          usb_host_device_addr_list_fill(sizeof(dev_addrs), dev_addrs, &num_devs);
          ESP_LOGW(TAG, "USB scan: %d device(s) known to host lib, cb_addr=%d, open=%d",
                   num_devs, dev_addr_, device_open_);
          char msg[80];
          snprintf(msg, sizeof(msg), "USB scan: %d devices, cb_addr=%d", num_devs, dev_addr_);
          log_ring_append_(msg);

          if (num_devs > 0) {
            for (int i = 0; i < num_devs; i++) {
              ESP_LOGW(TAG, "  Device at address %d (trying to open...)", dev_addrs[i]);
              // If callback missed it, try opening directly
              if (dev_addr_ == 0 && !device_open_) {
                dev_addr_ = dev_addrs[i];
              }
            }
          }
          heartbeat = 0;
        }
      }
    }
  }

  // ── Device Enumeration ────────────────────────────────────
  void handle_new_device_() {
    esp_err_t err = usb_host_device_open(client_hdl_, dev_addr_, &dev_hdl_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to open device: %s", esp_err_to_name(err));
      dev_addr_ = 0;
      return;
    }

    // Get device descriptor
    const usb_device_desc_t *desc;
    usb_host_get_device_descriptor(dev_hdl_, &desc);

    ESP_LOGI(TAG, "Device: VID=0x%04X PID=0x%04X", desc->idVendor, desc->idProduct);
    log_ring_append_("USB device opened");

    char msg[80];
    snprintf(msg, sizeof(msg), "VID=0x%04X PID=0x%04X", desc->idVendor, desc->idProduct);
    log_ring_append_(msg);

    tripplite_2012_ = desc->idVendor == TRIPPLITE_VID && desc->idProduct == TRIPPLITE_PID;
    // Other devices require their own validation.
    if (desc->idVendor != CYBERPOWER_VID && !tripplite_2012_) {
      ESP_LOGW(TAG, "Not a CyberPower device (VID 0x%04X), will try anyway", desc->idVendor);
      log_ring_append_("Warning: non-CyberPower VID, attempting HID Power Device anyway");
    }

    // Get string descriptors into local buffers — data_ is protected by mutex,
    // so write to stack first and copy under lock below.
    char tmp_model[64] = {};
    char tmp_serial[64] = {};
    get_string_descriptor_(desc->iProduct, tmp_model, sizeof(tmp_model));
    get_string_descriptor_(desc->iSerialNumber, tmp_serial, sizeof(tmp_serial));
    ESP_LOGI(TAG, "Model: %s, Serial: %s", tmp_model, tmp_serial);

    // Find HID interface
    const usb_config_desc_t *config_desc;
    usb_host_get_active_config_descriptor(dev_hdl_, &config_desc);

    if (!find_hid_interface_(config_desc)) {
      ESP_LOGE(TAG, "No HID interface found");
      log_ring_append_("ERROR: No HID interface found");
      usb_host_device_close(client_hdl_, dev_hdl_);
      dev_addr_ = 0;
      return;
    }

    // Claim the HID interface
    err = usb_host_interface_claim(client_hdl_, dev_hdl_, hid_iface_num_, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to claim interface %d: %s", hid_iface_num_, esp_err_to_name(err));
      usb_host_device_close(client_hdl_, dev_hdl_);
      dev_addr_ = 0;
      return;
    }

    ESP_LOGI(TAG, "HID interface %d claimed, report desc len=%d", hid_iface_num_, hid_report_desc_len_);

    // Read and parse HID report descriptor
    if (!read_hid_report_descriptor_()) {
      ESP_LOGE(TAG, "Failed to read/parse HID report descriptor");
      log_ring_append_("ERROR: HID report descriptor parse failed");
      usb_host_interface_release(client_hdl_, dev_hdl_, hid_iface_num_);
      usb_host_device_close(client_hdl_, dev_hdl_);
      dev_addr_ = 0;
      return;
    }

    snprintf(msg, sizeof(msg), "HID parsed: %d fields found", (int)report_map_.fields.size());
    log_ring_append_(msg);
    ESP_LOGI(TAG, "%s", msg);

    // Detect which NUT-style commands this UPS accepts
    probe_capabilities_();

    xSemaphoreTake(data_mutex_, portMAX_DELAY);
    strncpy(data_.model, tmp_model, sizeof(data_.model) - 1);
    data_.model[sizeof(data_.model) - 1] = '\0';
    strncpy(data_.serial, tmp_serial, sizeof(data_.serial) - 1);
    data_.serial[sizeof(data_.serial) - 1] = '\0';
    data_.connected = true;

    // Extract VA rating from model name (e.g. "BR1200ELCD" → 1200)
    // CyberPower naming: BR/CP/PR + digits (VA rating) + suffix
    if (!std::isfinite(data_.rating_power_va) || data_.rating_power_va <= 0) {
      const char *p = data_.model;
      while (*p && !(*p >= '0' && *p <= '9')) p++;  // skip letters
      if (*p) {
        data_.rating_power_va = (float)atoi(p);
        ESP_LOGI(TAG, "Rating VA from model name: %.0f", data_.rating_power_va);
      }
    }
    xSemaphoreGive(data_mutex_);

    device_open_ = true;
    if (interrupt_ep_ && interrupt_mps_) {
      if (usb_host_transfer_alloc(interrupt_mps_, 0, &interrupt_xfer_) == ESP_OK) {
        interrupt_xfer_->device_handle = dev_hdl_;
        interrupt_xfer_->bEndpointAddress = interrupt_ep_;
        interrupt_xfer_->num_bytes = interrupt_mps_;
        interrupt_xfer_->callback = interrupt_cb_;
        interrupt_xfer_->context = this;
        interrupt_pending_ = usb_host_transfer_submit(interrupt_xfer_) == ESP_OK;
        ESP_LOGI(TAG, "HID interrupt polling: endpoint 0x%02x, packet %u, active=%d",
                 interrupt_ep_, interrupt_mps_, interrupt_pending_);
      }
    }
    log_ring_append_("UPS connected and ready");
  }

  static void interrupt_cb_(usb_transfer_t *transfer) {
    auto *self = static_cast<UsbHidUpsComponent *>(transfer->context);
    self->interrupt_pending_ = false;
    // Keep an interrupt-IN request queued, as a normal HID host does.
    // Measurements still come from the explicitly polled feature reports.
    if (!self->device_gone_ && self->device_open_ &&
        transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
      self->interrupt_pending_ = usb_host_transfer_submit(transfer) == ESP_OK;
    }
  }

  void handle_disconnect_() {
    if (interrupt_xfer_) {
      if (interrupt_pending_) {
        usb_host_endpoint_halt(dev_hdl_, interrupt_ep_);
        usb_host_endpoint_flush(dev_hdl_, interrupt_ep_);
        // The completion arrives through the client event pump on the next loop.
        return;
      }
      usb_host_transfer_free(interrupt_xfer_);
      interrupt_xfer_ = nullptr;
    }
    if (device_open_) {
      usb_host_interface_release(client_hdl_, dev_hdl_, hid_iface_num_);
      usb_host_device_close(client_hdl_, dev_hdl_);
    }
    device_open_ = false;
    device_gone_ = false;
    interrupt_ep_ = 0;
    interrupt_mps_ = 0;
    dev_addr_ = 0;
    dev_hdl_ = nullptr;
    report_map_.fields.clear();
    supports_beeper_ = supports_battery_test_ = supports_load_control_ = false;
    test_active_until_ms_ = 0;
    if (cmd_queue_) xQueueReset(cmd_queue_);  // drop pending commands

    xSemaphoreTake(data_mutex_, portMAX_DELAY);
    data_.connected = false;
    data_.power_state = PowerState::NORMAL;
    data_.power_fail_event_sent = false;
    xSemaphoreGive(data_mutex_);

    publish_pending_ = true;
  }

  // ── Find HID interface in config descriptor ───────────────
  bool find_hid_interface_(const usb_config_desc_t *config_desc) {
    const uint8_t *p = (const uint8_t *)config_desc;
    hid_report_desc_len_ = 0;
    interrupt_ep_ = 0;
    interrupt_mps_ = 0;
    size_t total_len = config_desc->wTotalLength;
    size_t offset = 0;

    while (offset < total_len) {
      uint8_t desc_len = p[offset];
      uint8_t desc_type = p[offset + 1];
      if (desc_len == 0) break;

      if (desc_type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
        const usb_intf_desc_t *iface = (const usb_intf_desc_t *)&p[offset];
        // HID class = 0x03
        if (iface->bInterfaceClass == 0x03) {
          hid_iface_num_ = iface->bInterfaceNumber;

          // Look for HID descriptor right after interface descriptor
          size_t next = offset + desc_len;
          while (next < total_len) {
            uint8_t nd_len = p[next];
            uint8_t nd_type = p[next + 1];
            if (nd_len == 0) break;

            if (nd_type == USB_DT_HID && nd_len >= 9) {
              // HID descriptor: byte 7-8 = wDescriptorLength (report desc length)
              hid_report_desc_len_ = p[next + 7] | (p[next + 8] << 8);
            }
            if (nd_type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && nd_len >= sizeof(usb_ep_desc_t)) {
              auto *ep = (const usb_ep_desc_t *)&p[next];
              if ((ep->bEndpointAddress & 0x80) && (ep->bmAttributes & 3) == 3) {
                interrupt_ep_ = ep->bEndpointAddress;
                interrupt_mps_ = ep->wMaxPacketSize & 0x7ff;
              }
            }

            // Stop if we hit another interface descriptor
            if (nd_type == USB_B_DESCRIPTOR_TYPE_INTERFACE) break;
            next += nd_len;
          }

          if (hid_report_desc_len_ != 0) return true;

          // Fallback: assume 256 bytes if HID descriptor not found
          ESP_LOGW(TAG, "HID descriptor not found, assuming report desc len=256");
          hid_report_desc_len_ = 256;
          return true;
        }
      }
      offset += desc_len;
    }
    return false;
  }

  // ── Control Transfer Helpers ──────────────────────────────
  static void ctrl_xfer_cb_(usb_transfer_t *transfer) {
    auto *self = static_cast<UsbHidUpsComponent *>(transfer->context);
    xSemaphoreGive(self->ctrl_sem_);
  }

  // Synchronous control transfer — blocks until complete
  // actual_len, when given, receives the number of bytes actually
  // returned by an IN transfer. A device may answer with fewer bytes
  // than requested; without this the caller cannot tell, and anything
  // past that point in its buffer is whatever was there before.
  esp_err_t ctrl_transfer_sync_(uint8_t bmRequestType, uint8_t bRequest,
                                 uint16_t wValue, uint16_t wIndex,
                                 uint16_t wLength, uint8_t *data_out = nullptr,
                                 size_t *actual_len = nullptr) {
    if (device_gone_ || !dev_hdl_) return ESP_ERR_INVALID_STATE;
    ctrl_xfer_->device_handle = dev_hdl_;
    ctrl_xfer_->bEndpointAddress = 0;

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl_xfer_->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;

    // If OUT transfer, copy data after setup packet
    if (!(bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) && data_out && wLength > 0) {
      memcpy(ctrl_xfer_->data_buffer + sizeof(usb_setup_packet_t), data_out, wLength);
    }

    ctrl_xfer_->num_bytes = sizeof(usb_setup_packet_t) + wLength;

    // Submit and pump client events while waiting for completion.
    // The callback is delivered via usb_host_client_handle_events(),
    // so we must keep calling it or the callback never fires (deadlock).
    xSemaphoreTake(ctrl_sem_, 0);  // Clear semaphore
    esp_err_t err = usb_host_transfer_submit_control(client_hdl_, ctrl_xfer_);
    if (err != ESP_OK) return err;

    bool got_it = false;
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(5000)) {
      if (xSemaphoreTake(ctrl_sem_, 0) == pdTRUE) {
        got_it = true;
        break;
      }
      usb_host_client_handle_events(client_hdl_, pdMS_TO_TICKS(50));
    }

    if (!got_it) {
      ESP_LOGE(TAG, "Control transfer timeout");
      return ESP_ERR_TIMEOUT;
    }

    if (ctrl_xfer_->status != USB_TRANSFER_STATUS_COMPLETED) {
      ESP_LOGE(TAG, "Control transfer failed, status=%d", ctrl_xfer_->status);
      return ESP_FAIL;
    }

    // Copy response data back
    if ((bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN) && data_out && wLength > 0) {
      // actual_num_bytes covers the setup packet too. Guard the
      // subtraction: a stack that reports less than the setup size would
      // otherwise wrap size_t into a huge value.
      size_t total = ctrl_xfer_->actual_num_bytes;
      size_t actual = (total > sizeof(usb_setup_packet_t))
                    ? total - sizeof(usb_setup_packet_t) : 0;
      if (actual > wLength) actual = wLength;
      memcpy(data_out, ctrl_xfer_->data_buffer + sizeof(usb_setup_packet_t), actual);
      if (actual_len) *actual_len = actual;
    } else if (actual_len) {
      *actual_len = 0;
    }

    return ESP_OK;
  }

  // ── Get String Descriptor ─────────────────────────────────
  void get_string_descriptor_(uint8_t index, char *buf, size_t buf_size) {
    buf[0] = '\0';
    if (index == 0) return;

    uint8_t tmp[128];
    esp_err_t err = ctrl_transfer_sync_(
      USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
      USB_B_REQUEST_GET_DESCRIPTOR,
      (USB_B_DESCRIPTOR_TYPE_STRING << 8) | index,
      0x0409,  // English
      sizeof(tmp), tmp);

    if (err != ESP_OK || tmp[0] < 4) return;

    // Convert UTF-16LE to ASCII
    size_t str_len = (tmp[0] - 2) / 2;
    if (str_len >= buf_size) str_len = buf_size - 1;
    for (size_t i = 0; i < str_len; i++) {
      uint16_t ch = tmp[2 + i * 2] | (tmp[3 + i * 2] << 8);
      buf[i] = (ch < 128) ? (char)ch : '?';
    }
    buf[str_len] = '\0';
  }

  // ── Read & Parse HID Report Descriptor ────────────────────
  bool read_hid_report_descriptor_() {
    size_t desc_len = hid_report_desc_len_;
    if (desc_len > CTRL_BUF_SIZE - sizeof(usb_setup_packet_t))
      desc_len = CTRL_BUF_SIZE - sizeof(usb_setup_packet_t);

    // calloc, not malloc: a short transfer leaves the tail untouched,
    // and parsing uninitialised heap yields a different report map on
    // every boot — fields silently disappear off the end.
    uint8_t *desc_buf = (uint8_t *)calloc(1, desc_len);
    if (!desc_buf) return false;

    // GET_DESCRIPTOR (HID Report Descriptor) — Standard request to interface.
    // Retried, because a short answer here costs whole fields: the tail
    // of a Power Device descriptor holds the command reports (Test,
    // DelayBeforeShutdown), so losing it silently disables commands.
    size_t got = 0;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
      memset(desc_buf, 0, desc_len);
      got = 0;
      err = ctrl_transfer_sync_(
        USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
        USB_B_REQUEST_GET_DESCRIPTOR,
        (USB_DT_HID_REPORT << 8),
        hid_iface_num_,
        desc_len, desc_buf, &got);

      if (err == ESP_OK && got == desc_len) break;

      ESP_LOGW(TAG, "HID report descriptor read attempt %d: %s, got %u of %u bytes",
               attempt, esp_err_to_name(err), (unsigned) got, (unsigned) desc_len);
      vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to get HID report descriptor: %s", esp_err_to_name(err));
      free(desc_buf);
      return false;
    }

    if (got < desc_len) {
      // Parse what arrived rather than nothing, but say so: an
      // incomplete map is exactly how sensors or commands go missing.
      char msg[96];
      snprintf(msg, sizeof(msg), "WARN: HID desc short read %u/%u bytes - map may be incomplete",
               (unsigned) got, (unsigned) desc_len);
      ESP_LOGW(TAG, "%s", msg);
      log_ring_append_(msg);
    }

    ESP_LOGI(TAG, "HID Report Desc: %u of %u bytes", (unsigned) got, (unsigned) desc_len);

    // Parse only what was actually received.
    report_map_ = {};
    bool ok = parse_report_descriptor(desc_buf, got, report_map_);
    free(desc_buf);

    if (ok) {
      if (tripplite_2012_) {
        size_t corrected = fix_tripplite_power_units(report_map_);
        if (corrected) ESP_LOGI(TAG, "Corrected Tripp Lite power exponent for %u field(s)", (unsigned)corrected);
      }
      ESP_LOGI(TAG, "Parsed %d HID fields", (int)report_map_.fields.size());
      dump_report_map_();
    }

    return ok;
  }

  // ── Diagnostic: dump the parsed HID report map ────────────
  // Purely diagnostic — reads nothing extra from the device and changes
  // no behaviour. Output goes to the ESPHome log AND the web UI /log
  // page, because a burst this size can outrun the API log buffer.
  //
  // Why this exists: parse_report_descriptor() carries no collection
  // context (it resets local state on COLLECTION). A usage that appears
  // inside several collections therefore lands in the map several times
  // with nothing to tell the copies apart, and find() returns whichever
  // comes first in descriptor order. Voltage (0x0030) is exactly such a
  // usage — the HID Power Device Class places it under UPS.Input,
  // UPS.Output and UPS.PowerSummary alike. This dump shows how many
  // candidates exist per usage and which one is actually being read.
  static const char *report_type_name_(ReportType t) {
    switch (t) {
      case ReportType::INPUT:   return "INPUT";
      case ReportType::OUTPUT:  return "OUTPUT";
      case ReportType::FEATURE: return "FEATURE";
    }
    return "?";
  }

  void log_both_(const char *s) {
    ESP_LOGI(TAG, "%s", s);
    log_ring_append_(s);
  }

  // One line per lookup the poller actually performs, so the dump can be
  // read as "this is where each sensor gets its number from".
  // eff= is the exponent after resolving the unit's built-in scale; that
  // is the factor of ten actually applied.
  void dump_usage_resolution_(const char *label, uint16_t page, uint16_t usage,
                              uint16_t collection) {
    char msg[192];
    const HidField *picked = report_map_.find(page, usage, collection);
    int candidates = report_map_.count(page, usage);

    if (!picked) {
      snprintf(msg, sizeof(msg),
               "  %-16s %04X/%04X coll=%04X -> NOT FOUND  [%d on this usage]",
               label, (unsigned) page, (unsigned) usage,
               (unsigned) collection, candidates);
      log_both_(msg);
      return;
    }

    snprintf(msg, sizeof(msg),
             "  %-16s %04X/%04X coll=%04X -> rpt=%u/%s off=%u len=%u exp=%d unit=%08X eff=%d  [%d on this usage]",
             label, (unsigned) page, (unsigned) usage, (unsigned) collection,
             (unsigned) picked->report_id, report_type_name_(picked->report_type),
             (unsigned) picked->bit_offset, (unsigned) picked->bit_size,
             (int) picked->unit_exponent, (unsigned) picked->unit,
             (int) hid_unit_exponent(picked->unit, picked->unit_exponent),
             candidates);
    log_both_(msg);

    // Usage occurs more than once: list every copy with its collection,
    // so a wrong pick on an untested model can be spotted from the log
    // alone.
    if (candidates > 1) {
      for (auto &f : report_map_.fields) {
        if (f.usage_page != page || f.usage != usage) continue;
        snprintf(msg, sizeof(msg),
                 "      coll=%04X rpt=%u/%-7s off=%3u len=%2u log=%d..%d exp=%d unit=%08X%s",
                 (unsigned) f.collection, (unsigned) f.report_id,
                 report_type_name_(f.report_type),
                 (unsigned) f.bit_offset, (unsigned) f.bit_size,
                 (int) f.logical_min, (int) f.logical_max,
                 (int) f.unit_exponent, (unsigned) f.unit,
                 (&f == picked) ? "  <-- used" : "");
        log_both_(msg);
      }
    }
  }

  void dump_report_map_() {
    char msg[192];

    // Resolution summary first, and to both sinks. The ESPHome API log
    // buffer drops bursts (45 fields at once cost us the whole tail of
    // this dump once already), so the part that actually answers the
    // question has to come out before the bulk.
    snprintf(msg, sizeof(msg), "===== usage resolution (%d fields parsed) =====",
             (int) report_map_.fields.size());
    log_both_(msg);
    dump_usage_resolution_("UtilityVoltage", USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_VOLTAGE, PD_COLL_INPUT);
    dump_usage_resolution_("OutputVoltage",  USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_VOLTAGE, PD_COLL_OUTPUT);
    dump_usage_resolution_("OutputPower", USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_ACTIVE_POWER, PD_COLL_OUTPUT);
    dump_usage_resolution_("InputFrequency", USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_FREQUENCY, PD_COLL_INPUT);
    dump_usage_resolution_("BatteryVoltage", USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_VOLTAGE, tripplite_2012_ ? 0x0010 : PD_COLL_POWER_SUMMARY);
    dump_usage_resolution_("RatingVoltage",  USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_CONFIG_VOLTAGE, PD_COLL_INPUT);
    dump_usage_resolution_("PercentLoad",    USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_PERCENT_LOAD, 0);
    dump_usage_resolution_("RatingPowerVA",  USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_CONFIG_APPARENT_POWER, 0);
    dump_usage_resolution_("RatingPowerW",   USAGE_PAGE_POWER_DEVICE,
                           PD_USAGE_CONFIG_ACTIVE_POWER, 0);
    dump_usage_resolution_("RemainingCap",   USAGE_PAGE_BATTERY,
                           BAT_USAGE_REMAINING_CAPACITY, 0);
    dump_usage_resolution_("RuntimeToEmpty", USAGE_PAGE_BATTERY,
                           BAT_USAGE_RUNTIME_TO_EMPTY, 0);
    log_both_("===== full field table follows on the web UI /log page =====");

    // Bulk table to the ring buffer only: it holds 8 KB, enough for the
    // whole map, and writing it there cannot starve the API logger.
    int i = 0;
    for (auto &f : report_map_.fields) {
      snprintf(msg, sizeof(msg),
               "  [%02d] %04X/%04X coll=%04X rpt=%u/%-7s off=%3u len=%2u log=%d..%d exp=%d unit=%08X",
               i++, (unsigned) f.usage_page, (unsigned) f.usage,
               (unsigned) f.collection,
               (unsigned) f.report_id, report_type_name_(f.report_type),
               (unsigned) f.bit_offset, (unsigned) f.bit_size,
               (int) f.logical_min, (int) f.logical_max,
               (int) f.unit_exponent, (unsigned) f.unit);
      log_ring_append_(msg);
    }
  }

  // ── Read a single HID Feature/Input Report ────────────────
  bool read_hid_report_(uint8_t report_id, ReportType type, uint8_t *buf, size_t buf_size) {
    uint8_t hid_type = (type == ReportType::INPUT) ? HID_REPORT_TYPE_INPUT : HID_REPORT_TYPE_FEATURE;
    uint16_t wValue = (hid_type << 8) | report_id;
    size_t actual = 0;

    esp_err_t err = ctrl_transfer_sync_(
      USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
      HID_REQ_GET_REPORT,
      wValue,
      hid_iface_num_,
      buf_size, buf, &actual);

    return err == ESP_OK && actual == buf_size && (report_id == 0 || buf[0] == report_id);
  }

  // ── Read a single field value from UPS ────────────────────
  bool read_field_value_(const HidField *field, int32_t &value) {
    // Commands always get a fresh report; only polling shares a cache.
    HidReportCache fresh;
    auto &cache = poll_cache_active_ ? poll_cache_ : fresh;
    return cache.read_field(report_map_, field, value,
      [this](uint8_t id, ReportType type, uint8_t *buf, size_t len) {
        return read_hid_report_(id, type, buf, len);
      });
  }

  // ── Read a field and convert it to its physical value ─────
  // Use this for measurements. Status bits carry no unit and are read
  // with read_field_value_ instead.
  bool read_field_scaled_(const HidField *field, float &value) {
    int32_t raw;
    if (!read_field_value_(field, raw)) return false;
    value = scale_field_value(*field, raw);
    return true;
  }

  // ── Write a single HID Feature Report (SET_REPORT) ────────
  bool write_hid_report_(uint8_t report_id, ReportType type, uint8_t *buf, size_t len) {
    uint8_t hid_type = (type == ReportType::INPUT) ? HID_REPORT_TYPE_INPUT : HID_REPORT_TYPE_FEATURE;
    uint16_t wValue = (hid_type << 8) | report_id;

    esp_err_t err = ctrl_transfer_sync_(
      USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
      HID_REQ_SET_REPORT,
      wValue,
      hid_iface_num_,
      len, buf);

    return (err == ESP_OK);
  }

  // ── Write a single field value to the UPS ─────────────────
  // Read-modify-write: fetches the current FEATURE report, sets only
  // this field's bits, and writes it back — so sibling fields sharing
  // the same report ID are preserved.
  bool set_field_value_(const HidField *field, int32_t value) {
    if (!field) return false;

    // Report data size (max bit offset + size across this report ID)
    size_t report_bytes = 0;
    for (auto &f : report_map_.fields) {
      if (f.report_id == field->report_id && f.report_type == field->report_type) {
        size_t end = (f.bit_offset + f.bit_size + 7) / 8;
        if (end > report_bytes) report_bytes = end;
      }
    }
    if (report_bytes == 0) report_bytes = 8;

    // Buffer layout matches GET_REPORT: [report_id][data...]
    size_t xfer_bytes = report_bytes + 1;
    if (xfer_bytes > 63) xfer_bytes = 63;

    uint8_t report_buf[64] = {};
    // Seed with current contents (best effort; zero-fill if read fails)
    if (!read_hid_report_(field->report_id, field->report_type, report_buf, xfer_bytes))
      memset(report_buf, 0, sizeof(report_buf));

    report_buf[0] = field->report_id;
    encode_field_value(report_buf + 1, *field, value);

    return write_hid_report_(field->report_id, field->report_type, report_buf, xfer_bytes);
  }

  // ── Execute a single queued command (USB task context) ────
  void execute_command_(UpsCommand cmd) {
    if (!device_open_) {
      ESP_LOGW(TAG, "Command %d ignored — UPS not connected", (int)cmd);
      return;
    }

    const HidField *f = nullptr;
    int32_t value = 0;
    const char *name = "?";

    switch (cmd) {
      case UpsCommand::BEEPER_MUTE:
        f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_AUDIBLE_ALARM_CTRL);
        value = 3; name = "beeper.mute"; break;
      case UpsCommand::BEEPER_ENABLE:
        f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_AUDIBLE_ALARM_CTRL);
        value = 2; name = "beeper.enable"; break;
      case UpsCommand::BEEPER_DISABLE:
        f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_AUDIBLE_ALARM_CTRL);
        value = 1; name = "beeper.disable"; break;
      case UpsCommand::TEST_BATTERY_START:
        f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_TEST_CMD);
        value = 1; name = "test.battery.start"; break;
      case UpsCommand::TEST_BATTERY_STOP:
        f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_TEST_CMD);
        value = 3; name = "test.battery.stop"; break;
      case UpsCommand::SHUTDOWN_STOP:
        f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_DELAY_BEFORE_SHUTDOWN);
        value = -1; name = "shutdown.stop"; break;
      case UpsCommand::SHUTDOWN_REBOOT:
        f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_DELAY_BEFORE_REBOOT);
        value = REBOOT_DELAY_S; name = "shutdown.reboot"; break;
      case UpsCommand::LOAD_OFF_DELAY:
        f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_DELAY_BEFORE_SHUTDOWN);
        value = LOAD_OFF_DELAY_S; name = "load.off.delay"; break;
    }

    char msg[80];
    if (!f) {
      ESP_LOGW(TAG, "Command '%s' not supported by this UPS", name);
      snprintf(msg, sizeof(msg), "Command '%s' NOT supported", name);
      log_ring_append_(msg);
      return;
    }

    bool ok = set_field_value_(f, value);
    ESP_LOGI(TAG, "Command '%s' (=%ld) -> %s", name, (long)value, ok ? "OK" : "FAILED");
    snprintf(msg, sizeof(msg), "Command '%s' %s", name, ok ? "sent" : "FAILED");
    log_ring_append_(msg);

    // A running self-test puts the UPS on battery deliberately — arm the
    // suppression window so it is not mistaken for a real power failure.
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (ok && cmd == UpsCommand::TEST_BATTERY_START) {
      test_active_until_ms_ = now_ms + TEST_SUPPRESS_MS;
      ESP_LOGI(TAG, "Self-test started — power-fail logic suppressed for %lus",
               (unsigned long)(TEST_SUPPRESS_MS / 1000));
      log_ring_append_("Self-test: power-fail logic suppressed");
    } else if (cmd == UpsCommand::TEST_BATTERY_STOP) {
      test_active_until_ms_ = 0;
    }
  }

  // ── Drain and execute all queued commands (USB task) ──────
  void process_commands_() {
    if (!cmd_queue_) return;
    uint8_t c;
    while (xQueueReceive(cmd_queue_, &c, 0) == pdTRUE) {
      execute_command_((UpsCommand)c);
    }
  }

  // ── Probe which command reports this UPS exposes ──────────
  void probe_capabilities_() {
    supports_beeper_ =
      report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_AUDIBLE_ALARM_CTRL) != nullptr;
    supports_battery_test_ =
      report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_TEST_CMD) != nullptr;
    supports_load_control_ =
      report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_DELAY_BEFORE_SHUTDOWN) != nullptr;

    ESP_LOGI(TAG, "UPS command support: beeper=%s test=%s load-control=%s",
             supports_beeper_ ? "YES" : "no",
             supports_battery_test_ ? "YES" : "no",
             supports_load_control_ ? "YES" : "no");
    char msg[80];
    snprintf(msg, sizeof(msg), "Cmd support: beeper=%s test=%s load=%s",
             supports_beeper_ ? "YES" : "no",
             supports_battery_test_ ? "YES" : "no",
             supports_load_control_ ? "YES" : "no");
    log_ring_append_(msg);
  }

  // ── Poll all UPS data ─────────────────────────────────────
  // USB control transfers take ~100-200ms each × ~15 fields = ~2-3s total.
  // We build a local snapshot WITHOUT holding data_mutex_ (USB transfers would
  // otherwise starve web UI and template sensor readers). Mutex is only taken
  // briefly at the start (to seed from current state) and at the end (atomic commit).
  void poll_ups_data_() {
    UpsData tmp;

    // Seed local copy from current data_ (preserves model/serial/connected/state-machine fields)
    xSemaphoreTake(data_mutex_, portMAX_DELAY);
    tmp = data_;
    xSemaphoreGive(data_mutex_);

    // Failed or unsupported reads are unavailable, never a previous sample.
    tmp.invalidate_readings();
    poll_cache_.clear();
    poll_cache_active_ = true;
    int32_t val;
    float   fval;

    // ── Sensor values (no mutex held — transfers can take seconds) ──
    //
    // Voltage (0x30) and ConfigVoltage (0x40) each occur once per
    // collection, so both are looked up with the collection that gives
    // them their meaning. Without it the first copy in descriptor order
    // wins, which on a BR1200ELCD is the PowerSummary one — the battery.
    // That is how mains voltage came to read a rock-steady 252 V: the
    // raw 252 was a float-charged 24 V pack at 25.2 V.

    auto *f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_VOLTAGE, PD_COLL_INPUT);
    if (f && read_field_scaled_(f, fval)) tmp.utility_voltage = fval;

    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_VOLTAGE, PD_COLL_OUTPUT);
    if (f && read_field_scaled_(f, fval)) {
      tmp.output_voltage = fval;
    }

    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_FREQUENCY, PD_COLL_INPUT);
    if (f && read_field_scaled_(f, fval)) tmp.input_frequency = fval;

    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_ACTIVE_POWER, PD_COLL_OUTPUT);
    if (f && read_field_scaled_(f, fval)) tmp.output_power = fval;

    // NUT tripplite-hid: UPS.BatterySystem.Battery.Voltage, corrected by 0.1.
    // On 09ae:2012 PowerSummary.Voltage is a mains reading, not the battery.
    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_VOLTAGE,
                        tripplite_2012_ ? 0x0010 : PD_COLL_POWER_SUMMARY);
    if (f && read_field_scaled_(f, fval))
      tmp.battery_voltage = fval * (tripplite_2012_ ? 0.1f : 1.0f);

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_REMAINING_CAPACITY);
    if (f && read_field_scaled_(f, fval)) tmp.battery_capacity = fval;

    f = report_map_.find(USAGE_PAGE_BATTERY, BAT_USAGE_RUNTIME_TO_EMPTY);
    if (f && read_field_scaled_(f, fval)) tmp.remaining_runtime_sec = fval;

    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_PERCENT_LOAD);
    if (f && read_field_scaled_(f, fval)) tmp.load_percent = fval;

    // Nominal mains voltage lives in the Input collection; the
    // PowerSummary copy is the nominal *battery* voltage (24 V here).
    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_CONFIG_VOLTAGE, PD_COLL_INPUT);
    if (f && read_field_scaled_(f, fval)) tmp.rating_voltage = fval;

    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_CONFIG_APPARENT_POWER);
    if (f && read_field_scaled_(f, fval)) tmp.rating_power_va = fval;
    // rating_power_va may also come from model name (set during connect)

    // Nameplate active power. Reporting it removes the need to guess a
    // power factor when converting percent load into watts.
    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_CONFIG_ACTIVE_POWER);
    if (f && read_field_scaled_(f, fval)) tmp.rating_power_w = fval;

    // ── Status bits: shared reports are read once per cycle ──
    auto read_status = [&](uint16_t page, uint16_t usage, bool &state, bool &valid) {
      const auto *field = report_map_.find(page, usage);
      valid = field && read_field_value_(field, val);
      if (valid) state = val != 0;
    };
    read_status(USAGE_PAGE_BATTERY, BAT_USAGE_AC_PRESENT, tmp.ac_present, tmp.ac_present_valid);
    read_status(USAGE_PAGE_BATTERY, BAT_USAGE_DISCHARGING, tmp.on_battery, tmp.on_battery_valid);
    read_status(USAGE_PAGE_BATTERY, BAT_USAGE_CHARGING, tmp.charging, tmp.charging_valid);
    read_status(USAGE_PAGE_BATTERY, BAT_USAGE_BELOW_REMAINING_CAP, tmp.battery_low_flag, tmp.battery_low_flag_valid);
    read_status(USAGE_PAGE_BATTERY, BAT_USAGE_NEED_REPLACEMENT, tmp.replace_battery, tmp.replace_battery_valid);
    read_status(USAGE_PAGE_BATTERY, BAT_USAGE_FULLY_CHARGED, tmp.fully_charged, tmp.fully_charged_valid);
    read_status(USAGE_PAGE_BATTERY, BAT_USAGE_FULLY_DISCHARGED, tmp.fully_discharged, tmp.fully_discharged_valid);
    read_status(USAGE_PAGE_POWER_DEVICE, PD_USAGE_SHUTDOWN_IMMINENT, tmp.shutdown_imminent, tmp.shutdown_imminent_valid);
    read_status(USAGE_PAGE_POWER_DEVICE, PD_USAGE_OVERLOAD, tmp.overload, tmp.overload_valid);
    read_status(USAGE_PAGE_POWER_DEVICE, PD_USAGE_BOOST, tmp.avr_boost, tmp.avr_boost_valid);
    read_status(USAGE_PAGE_POWER_DEVICE, PD_USAGE_BUCK, tmp.avr_buck, tmp.avr_buck_valid);
    read_status(USAGE_PAGE_POWER_DEVICE, PD_USAGE_VOLTAGE_OUT_OF_RANGE, tmp.voltage_out_of_range, tmp.voltage_out_of_range_valid);
    read_status(USAGE_PAGE_POWER_DEVICE, PD_USAGE_OVER_TEMPERATURE, tmp.over_temperature, tmp.over_temperature_valid);
    read_status(USAGE_PAGE_POWER_DEVICE, PD_USAGE_INTERNAL_FAILURE, tmp.internal_failure, tmp.internal_failure_valid);
    read_status(USAGE_PAGE_POWER_DEVICE, PD_USAGE_AWAITING_POWER, tmp.awaiting_power, tmp.awaiting_power_valid);

    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_TEST_CMD, PD_COLL_BATTERY_SYSTEM);
    if (f && read_field_value_(f, val)) tmp.self_test_result = val;
    f = report_map_.find(USAGE_PAGE_POWER_DEVICE, PD_USAGE_AUDIBLE_ALARM_CTRL);
    if (f && read_field_value_(f, val)) tmp.beeper_status = val;

    poll_cache_active_ = false;

    // ── State Machine (operates on local snapshot) ──
    if (tmp.ac_present_valid && tmp.on_battery_valid) update_power_state_on_(tmp);
    tmp.last_poll_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (device_gone_) return;

    // ── Atomic commit — mutex held for microseconds only ──
    xSemaphoreTake(data_mutex_, portMAX_DELAY);
    data_ = tmp;
    xSemaphoreGive(data_mutex_);

    publish_pending_ = true;
  }

  // ── Power State Machine ───────────────────────────────────
  // Operates on a local UpsData snapshot (no mutex held).
  void update_power_state_on_(UpsData &d) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);  // millis

    // ── Self-test suppression ──
    // A battery self-test intentionally runs the UPS on battery (and can
    // briefly report a low capacity). Do NOT treat that as a power failure,
    // or shutdown automations would fire during a routine test.
    if (test_active_until_ms_ != 0) {
      if (now < test_active_until_ms_) {
        if (d.power_state != PowerState::NORMAL) {
          d.power_state = PowerState::NORMAL;
          d.power_fail_event_sent = false;
        }
        return;
      }
      test_active_until_ms_ = 0;  // window elapsed — resume normal logic
    }

    // ── Transitions ──

    // AC returned -> back to normal
    if (d.ac_present && !d.on_battery) {
      if (d.power_state != PowerState::NORMAL) {
        d.power_state = PowerState::NORMAL;
        d.power_fail_event_sent = false;
        set_event_on_(d, "AC Restored");
        ESP_LOGI(TAG, "AC restored, state -> NORMAL");
        log_ring_append_("AC restored -> NORMAL");
      }
      return;
    }

    // UPS reports shutdown imminent
    if (d.shutdown_imminent_valid && d.shutdown_imminent) {
      if (d.power_state != PowerState::SHUTDOWN_IMMINENT) {
        d.power_state = PowerState::SHUTDOWN_IMMINENT;
        set_event_on_(d, "Shutdown Imminent");
        ESP_LOGW(TAG, "UPS: Shutdown imminent!");
        log_ring_append_("SHUTDOWN IMMINENT!");
      }
      return;
    }

    // Check battery low thresholds
    bool runtime_low = (d.remaining_runtime_sec > 0 &&
                        d.remaining_runtime_sec < (float)battery_low_runtime_s_);
    bool capacity_low = (d.battery_capacity > 0 &&
                         d.battery_capacity < (float)battery_low_capacity_pct_);

    if (d.on_battery && (runtime_low || capacity_low || (d.battery_low_flag_valid && d.battery_low_flag))) {
      if (d.power_state != PowerState::BATTERY_LOW &&
          d.power_state != PowerState::SHUTDOWN_IMMINENT) {
        d.power_state = PowerState::BATTERY_LOW;
        set_event_on_(d, "Battery Low");
        ESP_LOGW(TAG, "Battery low! runtime=%.0fs, capacity=%.0f%%",
                 d.remaining_runtime_sec, d.battery_capacity);
        log_ring_append_("BATTERY LOW!");
      }
      return;
    }

    // On battery but not yet low
    if (d.on_battery) {
      if (d.power_state == PowerState::NORMAL) {
        // Start grace period
        d.power_state = PowerState::POWER_FAIL_GRACE;
        d.power_fail_start_ms = now;
        ESP_LOGW(TAG, "Power failure detected, grace period %lus", power_fail_delay_s_);
        log_ring_append_("Power failure detected, starting grace period");
      }

      if (d.power_state == PowerState::POWER_FAIL_GRACE && !d.power_fail_event_sent) {
        uint32_t elapsed = now - d.power_fail_start_ms;
        if (elapsed >= power_fail_delay_s_ * 1000) {
          // Grace period expired — fire power failure event ONCE
          d.power_fail_event_sent = true;
          set_event_on_(d, "Power Failure");
          ESP_LOGW(TAG, "Power failure grace period expired!");
          log_ring_append_("Power failure event fired (grace expired)");
        }
      }
    }
  }

  void set_event_on_(UpsData &d, const char *event) {
    strncpy(d.last_event, event, sizeof(d.last_event) - 1);
    d.last_event[sizeof(d.last_event) - 1] = '\0';
    d.last_event_time = (uint32_t)(esp_timer_get_time() / 1000000);  // seconds since boot

    // Fire ESPHome custom event
    fire_homeassistant_event_(event);
  }

  void fire_homeassistant_event_(const char *event_type) {
    ESP_LOGI(TAG, "Firing HA event: cyberpower_ups / %s", event_type);
  }

  // ── Publish Sensor Values to ESPHome ──────────────────────
  void publish_sensors_(const UpsData &d) {
    // Values are stored in UpsData and read by template sensor lambdas in YAML.
    // No direct publishing needed — ESPHome template sensors poll get_data().
  }
};

// ── Include web UI (needs full class definition) ────────────
// Use ESPHome API and OTA; no separate web interface.

// ── Deferred method implementations ─────────────────────────
inline void UsbHidUpsComponent::setup() {
  ESP_LOGI(TAG, "CyberPower UPS Monitor starting... (%s)", FW_BUILD_ID);
  log_ring_init_();

  data_mutex_ = xSemaphoreCreateMutex();
  ctrl_sem_ = xSemaphoreCreateBinary();
  cmd_queue_ = xQueueCreate(8, sizeof(uint8_t));

  // Load config from NVS
  load_config_();

  // Start USB host library task
  xTaskCreatePinnedToCore(usb_lib_task_entry_, "usb_lib", 8192, this, 10, nullptr, 0);

  // Start USB monitor task
  xTaskCreatePinnedToCore(usb_mon_task_entry_, "usb_mon", 8192, this, 5, nullptr, 1);

  // Start web UI
  // Standalone upstream web UI is disabled for this ESPHome prototype.

  log_ring_append_("Component initialized");
}

inline void UsbHidUpsComponent::loop() {
  // Publish sensor values to ESPHome from main loop (thread-safe)
  if (!publish_pending_) return;
  publish_pending_ = false;

  UpsData snapshot;
  xSemaphoreTake(data_mutex_, portMAX_DELAY);
  snapshot = data_;
  xSemaphoreGive(data_mutex_);

  publish_sensors_(snapshot);
}

}  // namespace usb_hid_ups
}  // namespace esphome
