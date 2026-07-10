#include "lvgl.h"      /* https://github.com/lvgl/lvgl.git */

#include "Arduino.h"
#include "WiFi.h"
#include "Wire.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "factory_gui.h"
#include "pin_config.h"
#include "constants.h"
#include "config.h"
#include "sntp.h"
#include "time.h"
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_task_wdt.h"
#include "HardwareSerial.h"
HardwareSerial SERIALBMS(0);

#include "BMSModuleManager.h"
#include "Logger.h"

#ifndef USE_MQTT
#define USE_MQTT 0
#endif

#if USE_MQTT
#include "WifiManager.h"
#include "MqttPublisher.h"
#include "secrets.h"
#endif

// Global instances
BMSModuleManager bms;
EEPROMSettings settings;

#if USE_MQTT
namespace {
WifiManager wifiManager;
MqttPublisher mqttPublisher;
}
#endif

namespace {
struct AppContext {
  lv_obj_t *bmsLabel = nullptr;
  uint32_t nextTelemetryPoll = 0;
  uint32_t nextGuiUpdate = 0;
  uint32_t nextPageSwitch = 0;
  uint32_t nextStatusRefresh = 0;
  uint32_t nextMqttPublish = 0;
  bool isBalancing = false;
  uint32_t lastBalanceMs = 0;
};

// Buffer sizes with 20% safety margin
constexpr size_t PACK_STATUS_BUFFER_SIZE = 230;  // 192 + 20% safety margin
constexpr size_t MODULE_STATUS_BYTES_PER_MODULE = 96;  // ~80 bytes per module + 20% safety margin
constexpr size_t MODULE_STATUS_BUFFER_SIZE = (MAX_MODULE_ADDR + 1) * MODULE_STATUS_BYTES_PER_MODULE;
constexpr size_t DISPLAY_BUFFER_SIZE = PACK_STATUS_BUFFER_SIZE + MODULE_STATUS_BUFFER_SIZE;

// Compile-time assertion to ensure buffer size calculations are reasonable
static_assert(MODULE_STATUS_BYTES_PER_MODULE >= 80,
              "MODULE_STATUS_BYTES_PER_MODULE must be at least 80 bytes to hold formatted module data");
static_assert(MODULE_STATUS_BUFFER_SIZE <= 32768,
              "MODULE_STATUS_BUFFER_SIZE exceeds reasonable limit (likely configuration error)");

AppContext appContext;
char packStatusBuffer[PACK_STATUS_BUFFER_SIZE];
char moduleStatusBuffer[MODULE_STATUS_BUFFER_SIZE];
char displayBuffer[DISPLAY_BUFFER_SIZE];
} // namespace

lv_obj_t *bms_label = nullptr;

esp_lcd_panel_io_handle_t io_handle = NULL;
static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      // contains callback functions
static lv_color_t *lv_disp_buf;
static bool is_initialized_lvgl = false;

void wifi_test(void);
void printLocalTime();
void SmartConfig();

static void configurePowerRails();
static void configureWatchdog();
static void configureSerialConsole();
static void configureTimeServices();
static esp_lcd_panel_handle_t initializeDisplayHardware();
static void initializeLvgl(esp_lcd_panel_handle_t panel_handle);
static void initializeUserInterface(AppContext &context);
static void configureBmsSubsystem();
static void waitForBmsInitialization();
static void handleTelemetry(AppContext &context, uint32_t now);
static void handleGuiUpdates(AppContext &context, uint32_t now);
static void formatStatusBuffers(const BMSModuleManager::PackTelemetry &telemetry, bool isBalancing);
static int computeStateOfChargePercent(float packVoltage);
static void maintainBalancingState(AppContext &context, const BMSModuleManager::PackTelemetry &telemetry, uint32_t nowMs);
static void appendModuleSummaryLine(char *buffer, size_t bufferSize, size_t &offset, int index, const BMSModuleManager::ModuleTelemetry &module);
static void watchdogResetShim();

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
  if (is_initialized_lvgl) {
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
  }
  return false;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
  int offsetx1 = area->x1;
  int offsetx2 = area->x2;
  int offsety1 = area->y1;
  int offsety2 = area->y2;
  // copy a buffer's content to a specific area of the display
  esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void configurePowerRails() {
  if (PIN_POWER_ON >= 0) {
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, HIGH);
  }
  pinMode(PIN_BMS_FAULT, INPUT_PULLUP);
}

