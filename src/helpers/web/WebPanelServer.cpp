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
      <p>Use the repeater admin password to unlock the panel. Accept the self-signed certificate warning in your browser first.</p>
      <input id="password" type="password" placeholder="Admin password" autocomplete="current-password">
      <button id="loginBtn">Unlock</button>
      <div id="status"></div>
    </section>
  </main>
  <script>
    const statusEl = document.getElementById("status");
    async function login() {
      const pwd = document.getElementById("password").value;
      const res = await fetch("/login", { method:"POST", body: pwd });
      const text = await res.text();
      if (!res.ok) {
        statusEl.textContent = text || "Access denied";
        return;
      }
      sessionStorage.setItem("repeater-token", text.trim());
      window.location.replace("/app");
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
    p { color:var(--text-muted); margin:8px 0 0; }
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
    .panel-warning { min-height:1.4em; font-size:13px; color:var(--status-red); }
    .panel-note { font-size:13px; color:var(--text-muted); }
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
    .actions-bar { display:grid; grid-template-columns:1fr auto 1fr; align-items:center; gap:12px; }
    .actions-group { display:flex; gap:10px; flex-wrap:wrap; }
    .actions-group.left { justify-self:start; }
    .actions-group.center { justify-self:center; }
    .actions-group.right { justify-self:end; }
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
    @media (max-width:760px) {
      body { font-size:15px; }
      main { padding:16px; }
      .card { padding:16px; margin-bottom:14px; }
      .row, .row3, .row-command, .metric-grid, .hud-grid-1, .hud-grid-2, .hud-grid-3, .core-grid, .core-metrics, .broker-stack, .broker-grid, .broker-grid.single, .broker-grid.two { grid-template-columns:1fr; }
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
      .actions-group.center { grid-template-columns:1fr; justify-items:center; }
      #actionsPanel .actions-group button { width:100%; }
      #actionsPanel .actions-group.center button { width:auto; min-width:56px; }
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
      <p>Use the repeater admin password to unlock the command console. Accept the self-signed certificate warning in your browser first.</p>
      <div class="row" style="margin-top:14px">
        <input id="password" type="password" placeholder="Admin password">
        <button id="loginBtn">Unlock</button>
      </div>
      <div id="status"></div>
    </section>

    <section class="card" id="actionsPanel" style="display:none">
      <h2>Actions</h2>
      <div class="actions-bar">
        <div class="actions-group left">
          <button id="advertBtn" class="action-advert">Advert</button>
          <button id="otaBtn" class="action-dreamy">Start OTA</button>
        </div>
        <div class="actions-group center">
          <button id="themeToggle" class="themebtn" aria-label="Toggle theme" title="Toggle theme">☾</button>
        </div>
        <div class="actions-group right">
          <button id="rebootBtn" class="action-caution">Reboot</button>
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
      <p>Only the allowlisted commands exposed by this panel will run here.</p>
      <div id="reply" class="terminal"></div>
    </section>

    <section class="card" id="repeaterSettingsPanel" style="display:none">
      <h2>Repeater Settings</h2>
      <div class="stack">
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
          <label class="label" for="guestPassword">Guest Password</label>
          <div class="inline-actions">
            <input id="guestPassword" type="password" placeholder="new guest password">
	            <span class="placeholder-slot" aria-hidden="true"></span>
            <button class="savebtn" data-prefix="set guest.password " data-input="guestPassword">Save</button>
          </div>
        </div>
        <div class="field-card">
          <label class="label" for="privateKey">Private Key</label>
          <div class="inline-actions">
            <input id="privateKey" placeholder="64-hex-char private key">
	            <span class="placeholder-slot" aria-hidden="true"></span>
            <button class="savebtn" data-prefix="set prv.key " data-input="privateKey">Save</button>
          </div>
          <p>Changing the private key requires a reboot to apply.</p>
        </div>
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
        <div class="field-card">
          <label class="label" for="ownerInfo">Owner Info</label>
          <div class="inline-actions">
            <textarea id="ownerInfo" placeholder="Free text shown in owner info"></textarea>
            <button class="iconbtn" data-load-cmd="get owner.info" data-load-input="ownerInfo" data-load-format="multiline" title="Refresh owner info">&#8635;</button>
            <button id="saveOwnerInfo" class="savebtn">Save</button>
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
        <button id="getStatsBtn">Get Stats</button>
      </div>
      <div id="statsDashboard" class="stats-shell">
        <div class="stats-empty">Press Get Stats to load the dashboard.</div>
      </div>
    </section>

  </main>
  <script>
    let token = sessionStorage.getItem("repeater-token") || "";
    let commandQueue = Promise.resolve();
    const statusEl = document.getElementById("status");
    const replyEl = document.getElementById("reply");
    const themeToggleEl = document.getElementById("themeToggle");
    const rootEl = document.documentElement;
    function redirectToLogin() {
      sessionStorage.removeItem("repeater-token");
      token = "";
      window.location.replace("/");
    }
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
    function toggleTheme() {
      const next = rootEl.dataset.theme === "dark" ? "light" : "dark";
      localStorage.setItem("repeater-theme", next);
      applyTheme(next);
    }
    applyTheme(getPreferredTheme());
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
    function clamp(value, min, max) {
      return Math.min(max, Math.max(min, value));
    }
    function pctRange(value, min, max) {
      if (!Number.isFinite(value) || max <= min) return 0;
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
    function renderMeter(label, value, percent, note, invert) {
      const pct = clamp(Math.round(percent), 0, 100);
      const tone = toneForPercent(pct, invert);
      return `<div class="hud-row">
        <div class="hud-kpi">
          <div>
            <div class="metric-label">${escapeHtml(label)}</div>
            <div class="hud-value">${escapeHtml(value)}</div>
          </div>
          <div class="hud-sub">${escapeHtml(note)}</div>
        </div>
        <div class="meter"><div class="meter-fill${tone ? " " + tone : ""}" style="width:${pct}%"></div></div>
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
      const rssiPct = pctRange(wifi.rssi, -100, -50);
      const statusNote = wifi.status === "connected" ? (wifi.signal || "linked") : (wifi.state || "idle");
      return `<section class="hud-card">
        <h3>Wi-Fi</h3>
        ${renderMeter("Signal Quality", Number.isFinite(wifi.quality) ? wifi.quality + "%" : "--", qualityPct, statusNote, false)}
        ${renderMeter("RSSI", Number.isFinite(wifi.rssi) ? wifi.rssi + " dBm" : "--", rssiPct, wifi.ssid || "-", false)}
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
      const rssiPct = pctRange(radio.last_rssi, -120, -20);
      const snrPct = pctRange(radio.last_snr, -20, 20);
      const noisePct = pctRange(radio.noise_floor, -130, -60);
      const totalAir = (radio.tx_air_secs || 0) + (radio.rx_air_secs || 0);
      const txShare = pctRatio(radio.tx_air_secs || 0, totalAir);
      return `<section class="hud-card">
        <h3>Radio</h3>
        ${renderMeter("RSSI", (radio.last_rssi ?? "--") + " dBm", rssiPct, "signal strength", false)}
        ${renderMeter("SNR", Number.isFinite(radio.last_snr) ? radio.last_snr.toFixed(1) + " dB" : "--", snrPct, "link quality", false)}
        ${renderMeter("Noise Floor", (radio.noise_floor ?? "--") + " dBm", noisePct, "ambient RF", true)}
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
      return `<section class="hud-card">
        <h3>Memory</h3>
        ${renderMeter("Heap Largest Block", formatBytes(heapMax), pctRatio(heapMax, heapFree), "largest alloc vs free", false)}
        ${renderMeter("PSRAM Largest Block", formatBytes(psramMax), pctRatio(psramMax, psramFree), "largest alloc vs free", false)}
        <div class="metric-grid">
          ${renderMetric("Heap Free", formatBytes(heapFree))}
          ${renderMetric("Heap Min", formatBytes(memory.heap_min || 0))}
          ${renderMetric("PSRAM Free", formatBytes(psramFree))}
          ${renderMetric("PSRAM Min", formatBytes(memory.psram_min || 0))}
        </div>
      </section>`;
    }
    function renderStatsDashboard(results, errors) {
      const dashboardEl = document.getElementById("statsDashboard");
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
    function showAuthedUi(show) {
      document.getElementById("login").style.display = show ? "none" : "block";
      document.getElementById("cliPanel").style.display = show ? "block" : "none";
      document.getElementById("quickCommandsPanel").style.display = show ? "block" : "none";
      document.getElementById("mqttSettingsPanel").style.display = show ? "block" : "none";
      document.getElementById("statsPanel").style.display = show ? "block" : "none";
      document.getElementById("actionsPanel").style.display = show ? "block" : "none";
      document.getElementById("repeaterSettingsPanel").style.display = show ? "block" : "none";
      if (!show) {
        commandQueue = Promise.resolve();
        const passwordEl = document.getElementById("password");
        if (passwordEl) passwordEl.value = "";
        if (statusEl) statusEl.textContent = "";
        document.getElementById("statsDashboard").innerHTML = '<div class="stats-empty">Press Get Stats to load the dashboard.</div>';
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
        if (recordHistory) {
          appendHistory(cmd, text, res.ok);
        }
        return { ok:res.ok, text };
      });
    }
    function runPrefixed(prefix, inputId) {
      const value = document.getElementById(inputId).value;
      runCommand(prefix + value);
    }
    async function loadField(cmd, inputId, format) {
      const result = await runCommand(cmd);
      if (!result.ok) return;
      let value = parseReplyValue(result.text);
      if (format === "multiline") {
        value = value.replace(/\|/g, "\n");
      }
      document.getElementById(inputId).value = value;
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
	    function applyBootstrapData(data) {
	      if (!data) return;
	      if (typeof data.name === "string") document.getElementById("nodeName").value = data.name;
	      if (typeof data.mqtt_iata === "string" && data.mqtt_iata.length) document.getElementById("mqttIata").value = data.mqtt_iata;
	      if (typeof data.mqtt_owner === "string") document.getElementById("mqttOwner").value = data.mqtt_owner;
	      if (typeof data.mqtt_email === "string") document.getElementById("mqttEmail").value = data.mqtt_email;
	      if (typeof data.advert_interval === "string") document.getElementById("advertInterval").value = data.advert_interval;
	      if (typeof data.flood_interval === "string") document.getElementById("floodInterval").value = data.flood_interval;
	      if (typeof data.flood_max === "string") document.getElementById("floodMax").value = data.flood_max;
	      if (typeof data.mqtt_eastmesh_au === "string") setBrokerToggle("mqttEastmeshAu", data.mqtt_eastmesh_au);
	      if (typeof data.mqtt_letsmesh_eu === "string") setBrokerToggle("mqttLetsmeshEu", data.mqtt_letsmesh_eu);
	      if (typeof data.mqtt_letsmesh_us === "string") setBrokerToggle("mqttLetsmeshUs", data.mqtt_letsmesh_us);
	      updateBrokerWarning();
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
	    function updateBrokerWarning() {
	      document.getElementById("mqttBrokerWarning").textContent = "";
	    }
	    async function loadBrokerState(cmd, inputId) {
	      const result = await runCommand(cmd);
	      if (!result.ok) return;
	      setBrokerToggle(inputId, parseReplyValue(result.text));
	      updateBrokerWarning();
	    }
    async function refreshStats() {
      const getStatsBtn = document.getElementById("getStatsBtn");
      getStatsBtn.disabled = true;
      getStatsBtn.textContent = "Loading...";
      document.getElementById("statsDashboard").innerHTML = '<div class="stats-empty">Loading dashboard...</div>';
      const results = {};
      const errors = [];
      try {
        const payload = await fetchJson("/api/stats");
        if (!payload) throw new Error("no stats payload");
        if (typeof payload.wifi === "string") {
          const parsed = parseWifiStatusReply(payload.wifi);
          if (parsed != null) results.wifi = parsed;
          else errors.push("get wifi.status: invalid reply");
        }
        if (typeof payload.wifi_powersave === "string") {
          results.wifi_powersave = parseReplyValue(payload.wifi_powersave) || "--";
        }
        for (const key of ["core", "radio", "packets", "memory"]) {
          if (typeof payload[key] === "string") {
            const parsed = parseJsonReply(payload[key]);
            if (parsed != null) results[key] = parsed;
            else errors.push(key + ": invalid reply");
          }
        }
      } catch (error) {
        errors.push(error && error.message ? error.message : "stats request failed");
      }
      renderStatsDashboard(results, errors);
      getStatsBtn.disabled = false;
      getStatsBtn.textContent = "Get Stats";
    }
    document.getElementById("runBtn").onclick = () => runCommand(document.getElementById("command").value);
    document.getElementById("getStatsBtn").onclick = () => refreshStats();
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
	        updateBrokerWarning();
	        return;
	      }
	      setBrokerToggle("mqttEastmeshAu", enabled ? "on" : "off");
	      refreshEastmeshModeUi();
	      refreshLetsmeshModeUi();
	      updateBrokerWarning();
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
	        updateBrokerWarning();
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
	          updateBrokerWarning();
	          return;
	        }
	        setBrokerToggle(inputId, nextState);
	      }
	      refreshLetsmeshModeUi();
	      updateBrokerWarning();
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
    document.getElementById("logoutBtn").onclick = () => {
      redirectToLogin();
    };
	    async function initApp() {
      if (!token) {
        redirectToLogin();
        return;
      }
      showAuthedUi(true);
      try {
        applyBootstrapData(await fetchJson("/api/bootstrap"));
      } catch (_) {
        if (!token) {
          return;
        }
        await Promise.all([
          loadField("get name", "nodeName"),
          loadField("get mqtt.iata", "mqttIata"),
          loadField("get mqtt.owner", "mqttOwner"),
          loadField("get mqtt.email", "mqttEmail"),
          loadBrokerState("get mqtt.eastmesh-au", "mqttEastmeshAu"),
          loadBrokerState("get mqtt.letsmesh-eu", "mqttLetsmeshEu"),
          loadBrokerState("get mqtt.letsmesh-us", "mqttLetsmeshUs"),
          loadField("get advert.interval", "advertInterval"),
          loadField("get flood.advert.interval", "floodInterval"),
          loadField("get flood.max", "floodMax")
        ]);
      }
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
  config.httpd.max_uri_handlers = 6;
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
  httpd_uri_t login_uri = {.uri = "/login", .method = HTTP_POST, .handler = &WebPanelServer::handleLogin, .user_ctx = &_route_context};
  httpd_uri_t command_uri = {.uri = "/api/command", .method = HTTP_POST, .handler = &WebPanelServer::handleCommand, .user_ctx = &_route_context};
  httpd_uri_t bootstrap_uri = {.uri = "/api/bootstrap", .method = HTTP_GET, .handler = &WebPanelServer::handleBootstrap, .user_ctx = &_route_context};
  httpd_uri_t stats_uri = {.uri = "/api/stats", .method = HTTP_GET, .handler = &WebPanelServer::handleStats, .user_ctx = &_route_context};
  httpd_register_uri_handler(_server, &index_uri);
  httpd_register_uri_handler(_server, &app_uri);
  httpd_register_uri_handler(_server, &login_uri);
  httpd_register_uri_handler(_server, &command_uri);
  httpd_register_uri_handler(_server, &bootstrap_uri);
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

esp_err_t WebPanelServer::handleBootstrap(httpd_req_t* req) {
  auto* ctx = static_cast<RouteContext*>(req->user_ctx);
  if (ctx == nullptr || ctx->self == nullptr || ctx->self->_runner == nullptr) {
    return httpd_resp_send_500(req);
  }
  if (!ctx->self->isAuthorized(req)) {
    return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
  }

  ctx->self->noteActivity();
  char* reply = allocScratchBuffer(kWebReplyBufferSize);
  if (reply == nullptr) {
    freeScratchBuffer(reply);
    return httpd_resp_send_500(req);
  }

  const struct {
    const char* key;
    const char* command;
	  } fields[] = {
	      {"name", "get name"},
	      {"mqtt_iata", "get mqtt.iata"},
	      {"mqtt_owner", "get mqtt.owner"},
	      {"mqtt_email", "get mqtt.email"},
	      {"mqtt_eastmesh_au", "get mqtt.eastmesh-au"},
	      {"mqtt_letsmesh_eu", "get mqtt.letsmesh-eu"},
	      {"mqtt_letsmesh_us", "get mqtt.letsmesh-us"},
	      {"advert_interval", "get advert.interval"},
	      {"flood_interval", "get flood.advert.interval"},
	      {"flood_max", "get flood.max"},
	  };

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (sendChunk(req, "{") != ESP_OK) {
    freeScratchBuffer(reply);
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_FAIL;
  }
  for (size_t i = 0; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    memset(reply, 0, kWebReplyBufferSize);
    ctx->self->_runner->runWebCommand(fields[i].command, reply, kWebReplyBufferSize);
    const char* value = reply;
    if (value[0] == '>' && value[1] == ' ') {
      value += 2;
    }
    if (strcmp(value, "-") == 0) {
      value = "";
    }
    if (sendJsonFieldChunk(req, fields[i].key, value, i != 0) != ESP_OK) {
      freeScratchBuffer(reply);
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
  char* reply = allocScratchBuffer(kWebReplyBufferSize);
  if (reply == nullptr) {
    freeScratchBuffer(reply);
    return httpd_resp_send_500(req);
  }

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
    freeScratchBuffer(reply);
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_FAIL;
  }
  for (size_t i = 0; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    memset(reply, 0, kWebReplyBufferSize);
    ctx->self->_runner->runWebCommand(fields[i].command, reply, kWebReplyBufferSize);
    if (sendJsonFieldChunk(req, fields[i].key, reply, i != 0) != ESP_OK) {
      freeScratchBuffer(reply);
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
