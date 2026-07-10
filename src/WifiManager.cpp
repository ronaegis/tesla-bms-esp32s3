#include "WifiManager.h"

#ifndef USE_MQTT
#define USE_MQTT 0
#endif

#if USE_MQTT

#include <WiFi.h>

#include "Logger.h"
#include "constants.h"

namespace {
constexpr uint32_t kConnectTimeoutMs = 20000;
}

void WifiManager::begin(const char *ssid, const char *password) {
  ssid_ = ssid;
  password_ = password;
  backoffMs_ = WIFI_RECONNECT_BACKOFF_MIN_MS;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);
  state_ = State::Idle;
  Logger::info("WifiManager: WiFi STA mode, target SSID '%s'", ssid_);
  startConnect(millis());
}

void WifiManager::startConnect(uint32_t nowMs) {
  Logger::debug("WifiManager: connecting to '%s'", ssid_);
  WiFi.disconnect(false, true);
  WiFi.begin(ssid_, password_);
  attemptStartedMs_ = nowMs;
  state_ = State::Connecting;
}

void WifiManager::loop(uint32_t nowMs) {
  if (state_ == State::Disabled) {
    return;
  }

  const wl_status_t wlStatus = WiFi.status();

  switch (state_) {
    case State::Connecting: {
      if (wlStatus == WL_CONNECTED) {
        backoffMs_ = WIFI_RECONNECT_BACKOFF_MIN_MS;
        state_ = State::Connected;
        Logger::info("WifiManager: connected, IP=%s, RSSI=%d",
                     WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return;
      }
      if ((nowMs - attemptStartedMs_) >= kConnectTimeoutMs) {
        Logger::warn("WifiManager: connect timed out, backing off %u ms", backoffMs_);
        WiFi.disconnect(false, true);
        nextAttemptMs_ = nowMs + backoffMs_;
        backoffMs_ = backoffMs_ * 2;
        if (backoffMs_ > WIFI_RECONNECT_BACKOFF_MAX_MS) {
          backoffMs_ = WIFI_RECONNECT_BACKOFF_MAX_MS;
        }
        state_ = State::Failed;
      }
      return;
    }

    case State::Connected: {
      if (wlStatus != WL_CONNECTED) {
        Logger::warn("WifiManager: link lost (status=%d), reconnecting", wlStatus);
        nextAttemptMs_ = nowMs;
        state_ = State::Failed;
      }
      return;
    }

    case State::Idle:
    case State::Failed: {
      if (nowMs >= nextAttemptMs_) {
        startConnect(nowMs);
      }
      return;
    }

    case State::Disabled:
      return;
  }
}

bool WifiManager::isConnected() const {
  return state_ == State::Connected && WiFi.status() == WL_CONNECTED;
}

int WifiManager::rssi() const {
  return isConnected() ? WiFi.RSSI() : 0;
}

#endif  // USE_MQTT