static void configureWatchdog() {
  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL); // Add current thread to WDT watch
}

static void watchdogResetShim() {
  (void)esp_task_wdt_reset();
}

static void configureSerialConsole() {
  Serial.begin(115200);
}

static void configureTimeServices() {
  configTime(GMT_OFFSET_SEC, DAY_LIGHT_OFFSET_SEC, NTP_SERVER1, NTP_SERVER2);
}

static esp_lcd_panel_handle_t initializeDisplayHardware() {
#if TESLA_BMS_LCD_BUS == TESLA_BMS_LCD_BUS_I80
  pinMode(PIN_LCD_RD, OUTPUT);
  digitalWrite(PIN_LCD_RD, HIGH);

  esp_lcd_i80_bus_handle_t i80_bus = NULL;
  esp_lcd_i80_bus_config_t bus_config = {
      .dc_gpio_num = PIN_LCD_DC,
      .wr_gpio_num = PIN_LCD_WR,
      .clk_src = LCD_CLK_SRC_PLL160M,
      .data_gpio_nums =
          {
              PIN_LCD_D0,
              PIN_LCD_D1,
              PIN_LCD_D2,
              PIN_LCD_D3,
              PIN_LCD_D4,
              PIN_LCD_D5,
              PIN_LCD_D6,
              PIN_LCD_D7,
          },
      .bus_width = 8,
      .max_transfer_bytes = LVGL_LCD_BUF_SIZE * sizeof(uint16_t),
  };
  ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));
#elif TESLA_BMS_LCD_BUS == TESLA_BMS_LCD_BUS_SPI
  spi_bus_config_t bus_config = {
      .mosi_io_num = PIN_LCD_MOSI,
      .miso_io_num = -1,
      .sclk_io_num = PIN_LCD_SCLK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = LVGL_LCD_BUF_SIZE * sizeof(uint16_t),
  };
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
#else
#error "Unsupported display bus"
#endif

#if TESLA_BMS_LCD_BUS == TESLA_BMS_LCD_BUS_I80
  esp_lcd_panel_io_i80_config_t io_config = {
      .cs_gpio_num = PIN_LCD_CS,
      .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
      .trans_queue_depth = 20,
      .on_color_trans_done = example_notify_lvgl_flush_ready,
      .user_ctx = &disp_drv,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
      .dc_levels =
          {
              .dc_idle_level = 0,
              .dc_cmd_level = 0,
              .dc_dummy_level = 0,
              .dc_data_level = 1,
          },
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));
#elif TESLA_BMS_LCD_BUS == TESLA_BMS_LCD_BUS_SPI
  esp_lcd_panel_io_spi_config_t io_config = {
      .cs_gpio_num = PIN_LCD_CS,
      .dc_gpio_num = PIN_LCD_DC,
      .spi_mode = 0,
      .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
      .trans_queue_depth = 20,
      .on_color_trans_done = example_notify_lvgl_flush_ready,
      .user_ctx = &disp_drv,
      .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle));
#endif

  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = PIN_LCD_RES,
      .color_space = ESP_LCD_COLOR_SPACE_RGB,
      .bits_per_pixel = 16,
  };
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
  esp_lcd_panel_reset(panel_handle);
  esp_lcd_panel_init(panel_handle);
  esp_lcd_panel_invert_color(panel_handle, LCD_PANEL_INVERT_COLORS);
  esp_lcd_panel_swap_xy(panel_handle, LCD_PANEL_SWAP_XY);
  esp_lcd_panel_mirror(panel_handle, LCD_PANEL_MIRROR_X, LCD_PANEL_MIRROR_Y);
  esp_lcd_panel_set_gap(panel_handle, LCD_PANEL_GAP_X, LCD_PANEL_GAP_Y);

  return panel_handle;
}

