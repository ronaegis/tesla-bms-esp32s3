#include "MqttPublisher.h"

#ifndef USE_MQTT
#define USE_MQTT 0
#endif

#if USE_MQTT

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <ctype.h>
#include <math.h>

#include "Logger.h"
#include "constants.h"

namespace {

constexpr const char *kAvailableOnline = "online";
constexpr const char *kAvailableOffline = "offline";
constexpr const char *kDiscoveryPrefix = "homeassistant";

// Slugify into [a-z0-9_], collapsing runs of non-alnum to single underscore.
void slugify(const char *src, char *dst, size_t dstSize) {
  if (dstSize == 0) {
    return;
  }
  size_t out = 0;
  bool lastUnderscore = true;  // suppress leading underscore
  for (const char *p = src; *p && out < dstSize - 1; ++p) {
    char c = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      dst[out++] = c;
      lastUnderscore = false;
    } else if (!lastUnderscore) {
      dst[out++] = '_';
      lastUnderscore = true;
    }
  }
  // Trim trailing underscore.
  while (out > 0 && dst[out - 1] == '_') {
    --out;
  }
  dst[out] = '\0';
}

}  // namespace

void MqttPublisher::begin(const Config &config) {
  config_ = config;
  backoffMs_ = MQTT_RECONNECT_BACKOFF_MIN_MS;
  discoveryPublished_ = false;

  buildIdentity();

  if (wifiClient_ == nullptr) {
    wifiClient_ = new WiFiClient();
  }
  if (mqttClient_ == nullptr) {
    mqttClient_ = new PubSubClient(*wifiClient_);
  }
  mqttClient_->setServer(config_.host, config_.port);
  mqttClient_->setKeepAlive(MQTT_KEEPALIVE_SEC);
  mqttClient_->setBufferSize(MQTT_MAX_PACKET_SIZE);

  state_ = State::Idle;
  Logger::info("MqttPublisher: node id '%s', broker %s:%u",
               nodeId_, config_.host, config_.port);
}

void MqttPublisher::buildIdentity() {
  char nameSlug[24] = {0};
  slugify(config_.nodeName ? config_.nodeName : "tesla_bms", nameSlug, sizeof(nameSlug));
  if (nameSlug[0] == '\0') {
    snprintf(nameSlug, sizeof(nameSlug), "tesla_bms");
  }

  // MAC tail (last 4 hex chars) for per-board uniqueness.
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);
  char macTail[5] = {0};
  snprintf(macTail, sizeof(macTail), "%02x%02x", mac[4], mac[5]);

  snprintf(nodeId_, sizeof(nodeId_), "%s_%s", nameSlug, macTail);
  snprintf(availabilityTopic_, sizeof(availabilityTopic_), "%s/availability", nodeId_);
}

bool MqttPublisher::isConnected() const {
  return state_ == State::Connected && mqttClient_ != nullptr && mqttClient_->connected();
}

void MqttPublisher::loop(uint32_t nowMs, bool networkConnected) {
  if (state_ == State::Disabled || mqttClient_ == nullptr) {
    return;
  }

  if (!networkConnected) {
    if (state_ == State::Connected) {
      Logger::warn("MqttPublisher: WiFi down, dropping MQTT");
      mqttClient_->disconnect();
      discoveryPublished_ = false;
      state_ = State::Idle;
      nextAttemptMs_ = nowMs + MQTT_RECONNECT_BACKOFF_MIN_MS;
    }
    return;
  }

  if (state_ == State::Connected) {
    if (!mqttClient_->loop()) {
      Logger::warn("MqttPublisher: connection lost, will reconnect");
      discoveryPublished_ = false;
      state_ = State::Failed;
      nextAttemptMs_ = nowMs + backoffMs_;
    }
    return;
  }

  // Idle / Failed / Connecting → try to (re)connect when backoff elapses.
  if (nowMs < nextAttemptMs_) {
    return;
  }

  state_ = State::Connecting;
  if (attemptConnect(nowMs)) {
    backoffMs_ = MQTT_RECONNECT_BACKOFF_MIN_MS;
    state_ = State::Connected;
    publishAvailability(true);
    if (!discoveryPublished_) {
      publishDiscovery();
      discoveryPublished_ = true;
    }
  } else {
    Logger::warn("MqttPublisher: connect failed (rc=%d), backing off %u ms",
                 mqttClient_->state(), backoffMs_);
    state_ = State::Failed;
    nextAttemptMs_ = nowMs + backoffMs_;
    backoffMs_ *= 2;
    if (backoffMs_ > MQTT_RECONNECT_BACKOFF_MAX_MS) {
      backoffMs_ = MQTT_RECONNECT_BACKOFF_MAX_MS;
    }
  }
}

bool MqttPublisher::attemptConnect(uint32_t /*nowMs*/) {
  Logger::debug("MqttPublisher: connecting to %s:%u as '%s'",
                config_.host, config_.port, nodeId_);

  const char *user = (config_.user && config_.user[0]) ? config_.user : nullptr;
  const char *pass = (config_.password && config_.password[0]) ? config_.password : nullptr;

  // LWT: retained "offline" published by broker if we drop unexpectedly.
  return mqttClient_->connect(
      nodeId_,
      user,
      pass,
      availabilityTopic_,
      /*willQos*/ 1,
      /*willRetain*/ true,
      kAvailableOffline);
}

