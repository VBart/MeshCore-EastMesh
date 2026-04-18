#include "WebPanelServer.h"

#if defined(ESP_PLATFORM) && WITH_WEB_PANEL

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include <string.h>

#include "../mqtt/generated/WebPanelCert.h"

namespace {

#ifndef WEB_PANEL_STACK_SIZE
  #define WEB_PANEL_STACK_SIZE 6144
#endif

#ifndef WEB_PANEL_IDLE_TIMEOUT_MS
  #define WEB_PANEL_IDLE_TIMEOUT_MS (15UL * 60UL * 1000UL)
#endif

constexpr size_t kWebServerStackSize = WEB_PANEL_STACK_SIZE;
constexpr size_t kWebPasswordBufferSize = 80;
constexpr size_t kWebCommandBufferSize = 192;
constexpr size_t kWebReplyBufferSize = 256;
constexpr size_t kWebStatsQueryBufferSize = 96;
constexpr size_t kWebStatsReplyBufferSize = 4608;
constexpr size_t kWebPageChunkSize = 768;
constexpr unsigned long kWebIdleTimeoutMs = WEB_PANEL_IDLE_TIMEOUT_MS;

#if defined(MQTT_DEBUG) && MQTT_DEBUG
  #define WEB_PANEL_LOG(fmt, ...) Serial.printf("[WEB] " fmt "\n", ##__VA_ARGS__)
#else
  #define WEB_PANEL_LOG(...) do { } while (0)
#endif

char* allocScratchBuffer(size_t size) {
  void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr == nullptr) {
    ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
  return static_cast<char*>(ptr);
}

void freeScratchBuffer(void* ptr) {
  if (ptr != nullptr) {
    heap_caps_free(ptr);
  }
}

void bytesToHexUpper(const uint8_t* src, size_t len, char* dst, size_t dst_size) {
  if (dst_size == 0) {
    return;
  }
  size_t di = 0;
  for (size_t i = 0; i < len && di + 2 < dst_size; ++i) {
    snprintf(&dst[di], dst_size - di, "%02X", src[i]);
    di += 2;
  }
  dst[(di < dst_size) ? di : (dst_size - 1)] = 0;
}

esp_err_t sendChunk(httpd_req_t* req, const char* text) {
  return httpd_resp_sendstr_chunk(req, text != nullptr ? text : "");
}

esp_err_t sendProgmemChunked(httpd_req_t* req, const char* text) {
  if (text == nullptr) {
    return httpd_resp_send_chunk(req, nullptr, 0);
  }

  const size_t len = strlen(text);
  size_t offset = 0;
  while (offset < len) {
    const size_t chunk_len = ((len - offset) > kWebPageChunkSize) ? kWebPageChunkSize : (len - offset);
    if (httpd_resp_send_chunk(req, &text[offset], chunk_len) != ESP_OK) {
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }
    offset += chunk_len;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t sendJsonEscapedChunk(httpd_req_t* req, const char* src) {
  char chunk[48];
  size_t offset = 0;

  for (size_t i = 0; src != nullptr && src[i] != 0; ++i) {
    const char* escape = nullptr;
    char c = src[i];
    switch (c) {
      case '\\':
        escape = "\\\\";
        break;
      case '"':
        escape = "\\\"";
        break;
      case '\n':
        escape = "\\n";
        break;
      case '\r':
        escape = "\\r";
        break;
      case '\t':
        escape = "\\t";
        break;
      default:
        break;
    }

    const char* fragment = escape;
    char single[2] = {c, 0};
    if (fragment == nullptr) {
      fragment = single;
    }
    size_t frag_len = strlen(fragment);
    if (offset + frag_len >= sizeof(chunk) - 1) {
      chunk[offset] = 0;
      if (sendChunk(req, chunk) != ESP_OK) {
        return ESP_FAIL;
      }
      offset = 0;
    }
    memcpy(&chunk[offset], fragment, frag_len);
    offset += frag_len;
  }

  if (offset > 0) {
    chunk[offset] = 0;
    return sendChunk(req, chunk);
  }
  return ESP_OK;
}

esp_err_t sendJsonFieldChunk(httpd_req_t* req, const char* key, const char* value, bool comma) {
  char prefix[40];
  int written = snprintf(prefix, sizeof(prefix), "%s\"%s\":\"", comma ? "," : "", key);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(prefix)) {
    return ESP_FAIL;
  }
  if (sendChunk(req, prefix) != ESP_OK) {
    return ESP_FAIL;
  }
  if (sendJsonEscapedChunk(req, value != nullptr ? value : "") != ESP_OK) {
    return ESP_FAIL;
  }
  return sendChunk(req, "\"");
}

bool getQueryValue(httpd_req_t* req, const char* key, char* value, size_t value_size) {
  if (req == nullptr || key == nullptr || value == nullptr || value_size == 0) {
    return false;
  }
  const size_t query_len = httpd_req_get_url_query_len(req);
  if (query_len == 0 || query_len + 1 > kWebStatsQueryBufferSize) {
    return false;
  }

  char query[kWebStatsQueryBufferSize];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
    return false;
  }
  return httpd_query_key_value(query, key, value, value_size) == ESP_OK;
}

esp_err_t sendLegacyStatsBundle(httpd_req_t* req, WebPanelCommandRunner* runner, char* reply) {
  const struct {
    const char* key;
    const char* command;
  } fields[] = {
      {"wifi", "get wifi.status"},
      {"wifi_powersave", "get wifi.powersaving"},
      {"core", "stats-core"},
      {"radio", "stats-radio"},
      {"packets", "stats-packets"},
      {"memory", "memory"},
  };

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (sendChunk(req, "{") != ESP_OK) {
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_FAIL;
  }
  for (size_t i = 0; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    memset(reply, 0, kWebReplyBufferSize);
    runner->runWebCommand(fields[i].command, reply, kWebReplyBufferSize);
    if (sendJsonFieldChunk(req, fields[i].key, reply, i != 0) != ESP_OK) {
      httpd_resp_sendstr_chunk(req, nullptr);
      return ESP_FAIL;
    }
  }
  esp_err_t rc = sendChunk(req, "}");
  if (rc == ESP_OK) {
    rc = httpd_resp_sendstr_chunk(req, nullptr);
  } else {
    httpd_resp_sendstr_chunk(req, nullptr);
  }
  return rc;
}

const char kWebPanelLoginHtml[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Repeater Login</title>
  <style>
    :root {
      color-scheme: light;
      --accent:#2f8f4e;
      --accent-hover:#3fae61;
      --background:#f4f6f9;
      --text:#1f2937;
      --text-muted:#4b5563;
      --border:rgba(0,0,0,.08);
      --surface:#ffffff;
    }
    @media (prefers-color-scheme: dark) {
      :root {
        color-scheme: dark;
        --accent:#36a167;
        --accent-hover:#49c27d;
        --background:#222222;
        --text:#e6eaf0;
        --text-muted:#9aa4b2;
        --border:rgba(255,255,255,.08);
        --surface:#303030;
      }
    }
    html { min-height:100%; background:linear-gradient(180deg,var(--background),var(--surface)); background-repeat:no-repeat; background-attachment:fixed; }
    body { min-height:100vh; margin:0; display:grid; place-items:center; background:transparent; color:var(--text); font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,"Liberation Mono",monospace; }
    main { width:min(100%,420px); padding:20px; box-sizing:border-box; }
    .card { background:var(--surface); border:1px solid var(--border); border-radius:16px; padding:20px; }
    h1 { margin:0 0 12px; font-size:22px; }
    p { margin:0 0 16px; color:var(--text-muted); line-height:1.45; }
    input, button { width:100%; min-height:46px; box-sizing:border-box; border-radius:10px; font:inherit; }
    input { margin-bottom:12px; padding:12px; border:1px solid var(--border); background:transparent; color:var(--text); }
    button { border:none; background:var(--accent); color:#fff; cursor:pointer; font-weight:700; }
    button:hover { background:var(--accent-hover); }
    #status { min-height:1.4em; margin-top:12px; color:#c94a4a; white-space:pre-wrap; }
  </style>
</head>
<body>
  <main>
    <section class="card">
      <h1>Repeater Config</h1>
      <p>Use the repeater admin password to unlock the panel.</p>
      <input id="password" type="password" placeholder="Admin password" autocomplete="current-password">
      <button id="loginBtn">Unlock</button>
      <div id="status"></div>
    </section>
  </main>
  <script>
    const statusEl = document.getElementById("status");
    const LAST_PAGE_KEY = "repeater-last-page";
    function getPreferredPage() {
      const stored = localStorage.getItem(LAST_PAGE_KEY);
      return stored === "/stats" ? "/stats" : "/app";
    }
    async function login() {
      const pwd = document.getElementById("password").value;
      const res = await fetch("/login", { method:"POST", body: pwd });
      const text = await res.text();
      if (!res.ok) {
        statusEl.textContent = text || "Access denied";
        return;
      }
      sessionStorage.setItem("repeater-token", text.trim());
      window.location.replace(getPreferredPage());
    }
    document.getElementById("loginBtn").onclick = () => login();
    document.getElementById("password").addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        login();
      }
    });
  </script>
</body>
</html>
)HTML";

const char kWebPanelStatsDisabledHtml[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Stats Disabled</title>
  <style>
    :root {
      color-scheme: light;
      --accent:#2f8f4e;
      --background:#f4f6f9;
      --text:#1f2937;
      --text-muted:#4b5563;
      --border:rgba(0,0,0,.08);
      --surface:#ffffff;
    }
    html { min-height:100%; background:linear-gradient(180deg,var(--background),var(--surface)); background-repeat:no-repeat; background-attachment:fixed; }
    body { min-height:100vh; margin:0; display:grid; place-items:center; background:transparent; color:var(--text); font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,"Liberation Mono",monospace; }
    main { width:min(100%,460px); padding:20px; box-sizing:border-box; }
    .card { background:var(--surface); border:1px solid var(--border); border-radius:16px; padding:20px; }
    h1 { margin:0 0 12px; font-size:22px; }
    p { margin:0 0 14px; color:var(--text-muted); line-height:1.45; }
    a { color:var(--accent); text-decoration:none; font-weight:700; }
  </style>
</head>
<body>
  <main>
    <section class="card">
      <h1>Stats Disabled</h1>
      <p>The dedicated stats page is currently disabled for this node.</p>
      <p>Enable it with <strong>`set web.stats on`</strong> from the CLI or return to <a href="/app">/app</a>.</p>
    </section>
  </main>
</body>
</html>
)HTML";