static void initializeLvgl(esp_lcd_panel_handle_t panel_handle) {
  ledcSetup(LCD_BACKLIGHT_PWM_CHANNEL, LCD_BACKLIGHT_PWM_FREQ, LCD_BACKLIGHT_PWM_RESOLUTION);
  ledcAttachPin(PIN_LCD_BL, LCD_BACKLIGHT_PWM_CHANNEL);
  for (uint8_t i = 0; i < LCD_BACKLIGHT_MAX_BRIGHTNESS; i++) {
    ledcWrite(LCD_BACKLIGHT_PWM_CHANNEL, i);
    delay(BMS_COMMAND_DELAY_MS);
  }

  lv_init();
  lv_disp_buf = (lv_color_t *)heap_caps_malloc(LVGL_LCD_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (lv_disp_buf == NULL) {
    SERIALCONSOLE.println("FATAL ERROR: Failed to allocate LVGL display buffer!");
    SERIALCONSOLE.printf("Requested: %zu bytes (DMA + Internal)\n", LVGL_LCD_BUF_SIZE * sizeof(lv_color_t));
    SERIALCONSOLE.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
    SERIALCONSOLE.printf("Free PSRAM: %u bytes\n", ESP.getFreePsram());
    SERIALCONSOLE.println("System will reset via watchdog in 10 seconds...");

    // Display error on LCD if possible (may not work without buffer, but try)
    // Then let watchdog reset the system for recovery attempt
    delay(10000);

    // If watchdog didn't trigger, force a restart
    SERIALCONSOLE.println("Forcing system restart...");
    ESP.restart();
  }

  lv_disp_draw_buf_init(&disp_buf, lv_disp_buf, NULL, LVGL_LCD_BUF_SIZE);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = example_lvgl_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.user_data = panel_handle;
  lv_disp_drv_register(&disp_drv);

  is_initialized_lvgl = true;
}

static void initializeUserInterface(AppContext &context) {
  ui_begin();
  context.bmsLabel = bms_label;
}

static void configureBmsSubsystem() {
  bms.setWatchdogCallback(&watchdogResetShim);
  bms.renumberBoardIDs();
  Logger::setLoglevel(Logger::Off); // Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4
  bms.findBoards();
  bms.setPstrings(BMS_NUM_PARALLEL);
}

static void waitForBmsInitialization() {
  uint32_t elapsed = 0;
  while (elapsed < STARTUP_DELAY_MS) {
    delay(1000);
    esp_task_wdt_reset();
    elapsed += 1000;
  }
}

static void appendModuleSummaryLine(char *buffer,
                                    size_t bufferSize,
                                    size_t &offset,
                                    int index,
                                    const BMSModuleManager::ModuleTelemetry &module) {
  if (!module.present || offset >= bufferSize) {
    return;
  }

  int written;
  if (!module.telemetryValid) {
    written = snprintf(buffer + offset, bufferSize - offset, "Mod #%d: read error\n", index);
  } else {
    written = snprintf(buffer + offset,
                       bufferSize - offset,
                       "Mod #%d: %.2fv l: %.3fv h: %.3fv d=%.3fv\n",
                       index,
                       module.moduleVoltage,
                       module.lowCell,
                       module.highCell,
                       module.cellDelta);
  }

  if (written < 0) {
    Logger::error("Failed to format module %d summary (snprintf error)", index);
    return;
  }

  if (static_cast<size_t>(written) >= bufferSize - offset) {
    // Buffer overflow - truncate and warn
    Logger::warn("Module %d summary truncated (needed %d bytes, had %zu available)",
                 index, written, bufferSize - offset);
    offset = bufferSize - 1;
    buffer[offset] = '\0';
  } else {
    offset += static_cast<size_t>(written);
  }
}

static int computeStateOfChargePercent(float packVoltage) {
  if (BMS_NUM_SERIES <= 0) {
    return 0;
  }
  // Calculate per-module voltage (pack voltage divided by number of modules in series)
  float perModuleVoltage = packVoltage / static_cast<float>(BMS_NUM_SERIES);

  // Normalize based on module voltage range (each module is 6S)
  float normalized = (perModuleVoltage - SOC_MIN_MODULE_VOLTAGE) * 100.0f /
                     (SOC_MAX_MODULE_VOLTAGE - SOC_MIN_MODULE_VOLTAGE);

  // Clamp to 0-100%
  if (normalized < 0.0f) normalized = 0.0f;
  if (normalized > 100.0f) normalized = 100.0f;

  return static_cast<int>(normalized + 0.5f);
}

static void formatStatusBuffers(const BMSModuleManager::PackTelemetry &telemetry, bool isBalancing) {
  if (!telemetry.hasData) {
    snprintf(packStatusBuffer, PACK_STATUS_BUFFER_SIZE, "No valid module telemetry");
    moduleStatusBuffer[0] = '\0';
    return;
  }

  const int socPercent = computeStateOfChargePercent(telemetry.packVoltage);
  const char *balancingLine = isBalancing ? "\n\n*** BALANCING ***" : "";
  const char *faultLine = telemetry.faultPinActive ? "\n\nFAULT line active" : "";

  snprintf(packStatusBuffer,
           PACK_STATUS_BUFFER_SIZE,
           "SoC: %d %%\n\nVolts: %.2fv low: %.3fv high: %.3fv d=%.3fv%s%s",
           socPercent,
           telemetry.packVoltage,
           telemetry.lowestCell,
           telemetry.highestCell,
           telemetry.cellDelta,
           balancingLine,
           faultLine);

  moduleStatusBuffer[0] = '\0';
  size_t offset = 0;
  for (int i = 1; i <= MAX_MODULE_ADDR && offset < MODULE_STATUS_BUFFER_SIZE - 1; i++) {
    appendModuleSummaryLine(moduleStatusBuffer, MODULE_STATUS_BUFFER_SIZE, offset, i, telemetry.modules[i]);
  }
}

static void maintainBalancingState(AppContext &context,
                                   const BMSModuleManager::PackTelemetry &telemetry,
                                   uint32_t nowMs) {
  if (!telemetry.hasData) {
    context.isBalancing = false;
    return;
  }

  // Determine if cells currently need balancing
  bool cellsNeedBalancing = (telemetry.highestCell > BMS_BALANCE_VOLTAGE_MIN &&
                             telemetry.highestCell > (telemetry.lowestCell + BMS_BALANCE_VOLTAGE_DELTA));

  // If cells don't need balancing, stop reporting as balancing
  if (!cellsNeedBalancing) {
    context.isBalancing = false;
    return;
  }

  // Check if balancing timeout has elapsed (handles millis() wraparound correctly)
  bool timeoutElapsed = (context.lastBalanceMs > 0 && (nowMs - context.lastBalanceMs) >= BALANCE_TIME_MS);

  // Start or restart balancing if needed and enough time has passed
  if (context.lastBalanceMs == 0 || timeoutElapsed) {
    bms.balanceCells();
    context.isBalancing = true;
    context.lastBalanceMs = nowMs;
  } else {
    // Within the balance interval, keep reporting as balancing if cells need it
    context.isBalancing = cellsNeedBalancing;
  }
}

static void handleTelemetry(AppContext &context, uint32_t now) {
  if (now < context.nextTelemetryPoll) {
    return;
  }
  context.nextTelemetryPoll = now + LOOP_INTERVAL_MS;

  bms.collectTelemetry();
  const auto &telemetry = bms.getTelemetry();
  maintainBalancingState(context, telemetry, now);
  formatStatusBuffers(telemetry, context.isBalancing);

  // Trigger immediate status update
  context.nextStatusRefresh = now;
}

static void handleGuiUpdates(AppContext &context, uint32_t now) {
  if (now >= context.nextGuiUpdate) {
#if USE_WIFI
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      lv_msg_send(MSG_NEW_HOUR, &timeinfo.tm_hour);
      lv_msg_send(MSG_NEW_MIN, &timeinfo.tm_min);
    }
#endif
    uint32_t volt = (analogRead(PIN_BAT_VOLT) * BAT_VOLTAGE_DIVIDER_RATIO * ADC_REFERENCE_VOLTAGE * 1000) / ADC_RESOLUTION;
    lv_msg_send(MSG_NEW_VOLT, &volt);

#if USE_MQTT
    static char netStatus[64];
    const bool wifiUp = wifiManager.isConnected();
    const bool mqttUp = mqttPublisher.isConnected();
    if (!wifiUp) {
      snprintf(netStatus, sizeof(netStatus), "net: wifi down");
    } else if (!mqttUp) {
      snprintf(netStatus, sizeof(netStatus), "net: wifi %ddBm | mqtt --", wifiManager.rssi());
    } else {
      snprintf(netStatus, sizeof(netStatus), "net: wifi %ddBm | mqtt ok", wifiManager.rssi());
    }
    lv_msg_send(MSG_NET_STATUS, netStatus);
#endif

    context.nextGuiUpdate = now + GUI_UPDATE_INTERVAL_MS;
  }

  if (now >= context.nextPageSwitch) {
    ui_switch_page();
    context.nextPageSwitch = now + PAGE_SWITCH_INTERVAL_MS;
  }

  if (context.bmsLabel != nullptr && now >= context.nextStatusRefresh) {
    // Only update display with valid data
    if (bms.getTelemetry().hasData) {
      if (moduleStatusBuffer[0] != '\0') {
        snprintf(displayBuffer, DISPLAY_BUFFER_SIZE, "%s\n\n%s", packStatusBuffer, moduleStatusBuffer);
      } else {
        snprintf(displayBuffer, DISPLAY_BUFFER_SIZE, "%s", packStatusBuffer);
      }
      lv_label_set_text(context.bmsLabel, displayBuffer);
    } else {
      lv_label_set_text(context.bmsLabel, "No valid module data");
    }
    context.nextStatusRefresh = now + STATUS_UPDATE_INTERVAL_MS;
  }
}

