#pragma once

#include <stddef.h>

#if defined(ESP_PLATFORM)
  #include <WiFi.h>
  #if !defined(WITH_WEB_PANEL)
    #define WITH_WEB_PANEL 1
  #endif
  #if WITH_WEB_PANEL
    #include <esp_https_server.h>
  #endif
#endif

class WebPanelCommandRunner {
public:
  virtual ~WebPanelCommandRunner() = default;
  virtual void runWebCommand(const char* command, char* reply, size_t reply_size) = 0;
  virtual const char* getWebAdminPassword() const = 0;
};

class WebPanelServer {
public:
  WebPanelServer();

  void setCommandRunner(WebPanelCommandRunner* runner);
  bool start();
  void stop();
  bool isRunning() const;
  bool hasSessionToken() const;
  bool shouldAutoLock(unsigned long now_ms) const;
  void lockSession();

private:
#if defined(ESP_PLATFORM) && WITH_WEB_PANEL
  struct RouteContext {
    WebPanelServer* self;
  };

  WebPanelCommandRunner* _runner;
  httpd_handle_t _server;
  char _token[33];
  unsigned long _last_activity_ms;
  RouteContext _route_context;

  static esp_err_t handleIndex(httpd_req_t* req);
  static esp_err_t handleApp(httpd_req_t* req);
  static esp_err_t handleLogin(httpd_req_t* req);
  static esp_err_t handleCommand(httpd_req_t* req);
  static esp_err_t handleBootstrap(httpd_req_t* req);
  static esp_err_t handleStats(httpd_req_t* req);

  bool readRequestBody(httpd_req_t* req, char* buffer, size_t buffer_size) const;
  void refreshToken();
  bool isAuthorized(httpd_req_t* req) const;
  void noteActivity();
#else
  WebPanelCommandRunner* _runner;
#endif
};