const char kWebPanelAppHtml[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Repeater Config</title>
  <style>
    :root {
      color-scheme: light;
      --accent:#2f8f4e;
      --accent-hover:#3fae61;
      --background:#f4f6f9;
      --text:#1f2937;
      --text-muted:#4b5563;
      --border:rgba(0, 0, 0, 0.08);
      --surface1:#ffffff;
      --surface2:#f0f3f7;
      --card-bg:#ffffff;
      --input-bg:#ffffff;
      --terminal-bg:#f0f3f7;
      --terminal-border:rgba(0, 0, 0, 0.08);
      --terminal-cmd:#2f8f4e;
      --status-red:#c94a4a;
      --button-text:#ffffff;
      --button-secondary-text:#1f2937;
    }
    :root[data-theme="dark"] {
      color-scheme: dark;
      --accent:#36a167;
      --accent-hover:#49c27d;
      --background:#222222;
      --text:#e6eaf0;
      --text-muted:#9aa4b2;
      --border:rgba(255, 255, 255, 0.08);
      --surface1:#303030;
      --surface2:#343434;
      --card-bg:#303030;
      --input-bg:#343434;
      --terminal-bg:#222222;
      --terminal-border:rgba(255, 255, 255, 0.08);
      --terminal-cmd:#8fd3ff;
      --status-red:#d45a5a;
      --button-text:#ffffff;
      --button-secondary-text:#e6eaf0;
    }
    html { min-height:100%; background:linear-gradient(180deg,var(--background),var(--surface2)); background-repeat:no-repeat; background-attachment:fixed; }
    body { min-height:100vh; margin:0; background:transparent; color:var(--text); font-family:ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,"Liberation Mono",monospace; font-size:16px; line-height:1.4; transition:background .2s ease,color .2s ease; }
    main { max-width:920px; margin:0 auto; padding:24px; }
    .card { background:var(--card-bg); border:1px solid var(--border); border-radius:14px; padding:18px; margin-bottom:18px; }
    h1,h2,h3 { margin:0 0 12px; font-size:18px; }
    p { color:var(--text-muted); margin:8px 0 0; line-height:1.45; }
    input:not([type="checkbox"]), textarea, button, select { width:100%; min-height:44px; box-sizing:border-box; border-radius:10px; border:1px solid var(--border); background:var(--input-bg); color:var(--text); padding:12px; font-family:inherit; font-size:inherit; line-height:inherit; }
    input:not([type="checkbox"]), textarea, select, button { -webkit-appearance:none; appearance:none; }
    textarea { min-height:100px; resize:vertical; }
    button { width:auto; cursor:pointer; background:var(--accent); color:var(--button-text); border:none; font-weight:700; transition:background .2s ease,color .2s ease,border-color .2s ease; }
    button:hover { background:var(--accent-hover); }
    .row { display:grid; grid-template-columns:1fr 1fr; gap:12px; }
    .row-command { display:grid; grid-template-columns:minmax(0,1fr) auto; gap:12px; align-items:center; }
    .row3 { display:grid; grid-template-columns:1fr 1fr 1fr; gap:12px; }
    .quick { display:flex; flex-wrap:wrap; gap:10px; }
    .quick button, .iconbtn, .themebtn { background:var(--surface2); color:var(--button-secondary-text); border:1px solid var(--border); }
    .quick button:hover, .iconbtn:hover, .themebtn:hover { background:var(--surface1); }
    button.action-advert { background:linear-gradient(135deg,#d97706,#f59e0b); color:#fff7ed; border:none; }
    button.action-advert:hover { background:linear-gradient(135deg,#ea8f17,#ffb938); }
    button.action-caution { background:linear-gradient(135deg,#b94747,#d66a5f); color:#fff5f5; border:none; }
    button.action-caution:hover { background:linear-gradient(135deg,#c75656,#e57b70); }
    button.action-dreamy { background:linear-gradient(135deg,#4e7ac7,#8b7cf6); color:#f7f7ff; border:none; }
    button.action-dreamy:hover { background:linear-gradient(135deg,#5c89d4,#9a8cff); }
    .stack { display:grid; gap:12px; }
    .field-card { display:grid; gap:10px; }
    .section-group { background:var(--surface2); border:1px solid var(--border); border-radius:12px; padding:14px; display:grid; gap:12px; }
    .section-group h3 { margin:0; font-size:13px; color:var(--text-muted); text-transform:uppercase; letter-spacing:.08em; }
    .inline-actions { display:grid; grid-template-columns:minmax(0,1fr) auto auto; gap:8px; align-items:center; }
    .label { font-size:12px; color:var(--text-muted); margin-bottom:6px; display:block; }
    .fieldline { display:grid; grid-template-columns:1fr auto; gap:8px; align-items:center; }
    .iconbtn { width:44px; padding:12px 0; }
    .placeholder-slot { display:block; width:44px; height:44px; }
    .savebtn { width:100%; background:var(--accent); color:var(--button-text); border:none; }
    .savebtn:hover { background:var(--accent-hover); }
    .broker-stack { display:grid; grid-template-columns:minmax(0,1fr) minmax(0,2fr); gap:12px; align-items:start; }
    .broker-group { display:grid; gap:8px; align-content:start; }
    .broker-grid { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:10px; }
    .broker-grid.single { grid-template-columns:1fr; }
    .broker-grid.two { grid-template-columns:repeat(2,minmax(0,1fr)); }
    .broker-grid.one-two { grid-template-columns:minmax(0,1fr) minmax(0,2fr); }
    .broker-card { background:var(--surface2); border:1px solid var(--border); border-radius:12px; padding:12px; display:grid; gap:10px; min-height:124px; align-content:start; }
    .broker-row { display:flex; align-items:center; justify-content:space-between; gap:12px; }
    .broker-copy { display:grid; gap:4px; min-width:0; }
    .broker-title { font-size:12px; color:var(--text-muted); text-transform:uppercase; letter-spacing:.06em; }
    .broker-group-title { font-size:12px; color:var(--text-muted); text-transform:uppercase; letter-spacing:.08em; }
    .broker-state { font-size:13px; color:var(--text); }
    .broker-state.on { color:var(--accent); }
    .broker-mode { display:grid; gap:10px; align-content:start; height:100%; }
    .mode-slider { display:grid; gap:10px; }
    .mode-slider input[type="range"] { width:100%; min-height:auto; padding:0; border:none; background:transparent; appearance:none; -webkit-appearance:none; }
    .mode-slider input[type="range"]::-webkit-slider-runnable-track { height:12px; border-radius:999px; background:linear-gradient(90deg,var(--accent),var(--accent-hover)); border:1px solid var(--border); }
    .mode-slider input[type="range"]::-moz-range-track { height:12px; border-radius:999px; background:linear-gradient(90deg,var(--accent),var(--accent-hover)); border:1px solid var(--border); }
    .mode-slider input[type="range"]::-webkit-slider-thumb { -webkit-appearance:none; appearance:none; width:26px; height:26px; margin-top:-8px; border-radius:50%; border:2px solid var(--surface1); background:#ffffff; box-shadow:0 2px 8px rgba(0,0,0,.2); }
    .mode-slider input[type="range"]::-moz-range-thumb { width:26px; height:26px; border-radius:50%; border:2px solid var(--surface1); background:#ffffff; box-shadow:0 2px 8px rgba(0,0,0,.2); }
    .mode-slider input[type="range"]:disabled { opacity:.55; }
    .mode-labels { display:grid; grid-template-columns:repeat(4,minmax(0,1fr)); gap:8px; font-size:13px; color:var(--text-muted); text-align:center; }
    .mode-labels.two { grid-template-columns:repeat(2,minmax(0,1fr)); }
    .mode-label { padding:2px 0; border-radius:999px; transition:background .2s ease,color .2s ease; }
    .mode-label.active { background:rgba(63,174,97,.18); color:var(--text); font-weight:700; }
    :root[data-theme="dark"] .mode-label.active { background:rgba(73,194,125,.24); }
    .mode-label.disabled { opacity:.4; }
    .visually-hidden { position:absolute; width:1px; height:1px; padding:0; margin:-1px; overflow:hidden; clip:rect(0,0,0,0); white-space:nowrap; border:0; }
    .panel-copy, .panel-note, .panel-status, .panel-warning, .stats-empty, .stats-error, .events-empty, .spark-status { font-size:13px; line-height:1.45; font-weight:400; }
    .panel-warning { min-height:1.4em; color:var(--status-red); }
    .panel-note { color:var(--text-muted); }
    .panel-status { min-height:1.4em; color:var(--text-muted); }
    .themebtn { padding:10px 14px; }
    #status { white-space:pre-wrap; color:var(--text-muted); min-height:1.4em; }
    .terminal { background:var(--terminal-bg); border:1px solid var(--terminal-border); border-radius:12px; padding:14px; min-height:180px; max-height:320px; overflow:auto; font-family:inherit; font-size:14px; line-height:1.45; }
    .term-entry { margin:0 0 12px; }
    .term-cmd { color:var(--terminal-cmd); }
    .term-out { white-space:pre-wrap; color:var(--text); }
    .term-out.err { color:var(--status-red); }
    .stats-shell { display:grid; gap:12px; }
    .stats-empty, .stats-error { background:var(--surface2); border:1px dashed var(--border); border-radius:12px; padding:16px; color:var(--text-muted); }
    .stats-error { color:var(--status-red); }
    .trend-section-copy { display:grid; gap:6px; }
    .trend-section-copy p { margin:0; color:var(--text-muted); }
    .trend-grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:12px; }
    .trend-card { position:relative; background:linear-gradient(180deg,var(--surface2),var(--surface1)); border:1px solid var(--border); border-radius:14px; padding:14px; display:grid; gap:10px; }
    .trend-head { display:flex; justify-content:space-between; gap:10px; align-items:flex-start; }
    .trend-title { font-size:13px; color:var(--text-muted); text-transform:uppercase; letter-spacing:.08em; }
    .trend-value { font-size:24px; font-weight:700; color:var(--text); }
    .spark { width:100%; height:84px; border-radius:12px; background:rgba(255,255,255,.35); border:1px solid var(--border); display:block; }
    :root[data-theme="dark"] .spark { background:rgba(0,0,0,.16); }
    .spark-axis { display:flex; justify-content:space-between; gap:10px; color:var(--text-muted); min-height:1.3em; }
    .spark-axis span { font-size:12px; line-height:1.45; }
    .spark-axis span:last-child { text-align:right; }
    .spark-tooltip { position:absolute; right:14px; top:50px; padding:6px 8px; border-radius:8px; font-size:12px; color:var(--text); background:rgba(255,255,255,.92); border:1px solid var(--border); box-shadow:0 6px 18px rgba(0,0,0,.12); pointer-events:none; opacity:0; transform:translateY(-4px); transition:opacity .12s ease, transform .12s ease; }
    .spark-tooltip.visible { opacity:1; transform:translateY(0); }
    :root[data-theme="dark"] .spark-tooltip { background:rgba(24,24,24,.92); }
    .actions-bar { display:grid; grid-template-columns:1fr auto 1fr; align-items:center; gap:12px; }
    .actions-group { display:flex; gap:10px; flex-wrap:wrap; }
    .actions-group.left { justify-self:start; }
    .actions-group.center { justify-self:center; }
    .actions-group.right { justify-self:end; }
    .actions-group .active { background:var(--accent); color:var(--button-text); border-color:transparent; }
    .actions-group .active:hover { background:var(--accent-hover); }
    .hud-grid-1 { display:grid; grid-template-columns:1fr; gap:12px; }
    .hud-grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(220px,1fr)); gap:12px; }
    .hud-grid-2 { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:12px; }
    .hud-grid-3 { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:12px; }
    .hud-card { background:linear-gradient(180deg,var(--surface2),var(--surface1)); border:1px solid var(--border); border-radius:14px; padding:14px; display:grid; gap:12px; }
    .hud-card h3 { margin:0; font-size:13px; color:var(--text-muted); text-transform:uppercase; letter-spacing:.08em; }
    .hud-row { display:grid; gap:10px; }
    .hud-kpi { display:flex; align-items:flex-end; justify-content:space-between; gap:10px; }
    .hud-value { font-size:28px; font-weight:700; line-height:1; color:var(--text); }
    .hud-sub { font-size:12px; color:var(--text-muted); }
    .meter { height:10px; background:var(--background); border:1px solid var(--border); border-radius:999px; overflow:hidden; }
    .meter-fill { height:100%; border-radius:999px; background:linear-gradient(90deg,var(--accent),var(--accent-hover)); }
    .meter-fill.ok { background:linear-gradient(90deg,#6ea43f,#8cb857); }
    .meter-fill.warn { background:linear-gradient(90deg,#d7a531,#e9bf52); }
    .meter-fill.bad { background:linear-gradient(90deg,#bf4b4b,#dd6a6a); }
    .metric-grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:10px; }
    .core-grid { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:12px; }
    .core-grid .hud-row { align-content:start; }
    .core-metrics { grid-template-columns:repeat(4,minmax(0,1fr)); }
    .metric { background:rgba(255,255,255,.45); border:1px solid var(--border); border-radius:12px; padding:10px; }
    :root[data-theme="dark"] .metric { background:rgba(0,0,0,.16); }
    .metric-label { font-size:11px; color:var(--text-muted); text-transform:uppercase; letter-spacing:.06em; }
    .metric-value { margin-top:4px; font-size:16px; font-weight:700; color:var(--text); }
    .events-table-wrap { overflow-x:auto; border:1px solid var(--border); border-radius:12px; }
    .events-table { width:100%; border-collapse:collapse; }
    .events-table th, .events-table td { padding:10px 12px; text-align:left; font-size:13px; }
    .events-table th { color:var(--text-muted); text-transform:uppercase; letter-spacing:.06em; font-weight:700; background:rgba(255,255,255,.3); }
    .events-table td { color:var(--text); border-top:1px solid var(--border); }
    .events-table a { color:var(--accent); text-decoration:none; font-weight:700; }
    .events-table a:hover { text-decoration:underline; }
    :root[data-theme="dark"] .events-table th { background:rgba(0,0,0,.16); }
    .events-empty { color:var(--text-muted); }
    @media (max-width:760px) {
      body { font-size:15px; }
      main { padding:16px; }
      .card { padding:16px; margin-bottom:14px; }
      .row, .row3, .row-command, .metric-grid, .trend-grid, .hud-grid-1, .hud-grid-2, .hud-grid-3, .core-grid, .core-metrics, .broker-stack, .broker-grid, .broker-grid.single, .broker-grid.two, .broker-grid.one-two { grid-template-columns:1fr; }
      .inline-actions { grid-template-columns:minmax(0,1fr) auto auto; }
      .fieldline { grid-template-columns:minmax(0,1fr) auto; align-items:center; }
      .row-command button { width:100%; }
      .fieldline .iconbtn { width:44px; }
      .row-command button { justify-self:stretch; }
      .hud-kpi { align-items:flex-start; flex-direction:column; }
      .quick { gap:8px; }
      #quickCommandsPanel .quick { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); }
      .quick button, .themebtn { width:100%; }
      .actions-bar { display:grid; grid-template-columns:1fr; gap:10px; }
      .actions-group { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); }
      .actions-group.left, .actions-group.center, .actions-group.right { justify-self:stretch; }
      .actions-group.center { grid-template-columns:repeat(3,minmax(0,1fr)); justify-items:stretch; }
      .actions-group.right { grid-template-columns:repeat(3,minmax(0,1fr)); justify-items:stretch; }
      #actionsPanel .actions-group button { width:100%; }
      #actionsPanel .actions-group.center button { width:100%; min-width:0; }
      #actionsPanel .actions-group.right button { width:100%; min-width:0; }
      .broker-row { gap:10px; }
      .row > div + div, .row3 > div + div { margin-top:4px; }
      .row > div[style*="align-self:end"] { padding-top:4px; }
    }
  </style>
</head>
<body>
  <main>
    <section class="card" id="login" style="display:none">
      <h1>Repeater Config</h1>
      <p>Use the repeater admin password to unlock the command console.</p>
      <div class="row" style="margin-top:14px">
        <input id="password" type="password" placeholder="Admin password" maxlength="15">
        <button id="loginBtn">Unlock</button>
      </div>
      <div id="status"></div>
    </section>

    <section class="card" id="actionsPanel" style="display:none">
      <h2>Actions</h2>
      <div class="actions-bar">
        <div class="actions-group left">
          <button id="appPageBtn" class="themebtn">App</button>
          <button id="statsPageBtn" class="themebtn">Stats</button>
        </div>
        <div class="actions-group center">
          <button id="advertBtn" class="action-advert">Advert</button>
          <button id="otaBtn" class="action-dreamy">Start OTA</button>
          <button id="refreshPageBtn" class="action-dreamy">Refresh</button>
        </div>
        <div class="actions-group right">
          <button id="rebootBtn" class="action-caution">Reboot</button>
          <button id="themeToggle" class="themebtn" aria-label="Toggle theme" title="Toggle theme">☾</button>
          <button id="logoutBtn" class="themebtn action-caution">Logout</button>
        </div>
      </div>
    </section>

    <section class="card" id="quickCommandsPanel" style="display:none">
      <h2>Quick "get" Commands</h2>
      <div class="stack">
        <div>
          <span class="label">WiFi</span>
          <div class="quick">
            <button data-cmd="get wifi.status">wifi.status</button>
            <button data-cmd="get wifi.powersaving">wifi.powersaving</button>
          </div>
        </div>
        <div>
          <span class="label">MQTT</span>
          <div class="quick">
            <button data-cmd="get mqtt.status">mqtt.status</button>
            <button data-cmd="get mqtt.iata">mqtt.iata</button>
            <button data-cmd="get mqtt.owner">mqtt.owner</button>
            <button data-cmd="get mqtt.email">mqtt.email</button>
          </div>
        </div>
      </div>
    </section>

	    <section class="card" id="cliPanel" style="display:none">
      <h2>Run CLI Command</h2>
      <div class="row-command">
        <input id="command" placeholder="get mqtt.status">
        <button id="runBtn">Run</button>
      </div>
      <p class="panel-copy">Only the allowlisted commands exposed by this panel will run here.</p>
      <div id="reply" class="terminal"></div>
    </section>

    <section class="card" id="infoPanel" style="display:none">
      <h2>Info</h2>
      <div class="stack">
        <div class="row">
          <div class="field-card">
            <label class="label" for="roleValue">Role</label>
            <div class="fieldline">
              <input id="roleValue" readonly disabled>
              <span class="placeholder-slot" aria-hidden="true"></span>
            </div>
          </div>
          <div class="field-card">
            <label class="label" for="clockUtc">Clock UTC</label>
            <div class="inline-actions">
              <input id="clockUtc" readonly disabled>
              <button class="iconbtn" data-load-cmd="clock" data-load-input="clockUtc" title="Refresh clock UTC">&#8635;</button>
              <button id="syncClockBtn" class="savebtn">Sync</button>
            </div>
          </div>
        </div>
        <div class="field-card">
          <label class="label" for="publicKey">Public Key</label>
          <div class="fieldline">
            <input id="publicKey" readonly disabled>
            <button id="copyPublicKeyBtn" class="iconbtn" title="Copy public key">&#x2398;</button>
          </div>
        </div>
      </div>
    </section>

    <section class="card" id="repeaterSettingsPanel" style="display:none">
      <h2>Repeater Settings</h2>
      <div class="stack">
        <div class="section-group">
          <h3>Repeater Settings</h3>
          <div class="field-card">
            <label class="label" for="nodeName">Device Name</label>
            <div class="inline-actions">
              <input id="nodeName" placeholder="MeshCore-HOWL">
              <button class="iconbtn" data-load-cmd="get name" data-load-input="nodeName" title="Refresh device name">&#8635;</button>
              <button class="savebtn" data-prefix="set name " data-input="nodeName">Save</button>
            </div>
          </div>
          <div class="row">
            <div class="field-card">
              <div>
              <label class="label" for="nodeLat">Latitude</label>
              <div class="fieldline">
                <input id="nodeLat" placeholder="0.0">
                <button class="iconbtn" data-load-cmd="get lat" data-load-input="nodeLat" title="Refresh latitude">&#8635;</button>
              </div>
              </div>
              <button class="savebtn" data-prefix="set lat " data-input="nodeLat">Save latitude</button>
            </div>
            <div class="field-card">
              <div>
              <label class="label" for="nodeLon">Longitude</label>
              <div class="fieldline">
                <input id="nodeLon" placeholder="0.0">
                <button class="iconbtn" data-load-cmd="get lon" data-load-input="nodeLon" title="Refresh longitude">&#8635;</button>
              </div>
              </div>
              <button class="savebtn" data-prefix="set lon " data-input="nodeLon">Save longitude</button>
            </div>
          </div>
          <div class="field-card">
            <label class="label" for="privateKey">Private Key</label>
            <div class="inline-actions">
              <input id="privateKey" type="password" placeholder="64-hex-char private key">
              <button id="copyPrivateKeyBtn" class="iconbtn" title="Copy private key">&#x2398;</button>
              <button class="savebtn" data-prefix="set prv.key " data-input="privateKey">Save</button>
            </div>
            <p class="panel-copy">Changing the private key requires a reboot to apply.</p>
          </div>
          <div class="field-card">
            <label class="label" for="ownerInfo">Owner Info</label>
            <div class="inline-actions">
              <textarea id="ownerInfo" placeholder="Free text shown in owner info"></textarea>
              <button class="iconbtn" data-load-cmd="get owner.info" data-load-input="ownerInfo" data-load-format="multiline" title="Refresh owner info">&#8635;</button>
              <button id="saveOwnerInfo" class="savebtn">Save</button>
            </div>
          </div>
        </div>
        <div class="section-group">
          <h3>Access</h3>
          <div class="field-card">
            <label class="label" for="adminPassword">Admin Password</label>
            <div class="inline-actions">
              <input id="adminPassword" type="password" placeholder="new admin password" maxlength="15">
	            <span class="placeholder-slot" aria-hidden="true"></span>
              <button class="savebtn" data-prefix="password " data-input="adminPassword">Save</button>
            </div>
          </div>
          <div class="field-card">
            <label class="label" for="guestPassword">Guest Password</label>
            <div class="inline-actions">
              <input id="guestPassword" type="password" placeholder="new guest password" maxlength="15">
	            <span class="placeholder-slot" aria-hidden="true"></span>
              <button class="savebtn" data-prefix="set guest.password " data-input="guestPassword">Save</button>
            </div>
          </div>
        </div>
        <div class="section-group">
          <h3>Radio Settings</h3>
          <div class="field-card">
            <label class="label" for="radioPreset">Preset</label>
            <div class="row">
              <div class="field-card">
                <div>
                  <label class="label" for="radioCurrent">Current Radio</label>
                  <div class="fieldline">
                    <input id="radioCurrent" placeholder="915.800 / BW250 / SF10 / CR5" readonly disabled>
                    <button id="refreshRadioBtn" class="iconbtn" title="Refresh current radio">&#8635;</button>
                  </div>
                </div>
              </div>
              <div class="field-card">
                <div>
                  <label class="label" for="radioPreset">Preset</label>
                  <div class="inline-actions">
                    <select id="radioPreset">
                      <option value="">Loading presets...</option>
                    </select>
                    <button id="reloadRadioPresetsBtn" class="iconbtn" title="Reload presets">&#8635;</button>
                    <button id="applyRadioPresetBtn" class="savebtn">Save</button>
                  </div>
                </div>
              </div>
            </div>
            <div id="radioPresetStatus" class="panel-status"></div>
          </div>
          <div class="field-card">
            <label class="label" for="pathHashMode">Path Hash Mode</label>
            <div class="inline-actions">
              <select id="pathHashMode">
                <option value="0">1-byte</option>
                <option value="1">2-byte</option>
                <option value="2">3-byte</option>
              </select>
              <button class="iconbtn" data-load-cmd="get path.hash.mode" data-load-input="pathHashMode" title="Refresh path hash mode">&#8635;</button>
              <button class="savebtn" data-prefix="set path.hash.mode " data-input="pathHashMode">Save</button>
            </div>
          </div>
        </div>
        <div class="section-group">
          <h3>Advertising</h3>
          <div class="row3">
            <div class="field-card">
              <div>
              <label class="label" for="advertInterval">Advert Interval (minutes)</label>
              <div class="fieldline">
                <input id="advertInterval" placeholder="2">
                <button class="iconbtn" data-load-cmd="get advert.interval" data-load-input="advertInterval" title="Refresh advert interval">&#8635;</button>
              </div>
              </div>
              <button class="savebtn" data-prefix="set advert.interval " data-input="advertInterval">Save advert interval</button>
            </div>
            <div class="field-card">
              <div>
              <label class="label" for="floodInterval">Flood Interval (hours)</label>
              <div class="fieldline">
                <input id="floodInterval" placeholder="12">
                <button class="iconbtn" data-load-cmd="get flood.advert.interval" data-load-input="floodInterval" title="Refresh flood interval">&#8635;</button>
              </div>
              </div>
              <button class="savebtn" data-prefix="set flood.advert.interval " data-input="floodInterval">Save flood interval</button>
            </div>
            <div class="field-card">
              <div>
              <label class="label" for="floodMax">Flood Max</label>
                <div class="fieldline">
                  <input id="floodMax" placeholder="64">
                  <button class="iconbtn" data-load-cmd="get flood.max" data-load-input="floodMax" title="Refresh flood max">&#8635;</button>
                </div>
              </div>
              <button class="savebtn" data-prefix="set flood.max " data-input="floodMax">Save flood max</button>
            </div>
          </div>
        </div>
        <div class="section-group">
          <h3>Modes</h3>
          <div class="broker-grid one-two">
            <div class="broker-card">
              <div class="broker-mode">
                <div class="broker-row">
                  <div class="broker-copy">
                    <div class="broker-title">Ghost Node Mode</div>
                    <div class="broker-state" id="ghostNodeModeState">Off</div>
                  </div>
                </div>
                <div class="mode-slider" style="margin-top:10px">
                  <input id="ghostNodeMode" type="range" min="0" max="1" step="1" value="0" aria-label="Ghost node mode">
                  <div class="mode-labels two" aria-hidden="true">
                    <div class="mode-label" data-ghost-label="off">Off</div>
                    <div class="mode-label" data-ghost-label="on">On</div>
                  </div>
                </div>
                <input id="ghostNodeModeToggle" class="visually-hidden" type="checkbox" tabindex="-1" aria-hidden="true">
                <div id="ghostNodeModeStatus" class="panel-status"></div>
              </div>
            </div>
            <div class="broker-card">
              <div class="broker-mode">
                <div class="broker-row">
                  <div class="broker-copy">
                    <div class="broker-title">Description</div>
                  </div>
                </div>
                <div class="panel-note">Keeps web and MQTT running, but turns repeat off and disables both local and flood adverts. Turning it back off restores the prior repeat and advert settings if known, else falls back to repeat on, advert interval 60 minutes, and flood advert interval 12 hours.</div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </section>

    <section class="card" id="mqttSettingsPanel" style="display:none">
      <h2>MQTT Settings</h2>
      <div class="stack">
        <div class="field-card">
          <label class="label" for="mqttIata">MQTT IATA</label>
          <div class="inline-actions">
            <select id="mqttIata">
              <optgroup label="ACT">
                <option value="CBR">CBR - Canberra</option>
              </optgroup>
              <optgroup label="New South Wales">
                <option value="ABX">ABX - Albury</option>
                <option value="ARM">ARM - Armidale</option>
                <option value="BHQ">BHQ - Broken Hill</option>
                <option value="BNK">BNK - Ballina</option>
                <option value="CFS">CFS - Coffs Harbour</option>
                <option value="DBO">DBO - Dubbo</option>
                <option value="GFF">GFF - Griffith</option>
                <option value="GFN">GFN - Grafton</option>
                <option value="LDH">LDH - Lord Howe Island</option>
                <option value="LSY">LSY - Lismore</option>
                <option value="MIM">MIM - Merimbula</option>
                <option value="MRZ">MRZ - Moree</option>
                <option value="MYA">MYA - Moruya</option>
                <option value="NTL">NTL - Newcastle</option>
                <option value="OAG">OAG - Orange</option>
                <option value="PQQ">PQQ - Port Macquarie</option>
                <option value="SYD">SYD - Sydney</option>
                <option value="WGA">WGA - Wagga Wagga</option>
              </optgroup>
              <optgroup label="Queensland">
                <option value="ABM">ABM - Bamaga</option>
                <option value="BNE">BNE - Brisbane</option>
                <option value="CNS">CNS - Cairns</option>
                <option value="HTI">HTI - Hamilton Island</option>
                <option value="HVB">HVB - Hervey Bay</option>
                <option value="ISA">ISA - Mount Isa</option>
                <option value="LRE">LRE - Longreach</option>
                <option value="MCY">MCY - Sunshine Coast</option>
                <option value="MKY">MKY - Mackay</option>
                <option value="OOL">OOL - Gold Coast</option>
                <option value="PPP">PPP - Proserpine</option>
                <option value="ROK">ROK - Rockhampton</option>
                <option value="TSV">TSV - Townsville</option>
                <option value="WEI">WEI - Weipa</option>
                <option value="WTB">WTB - Toowoomba Wellcamp</option>
              </optgroup>
              <optgroup label="South Australia">
                <option value="ADL">ADL - Adelaide</option>
                <option value="KGC">KGC - Kingscote</option>
                <option value="MGB">MGB - Mount Gambier</option>
                <option value="PLO">PLO - Port Lincoln</option>
                <option value="WYA">WYA - Whyalla</option>
              </optgroup>
              <optgroup label="Tasmania">
                <option value="BWT">BWT - Burnie</option>
                <option value="DPO">DPO - Devonport</option>
                <option value="FLS">FLS - Flinders Island</option>
                <option value="HBA">HBA - Hobart</option>
                <option value="KNS">KNS - King Island</option>
                <option value="LST">LST - Launceston</option>
              </optgroup>
              <optgroup label="Victoria">
                <option value="AVV">AVV - Avalon</option>
                <option value="GEX">GEX - Geelong West</option>
                <option value="MEB">MEB - Essendon Fields</option>
                <option value="MEL" selected>MEL - Melbourne</option>
                <option value="MQL">MQL - Mildura</option>
              </optgroup>
            </select>
            <button class="iconbtn" data-load-cmd="get mqtt.iata" data-load-input="mqttIata" title="Refresh MQTT IATA">&#8635;</button>
            <button class="savebtn" data-prefix="set mqtt.iata " data-input="mqttIata">Save</button>
          </div>
        </div>
        <div class="field-card">
          <label class="label" for="mqttOwner">MQTT Owner</label>
          <div class="inline-actions">
            <input id="mqttOwner" placeholder="64-hex-char owner key">
            <button class="iconbtn" data-load-cmd="get mqtt.owner" data-load-input="mqttOwner" title="Refresh MQTT owner">&#8635;</button>
            <button class="savebtn" data-prefix="set mqtt.owner " data-input="mqttOwner">Save</button>
          </div>
        </div>
	        <div class="field-card">
	          <label class="label" for="mqttEmail">MQTT Email</label>
	          <div class="inline-actions">
	            <input id="mqttEmail" type="email" placeholder="owner@example.com">
	            <button class="iconbtn" data-load-cmd="get mqtt.email" data-load-input="mqttEmail" title="Refresh MQTT email">&#8635;</button>
	            <button class="savebtn" data-prefix="set mqtt.email " data-input="mqttEmail">Save</button>
	          </div>
	        </div>
	        <div class="field-card">
	          <label class="label">MQTT Servers</label>
	          <div class="broker-stack">
	            <div class="broker-group">
	              <div class="broker-group-title">EastMesh</div>
		              <div class="broker-grid single">
	                <div class="broker-card">
	                  <div class="broker-mode">
	                    <div class="broker-row">
	                      <div class="broker-copy">
	                        <div class="broker-title">EastMesh AU</div>
	                        <div class="broker-state" id="mqttEastmeshAuState">Off</div>
	                      </div>
	                    </div>
	                    <div class="mode-slider">
	                      <input id="mqttEastmeshMode" type="range" min="0" max="1" step="1" value="0" aria-label="EastMesh mode">
	                      <div class="mode-labels two" aria-hidden="true">
	                        <div class="mode-label" data-eastmesh-label="off">Off</div>
	                        <div class="mode-label" data-eastmesh-label="on">On</div>
	                      </div>
	                    </div>
	                    <input id="mqttEastmeshAu" class="visually-hidden" type="checkbox" tabindex="-1" aria-hidden="true">
	                  </div>
	                </div>
	              </div>
	            </div>
	            <div class="broker-group">
	              <div class="broker-group-title">LetsMesh</div>
	              <div class="broker-grid single">
	                <div class="broker-card">
	                  <div class="broker-mode">
	                    <div class="broker-row">
	                      <div class="broker-copy">
	                        <div class="broker-title">LetsMesh Mode</div>
	                        <div class="broker-state" id="mqttLetsmeshModeState">Off</div>
	                      </div>
	                    </div>
	                    <div class="mode-slider">
	                      <input id="mqttLetsmeshMode" type="range" min="0" max="3" step="1" value="0" aria-label="LetsMesh mode">
	                      <div class="mode-labels" aria-hidden="true">
	                        <div class="mode-label" data-letsmesh-label="off">Off</div>
	                        <div class="mode-label" data-letsmesh-label="eu">EU</div>
	                        <div class="mode-label" data-letsmesh-label="us">US</div>
	                        <div class="mode-label" data-letsmesh-label="both">Both</div>
	                      </div>
	                    </div>
	                    <input id="mqttLetsmeshEu" class="visually-hidden" type="checkbox" tabindex="-1" aria-hidden="true">
	                    <input id="mqttLetsmeshUs" class="visually-hidden" type="checkbox" tabindex="-1" aria-hidden="true">
	                  </div>
	                </div>
	              </div>
	            </div>
	          </div>
	          <div class="panel-note">If EastMesh is enabled, use only one LetsMesh broker. Enable both LetsMesh brokers only when EastMesh is off.</div>
	          <div id="mqttBrokerWarning" class="panel-warning"></div>
	        </div>
	      </div>
	    </section>

    <section class="card" id="statsPanel" style="display:none">
      <h2>Stats</h2>
      <div class="quick" style="margin-bottom:12px">
        <button id="openStatsPanelBtn">Open /stats</button>
      </div>
      <div class="stats-empty">Current status and historical trends have moved to the dedicated stats page.</div>
    </section>

    <section class="card" id="statsPagePanel" style="display:none">
      <h2>Stats</h2>
      <div id="statsSummary" class="stats-shell">
        <div class="stats-empty">Loading summary...</div>
      </div>
      <section class="hud-card" style="margin-top:12px">
        <h3>Trends</h3>
        <div class="trend-section-copy">
          <p id="statsTrendIntro" class="panel-copy">Loading recent trend guidance...</p>
        </div>
        <div class="trend-grid" id="statsTrends"></div>
      </section>
      <div id="statsNeighbours" style="margin-top:12px">
        <section class="hud-card">
          <h3>Neighbours</h3>
          <div class="events-empty">Loading neighbours...</div>
        </section>
      </div>
      <div id="statsEvents" style="margin-top:12px">
        <section class="hud-card">
          <h3>Events</h3>
          <div class="events-empty">Loading events...</div>
        </section>
      </div>
    </section>

  </main>
  <script>
    const RADIO_PRESETS_URL = "https://api.meshcore.nz/api/v1/config";
    const isStatsPage = window.location.pathname === "/stats";
    const LAST_PAGE_KEY = "repeater-last-page";
    const PANEL_TITLE_KEY = "repeater-panel-title";
    let token = sessionStorage.getItem("repeater-token") || "";
    let commandQueue = Promise.resolve();
    let radioPresetEntries = [];
    let currentRadioConfig = null;
    const statusEl = document.getElementById("status");
    const replyEl = document.getElementById("reply");
    const themeToggleEl = document.getElementById("themeToggle");
    const rootEl = document.documentElement;
    function updatePanelTitle(nameValue) {
      const fallbackTitle = "Repeater Config";
      const trimmedName = String(nameValue == null ? "" : nameValue).trim();
      const nextTitle = trimmedName ? trimmedName : fallbackTitle;
      document.title = nextTitle;
      if (trimmedName) {
        localStorage.setItem(PANEL_TITLE_KEY, trimmedName);
      } else {
        localStorage.removeItem(PANEL_TITLE_KEY);
      }
    }
    function applyCachedPanelTitle() {
      const cachedTitle = localStorage.getItem(PANEL_TITLE_KEY);
      if (cachedTitle && cachedTitle.trim()) {
        document.title = cachedTitle.trim();
      }
    }
    function rememberCurrentPage() {
      localStorage.setItem(LAST_PAGE_KEY, isStatsPage ? "/stats" : "/app");
    }
    function redirectToLogin() {
      rememberCurrentPage();
      sessionStorage.removeItem("repeater-token");
      token = "";
      window.location.replace("/");
    }
    rememberCurrentPage();
    applyCachedPanelTitle();
    function getPreferredTheme() {
      const saved = localStorage.getItem("repeater-theme");
      if (saved === "light" || saved === "dark") return saved;
      return window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
    }
    function applyTheme(theme) {
      rootEl.dataset.theme = theme;
      themeToggleEl.textContent = theme === "dark" ? "☀" : "☾";
      themeToggleEl.title = theme === "dark" ? "Switch to light mode" : "Switch to dark mode";
      themeToggleEl.setAttribute("aria-label", themeToggleEl.title);
    }
    function syncNavButton() {
      const appBtn = document.getElementById("appPageBtn");
      const statsBtn = document.getElementById("statsPageBtn");
      const centerGroup = document.querySelector("#actionsPanel .actions-group.center");
      const advertBtn = document.getElementById("advertBtn");
      const otaBtn = document.getElementById("otaBtn");
      const refreshBtn = document.getElementById("refreshPageBtn");
      if (appBtn) {
        appBtn.classList.toggle("active", !isStatsPage);
        appBtn.title = "Open app page";
        appBtn.onclick = () => window.location.assign("/app");
      }
      if (statsBtn) {
        statsBtn.classList.toggle("active", isStatsPage);
        statsBtn.title = "Open stats page";
        statsBtn.onclick = () => window.location.assign("/stats");
      }
      if (advertBtn) advertBtn.style.display = isStatsPage ? "none" : "";
      if (otaBtn) otaBtn.style.display = isStatsPage ? "none" : "";
      if (refreshBtn) refreshBtn.style.display = isStatsPage ? "" : "none";
      if (centerGroup) {
        centerGroup.style.gridTemplateColumns = isStatsPage
          ? "1fr"
          : "repeat(2,minmax(0,1fr))";
      }
    }
    function toggleTheme() {
      const next = rootEl.dataset.theme === "dark" ? "light" : "dark";
      localStorage.setItem("repeater-theme", next);
      applyTheme(next);
    }
    applyTheme(getPreferredTheme());
    syncNavButton();
    themeToggleEl.onclick = toggleTheme;
    function appendHistory(cmd, text, ok) {
      const entry = document.createElement("div");
      entry.className = "term-entry";
      const cmdLine = document.createElement("div");
      cmdLine.className = "term-cmd";
      cmdLine.textContent = "> " + cmd;
      const outLine = document.createElement("div");
      outLine.className = "term-out" + (ok ? "" : " err");
      outLine.textContent = text && text.length ? text : "OK";
      entry.appendChild(cmdLine);
      entry.appendChild(outLine);
      replyEl.appendChild(entry);
      replyEl.scrollTop = replyEl.scrollHeight;
    }
    function parseReplyValue(text) {
      return (text || "").replace(/^>\s*/, "").trim();
    }
    function isCommandError(text) {
      const value = parseReplyValue(text).toLowerCase();
      return value.startsWith("err") || value.startsWith("(err") || value.startsWith("error");
    }
    function clamp(value, min, max) {
      return Math.min(max, Math.max(min, value));
    }
    function pctRange(value, min, max) {
      if (!Number.isFinite(value) || max === min) return 0;
      return clamp(((value - min) * 100) / (max - min), 0, 100);
    }
    function pctRatio(value, total) {
      if (!Number.isFinite(value) || !Number.isFinite(total) || total <= 0) return 0;
      return clamp((value * 100) / total, 0, 100);
    }
    function escapeHtml(value) {
      return String(value == null ? "" : value)
          .replace(/&/g, "&amp;")
          .replace(/</g, "&lt;")
          .replace(/>/g, "&gt;")
          .replace(/\"/g, "&quot;")
          .replace(/'/g, "&#39;");
    }
    function formatDuration(seconds) {
      if (!Number.isFinite(seconds)) return "--";
      const secs = Math.max(0, Math.round(seconds));
      if (secs < 3600) {
        return Math.floor(secs / 60) + "m " + (secs % 60) + "s";
      }
      if (secs < 86400) {
        return Math.floor(secs / 3600) + "h " + Math.floor((secs % 3600) / 60) + "m";
      }
      return Math.floor(secs / 86400) + "d " + Math.floor((secs % 86400) / 3600) + "h";
    }
    function formatBytes(value) {
      if (!Number.isFinite(value)) return "--";
      const abs = Math.abs(value);
      if (abs >= 1024 * 1024) return (value / (1024 * 1024)).toFixed(2) + " MB";
      if (abs >= 1024) return (value / 1024).toFixed(1) + " KB";
      return Math.round(value) + " B";
    }
    function formatRelativeAge(seconds) {
      if (!Number.isFinite(seconds) || seconds <= 0) return "now";
      const secs = Math.max(0, Math.round(seconds));
      if (secs < 3600) {
        return Math.floor(secs / 60) + "m ago";
      }
      if (secs < 86400) {
        return Math.floor(secs / 3600) + "h ago";
      }
      return Math.floor(secs / 86400) + "d ago";
    }
    function formatDecimal(value, digits) {
      const num = Number.parseFloat(value);
      if (!Number.isFinite(num)) return "";
      return num.toFixed(digits);
    }
    function normalizeRadioConfig(config) {
      if (!config) return null;
      const frequency = Number.parseFloat(config.frequency);
      const bandwidth = Number.parseFloat(config.bandwidth);
      const spreadingFactor = Number.parseInt(config.spreadingFactor, 10);
      const codingRate = Number.parseInt(config.codingRate, 10);
      if (!Number.isFinite(frequency) || !Number.isFinite(bandwidth) || !Number.isFinite(spreadingFactor) || !Number.isFinite(codingRate)) {
        return null;
      }
      return {
        frequency,
        bandwidth,
        spreadingFactor,
        codingRate
      };
    }
    function radioSignature(config) {
      const normalized = normalizeRadioConfig(config);
      if (!normalized) return "";
      return [
        normalized.frequency.toFixed(3),
        normalized.bandwidth.toFixed(3),
        normalized.spreadingFactor,
        normalized.codingRate
      ].join("|");
    }
    function formatRadioConfig(config) {
      const normalized = normalizeRadioConfig(config);
      if (!normalized) return "";
      return `${normalized.frequency.toFixed(3)} / BW${normalized.bandwidth.toFixed(3)} / SF${normalized.spreadingFactor} / CR${normalized.codingRate}`;
    }
    function parseRadioValue(value) {
      const parts = String(value || "").split(",");
      if (parts.length !== 4) return null;
      return normalizeRadioConfig({
        frequency: parts[0],
        bandwidth: parts[1],
        spreadingFactor: parts[2],
        codingRate: parts[3]
      });
    }
    function setRadioPresetStatus(message, isError) {
      const el = document.getElementById("radioPresetStatus");
      if (!el) return;
      el.textContent = message || "";
      el.style.color = isError ? "var(--status-red)" : "var(--text-muted)";
    }
    function setGhostNodeModeStatus(message, isError) {
      const el = document.getElementById("ghostNodeModeStatus");
      if (!el) return;
      el.textContent = message || "";
      el.style.color = isError ? "var(--status-red)" : "var(--text-muted)";
    }
    function parseIntegerValue(value) {
      const parsed = Number.parseInt(String(value || "").trim(), 10);
      return Number.isFinite(parsed) ? parsed : null;
    }
    function readGhostNodeStoredState() {
      const repeat = sessionStorage.getItem("ghost-node-repeat");
      const advert = parseIntegerValue(sessionStorage.getItem("ghost-node-advert"));
      const flood = parseIntegerValue(sessionStorage.getItem("ghost-node-flood"));
      return {
        repeat: repeat === "off" ? "off" : "on",
        advert,
        flood
      };
    }
    function saveGhostNodeStoredState(repeatState, advertInterval, floodInterval) {
      sessionStorage.setItem("ghost-node-repeat", repeatState === "off" ? "off" : "on");
      sessionStorage.setItem("ghost-node-advert", String(advertInterval));
      sessionStorage.setItem("ghost-node-flood", String(floodInterval));
    }
    function clearGhostNodeStoredState() {
      sessionStorage.removeItem("ghost-node-repeat");
      sessionStorage.removeItem("ghost-node-advert");
      sessionStorage.removeItem("ghost-node-flood");
    }
    function refreshGhostNodeModeUi(enabled) {
      const input = document.getElementById("ghostNodeModeToggle");
      if (input) input.checked = !!enabled;
      const slider = document.getElementById("ghostNodeMode");
      if (slider) slider.value = enabled ? "1" : "0";
      const state = document.getElementById("ghostNodeModeState");
      if (state) {
        state.textContent = enabled ? "On" : "Off";
        state.classList.toggle("on", !!enabled);
      }
      document.querySelectorAll("[data-ghost-label]").forEach((label) => {
        label.classList.toggle("active", label.dataset.ghostLabel === (enabled ? "on" : "off"));
      });
    }
    async function loadGhostNodeModeState(options = {}) {
      const repeatResult = await runCommand("get repeat", options);
      if (!repeatResult.ok) return;
      const repeatState = parseReplyValue(repeatResult.text).toLowerCase();
      const advertInterval = parseIntegerValue(document.getElementById("advertInterval").value);
      const floodInterval = parseIntegerValue(document.getElementById("floodInterval").value);
      const enabled = repeatState === "off" && advertInterval === 0 && floodInterval === 0;
      refreshGhostNodeModeUi(enabled);
      if (!enabled) setGhostNodeModeStatus("", false);
    }
    async function setGhostNodeMode(enabled) {
      const currentAdvert = parseIntegerValue(document.getElementById("advertInterval").value);
      const currentFlood = parseIntegerValue(document.getElementById("floodInterval").value);
      if (enabled) {
        const repeatResult = await runCommand("get repeat", { recordHistory:false, updateInput:false });
        if (!repeatResult.ok) {
          setGhostNodeModeStatus(parseReplyValue(repeatResult.text) || "Unable to read repeat state.", true);
          refreshGhostNodeModeUi(false);
          return;
        }
        const repeatState = parseReplyValue(repeatResult.text).toLowerCase() === "off" ? "off" : "on";
        saveGhostNodeStoredState(
          repeatState,
          currentAdvert !== null ? currentAdvert : 60,
          currentFlood !== null ? currentFlood : 12
        );
      }
      const restore = readGhostNodeStoredState();
      const commands = enabled
        ? ["set repeat off", "set advert.interval 0", "set flood.advert.interval 0"]
        : [
            `set repeat ${restore.repeat === "off" ? "off" : "on"}`,
            `set advert.interval ${restore.advert !== null && restore.advert > 0 ? restore.advert : 60}`,
            `set flood.advert.interval ${restore.flood !== null && restore.flood > 0 ? restore.flood : 12}`
          ];
      setGhostNodeModeStatus(enabled ? "Applying ghost node mode..." : "Restoring repeater mode...", false);
      for (const command of commands) {
        const result = await runCommand(command);
        if (!result.ok) {
          setGhostNodeModeStatus(parseReplyValue(result.text) || ("Unable to apply: " + command), true);
          await loadGhostNodeModeState({ recordHistory:false, updateInput:false });
          return;
        }
      }
      await Promise.all([
        loadField("get advert.interval", "advertInterval", null, { recordHistory:false, updateInput:false }),
        loadField("get flood.advert.interval", "floodInterval", null, { recordHistory:false, updateInput:false })
      ]);
      if (!enabled) clearGhostNodeStoredState();
      await loadGhostNodeModeState({ recordHistory:false, updateInput:false });
      setGhostNodeModeStatus(
        enabled ? "Ghost Node Mode enabled. Repeat is off and adverts are disabled."
                : "Ghost Node Mode disabled. Repeat and advert settings restored.",
        false
      );
    }
    function syncRadioPresetUi() {
      const currentEl = document.getElementById("radioCurrent");
      if (currentEl) {
        currentEl.value = currentRadioConfig ? formatRadioConfig(currentRadioConfig) : "";
      }
      const selectEl = document.getElementById("radioPreset");
      const applyBtn = document.getElementById("applyRadioPresetBtn");
      if (!selectEl || !applyBtn) return;
      const currentSig = radioSignature(currentRadioConfig);
      let matchedIndex = -1;
      for (let i = 0; i < radioPresetEntries.length; i++) {
        if (radioSignature(radioPresetEntries[i]) === currentSig) {
          matchedIndex = i;
          break;
        }
      }
      if (!radioPresetEntries.length) {
        applyBtn.disabled = true;
        return;
      }
      if (matchedIndex >= 0) {
        selectEl.value = String(matchedIndex);
        applyBtn.disabled = false;
        return;
      }
      selectEl.value = "";
      applyBtn.disabled = true;
    }
    function toneForPercent(percent, invert) {
      if (invert) {
        if (percent >= 70) return "bad";
        if (percent >= 35) return "warn";
        return "";
      }
      if (percent >= 75) return "";
      if (percent >= 40) return "warn";
      return "bad";
    }
    function toneForThreshold(value, greenMin, yellowMin) {
      if (!Number.isFinite(value)) return "bad";
      if (value >= greenMin) return "";
      if (value >= yellowMin) return "warn";
      return "bad";
    }
    function toneForThresholdDescending(value, greenMax, yellowMax) {
      if (!Number.isFinite(value)) return "bad";
      if (value <= greenMax) return "";
      if (value <= yellowMax) return "warn";
      return "bad";
    }
    function toneForHeapFreePercent(percent) {
      if (!Number.isFinite(percent)) return "bad";
      if (percent >= 75) return "";
      if (percent >= 55) return "ok";
      if (percent >= 35) return "warn";
      return "bad";
    }
    function toneForLargestBlockPercent(percent) {
      if (!Number.isFinite(percent)) return "bad";
      if (percent >= 75) return "";
      if (percent >= 55) return "ok";
      if (percent >= 35) return "warn";
      return "bad";
    }
    function colorForHeapFreePercent(percent) {
      const tone = toneForHeapFreePercent(percent);
      if (tone === "ok") return "#6ea43f";
      if (tone === "warn") return "#d7a531";
      if (tone === "bad") return "#d14343";
      return "#2f8f4e";
    }
    function renderMeter(label, value, percent, note, toneOrInvert, invertFill = false) {
      const pct = clamp(Math.round(percent), 0, 100);
      const tone = typeof toneOrInvert === "string" ? toneOrInvert : toneForPercent(pct, !!toneOrInvert);
      return `<div class="hud-row">
        <div class="hud-kpi">
          <div>
            <div class="metric-label">${escapeHtml(label)}</div>
            <div class="hud-value">${escapeHtml(value)}</div>
          </div>
          <div class="hud-sub">${escapeHtml(note)}</div>
        </div>
        <div class="meter"><div class="meter-fill${tone ? " " + tone : ""}" style="width:${pct}%;${invertFill ? "margin-left:auto;" : ""}"></div></div>
      </div>`;
    }
    function renderMetric(label, value) {
      return `<div class="metric">
        <div class="metric-label">${escapeHtml(label)}</div>
        <div class="metric-value">${escapeHtml(value)}</div>
      </div>`;
    }
    function renderMissingCard(title, message) {
      return `<section class="hud-card">
        <h3>${escapeHtml(title)}</h3>
        <div class="stats-empty">${escapeHtml(message)}</div>
      </section>`;
    }
    function parseJsonReply(text) {
      const value = parseReplyValue(text);
      if (!value) return null;
      try {
        return JSON.parse(value);
      } catch (_) {
        return null;
      }
    }
    function parseKeyedReply(text, keys) {
      const value = parseReplyValue(text);
      if (!value) return null;
      const found = [];
      for (const key of keys) {
        const marker = key + ":";
        const index = value.indexOf(marker);
        if (index >= 0) {
          found.push({ key, index });
        }
      }
      if (!found.length) return null;
      found.sort((a, b) => a.index - b.index);
      const parsed = {};
      for (let i = 0; i < found.length; i++) {
        const current = found[i];
        const start = current.index + current.key.length + 1;
        const end = i + 1 < found.length ? found[i + 1].index : value.length;
        parsed[current.key] = value.slice(start, end).trim();
      }
      return parsed;
    }
    function parseWifiStatusReply(text) {
      const parsed = parseKeyedReply(text, ["ssid", "status", "code", "state", "ip", "rssi", "quality", "signal"]);
      if (!parsed) return null;
      const rssi = Number.parseInt(parsed.rssi, 10);
      const quality = Number.parseInt(String(parsed.quality || "").replace("%", ""), 10);
      const code = Number.parseInt(parsed.code, 10);
      return {
        ssid: parsed.ssid || "-",
        status: parsed.status || "unknown",
        code: Number.isFinite(code) ? code : null,
        state: parsed.state || "unknown",
        ip: parsed.ip || "--",
        rssi: Number.isFinite(rssi) ? rssi : null,
        quality: Number.isFinite(quality) ? quality : null,
        signal: parsed.signal || "--"
      };
    }
    function renderCoreCard(core) {
      const batteryPct = pctRange(core.battery_mv, 3000, 4200);
      const queuePct = pctRange(core.queue_len, 0, 12);
      const errorsPct = core.errors > 0 ? 100 : 0;
      return `<section class="hud-card">
        <h3>Core</h3>
        <div class="core-grid">
          ${renderMeter("Battery", Math.round(batteryPct) + "%", batteryPct, (core.battery_mv || 0) + " mV", false)}
          ${renderMeter("Queue", String(core.queue_len ?? 0), queuePct, "outbound packets", true)}
          ${renderMeter("Errors", String(core.errors ?? 0), errorsPct, "sticky error flags", true)}
        </div>
        <div class="metric-grid core-metrics">
          ${renderMetric("Uptime", formatDuration(core.uptime_secs))}
          ${renderMetric("Battery", (core.battery_mv || 0) + " mV")}
          ${renderMetric("Queue", String(core.queue_len ?? 0))}
          ${renderMetric("Errors", String(core.errors ?? 0))}
        </div>
      </section>`;
    }
    function renderWifiCard(wifi, powersave) {
      const qualityPct = Number.isFinite(wifi.quality) ? clamp(wifi.quality, 0, 100) : pctRange(wifi.rssi, -100, -50);
      const rssiPct = pctRange(wifi.rssi, -90, -50);
      const statusNote = wifi.status === "connected" ? (wifi.signal || "linked") : (wifi.state || "idle");
      const qualityTone = Number.isFinite(wifi.rssi)
        ? toneForThreshold(wifi.rssi, -67, -75)
        : toneForThreshold(qualityPct, 67, 40);
      const rssiTone = toneForThreshold(wifi.rssi, -67, -75);
      return `<section class="hud-card">
        <h3>Wi-Fi</h3>
        ${renderMeter("Signal Quality", Number.isFinite(wifi.quality) ? wifi.quality + "%" : "--", qualityPct, statusNote, qualityTone)}
        ${renderMeter("RSSI", Number.isFinite(wifi.rssi) ? wifi.rssi + " dBm" : "--", rssiPct, wifi.ssid || "-", rssiTone)}
        <div class="metric-grid">
          ${renderMetric("Status", wifi.status || "--")}
          ${renderMetric("State", wifi.state || "--")}
          ${renderMetric("SSID", wifi.ssid || "-")}
          ${renderMetric("IP", wifi.ip || "--")}
          ${renderMetric("Power Save", powersave || "--")}
          ${renderMetric("Code", wifi.code == null ? "--" : wifi.code)}
        </div>
      </section>`;
    }
    function renderRadioCard(radio) {
      const rssiPct = pctRange(radio.last_rssi, -125, -30);
      const snrPct = pctRange(radio.last_snr, -15, 15);
      const noisePct = 100 - pctRange(radio.noise_floor, -130, -50);
      const totalAir = (radio.tx_air_secs || 0) + (radio.rx_air_secs || 0);
      const txShare = pctRatio(radio.tx_air_secs || 0, totalAir);
      const rssiTone = toneForThreshold(radio.last_rssi, -90, -110);
      const snrTone = toneForThreshold(radio.last_snr, 5, -5);
      const noiseTone = toneForThresholdDescending(radio.noise_floor, -110, -95);
      return `<section class="hud-card">
        <h3>Radio</h3>
        ${renderMeter("RSSI", (radio.last_rssi ?? "--") + " dBm", rssiPct, "signal strength", rssiTone)}
        ${renderMeter("SNR", Number.isFinite(radio.last_snr) ? radio.last_snr.toFixed(1) + " dB" : "--", snrPct, "link quality", snrTone)}
        ${renderMeter("Noise Floor", (radio.noise_floor ?? "--") + " dBm", noisePct, "ambient RF", noiseTone)}
        <div class="metric-grid">
          ${renderMetric("TX Air", String(radio.tx_air_secs ?? 0) + " s")}
          ${renderMetric("RX Air", String(radio.rx_air_secs ?? 0) + " s")}
          ${renderMetric("TX Share", Math.round(txShare) + "%")}
          ${renderMetric("Total Air", String(totalAir) + " s")}
        </div>
      </section>`;
    }
    function renderPacketsCard(packets) {
      const sent = packets.sent || 0;
      const recv = packets.recv || 0;
      const floodTx = packets.flood_tx || 0;
      const directTx = packets.direct_tx || 0;
      const floodRx = packets.flood_rx || 0;
      const directRx = packets.direct_rx || 0;
      return `<section class="hud-card">
        <h3>Packets</h3>
        ${renderMeter("TX Flood Share", Math.round(pctRatio(floodTx, sent)) + "%", pctRatio(floodTx, sent), floodTx + " flood / " + directTx + " direct", false)}
        ${renderMeter("RX Flood Share", Math.round(pctRatio(floodRx, recv)) + "%", pctRatio(floodRx, recv), floodRx + " flood / " + directRx + " direct", false)}
        <div class="metric-grid">
          ${renderMetric("Sent", sent)}
          ${renderMetric("Recv", recv)}
          ${renderMetric("TX Direct", directTx)}
          ${renderMetric("RX Direct", directRx)}
          ${renderMetric("Recv Errors", packets.recv_errors || 0)}
          ${renderMetric("Balance", recv - sent)}
        </div>
      </section>`;
    }
    function renderMemoryCard(memory) {
      const heapFree = memory.heap_free || 0;
      const heapMax = memory.heap_max || 0;
      const psramFree = memory.psram_free || 0;
      const psramMax = memory.psram_max || 0;
      const heapFreePct = pctRange(heapFree, 0, 128 * 1024);
      const heapLargestPct = pctRatio(heapMax, heapFree);
      const psramLargestPct = pctRatio(psramMax, psramFree);
      return `<section class="hud-card">
        <h3>Memory</h3>
        ${renderMeter("Heap Free", formatBytes(heapFree), heapFreePct, "total free heap", toneForHeapFreePercent(heapFreePct))}
        ${renderMeter("Heap Largest Block", formatBytes(heapMax), heapLargestPct, "largest alloc vs free", toneForLargestBlockPercent(heapLargestPct))}
        ${renderMeter("PSRAM Largest Block", formatBytes(psramMax), psramLargestPct, "largest alloc vs free", toneForLargestBlockPercent(psramLargestPct))}
        <div class="metric-grid">
          ${renderMetric("Heap Free", formatBytes(heapFree))}
          ${renderMetric("Heap Min", formatBytes(memory.heap_min || 0))}
          ${renderMetric("PSRAM Free", formatBytes(psramFree))}
          ${renderMetric("PSRAM Min", formatBytes(memory.psram_min || 0))}
        </div>
      </section>`;
    }
    function renderStatsDashboard(results, errors, targetId = "statsSummary") {
      const dashboardEl = document.getElementById(targetId);
      const notices = errors.length ? `<div class="stats-error">${errors.map((item) => escapeHtml(item)).join("<br>")}</div>` : "";
      const coreCards = [
        results.core ? renderCoreCard(results.core) : renderMissingCard("Core", "stats-core unavailable")
      ];
      const middleCards = [
        results.radio ? renderRadioCard(results.radio) : renderMissingCard("Radio", "stats-radio unavailable"),
        results.memory ? renderMemoryCard(results.memory) : renderMissingCard("Memory", "memory unavailable")
      ];
      const lowerCards = [
        results.wifi ? renderWifiCard(results.wifi, results.wifi_powersave) : renderMissingCard("Wi-Fi", "wifi.status unavailable"),
        results.packets ? renderPacketsCard(results.packets) : renderMissingCard("Packets", "stats-packets unavailable")
      ];
      dashboardEl.innerHTML = notices +
        `<div class="hud-grid-1">${coreCards.join("")}</div>` +
        `<div class="hud-grid-2">${middleCards.join("")}</div>` +
        `<div class="hud-grid-2">${lowerCards.join("")}</div>`;
    }
    function renderServicesCard(summary) {
      const services = summary && summary.services ? summary.services : {};
      const archive = summary && summary.archive ? summary.archive : {};
      const packets = summary && summary.packets ? summary.packets : {};
      const archiveTotal = archive.total_bytes || 0;
      const archiveUsed = archive.used_bytes || 0;
      const archiveFree = Math.max(0, archiveTotal - archiveUsed);
      const archiveFreePct = archiveTotal > 0 ? Math.round((archiveFree / archiveTotal) * 100) : 0;
      const archiveMetrics = archive.available ? `
          ${renderMetric("Card", archive.type || "--")}
          ${renderMetric("Archive Total", formatArchiveGigabytes(archiveTotal))}
          ${renderMetric("Archive Used", formatArchiveUsed(archiveUsed))}
          ${renderMetric("Archive Free", archiveTotal > 0 ? (formatArchiveGigabytes(archiveFree) + " (" + archiveFreePct + "%)") : "--")}` : "";
      return `<section class="hud-card">
        <h3>Services</h3>
        <div class="metric-grid">
          ${renderMetric("MQTT", services.mqtt_state || (services.mqtt_connected ? "up" : "down"))}
          ${renderMetric("Web", services.web_panel_up ? services.web_auth || "up" : "down")}
          ${renderMetric("Archive", archive.available ? archive.logical || "archive" : "unavailable")}
          ${renderMetric("Neighbours", packets.neighbors || 0)}
          ${archiveMetrics}
        </div>
      </section>`;
    }
    function renderEventsSection(events) {
      if (!Array.isArray(events) || !events.length) {
        return `<section class="hud-card">
          <h3>Events</h3>
          <div class="events-empty">No recent events</div>
        </section>`;
      }
      const rows = events.map((event) => `<tr>
        <td>${escapeHtml(event.type || "event")}</td>
        <td>${escapeHtml(formatDuration(event.t || 0))}</td>
      </tr>`).join("");
      return `<section class="hud-card">
        <h3>Events</h3>
        <div class="events-table-wrap">
          <table class="events-table">
            <thead>
              <tr>
                <th>Event</th>
                <th>Age</th>
              </tr>
            </thead>
            <tbody>${rows}</tbody>
          </table>
        </div>
      </section>`;
    }
    function renderNeighboursSection(neighbours) {
      if (!Array.isArray(neighbours) || !neighbours.length) {
        return `<section class="hud-card">
          <h3>Neighbours</h3>
          <div class="events-empty">No recent neighbours</div>
        </section>`;
      }
      const renderNeighbourId = (neighbour) => {
        const shortId = escapeHtml(neighbour.id || "--");
        const fullId = typeof neighbour.full_id === "string" ? neighbour.full_id.trim() : "";
        if (!/^[0-9A-Fa-f]{64}$/.test(fullId)) {
          return shortId;
        }
        const href = "https://core.eastmesh.au/#/nodes/" + encodeURIComponent(fullId.toLowerCase());
        return `<a href="${href}" target="_blank" rel="noopener noreferrer">${shortId}</a>`;
      };
      const rows = neighbours.map((neighbour) => `<tr>
        <td>${renderNeighbourId(neighbour)}</td>
        <td>${Number.isFinite(neighbour.snr_db) ? neighbour.snr_db.toFixed(2) + " dB" : "--"}</td>
        <td>${escapeHtml(formatDuration(neighbour.heard_secs_ago || 0))}</td>
        <td>${escapeHtml(formatDuration(neighbour.advert_secs_ago || 0))}</td>
      </tr>`).join("");
      return `<section class="hud-card">
        <h3>Neighbours</h3>
        <div class="events-table-wrap">
          <table class="events-table">
            <thead>
              <tr>
                <th>ID</th>
                <th>SNR</th>
                <th>Heard</th>
                <th>Advert</th>
              </tr>
            </thead>
            <tbody>${rows}</tbody>
          </table>
        </div>
      </section>`;
    }
    function renderStatsSummary(payload) {
      const results = {
        core: payload && payload.core ? payload.core : null,
        radio: payload && payload.radio ? payload.radio : null,
        packets: payload && payload.packets ? payload.packets : null,
        memory: payload && payload.memory ? payload.memory : null,
        wifi: payload && payload.wifi ? payload.wifi : null,
        wifi_powersave: payload && payload.wifi ? payload.wifi.powersave : "--"
      };
      renderStatsDashboard(results, [], "statsSummary");
      const summaryEl = document.getElementById("statsSummary");
      if (!summaryEl) return;
      summaryEl.innerHTML += `<div class="hud-grid-1">${renderServicesCard(payload)}</div>`;
      const introEl = document.getElementById("statsTrendIntro");
      if (introEl) {
        const intervalSecs = payload && payload.history && Number.isFinite(payload.history.sample_interval_secs)
          ? payload.history.sample_interval_secs
          : 0;
        const restored = !!(payload && payload.history && payload.history.archive_restored);
        const restoredSamples = payload && payload.history && Number.isFinite(payload.history.archive_restored_samples)
          ? payload.history.archive_restored_samples
          : 0;
        const archiveIntervalSecs = payload && payload.history && Number.isFinite(payload.history.archive_summary_interval_secs)
          ? payload.history.archive_summary_interval_secs
          : 0;
        const sampleLabel = intervalSecs >= 60 && intervalSecs % 60 === 0
          ? ((intervalSecs / 60) === 1 ? "1 minute" : ((intervalSecs / 60) + " minutes"))
          : (intervalSecs > 0 ? (intervalSecs + " seconds") : "regular");
        if (restored && restoredSamples > 0) {
          const archiveLabel = archiveIntervalSecs >= 60 && archiveIntervalSecs % 60 === 0
            ? ((archiveIntervalSecs / 60) === 1 ? "1 minute" : ((archiveIntervalSecs / 60) + " minutes"))
            : (archiveIntervalSecs > 0 ? (archiveIntervalSecs + " seconds") : "archive");
          introEl.textContent = `These graphs show recent history. Restored ${restoredSamples} archived ${restoredSamples === 1 ? "point" : "points"} from SD. Archived points are sampled every ${archiveLabel}; new live points are added every ${sampleLabel}. Hover a chart to inspect point values.`;
        } else {
          introEl.textContent = `These graphs show recent history only. New points are added every ${sampleLabel}, and older points roll out of memory as the buffer fills. Hover a chart to inspect point values.`;
        }
      }
      const trendsPanel = document.getElementById("statsNeighbours");
      if (trendsPanel) {
        trendsPanel.innerHTML = renderNeighboursSection(payload && Array.isArray(payload.neighbors_detail) ? payload.neighbors_detail : []);
      }
      const eventsPanel = document.getElementById("statsEvents");
      if (eventsPanel) {
        eventsPanel.innerHTML = renderEventsSection(payload && payload.events ? payload.events : []);
      }
    }
    function formatTrendValue(key, value) {
      if (!Number.isFinite(value)) return "--";
      if (key === "battery") return Math.round(value) + " mV";
      if (key === "memory") return formatBytes(value);
      if (key === "packets") return Math.round(value) + " pkts";
      if (key === "signal") return (value / 4).toFixed(1) + " dBm";
      if (key === "noise_floor") return (value / 4).toFixed(1) + " dBm";
      return String(value);
    }
    function formatArchiveGigabytes(value) {
      if (!Number.isFinite(value) || value <= 0) return "--";
      const gb = value / (1024 * 1024 * 1024);
      return (gb >= 10 ? gb.toFixed(1) : gb.toFixed(2)) + " GB";
    }
    function formatArchiveUsed(value) {
      if (!Number.isFinite(value)) return "--";
      const abs = Math.abs(value);
      if (abs >= 1024 * 1024 * 1024) {
        const gb = value / (1024 * 1024 * 1024);
        return (Math.abs(gb) >= 10 ? gb.toFixed(1) : gb.toFixed(2)) + " GB";
      }
      if (abs >= 1024 * 1024) return (value / (1024 * 1024)).toFixed(2) + " MB";
      if (abs >= 1024) return (value / 1024).toFixed(1) + " KB";
      return Math.round(value) + " B";
    }
    function sparkStrokeColor(key, points) {
      if (key === "packets") return "#d97706";
      if (key === "signal") return "#3b82f6";
      if (key === "noise_floor") return "#94a3b8";
      if (key === "memory") {
        const values = Array.isArray(points) ? points.map((item) => item && item[1]).filter((value) => Number.isFinite(value)) : [];
        const recentValues = values.slice(-5);
        const minRecent = recentValues.length ? Math.min(...recentValues) : NaN;
        const percent = pctRange(minRecent, 0, 128 * 1024);
        return colorForHeapFreePercent(percent);
      }
      return "#2f8f4e";
    }
    function sparkValueRange(key, values) {
      if (key === "signal") {
        return { min:(-125 * 4), max:(-30 * 4) };
      }
      if (key === "noise_floor") {
        return { min:(-130 * 4), max:(-50 * 4) };
      }
      let minValue = Math.min(...values);
      let maxValue = Math.max(...values);
      if (minValue === maxValue) {
        minValue -= 1;
        maxValue += 1;
      }
      return { min:minValue, max:maxValue };
    }
    function sparkGuideValues(key) {
      if (key === "signal") {
        return [-120, -110, -100, -90, -80, -70, -60, -50, -40].map((value) => value * 4);
      }
      if (key === "noise_floor") {
        return [-120, -110, -100, -90, -80, -70, -60, -50].map((value) => value * 4);
      }
      return [];
    }
    function sparkBands(key) {
      if (key === "signal") {
        return [
          { from:(-125 * 4), to:(-110 * 4), color:"rgba(191,75,75,0.14)" },
          { from:(-110 * 4), to:(-90 * 4), color:"rgba(215,165,49,0.14)" },
          { from:(-90 * 4), to:(-30 * 4), color:"rgba(47,143,78,0.12)" }
        ];
      }
      if (key === "noise_floor") {
        return [
          { from:(-130 * 4), to:(-110 * 4), color:"rgba(47,143,78,0.12)" },
          { from:(-110 * 4), to:(-95 * 4), color:"rgba(215,165,49,0.14)" },
          { from:(-95 * 4), to:(-50 * 4), color:"rgba(191,75,75,0.14)" }
        ];
      }
      return [];
    }
    function drawSparkline(canvas, points, key, hoverIndex) {
      if (!canvas || !canvas.getContext) return;
      const ctx = canvas.getContext("2d");
      const width = canvas.clientWidth || 240;
      const height = canvas.clientHeight || 84;
      canvas.width = width * (window.devicePixelRatio || 1);
      canvas.height = height * (window.devicePixelRatio || 1);
      ctx.setTransform(window.devicePixelRatio || 1, 0, 0, window.devicePixelRatio || 1, 0, 0);
      ctx.clearRect(0, 0, width, height);
      if (!Array.isArray(points) || points.length < 1) return;
      const values = points.map((item) => item[1]).filter((value) => Number.isFinite(value));
      if (values.length < 1) return;
      const range = sparkValueRange(key, values);
      const minValue = range.min;
      const maxValue = range.max;
      const plotLeft = 4;
      const plotRight = width - 4;
      const plotTop = 8;
      const plotBottom = height - 8;
      const scaleY = (value) => {
        const clamped = clamp(value, minValue, maxValue);
        if (key === "noise_floor") {
          return plotTop + (((clamped - minValue) / (maxValue - minValue)) * (plotBottom - plotTop));
        }
        return plotBottom - (((clamped - minValue) / (maxValue - minValue)) * (plotBottom - plotTop));
      };
      sparkBands(key).forEach((band) => {
        const y1 = scaleY(band.from);
        const y2 = scaleY(band.to);
        const top = Math.min(y1, y2);
        const bandHeight = Math.max(1, Math.abs(y2 - y1));
        ctx.fillStyle = band.color;
        ctx.fillRect(plotLeft, top, plotRight - plotLeft, bandHeight);
      });
      sparkGuideValues(key).forEach((guide) => {
        const y = scaleY(guide);
        ctx.strokeStyle = "rgba(75,85,99,0.18)";
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(plotLeft, y);
        ctx.lineTo(plotRight, y);
        ctx.stroke();
      });
      const coords = points.map((point, index) => ({
        x: (index / Math.max(1, points.length - 1)) * (plotRight - plotLeft) + plotLeft,
        y: scaleY(point[1])
      }));
      const strokeColor = sparkStrokeColor(key, points);
      if (key === "packets") {
        const slotWidth = (plotRight - plotLeft) / Math.max(points.length, 1);
        const barWidth = Math.max(3, Math.min(18, slotWidth * 0.68));
        coords.forEach((point, index) => {
          const left = clamp(point.x - (barWidth / 2), plotLeft, plotRight - barWidth);
          const top = point.y;
          const barHeight = Math.max(1, plotBottom - top);
          ctx.fillStyle = Number.isInteger(hoverIndex) && hoverIndex === index ? "#f59e0b" : strokeColor;
          ctx.fillRect(left, top, barWidth, barHeight);
        });
        return;
      }
      ctx.lineWidth = 2;
      ctx.strokeStyle = strokeColor;
      ctx.beginPath();
      coords.forEach((point, index) => {
        if (index === 0) ctx.moveTo(point.x, point.y);
        else ctx.lineTo(point.x, point.y);
      });
      ctx.stroke();
      if (Number.isInteger(hoverIndex) && hoverIndex >= 0 && hoverIndex < coords.length) {
        const hover = coords[hoverIndex];
        ctx.fillStyle = strokeColor;
        ctx.beginPath();
        ctx.arc(hover.x, hover.y, 3.5, 0, Math.PI * 2);
        ctx.fill();
      }
    }
    function bindSparkHover(canvas, points, key) {
      const tooltip = document.getElementById("tooltip-" + key);
      if (!canvas || !tooltip || !Array.isArray(points) || points.length < 1) return;
      const updateHover = (index) => {
        drawSparkline(canvas, points, key, index);
        if (index == null || index < 0 || index >= points.length) {
          tooltip.textContent = "";
          tooltip.classList.remove("visible");
          return;
        }
        tooltip.textContent = formatTrendValue(key, points[index][1]);
        tooltip.classList.add("visible");
      };
      canvas.onmousemove = (event) => {
        const rect = canvas.getBoundingClientRect();
        const width = rect.width || 1;
        const x = Math.max(0, Math.min(width, event.clientX - rect.left));
        const index = Math.max(0, Math.min(points.length - 1, Math.round((x / width) * (points.length - 1))));
        updateHover(index);
      };
      canvas.onmouseleave = () => updateHover(null);
      canvas.ontouchstart = (event) => {
        const touch = event.touches && event.touches[0];
        if (!touch) return;
        const rect = canvas.getBoundingClientRect();
        const width = rect.width || 1;
        const x = Math.max(0, Math.min(width, touch.clientX - rect.left));
        const index = Math.max(0, Math.min(points.length - 1, Math.round((x / width) * (points.length - 1))));
        updateHover(index);
      };
      canvas.ontouchend = () => updateHover(null);
    }
    function setTrendCardState(key, title, value) {
      const card = document.getElementById("trend-" + key);
      if (!card) return;
      card.innerHTML = `<div class="trend-head">
        <div>
          <div class="trend-title">${escapeHtml(title)}</div>
          <div class="trend-value">${escapeHtml(value)}</div>
        </div>
      </div>
      <div class="spark-tooltip" id="tooltip-${key}"></div>
      <canvas class="spark" id="canvas-${key}"></canvas>
      <div class="spark-axis" id="axis-${key}">
        <span id="axis-left-${key}"></span>
        <span id="axis-right-${key}"></span>
      </div>`;
    }
    function renderTrendResult(key, payload) {
      const title = payload && payload.title ? payload.title : key;
      const current = payload && Number.isFinite(payload.current) ? payload.current : NaN;
      setTrendCardState(key, title, formatTrendValue(key, current));
      const canvas = document.getElementById("canvas-" + key);
      const points = payload && Array.isArray(payload.points) ? payload.points : [];
      drawSparkline(canvas, points, key);
      bindSparkHover(canvas, points, key);
      const axisLeftEl = document.getElementById("axis-left-" + key);
      const axisRightEl = document.getElementById("axis-right-" + key);
      if (axisLeftEl && axisRightEl) {
        if (points.length) {
          axisLeftEl.textContent = formatRelativeAge(payload && Number.isFinite(payload.oldest_age_secs) ? payload.oldest_age_secs : 0);
          axisRightEl.textContent = formatRelativeAge(payload && Number.isFinite(payload.latest_age_secs) ? payload.latest_age_secs : 0);
        } else {
          axisLeftEl.textContent = "";
          axisRightEl.textContent = "No recent data";
        }
      }
    }
    function renderTrendError(key, message) {
      setTrendCardState(key, key, "--");
      const axisLeftEl = document.getElementById("axis-left-" + key);
      const axisRightEl = document.getElementById("axis-right-" + key);
      if (axisLeftEl && axisRightEl) {
        axisLeftEl.textContent = "";
        axisRightEl.textContent = message || "Unavailable";
        axisRightEl.style.color = "var(--status-red)";
      }
    }
    function initTrendCards() {
      const trendsEl = document.getElementById("statsTrends");
      if (!trendsEl) return;
      trendsEl.innerHTML = ["battery", "memory", "signal", "noise_floor", "packets"].map((key) =>
        `<section class="trend-card" id="trend-${key}">
          <div class="trend-title">${escapeHtml(key)}</div>
          <div class="spark-axis"><span></span><span>Loading...</span></div>
        </section>`
      ).join("");
    }
    function showAuthedUi(show) {
      document.getElementById("login").style.display = show ? "none" : "block";
      document.getElementById("actionsPanel").style.display = show ? "block" : "none";
      document.getElementById("cliPanel").style.display = show && !isStatsPage ? "block" : "none";
      document.getElementById("quickCommandsPanel").style.display = show && !isStatsPage ? "block" : "none";
      document.getElementById("mqttSettingsPanel").style.display = show && !isStatsPage ? "block" : "none";
      document.getElementById("infoPanel").style.display = show && !isStatsPage ? "block" : "none";
      document.getElementById("statsPanel").style.display = show && !isStatsPage ? "none" : "none";
      document.getElementById("statsPagePanel").style.display = show && isStatsPage ? "block" : "none";
      document.getElementById("repeaterSettingsPanel").style.display = show && !isStatsPage ? "block" : "none";
      if (!show) {
        commandQueue = Promise.resolve();
        const passwordEl = document.getElementById("password");
        if (passwordEl) passwordEl.value = "";
        if (statusEl) statusEl.textContent = "";
        updatePanelTitle();
        const summaryEl = document.getElementById("statsSummary");
        if (summaryEl) summaryEl.innerHTML = '<div class="stats-empty">Loading summary...</div>';
        const trendsEl = document.getElementById("statsTrends");
        if (trendsEl) trendsEl.innerHTML = "";
      }
    }
    function queueCommand(task) {
      const next = commandQueue.then(task, task);
      commandQueue = next.catch(() => {});
      return next;
    }
    async function runCommand(cmd, options = {}) {
      if (!token) {
        redirectToLogin();
        return { ok:false, text:"Unauthorized" };
      }
      const recordHistory = options.recordHistory !== false;
      const updateInput = options.updateInput !== false;
      return queueCommand(async () => {
        if (updateInput) {
          document.getElementById("command").value = cmd;
        }
        const res = await fetch("/api/command", { method:"POST", headers:{ "X-Auth-Token": token }, body: cmd });
        const text = await res.text();
        if (res.status === 401) {
          redirectToLogin();
          return { ok:false, text };
        }
        const ok = res.ok && !isCommandError(text);
        if (recordHistory) {
          appendHistory(cmd, text, ok);
        }
        return { ok, text };
      });
    }
    async function runPrefixed(prefix, inputId) {
      const input = document.getElementById(inputId);
      if (!input) return;
      const maxLength = Number.isFinite(input.maxLength) && input.maxLength > 0 ? input.maxLength : null;
      const value = maxLength ? input.value.slice(0, maxLength) : input.value;
      if (value !== input.value) input.value = value;
      const result = await runCommand(prefix + value);
      if (!result.ok) return;
      if (inputId === "nodeName") {
        await loadField("get name", "nodeName", null, { recordHistory:false, updateInput:false });
      }
    }
    async function loadField(cmd, inputId, format, options = {}) {
      const result = await runCommand(cmd, options);
      if (!result.ok) return;
      let value = parseReplyValue(result.text);
      if (format === "multiline") {
        value = value.replace(/\|/g, "\n");
      } else if (format === "uppercase") {
        value = value.toUpperCase();
      }
      document.getElementById(inputId).value = value;
      if (inputId === "nodeName") {
        updatePanelTitle(value);
      }
    }
    async function copyToClipboard(value, successMessage) {
      try {
        if (navigator.clipboard && navigator.clipboard.writeText) {
          await navigator.clipboard.writeText(value);
        } else {
          throw new Error("Clipboard unavailable");
        }
        statusEl.textContent = successMessage;
      } catch (_) {
        statusEl.textContent = "Copy failed";
      }
    }
    async function syncClock() {
      let epoch = Math.floor(Date.now() / 1000);
      let result = await runCommand("time " + String(epoch));
      if (result.ok) {
        await loadField("clock", "clockUtc");
        statusEl.textContent = "Clock synced";
        return;
      }
      const message = parseReplyValue(result.text).toLowerCase();
      if (!message.includes("cannot go backwards")) {
        statusEl.textContent = parseReplyValue(result.text) || "Clock sync failed";
        return;
      }
      if (!confirm("Device clock appears ahead of browser time. Force clock sync backwards?")) {
        statusEl.textContent = "Clock sync cancelled";
        return;
      }
      epoch = Math.floor(Date.now() / 1000);
      result = await runCommand("time.force " + String(epoch));
      if (result.ok) {
        await loadField("clock", "clockUtc");
        statusEl.textContent = "Clock force-synced";
        return;
      }
      statusEl.textContent = parseReplyValue(result.text) || "Clock force sync failed";
    }
    async function fetchJson(path) {
      if (!token) {
        redirectToLogin();
        return null;
      }
      return queueCommand(async () => {
        const res = await fetch(path, { headers:{ "X-Auth-Token": token } });
        const text = await res.text();
        if (res.status === 401) {
          redirectToLogin();
          throw new Error("Unauthorized");
        }
        if (!res.ok) {
          throw new Error(text || "request failed");
        }
        return JSON.parse(text);
      });
    }
	    function getLetsmeshMode() {
	      const eu = document.getElementById("mqttLetsmeshEu");
	      const us = document.getElementById("mqttLetsmeshUs");
	      const euOn = eu && eu.checked;
	      const usOn = us && us.checked;
	      if (euOn && usOn) return "both";
	      if (euOn) return "eu";
	      if (usOn) return "us";
	      return "off";
	    }
	    function refreshEastmeshModeUi() {
	      const input = document.getElementById("mqttEastmeshAu");
	      const enabled = !!(input && input.checked);
	      const slider = document.getElementById("mqttEastmeshMode");
	      if (slider) {
	        slider.value = enabled ? "1" : "0";
	      }
	      document.querySelectorAll("[data-eastmesh-label]").forEach((label) => {
	        label.classList.toggle("active", label.dataset.eastmeshLabel === (enabled ? "on" : "off"));
	      });
	    }
	    function getLetsmeshModeIndex(mode) {
	      const order = { off:0, eu:1, us:2, both:3 };
	      return Object.prototype.hasOwnProperty.call(order, mode) ? order[mode] : 0;
	    }
	    function clampLetsmeshModeIndex(index) {
	      const eastmesh = document.getElementById("mqttEastmeshAu");
	      const eastmeshEnabled = !!(eastmesh && eastmesh.checked);
	      const bounded = Math.max(0, Math.min(3, index));
	      return eastmeshEnabled && bounded === 3 ? 1 : bounded;
	    }
	    function refreshLetsmeshModeUi() {
	      const mode = getLetsmeshMode();
	      const eastmesh = document.getElementById("mqttEastmeshAu");
	      const eastmeshEnabled = !!(eastmesh && eastmesh.checked);
	      const state = document.getElementById("mqttLetsmeshModeState");
	      const labels = { off:"Off", eu:"EU", us:"US", both:"Both" };
	      if (state) {
	        state.textContent = labels[mode] || "Off";
	        state.classList.toggle("on", mode !== "off");
	      }
	      const slider = document.getElementById("mqttLetsmeshMode");
	      if (slider) {
	        slider.max = "3";
	        slider.value = String(clampLetsmeshModeIndex(getLetsmeshModeIndex(mode)));
	      }
	      document.querySelectorAll("[data-letsmesh-label]").forEach((label) => {
	        const labelMode = label.dataset.letsmeshLabel;
	        label.classList.toggle("active", labelMode === mode);
	        label.classList.toggle("disabled", eastmeshEnabled && labelMode === "both");
	      });
	    }
	    function setBrokerToggle(inputId, state) {
	      const input = document.getElementById(inputId);
	      if (!input) return;
	      const enabled = state === "on";
	      input.checked = enabled;
	      const label = document.getElementById(inputId + "State");
	      if (label) {
	        label.textContent = enabled ? "On" : "Off";
	        label.classList.toggle("on", enabled);
	      }
	      if (inputId === "mqttLetsmeshEu" || inputId === "mqttLetsmeshUs") {
	        refreshLetsmeshModeUi();
	      } else if (inputId === "mqttEastmeshAu") {
	        refreshEastmeshModeUi();
	        refreshLetsmeshModeUi();
	      }
	    }
    async function loadBrokerState(cmd, inputId, options = {}) {
      const result = await runCommand(cmd, options);
      if (!result.ok) return;
      setBrokerToggle(inputId, parseReplyValue(result.text));
    }
    async function loadRadioConfig(options = {}) {
      const result = await runCommand("get radio", options);
      if (!result.ok) {
        setRadioPresetStatus("Unable to load current radio config.", true);
        return;
      }
      const parsed = parseRadioValue(parseReplyValue(result.text));
      if (!parsed) {
        setRadioPresetStatus("Current radio config was not recognised.", true);
        return;
      }
      currentRadioConfig = parsed;
      setRadioPresetStatus("");
      syncRadioPresetUi();
    }
    function pause(ms) {
      return new Promise((resolve) => setTimeout(resolve, ms));
    }
    async function loadSection(title, tasks) {
      statusEl.textContent = title;
      for (const task of tasks) {
        await task();
      }
      await pause(40);
    }
    async function loadRadioPresets() {
      const selectEl = document.getElementById("radioPreset");
      const applyBtn = document.getElementById("applyRadioPresetBtn");
      if (selectEl) {
        selectEl.innerHTML = '<option value="">Loading presets...</option>';
      }
      if (applyBtn) applyBtn.disabled = true;
      setRadioPresetStatus("");
      try {
        const res = await fetch(RADIO_PRESETS_URL, { cache:"no-store" });
        if (!res.ok) {
          throw new Error("Preset service returned " + res.status);
        }
        const payload = await res.json();
        const config = payload && payload.config ? payload.config : {};
        const suggested = config && config.suggested_radio_settings ? config.suggested_radio_settings : {};
        const entries = Array.isArray(suggested.entries) ? suggested.entries : [];
        radioPresetEntries = entries.map((entry) => ({
          title: String(entry.title || "Unnamed preset"),
          description: String(entry.description || ""),
          frequency: formatDecimal(entry.frequency, 3),
          bandwidth: formatDecimal(entry.bandwidth, 3),
          spreadingFactor: Number.parseInt(entry.spreading_factor, 10),
          codingRate: Number.parseInt(entry.coding_rate, 10)
        })).filter((entry) => radioSignature(entry));
        if (!selectEl) return;
        const options = ['<option value="">Custom / current</option>'];
        for (let i = 0; i < radioPresetEntries.length; i++) {
          const preset = radioPresetEntries[i];
          options.push(`<option value="${i}">${escapeHtml(preset.title)}</option>`);
        }
        selectEl.innerHTML = options.join("");
        syncRadioPresetUi();
      } catch (error) {
        radioPresetEntries = [];
        if (selectEl) {
          selectEl.innerHTML = '<option value="">Community presets unavailable</option>';
        }
        if (applyBtn) applyBtn.disabled = true;
        setRadioPresetStatus(error && error.message ? error.message : "Unable to load community presets.", true);
      }
    }
    async function loadStatsPage() {
      const summaryEl = document.getElementById("statsSummary");
      if (!summaryEl) return;
      summaryEl.innerHTML = '<div class="stats-empty">Loading summary...</div>';
      initTrendCards();
      try {
        const payload = await fetchJson("/api/stats?view=summary");
        if (!payload) throw new Error("no summary payload");
        renderStatsSummary(payload);
      } catch (error) {
        summaryEl.innerHTML = `<div class="stats-error">${escapeHtml(error && error.message ? error.message : "summary unavailable")}</div>`;
        return;
      }

      const seriesOrder = ["battery", "memory", "signal", "noise_floor", "packets"];
      for (const key of seriesOrder) {
        try {
          const payload = await fetchJson("/api/stats?series=" + encodeURIComponent(key));
          renderTrendResult(key, payload);
        } catch (error) {
          renderTrendError(key, error && error.message ? error.message : "Unavailable");
        }
        await pause(40);
      }
    }
    document.getElementById("runBtn").onclick = () => runCommand(document.getElementById("command").value);
    const openStats = () => window.location.assign("/stats");
    const openApp = () => window.location.assign("/app");
    document.getElementById("openStatsPanelBtn").onclick = () => openStats();
    document.getElementById("command").addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        document.getElementById("runBtn").click();
      }
    });
	    document.querySelectorAll("[data-cmd]").forEach((btn) => btn.onclick = () => runCommand(btn.dataset.cmd));
	    document.querySelectorAll("[data-prefix]").forEach((btn) => btn.onclick = () => runPrefixed(btn.dataset.prefix, btn.dataset.input));
	    document.querySelectorAll("[data-load-cmd]").forEach((btn) => btn.onclick = () => loadField(btn.dataset.loadCmd, btn.dataset.loadInput, btn.dataset.loadFormat));
	    async function setEastmeshMode(enabled) {
	      if (enabled && getLetsmeshMode() === "both") {
	        await setLetsmeshMode("eu");
	      }
	      const result = await runCommand(enabled ? "set mqtt.eastmesh-au on" : "set mqtt.eastmesh-au off");
	      if (!result.ok) {
	        refreshEastmeshModeUi();
	        refreshLetsmeshModeUi();
	        return;
	      }
	      setBrokerToggle("mqttEastmeshAu", enabled ? "on" : "off");
	      refreshEastmeshModeUi();
	      refreshLetsmeshModeUi();
	    }
	    const eastmeshModeSlider = document.getElementById("mqttEastmeshMode");
	    if (eastmeshModeSlider) {
	      eastmeshModeSlider.addEventListener("input", () => {
	        eastmeshModeSlider.value = (Number.parseInt(eastmeshModeSlider.value, 10) || 0) >= 1 ? "1" : "0";
	      });
	      eastmeshModeSlider.addEventListener("change", () => {
	        setEastmeshMode((Number.parseInt(eastmeshModeSlider.value, 10) || 0) >= 1);
	      });
	    }
	    async function setLetsmeshMode(mode) {
	      const eastmesh = document.getElementById("mqttEastmeshAu");
	      if (mode === "both" && eastmesh && eastmesh.checked) {
	        refreshLetsmeshModeUi();
	        return;
	      }
	      const desired = {
	        eu: mode === "eu" || mode === "both",
	        us: mode === "us" || mode === "both"
	      };
	      const currentEu = document.getElementById("mqttLetsmeshEu").checked;
	      const currentUs = document.getElementById("mqttLetsmeshUs").checked;
	      const commands = [];
	      if (currentEu && !desired.eu) commands.push(["mqttLetsmeshEu", "set mqtt.letsmesh-eu off", "off"]);
	      if (currentUs && !desired.us) commands.push(["mqttLetsmeshUs", "set mqtt.letsmesh-us off", "off"]);
	      if (!currentEu && desired.eu) commands.push(["mqttLetsmeshEu", "set mqtt.letsmesh-eu on", "on"]);
	      if (!currentUs && desired.us) commands.push(["mqttLetsmeshUs", "set mqtt.letsmesh-us on", "on"]);
	      for (const [inputId, command, nextState] of commands) {
	        const result = await runCommand(command);
	        if (!result.ok) {
	          refreshLetsmeshModeUi();
	          return;
	        }
	        setBrokerToggle(inputId, nextState);
	      }
	      refreshLetsmeshModeUi();
	    }
	    const letsmeshModeSlider = document.getElementById("mqttLetsmeshMode");
	    if (letsmeshModeSlider) {
	      letsmeshModeSlider.addEventListener("input", () => {
	        letsmeshModeSlider.value = String(clampLetsmeshModeIndex(Number.parseInt(letsmeshModeSlider.value, 10) || 0));
	      });
	      letsmeshModeSlider.addEventListener("change", () => {
	        const modes = ["off", "eu", "us", "both"];
	        const index = clampLetsmeshModeIndex(Number.parseInt(letsmeshModeSlider.value, 10) || 0);
	        setLetsmeshMode(modes[index] || "off");
	      });
	    }
	    document.getElementById("saveOwnerInfo").onclick = () => {
	      const value = document.getElementById("ownerInfo").value.replace(/\n/g, "|");
	      runCommand("set owner.info " + value);
	    };
    document.getElementById("refreshRadioBtn").onclick = () => loadRadioConfig();
    document.getElementById("reloadRadioPresetsBtn").onclick = () => loadRadioPresets();
    document.getElementById("copyPublicKeyBtn").onclick = () => copyToClipboard(document.getElementById("publicKey").value, "Public key copied");
    document.getElementById("copyPrivateKeyBtn").onclick = () => copyToClipboard(document.getElementById("privateKey").value.toUpperCase(), "Private key copied");
    document.getElementById("syncClockBtn").onclick = () => syncClock();
    const ghostNodeModeSlider = document.getElementById("ghostNodeMode");
    if (ghostNodeModeSlider) {
      ghostNodeModeSlider.addEventListener("input", () => {
        ghostNodeModeSlider.value = (Number.parseInt(ghostNodeModeSlider.value, 10) || 0) >= 1 ? "1" : "0";
      });
      ghostNodeModeSlider.addEventListener("change", () => {
        const enabled = (Number.parseInt(ghostNodeModeSlider.value, 10) || 0) >= 1;
        const prompt = enabled
          ? "Enable Ghost Node Mode? This leaves web and MQTT on, but turns repeat off and disables adverts."
          : "Disable Ghost Node Mode and restore the prior repeat/advert settings?";
        if (!confirm(prompt)) {
          loadGhostNodeModeState({ recordHistory:false, updateInput:false });
          return;
        }
        setGhostNodeMode(enabled);
      });
    }
    document.getElementById("radioPreset").addEventListener("change", (event) => {
      const value = event.target.value;
      if (value === "") {
        syncRadioPresetUi();
        return;
      }
      const preset = radioPresetEntries[Number.parseInt(value, 10)];
      if (!preset) {
        syncRadioPresetUi();
        return;
      }
      document.getElementById("applyRadioPresetBtn").disabled = false;
    });
    document.getElementById("applyRadioPresetBtn").onclick = async () => {
      const selectEl = document.getElementById("radioPreset");
      const index = Number.parseInt(selectEl.value, 10);
      const preset = radioPresetEntries[index];
      if (!preset) {
        setRadioPresetStatus("Select a community preset first.", true);
        return;
      }
      const command = `set radio ${preset.frequency} ${preset.bandwidth} ${preset.spreadingFactor} ${preset.codingRate}`;
      const result = await runCommand(command);
      if (!result.ok) {
        setRadioPresetStatus(parseReplyValue(result.text) || "Unable to apply radio preset.", true);
        return;
      }
      currentRadioConfig = normalizeRadioConfig(preset);
      syncRadioPresetUi();
      setRadioPresetStatus("Preset saved. Reboot to apply the new radio settings.", false);
    };
    document.getElementById("rebootBtn").onclick = async () => {
      if (confirm("Reboot the repeater now?")) {
        await runCommand("reboot");
      }
    };
    document.getElementById("advertBtn").onclick = async () => {
      if (confirm("Send an advert now?")) {
        await runCommand("advert");
      }
    };
    document.getElementById("otaBtn").onclick = async () => {
      if (confirm("Start OTA mode now?")) {
        await runCommand("start ota");
      }
    };
    document.getElementById("refreshPageBtn").onclick = async () => {
      if (isStatsPage) {
        await loadStatsPage();
      } else {
        await initApp();
      }
    };
    document.getElementById("logoutBtn").onclick = () => {
      redirectToLogin();
    };
	    async function initApp() {
      if (!token) {
        redirectToLogin();
        return;
      }
      showAuthedUi(true);
      if (isStatsPage) {
        try {
          await loadStatsPage();
          statusEl.textContent = "Ready";
        } catch (error) {
          statusEl.textContent = error && error.message ? error.message : "Unable to load stats.";
        }
        return;
      }
      const quiet = { recordHistory:false, updateInput:false };
      try {
        await loadSection("Loading info...", [
          () => loadField("get role", "roleValue", null, quiet),
          () => loadField("clock", "clockUtc", null, quiet),
          () => loadField("get public.key", "publicKey", "uppercase", quiet)
        ]);
        await loadSection("Loading repeater settings...", [
          () => loadField("get name", "nodeName", null, quiet),
          () => loadField("get lat", "nodeLat", null, quiet),
          () => loadField("get lon", "nodeLon", null, quiet),
          () => loadField("get prv.key", "privateKey", "uppercase", quiet),
          () => loadField("get owner.info", "ownerInfo", "multiline", quiet)
        ]);
        await loadSection("Loading radio settings...", [
          () => loadRadioConfig(quiet),
          () => loadField("get path.hash.mode", "pathHashMode", null, quiet)
        ]);
        await loadSection("Loading advertising...", [
          () => loadField("get advert.interval", "advertInterval", null, quiet),
          () => loadField("get flood.advert.interval", "floodInterval", null, quiet),
          () => loadField("get flood.max", "floodMax", null, quiet),
          () => loadGhostNodeModeState(quiet)
        ]);
        await loadSection("Loading MQTT settings...", [
          () => loadField("get mqtt.iata", "mqttIata", null, quiet),
          () => loadField("get mqtt.owner", "mqttOwner", null, quiet),
          () => loadField("get mqtt.email", "mqttEmail", null, quiet),
          () => loadBrokerState("get mqtt.eastmesh-au", "mqttEastmeshAu", quiet),
          () => loadBrokerState("get mqtt.letsmesh-eu", "mqttLetsmeshEu", quiet),
          () => loadBrokerState("get mqtt.letsmesh-us", "mqttLetsmeshUs", quiet)
        ]);
        statusEl.textContent = "Ready";
      } catch (error) {
        if (!token) {
          return;
        }
        statusEl.textContent = error && error.message ? error.message : "Unable to load repeater settings.";
      }
      loadRadioPresets();
	    }
	    refreshEastmeshModeUi();
	    refreshLetsmeshModeUi();
	    initApp();
  </script>
</body>
</html>
)HTML";

}  // namespace

WebPanelServer::WebPanelServer()
    : _runner(nullptr), _server(nullptr), _token{0}, _last_activity_ms(0), _route_context{this} {
}

void WebPanelServer::setCommandRunner(WebPanelCommandRunner* runner) {
  _runner = runner;
}

bool WebPanelServer::start() {
  if (_server != nullptr || _runner == nullptr) {
    return _server != nullptr;
  }

  noteActivity();

  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.httpd.max_open_sockets = 2;
  config.httpd.max_uri_handlers = 7;
  config.httpd.max_resp_headers = 4;
  config.httpd.backlog_conn = 2;
  config.httpd.recv_wait_timeout = 2;
  config.httpd.send_wait_timeout = 2;
  config.httpd.stack_size = kWebServerStackSize;
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  config.servercert = reinterpret_cast<const uint8_t*>(mqtt_web_panel_cert::kServerCertPem);
  config.servercert_len = sizeof(mqtt_web_panel_cert::kServerCertPem);
#else
  // IDF 4.x uses the misnamed CA slot for the server certificate.
  config.cacert_pem = reinterpret_cast<const uint8_t*>(mqtt_web_panel_cert::kServerCertPem);
  config.cacert_len = sizeof(mqtt_web_panel_cert::kServerCertPem);
#endif
  config.prvtkey_pem = reinterpret_cast<const uint8_t*>(mqtt_web_panel_cert::kServerKeyPem);
  config.prvtkey_len = sizeof(mqtt_web_panel_cert::kServerKeyPem);

  esp_err_t rc = httpd_ssl_start(&_server, &config);
  if (rc != ESP_OK) {
    _server = nullptr;
    WEB_PANEL_LOG("server start failed rc=0x%x", static_cast<unsigned>(rc));
    return false;
  }

  httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = &WebPanelServer::handleIndex, .user_ctx = &_route_context};
  httpd_uri_t app_uri = {.uri = "/app", .method = HTTP_GET, .handler = &WebPanelServer::handleApp, .user_ctx = &_route_context};
  httpd_uri_t stats_page_uri = {.uri = "/stats", .method = HTTP_GET, .handler = &WebPanelServer::handleStatsPage, .user_ctx = &_route_context};
  httpd_uri_t login_uri = {.uri = "/login", .method = HTTP_POST, .handler = &WebPanelServer::handleLogin, .user_ctx = &_route_context};
  httpd_uri_t command_uri = {.uri = "/api/command", .method = HTTP_POST, .handler = &WebPanelServer::handleCommand, .user_ctx = &_route_context};
  httpd_uri_t stats_uri = {.uri = "/api/stats", .method = HTTP_GET, .handler = &WebPanelServer::handleStats, .user_ctx = &_route_context};
  httpd_register_uri_handler(_server, &index_uri);
  httpd_register_uri_handler(_server, &app_uri);
  httpd_register_uri_handler(_server, &stats_page_uri);
  httpd_register_uri_handler(_server, &login_uri);
  httpd_register_uri_handler(_server, &command_uri);
  httpd_register_uri_handler(_server, &stats_uri);
  WEB_PANEL_LOG("server started on https://%s/", WiFi.localIP().toString().c_str());
  return true;
}

void WebPanelServer::stop() {
  if (_server != nullptr) {
    WEB_PANEL_LOG("server stopped");
    httpd_ssl_stop(_server);
    _server = nullptr;
  }
  _token[0] = 0;
  _last_activity_ms = 0;
}

bool WebPanelServer::isRunning() const {
  return _server != nullptr;
}

bool WebPanelServer::hasSessionToken() const {
  return _token[0] != 0;
}

bool WebPanelServer::shouldAutoLock(unsigned long now_ms) const {
  if (_server == nullptr || _token[0] == 0 || kWebIdleTimeoutMs == 0 || _last_activity_ms == 0) {
    return false;
  }
  return now_ms - _last_activity_ms >= kWebIdleTimeoutMs;
}

void WebPanelServer::lockSession() {
  _token[0] = 0;
  _last_activity_ms = 0;
}

esp_err_t WebPanelServer::handleIndex(httpd_req_t* req) {
  auto* ctx = static_cast<RouteContext*>(req->user_ctx);
  if (ctx == nullptr || ctx->self == nullptr) {
    return httpd_resp_send_500(req);
  }
  ctx->self->noteActivity();
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return sendProgmemChunked(req, kWebPanelLoginHtml);
}

esp_err_t WebPanelServer::handleApp(httpd_req_t* req) {
  auto* ctx = static_cast<RouteContext*>(req->user_ctx);
  if (ctx == nullptr || ctx->self == nullptr) {
    return httpd_resp_send_500(req);
  }
  ctx->self->noteActivity();
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return sendProgmemChunked(req, kWebPanelAppHtml);
}

esp_err_t WebPanelServer::handleStatsPage(httpd_req_t* req) {
  auto* ctx = static_cast<RouteContext*>(req->user_ctx);
  if (ctx == nullptr || ctx->self == nullptr) {
    return httpd_resp_send_500(req);
  }
  ctx->self->noteActivity();
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (ctx->self->_runner != nullptr && !ctx->self->_runner->isWebStatsEnabled()) {
    return sendProgmemChunked(req, kWebPanelStatsDisabledHtml);
  }
  return sendProgmemChunked(req, kWebPanelAppHtml);
}

esp_err_t WebPanelServer::handleLogin(httpd_req_t* req) {
  auto* ctx = static_cast<RouteContext*>(req->user_ctx);
  if (ctx == nullptr || ctx->self == nullptr || ctx->self->_runner == nullptr) {
    return httpd_resp_send_500(req);
  }

  char* password = allocScratchBuffer(kWebPasswordBufferSize);
  if (password == nullptr) {
    return httpd_resp_send_500(req);
  }

  if (!ctx->self->readRequestBody(req, password, kWebPasswordBufferSize)) {
    freeScratchBuffer(password);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
  }

  if (strcmp(password, ctx->self->_runner->getWebAdminPassword()) != 0) {
    freeScratchBuffer(password);
    WEB_PANEL_LOG("login denied");
    return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Bad password");
  }

  freeScratchBuffer(password);
  ctx->self->refreshToken();
  ctx->self->noteActivity();
  WEB_PANEL_LOG("login accepted");
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, ctx->self->_token);
}

esp_err_t WebPanelServer::handleCommand(httpd_req_t* req) {
  auto* ctx = static_cast<RouteContext*>(req->user_ctx);
  if (ctx == nullptr || ctx->self == nullptr || ctx->self->_runner == nullptr) {
    return httpd_resp_send_500(req);
  }
  if (!ctx->self->isAuthorized(req)) {
    return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
  }

  char* command = allocScratchBuffer(kWebCommandBufferSize);
  char* reply = allocScratchBuffer(kWebReplyBufferSize);
  if (command == nullptr || reply == nullptr) {
    freeScratchBuffer(command);
    freeScratchBuffer(reply);
    return httpd_resp_send_500(req);
  }

  if (!ctx->self->readRequestBody(req, command, kWebCommandBufferSize)) {
    freeScratchBuffer(command);
    freeScratchBuffer(reply);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
  }

  ctx->self->noteActivity();
  memset(reply, 0, kWebReplyBufferSize);
  ctx->self->_runner->runWebCommand(command, reply, kWebReplyBufferSize);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t rc = httpd_resp_send(req, reply[0] ? reply : "OK", HTTPD_RESP_USE_STRLEN);
  freeScratchBuffer(command);
  freeScratchBuffer(reply);
  return rc;
}

esp_err_t WebPanelServer::handleStats(httpd_req_t* req) {
  auto* ctx = static_cast<RouteContext*>(req->user_ctx);
  if (ctx == nullptr || ctx->self == nullptr || ctx->self->_runner == nullptr) {
    return httpd_resp_send_500(req);
  }
  if (!ctx->self->isAuthorized(req)) {
    return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
  }

  ctx->self->noteActivity();
  char view[16];
  char series[24];
  char* reply = allocScratchBuffer(kWebStatsReplyBufferSize);
  if (reply == nullptr) {
    freeScratchBuffer(reply);
    return httpd_resp_send_500(req);
  }

  if (getQueryValue(req, "view", view, sizeof(view)) && strcmp(view, "legacy") == 0) {
    esp_err_t legacy_rc = sendLegacyStatsBundle(req, ctx->self->_runner, reply);
    freeScratchBuffer(reply);
    return legacy_rc;
  }

  if (!ctx->self->_runner->isWebStatsEnabled()) {
    freeScratchBuffer(reply);
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_send(req, "Stats disabled", HTTPD_RESP_USE_STRLEN);
  }

  bool ok = false;
  if (getQueryValue(req, "series", series, sizeof(series))) {
    ok = ctx->self->_runner->formatWebStatsSeriesJson(series, reply, kWebStatsReplyBufferSize);
  } else {
    ok = ctx->self->_runner->formatWebStatsSummaryJson(reply, kWebStatsReplyBufferSize);
  }

  if (!ok || reply[0] == 0) {
    freeScratchBuffer(reply);
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No stats data");
  }

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t rc = httpd_resp_send(req, reply, HTTPD_RESP_USE_STRLEN);
  freeScratchBuffer(reply);
  return rc;
}

bool WebPanelServer::readRequestBody(httpd_req_t* req, char* buffer, size_t buffer_size) const {
  if (req == nullptr || buffer == nullptr || buffer_size == 0 || req->content_len <= 0 ||
      req->content_len >= static_cast<int>(buffer_size)) {
    return false;
  }

  int remaining = req->content_len;
  int offset = 0;
  while (remaining > 0) {
    int read = httpd_req_recv(req, &buffer[offset], remaining);
    if (read <= 0) {
      return false;
    }
    offset += read;
    remaining -= read;
  }
  buffer[offset] = 0;
  return true;
}

void WebPanelServer::refreshToken() {
  uint8_t token[16];
  esp_fill_random(token, sizeof(token));
  bytesToHexUpper(token, sizeof(token), _token, sizeof(_token));
}

bool WebPanelServer::isAuthorized(httpd_req_t* req) const {
  if (_token[0] == 0) {
    return false;
  }
  char token[40];
  if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", token, sizeof(token)) != ESP_OK) {
    return false;
  }
  return strcmp(token, _token) == 0;
}

void WebPanelServer::noteActivity() {
  _last_activity_ms = millis();
}

#else

WebPanelServer::WebPanelServer()
    : _runner(nullptr) {
}

void WebPanelServer::setCommandRunner(WebPanelCommandRunner* runner) {
  _runner = runner;
}

bool WebPanelServer::start() {
  return false;
}

void WebPanelServer::stop() {
}

bool WebPanelServer::isRunning() const {
  return false;
}

bool WebPanelServer::hasSessionToken() const {
  return false;
}

bool WebPanelServer::shouldAutoLock(unsigned long) const {
  return false;
}

void WebPanelServer::lockSession() {
}

#endif