void setup() {
  configurePowerRails();
  configureWatchdog();
  configureSerialConsole();

#if USE_WIFI
  sntp_servermode_dhcp(1);
#endif

  configureTimeServices();

  esp_lcd_panel_handle_t panel_handle = initializeDisplayHardware();
  initializeLvgl(panel_handle);

  SERIALBMS.begin(612500, SERIAL_8N1, /* rx */ GPIO_NUM_2, /* tx */ GPIO_NUM_1);

#if USE_WIFI
  wifi_test();
#else
  initializeUserInterface(appContext);
  waitForBmsInitialization();
#endif

  if (bms_label == nullptr) {
    initializeUserInterface(appContext);
  } else {
    appContext.bmsLabel = bms_label;
  }

  configureBmsSubsystem();

#if USE_MQTT
  // Auto-disable: empty WIFI_SSID in secrets.h means the user has not
  // configured networking — skip WiFi/MQTT entirely and run BMS-only.
  if (WIFI_SSID[0] == '\0') {
    Logger::info("WIFI_SSID empty in secrets.h — networking disabled");
  } else {
    wifiManager.begin(WIFI_SSID, WIFI_PASSWORD);
    if (MQTT_HOST[0] == '\0') {
      Logger::info("MQTT_HOST empty in secrets.h — MQTT disabled (WiFi only)");
    } else {
      MqttPublisher::Config mqttCfg;
      mqttCfg.host = MQTT_HOST;
      mqttCfg.port = MQTT_PORT;
      mqttCfg.user = MQTT_USER;
      mqttCfg.password = MQTT_PASSWORD;
      mqttCfg.nodeName = MQTT_NODE_NAME;
      mqttPublisher.begin(mqttCfg);
    }
  }
#endif

  const uint32_t now = millis();
  appContext.nextTelemetryPoll = now;
  appContext.nextGuiUpdate = now;
  appContext.nextPageSwitch = now + PAGE_SWITCH_INTERVAL_MS;
  appContext.nextStatusRefresh = now;
  appContext.nextMqttPublish = now + MQTT_PUBLISH_INTERVAL_MS;
  snprintf(packStatusBuffer, PACK_STATUS_BUFFER_SIZE, "Initializing BMS...");
  moduleStatusBuffer[0] = '\0';
}