void MqttPublisher::publishAvailability(bool online) {
  if (mqttClient_ == nullptr) return;
  mqttClient_->publish(availabilityTopic_,
                       online ? kAvailableOnline : kAvailableOffline,
                       /*retained*/ true);
}

namespace {

// Helper to publish one HA discovery config. Builds JSON into a stack buffer.
// `objectId` must be unique within the device (becomes part of the entity id).
// `jsonExtra` callback receives a JsonObject to add component-specific fields
// (state_topic, value_template, unit_of_measurement, device_class, etc).
template <typename Fn>
bool publishOne(PubSubClient *mqtt,
                const char *component,
                const char *nodeId,
                const char *nodeName,
                const char *availabilityTopic,
                const char *objectId,
                const char *friendlyName,
                Fn extra) {
  StaticJsonDocument<512> doc;
  doc["name"] = friendlyName;

  char uniqueId[64];
  snprintf(uniqueId, sizeof(uniqueId), "%s_%s", nodeId, objectId);
  doc["uniq_id"] = uniqueId;
  doc["avty_t"] = availabilityTopic;

  JsonObject device = doc.createNestedObject("dev");
  JsonArray ids = device.createNestedArray("ids");
  ids.add(nodeId);
  device["name"] = nodeName;
  device["mf"] = "DIY";
  device["mdl"] = "Tesla BMS ESP32-S3";

  extra(doc.as<JsonObject>());

  char topic[128];
  snprintf(topic, sizeof(topic),
           "homeassistant/%s/%s/%s/config",
           component, nodeId, objectId);

  char payload[768];
  size_t len = serializeJson(doc, payload, sizeof(payload));
  if (len == 0 || len >= sizeof(payload)) {
    Logger::error("MqttPublisher: discovery payload overflow for %s", objectId);
    return false;
  }
  return mqtt->publish(topic, reinterpret_cast<const uint8_t *>(payload), len, /*retained*/ true);
}

}  // namespace

void MqttPublisher::publishDiscovery() {
  if (mqttClient_ == nullptr) return;

  char packStateTopic[64];
  snprintf(packStateTopic, sizeof(packStateTopic), "%s/pack/state", nodeId_);

  const char *name = (config_.nodeName && config_.nodeName[0]) ? config_.nodeName : "Tesla BMS";

  // ---- Pack-level sensors ----
  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             "pack_voltage", "Pack Voltage",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ value_json.v }}";
               o["unit_of_meas"] = "V";
               o["dev_cla"] = "voltage";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 2;
             });

  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             "soc", "State of Charge",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ value_json.soc }}";
               o["unit_of_meas"] = "%";
               o["dev_cla"] = "battery";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 0;
             });

  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             "low_cell", "Lowest Cell",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ value_json.low }}";
               o["unit_of_meas"] = "V";
               o["dev_cla"] = "voltage";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 3;
             });

  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             "high_cell", "Highest Cell",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ value_json.high }}";
               o["unit_of_meas"] = "V";
               o["dev_cla"] = "voltage";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 3;
             });

  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             "cell_delta", "Cell Delta",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ value_json.delta }}";
               o["unit_of_meas"] = "V";
               o["dev_cla"] = "voltage";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 3;
             });

  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             "temp_low", "Lowest Temperature",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ value_json.t_low }}";
               o["unit_of_meas"] = "°C";
               o["dev_cla"] = "temperature";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 1;
             });

  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             "temp_high", "Highest Temperature",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ value_json.t_high }}";
               o["unit_of_meas"] = "°C";
               o["dev_cla"] = "temperature";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 1;
             });

  publishOne(mqttClient_, "binary_sensor", nodeId_, name, availabilityTopic_,
             "fault", "Fault",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ 'ON' if value_json.fault else 'OFF' }}";
               o["dev_cla"] = "problem";
             });

  publishOne(mqttClient_, "binary_sensor", nodeId_, name, availabilityTopic_,
             "balancing", "Balancing",
             [&](JsonObject o) {
               o["stat_t"] = packStateTopic;
               o["val_tpl"] = "{{ 'ON' if value_json.balancing else 'OFF' }}";
             });

  Logger::debug("MqttPublisher: pack discovery published (9 entities)");
  // Per-module discovery is published lazily on first publishModule() call
  // to avoid sending configs for modules that aren't actually present.
}

