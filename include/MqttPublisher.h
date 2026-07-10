#ifndef MQTT_PUBLISHER_H_
#define MQTT_PUBLISHER_H_

#include <stdint.h>

#include <WiFiClient.h>

#include "BMSModuleManager.h"

// Forward declaration to keep PubSubClient out of the header.
// WiFiClient cannot be forward-declared: on Arduino-ESP32 core 3.x it is a
// typedef of NetworkClient, not a class.
class PubSubClient;

class MqttPublisher {
public:
  enum class State { Disabled, Idle, Connecting, Connected, Failed };

  struct Config {
    const char *host = nullptr;
    uint16_t port = 1883;
    const char *user = nullptr;
    const char *password = nullptr;
    // Friendly name shown in HA. Topic-safe id is derived from name+MAC.
    const char *nodeName = "Tesla BMS";
  };

  void begin(const Config &config);
  void loop(uint32_t nowMs, bool networkConnected);

  void publishPack(const BMSModuleManager::PackTelemetry &telemetry,
                   int socPercent,
                   bool isBalancing);
  void publishModule(int index, const BMSModuleManager::ModuleTelemetry &module);

  bool isConnected() const;
  State state() const { return state_; }
  const char *nodeId() const { return nodeId_; }

private:
  void buildIdentity();
  bool attemptConnect(uint32_t nowMs);
  void publishAvailability(bool online);
  void publishDiscovery();

  void publishModuleDiscovery(int index);

  Config config_{};
  State state_ = State::Disabled;
  uint32_t nextAttemptMs_ = 0;
  uint32_t backoffMs_ = 0;
  bool discoveryPublished_ = false;
  // Bit n set → discovery published for module with address n. 64 bits
  // covers MAX_MODULE_ADDR (62) + slack.
  uint64_t moduleDiscoveryMask_ = 0;

  // Derived identity (built once in begin()).
  char nodeId_[40] = {0};            // e.g. "tesla_bms_garage_a4c1"
  char availabilityTopic_[64] = {0}; // "<nodeId>/availability"

  WiFiClient *wifiClient_ = nullptr;
  PubSubClient *mqttClient_ = nullptr;
};

#endif // MQTT_PUBLISHER_H_