void loop() {
  lv_timer_handler();
  esp_task_wdt_reset();

  const uint32_t now = millis();
  handleTelemetry(appContext, now);
  handleGuiUpdates(appContext, now);

#if USE_MQTT
  wifiManager.loop(now);
  mqttPublisher.loop(now, wifiManager.isConnected());

  if (mqttPublisher.isConnected() && now >= appContext.nextMqttPublish) {
    const auto &telemetry = bms.getTelemetry();
    if (telemetry.hasData) {
      const int soc = computeStateOfChargePercent(telemetry.packVoltage);
      mqttPublisher.publishPack(telemetry, soc, appContext.isBalancing);
      for (int i = 1; i <= MAX_MODULE_ADDR; i++) {
        if (telemetry.modules[i].present) {
          mqttPublisher.publishModule(i, telemetry.modules[i]);
        }
      }
    }
    appContext.nextMqttPublish = now + MQTT_PUBLISH_INTERVAL_MS;
  }
#endif
}

#if USE_WIFI
void wifi_test(void) {
  String text;

  lv_obj_t *log_label = lv_label_create(lv_scr_act());
  lv_obj_align(log_label, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_width(log_label, LV_PCT(100));
  lv_label_set_text(log_label, "Scan WiFi");
  lv_label_set_long_mode(log_label, LV_LABEL_LONG_SCROLL);
  lv_label_set_recolor(log_label, true);
  LV_DELAY(1);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  int n = WiFi.scanNetworks();
  Serial.println("scan done");
  if (n == 0) {
    text = "no networks found";
  } else {
    text = n;
    text += " networks found\n";
    for (int i = 0; i < n; ++i) {
      text += (i + 1);
      text += ": ";
      text += WiFi.SSID(i);
      text += " (";
      text += WiFi.RSSI(i);
      text += ")";
      text += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " \n" : "*\n";
      delay(10);
    }
  }
  lv_label_set_text(log_label, text.c_str());
  Serial.println(text);
  LV_DELAY(2000);
  text = "Connecting to ";
  Serial.print("Connecting to ");
  text += WIFI_SSID;
  text += "\n";
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t last_tick = millis();
  uint32_t i = 0;
  bool is_smartconfig_connect = false;
  lv_label_set_long_mode(log_label, LV_LABEL_LONG_WRAP);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    text += ".";
    lv_label_set_text(log_label, text.c_str());
    LV_DELAY(100);
    if (millis() - last_tick > WIFI_CONNECT_WAIT_MAX_MS) { /* Automatically start smartconfig when connection times out */
      text += "\nConnection timed out, start smartconfig";
      lv_label_set_text(log_label, text.c_str());
      LV_DELAY(100);
      is_smartconfig_connect = true;
      WiFi.mode(WIFI_AP_STA);
      Serial.println("\r\n wait for smartconfig....");
      text += "\r\n wait for smartconfig....";
      text += "\nPlease use #ff0000 EspTouch# Apps to connect to the distribution network";
      lv_label_set_text(log_label, text.c_str());
      WiFi.beginSmartConfig();
      while (1) {
        LV_DELAY(100);
        if (WiFi.smartConfigDone()) {
          Serial.println("\r\nSmartConfig Success\r\n");
          Serial.printf("SSID:%s\r\n", WiFi.SSID().c_str());
          Serial.printf("PSW:%s\r\n", WiFi.psk().c_str());
          text += "\nSmartConfig Success";
          text += "\nSSID:";
          text += WiFi.SSID().c_str();
          text += "\nPSW:";
          text += WiFi.psk().c_str();
          lv_label_set_text(log_label, text.c_str());
          LV_DELAY(1000);
          last_tick = millis();
          break;
        }
      }
    }
  }
  if (!is_smartconfig_connect) {
    text += "\nCONNECTED \nTakes ";
    Serial.print("\n CONNECTED \nTakes ");
    text += millis() - last_tick;
    Serial.print(millis() - last_tick);
    text += " ms\n";
    Serial.println(" millseconds");
    lv_label_set_text(log_label, text.c_str());
  }
  LV_DELAY(2000);
  ui_begin();
}
#endif

void printLocalTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("No time available (yet)");
    return;
  }
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
}
