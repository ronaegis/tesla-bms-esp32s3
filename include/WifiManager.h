#ifndef WIFI_MANAGER_H_
#define WIFI_MANAGER_H_

#include <stdint.h>

class WifiManager {
public:
  enum class State { Disabled, Idle, Connecting, Connected, Failed };

  void begin(const char *ssid, const char *password);
  void loop(uint32_t nowMs);

  bool isConnected() const;
  State state() const { return state_; }
  int rssi() const;

private:
  void startConnect(uint32_t nowMs);

  State state_ = State::Disabled;
  const char *ssid_ = nullptr;
  const char *password_ = nullptr;
  uint32_t nextAttemptMs_ = 0;
  uint32_t attemptStartedMs_ = 0;
  uint32_t backoffMs_ = 0;
};

#endif // WIFI_MANAGER_H_
