#include "WebService.h"

#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  #include <WiFi.h>
#endif

WebService::WebService() : _fs(nullptr), _prefs{}, _runner(nullptr), _network(nullptr), _suspended_for_ota(false) {
  WebPrefsStore::setDefaults(_prefs);
}

void WebService::begin(FILESYSTEM* fs) {
  _fs = fs;
  WebPrefsStore::load(_fs, _prefs);
}

void WebService::end() {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  _panel.stop();
#endif
}

void WebService::suspendForOTA() {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  _suspended_for_ota = true;
  _panel.stop();
#endif
}

void WebService::prepareForOTAStart() {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  _suspended_for_ota = true;
  _panel.stopRedirectServer();
#endif
}

void WebService::loop() {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  ensureWebServer();
  if (_panel.isRunning() && _panel.shouldAutoLock(millis())) {
    _panel.lockSession();
  }
#endif
}

void WebService::setCommandRunner(WebPanelCommandRunner* runner) {
  _runner = runner;
  _panel.setCommandRunner(runner);
}

bool WebService::savePrefs() {
  return WebPrefsStore::save(_fs, _prefs);
}

bool WebService::setWebEnabled(bool enabled) {
  _prefs.web_enabled = enabled ? 1 : 0;
  if (!enabled) {
    _suspended_for_ota = false;
  }
  bool ok = savePrefs();
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  if (_prefs.web_enabled != 0 && !_suspended_for_ota) {
    ensureWebServer();
  } else {
    _panel.stop();
  }
#endif
  return ok;
}

bool WebService::setWebStatsEnabled(bool enabled) {
  _prefs.web_stats_enabled = enabled ? 1 : 0;
  return savePrefs();
}

void WebService::formatWebStatusReply(char* reply, size_t reply_size) const {
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  if (_runner == nullptr || _prefs.web_enabled == 0) {
    snprintf(reply, reply_size, "> web:off");
    return;
  }

  if (_suspended_for_ota) {
    snprintf(reply, reply_size, "> web:suspended ota");
    return;
  }

  if (!_panel.isRunning() || _network == nullptr || !_network->isWifiConnected()) {
    snprintf(reply, reply_size, "> web:down");
    return;
  }

  snprintf(reply, reply_size, "> web:up url:https://%s/ auth:%s", WiFi.localIP().toString().c_str(),
           _panel.hasSessionToken() ? "unlocked" : "locked");
#else
  snprintf(reply, reply_size, "> web:unsupported");
#endif
}

#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
void WebService::ensureWebServer() {
  if (_runner == nullptr || _prefs.web_enabled == 0 || _network == nullptr || !_network->isWifiConnected()) {
    _panel.stop();
    return;
  }
  if (_suspended_for_ota) {
    if (_panel.isRunning()) {
      _panel.stopRedirectServer();
    }
    return;
  }
  if (_panel.isRunning()) {
    return;
  }
  _panel.start();
}
#endif