void MqttPublisher::publishPack(const BMSModuleManager::PackTelemetry &telemetry,
                                int socPercent,
                                bool isBalancing) {
  if (!isConnected() || !telemetry.hasData) return;

  StaticJsonDocument<384> doc;
  doc["v"] = roundf(telemetry.packVoltage * 100.0f) / 100.0f;
  doc["low"] = roundf(telemetry.lowestCell * 1000.0f) / 1000.0f;
  doc["high"] = roundf(telemetry.highestCell * 1000.0f) / 1000.0f;
  doc["delta"] = roundf(telemetry.cellDelta * 1000.0f) / 1000.0f;
  doc["soc"] = socPercent;
  if (!isnan(telemetry.lowestPackTemp)) {
    doc["t_low"] = roundf(telemetry.lowestPackTemp * 10.0f) / 10.0f;
  }
  if (!isnan(telemetry.highestPackTemp)) {
    doc["t_high"] = roundf(telemetry.highestPackTemp * 10.0f) / 10.0f;
  }
  doc["fault"] = telemetry.faultPinActive ? 1 : 0;
  doc["balancing"] = isBalancing ? 1 : 0;

  int modulesOnline = 0;
  for (int i = 1; i <= MAX_MODULE_ADDR; i++) {
    if (telemetry.modules[i].present && telemetry.modules[i].telemetryValid) {
      modulesOnline++;
    }
  }
  doc["modules_online"] = modulesOnline;

  char topic[64];
  snprintf(topic, sizeof(topic), "%s/pack/state", nodeId_);

  char payload[384];
  size_t len = serializeJson(doc, payload, sizeof(payload));
  if (len == 0 || len >= sizeof(payload)) {
    Logger::error("MqttPublisher: pack payload overflow");
    return;
  }
  mqttClient_->publish(topic, reinterpret_cast<const uint8_t *>(payload), len, /*retained*/ false);
}

void MqttPublisher::publishModule(int index,
                                  const BMSModuleManager::ModuleTelemetry &module) {
  if (!isConnected() || index < 0 || index > 63) return;
  if (!module.present) return;

  // Lazy per-module discovery: emit configs the first time we see this module.
  const uint64_t bit = (uint64_t)1 << index;
  if ((moduleDiscoveryMask_ & bit) == 0) {
    publishModuleDiscovery(index);
    moduleDiscoveryMask_ |= bit;
  }

  StaticJsonDocument<256> doc;
  doc["v"] = roundf(module.moduleVoltage * 100.0f) / 100.0f;
  doc["low"] = roundf(module.lowCell * 1000.0f) / 1000.0f;
  doc["high"] = roundf(module.highCell * 1000.0f) / 1000.0f;
  doc["delta"] = roundf(module.cellDelta * 1000.0f) / 1000.0f;
  if (!isnan(module.lowTemp)) {
    doc["t_low"] = roundf(module.lowTemp * 10.0f) / 10.0f;
  }
  if (!isnan(module.highTemp)) {
    doc["t_high"] = roundf(module.highTemp * 10.0f) / 10.0f;
  }
  doc["valid"] = module.telemetryValid ? 1 : 0;
  doc["balance_mask"] = module.balancingCellMask;

  char topic[64];
  snprintf(topic, sizeof(topic), "%s/module/%d/state", nodeId_, index);

  char payload[256];
  size_t len = serializeJson(doc, payload, sizeof(payload));
  if (len == 0 || len >= sizeof(payload)) {
    Logger::error("MqttPublisher: module %d payload overflow", index);
    return;
  }
  mqttClient_->publish(topic, reinterpret_cast<const uint8_t *>(payload), len, /*retained*/ false);
}

void MqttPublisher::publishModuleDiscovery(int index) {
  const char *name = (config_.nodeName && config_.nodeName[0]) ? config_.nodeName : "Tesla BMS";

  char stateTopic[64];
  snprintf(stateTopic, sizeof(stateTopic), "%s/module/%d/state", nodeId_, index);

  char objId[32];
  char friendlyName[48];

  // Voltage
  snprintf(objId, sizeof(objId), "module_%d_voltage", index);
  snprintf(friendlyName, sizeof(friendlyName), "Module %d Voltage", index);
  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             objId, friendlyName,
             [&](JsonObject o) {
               o["stat_t"] = stateTopic;
               o["val_tpl"] = "{{ value_json.v }}";
               o["unit_of_meas"] = "V";
               o["dev_cla"] = "voltage";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 2;
             });

  // Cell delta
  snprintf(objId, sizeof(objId), "module_%d_delta", index);
  snprintf(friendlyName, sizeof(friendlyName), "Module %d Cell Delta", index);
  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             objId, friendlyName,
             [&](JsonObject o) {
               o["stat_t"] = stateTopic;
               o["val_tpl"] = "{{ value_json.delta }}";
               o["unit_of_meas"] = "V";
               o["dev_cla"] = "voltage";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 3;
             });

  // High temp
  snprintf(objId, sizeof(objId), "module_%d_t_high", index);
  snprintf(friendlyName, sizeof(friendlyName), "Module %d Temperature", index);
  publishOne(mqttClient_, "sensor", nodeId_, name, availabilityTopic_,
             objId, friendlyName,
             [&](JsonObject o) {
               o["stat_t"] = stateTopic;
               o["val_tpl"] = "{{ value_json.t_high }}";
               o["unit_of_meas"] = "°C";
               o["dev_cla"] = "temperature";
               o["stat_cla"] = "measurement";
               o["sug_dsp_prc"] = 1;
             });

  Logger::debug("MqttPublisher: module %d discovery published", index);
}

#endif  // USE_MQTT
