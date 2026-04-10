#include "WebPanelServer.h"

#if defined(ESP_PLATFORM) && WITH_WEB_PANEL

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include <string.h>

#include "../mqtt/generated/WebPanelCert.h"

namespace {

constexpr size_t kWebServerStackSize = 8192;
constexpr size_t kWebPasswordBufferSize = 80;
constexpr size_t kWebCommandBufferSize = 192;
constexpr size_t kWebReplyBufferSize = 256;
constexpr size_t kWebJsonBufferSize = 2048;

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

size_t appendJsonEscaped(char* dst, size_t dst_size, size_t offset, const char* src) {
  if (dst == nullptr || dst_size == 0) {
    return offset;
  }
  for (size_t i = 0; src != nullptr && src[i] != 0 && offset + 2 < dst_size; ++i) {
    char c = src[i];
    if (c == '\\' || c == '"') {
      if (offset + 2 >= dst_size) break;
      dst[offset++] = '\\';
      dst[offset++] = c;
    } else if (c == '\n') {
      if (offset + 2 >= dst_size) break;
      dst[offset++] = '\\';
      dst[offset++] = 'n';
    } else if (c == '\r') {
      if (offset + 2 >= dst_size) break;
      dst[offset++] = '\\';
      dst[offset++] = 'r';
    } else if (c == '\t') {
      if (offset + 2 >= dst_size) break;
      dst[offset++] = '\\';
      dst[offset++] = 't';
    } else {
      dst[offset++] = c;
    }
  }
  dst[(offset < dst_size) ? offset : (dst_size - 1)] = 0;
  return offset;
}

bool appendJsonField(char* dst, size_t dst_size, size_t& offset, const char* key, const char* value, bool comma) {
  int written = snprintf(&dst[offset], (offset < dst_size) ? (dst_size - offset) : 0, "%s\"%s\":\"", comma ? "," : "", key);
  if (written < 0 || offset + static_cast<size_t>(written) >= dst_size) {
    return false;
  }
  offset += static_cast<size_t>(written);
  offset = appendJsonEscaped(dst, dst_size, offset, value != nullptr ? value : "");
  if (offset + 2 >= dst_size) {
    return false;
  }
  dst[offset++] = '"';
  dst[offset] = 0;
  return true;
}

bool appendJsonFieldRaw(char* dst, size_t dst_size, size_t& offset, const char* key, const char* value, bool comma) {
  int written = snprintf(&dst[offset], (offset < dst_size) ? (dst_size - offset) : 0, "%s\"%s\":", comma ? "," : "", key);
  if (written < 0 || offset + static_cast<size_t>(written) >= dst_size) {
    return false;
  }
  offset += static_cast<size_t>(written);
  if (value == nullptr) {
    value = "null";
  }
  written = snprintf(&dst[offset], (offset < dst_size) ? (dst_size - offset) : 0, "%s", value);
  if (written < 0 || offset + static_cast<size_t>(written) >= dst_size) {
    return false;
  }
  offset += static_cast<size_t>(written);
  return true;
}

const char kWebPanelHtml[] PROGMEM = R"HTML(
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
    .broker-grid { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:10px; }
    .broker-card { background:var(--surface2); border:1px solid var(--border); border-radius:12px; padding:12px; display:grid; gap:10px; }
    .broker-row { display:flex; align-items:center; justify-content:space-between; gap:12px; }
    .broker-copy { display:grid; gap:4px; min-width:0; }
    .broker-title { font-size:12px; color:var(--text-muted); text-transform:uppercase; letter-spacing:.06em; }
    .broker-state { font-size:13px; color:var(--text); }
    .broker-state.on { color:var(--accent); }
    .switch { position:relative; display:inline-flex; width:54px; height:32px; flex:0 0 auto; }
    .switch input { position:absolute; opacity:0; width:0; height:0; min-height:0; padding:0; border:0; -webkit-appearance:none; appearance:none; }
    .slider { position:absolute; inset:0; border-radius:999px; background:var(--background); border:1px solid var(--border); transition:background .2s ease,border-color .2s ease; cursor:pointer; }
    .slider::before { content:""; position:absolute; width:24px; height:24px; left:3px; top:3px; border-radius:50%; background:var(--surface1); box-shadow:0 2px 8px rgba(0,0,0,.18); transition:transform .2s ease,background .2s ease; }
    .switch input:checked + .slider { background:var(--accent); border-color:transparent; }
    .switch input:checked + .slider::before { transform:translateX(22px); background:#fff; }
    .panel-warning { min-height:1.4em; font-size:13px; color:var(--status-red); }
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
      .row, .row3, .row-command, .metric-grid, .hud-grid-1, .hud-grid-2, .hud-grid-3, .core-grid, .core-metrics, .broker-grid { grid-template-columns:1fr; }
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
    <section class="card" id="login">
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
	          <div class="broker-grid">
	            <div class="broker-card">
	              <div class="broker-row">
	                <div class="broker-copy">
	                  <div class="broker-title">EastMesh AU</div>
	                  <div class="broker-state" id="mqttEastmeshAuState">Off</div>
	                </div>
	                <label class="switch" aria-label="Toggle EastMesh AU">
	                  <input id="mqttEastmeshAu" type="checkbox" data-broker-on="set mqtt.eastmesh-au on" data-broker-off="set mqtt.eastmesh-au off">
	                  <span class="slider"></span>
	                </label>
	              </div>
	            </div>
	            <div class="broker-card">
	              <div class="broker-row">
	                <div class="broker-copy">
	                  <div class="broker-title">LetsMesh EU</div>
	                  <div class="broker-state" id="mqttLetsmeshEuState">Off</div>
	                </div>
	                <label class="switch" aria-label="Toggle LetsMesh EU">
	                  <input id="mqttLetsmeshEu" type="checkbox" data-broker-on="set mqtt.letsmesh-eu on" data-broker-off="set mqtt.letsmesh-eu off">
	                  <span class="slider"></span>
	                </label>
	              </div>
	            </div>
	            <div class="broker-card">
	              <div class="broker-row">
	                <div class="broker-copy">
	                  <div class="broker-title">LetsMesh US</div>
	                  <div class="broker-state" id="mqttLetsmeshUsState">Off</div>
	                </div>
	                <label class="switch" aria-label="Toggle LetsMesh US">
	                  <input id="mqttLetsmeshUs" type="checkbox" data-broker-on="set mqtt.letsmesh-us on" data-broker-off="set mqtt.letsmesh-us off">
	                  <span class="slider"></span>
	                </label>
	              </div>
	            </div>
	          </div>
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
    let token = "";
    let commandQueue = Promise.resolve();
    const statusEl = document.getElementById("status");
    const replyEl = document.getElementById("reply");
    const themeToggleEl = document.getElementById("themeToggle");
    const rootEl = document.documentElement;
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
        ${renderMeter("Noise Floor", (radio.noise_floor ?? "--") + " dBm", noisePct, "ambient RF", false)}
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
        document.getElementById("password").value = "";
        statusEl.textContent = "";
        document.getElementById("statsDashboard").innerHTML = '<div class="stats-empty">Press Get Stats to load the dashboard.</div>';
      }
    }
    function queueCommand(task) {
      const next = commandQueue.then(task, task);
      commandQueue = next.catch(() => {});
      return next;
    }
    async function runCommand(cmd, options = {}) {
      if (!token) return { ok:false, text:"" };
      const recordHistory = options.recordHistory !== false;
      const updateInput = options.updateInput !== false;
      return queueCommand(async () => {
        if (updateInput) {
          document.getElementById("command").value = cmd;
        }
        const res = await fetch("/api/command", { method:"POST", headers:{ "X-Auth-Token": token }, body: cmd });
        const text = await res.text();
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
      if (!token) return null;
      return queueCommand(async () => {
        const res = await fetch(path, { headers:{ "X-Auth-Token": token } });
        const text = await res.text();
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
	    }
	    function updateBrokerWarning() {
	      const enabled = ["mqttEastmeshAu", "mqttLetsmeshEu", "mqttLetsmeshUs"].filter((inputId) => {
	        const input = document.getElementById(inputId);
	        return input && input.checked;
	      }).length;
	      document.getElementById("mqttBrokerWarning").textContent = enabled >= 3 ? "Running all 3 MQTT servers is not recommended. Prefer 2 at most." : "";
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
    document.getElementById("loginBtn").onclick = async () => {
      const pwd = document.getElementById("password").value;
      const res = await fetch("/login", { method:"POST", body: pwd });
      const text = await res.text();
      if (!res.ok) {
        statusEl.textContent = text || "Access denied";
        return;
      }
      token = text.trim();
      showAuthedUi(true);
	      try {
	        applyBootstrapData(await fetchJson("/api/bootstrap"));
	      } catch (_) {
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
    };
    document.getElementById("password").addEventListener("keydown", (event) => {
      if (event.key === "Enter") {
        event.preventDefault();
        document.getElementById("loginBtn").click();
      }
    });
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
	    document.querySelectorAll("[data-broker-on]").forEach((input) => input.addEventListener("change", () => {
	      const label = document.getElementById(input.id + "State");
	      if (label) {
	        label.textContent = input.checked ? "On" : "Off";
	        label.classList.toggle("on", input.checked);
	      }
	      updateBrokerWarning();
	      runCommand(input.checked ? input.dataset.brokerOn : input.dataset.brokerOff);
	    }));
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
      token = "";
      showAuthedUi(false);
    };
    showAuthedUi(false);
  </script>
</body>
</html>
)HTML";

}  // namespace

WebPanelServer::WebPanelServer()
    : _runner(nullptr), _server(nullptr), _token{0}, _route_context{this} {
}

void WebPanelServer::setCommandRunner(WebPanelCommandRunner* runner) {
  _runner = runner;
}

bool WebPanelServer::start() {
  if (_server != nullptr || _runner == nullptr) {
    return _server != nullptr;
  }

  if (_token[0] == 0) {
    refreshToken();
  }

  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.httpd.max_open_sockets = 2;
  config.httpd.max_uri_handlers = 5;
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
  httpd_uri_t login_uri = {.uri = "/login", .method = HTTP_POST, .handler = &WebPanelServer::handleLogin, .user_ctx = &_route_context};
  httpd_uri_t command_uri = {.uri = "/api/command", .method = HTTP_POST, .handler = &WebPanelServer::handleCommand, .user_ctx = &_route_context};
  httpd_uri_t bootstrap_uri = {.uri = "/api/bootstrap", .method = HTTP_GET, .handler = &WebPanelServer::handleBootstrap, .user_ctx = &_route_context};
  httpd_uri_t stats_uri = {.uri = "/api/stats", .method = HTTP_GET, .handler = &WebPanelServer::handleStats, .user_ctx = &_route_context};
  httpd_register_uri_handler(_server, &index_uri);
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
}

bool WebPanelServer::isRunning() const {
  return _server != nullptr;
}

bool WebPanelServer::hasSessionToken() const {
  return _token[0] != 0;
}

esp_err_t WebPanelServer::handleIndex(httpd_req_t* req) {
  auto* ctx = static_cast<RouteContext*>(req->user_ctx);
  if (ctx == nullptr || ctx->self == nullptr) {
    return httpd_resp_send_500(req);
  }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, kWebPanelHtml, HTTPD_RESP_USE_STRLEN);
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

  char* reply = allocScratchBuffer(kWebReplyBufferSize);
  char* json = allocScratchBuffer(kWebJsonBufferSize);
  if (reply == nullptr || json == nullptr) {
    freeScratchBuffer(reply);
    freeScratchBuffer(json);
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

  size_t offset = 0;
  json[offset++] = '{';
  json[offset] = 0;
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
    if (!appendJsonField(json, kWebJsonBufferSize, offset, fields[i].key, value, i != 0)) {
      freeScratchBuffer(reply);
      freeScratchBuffer(json);
      return httpd_resp_send_500(req);
    }
  }
  if (offset + 2 >= kWebJsonBufferSize) {
    freeScratchBuffer(reply);
    freeScratchBuffer(json);
    return httpd_resp_send_500(req);
  }
  json[offset++] = '}';
  json[offset] = 0;

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t rc = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  freeScratchBuffer(reply);
  freeScratchBuffer(json);
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

  char* reply = allocScratchBuffer(kWebReplyBufferSize);
  char* json = allocScratchBuffer(kWebJsonBufferSize);
  if (reply == nullptr || json == nullptr) {
    freeScratchBuffer(reply);
    freeScratchBuffer(json);
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

  size_t offset = 0;
  json[offset++] = '{';
  json[offset] = 0;
  for (size_t i = 0; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
    memset(reply, 0, kWebReplyBufferSize);
    ctx->self->_runner->runWebCommand(fields[i].command, reply, kWebReplyBufferSize);
    if (!appendJsonField(json, kWebJsonBufferSize, offset, fields[i].key, reply, i != 0)) {
      freeScratchBuffer(reply);
      freeScratchBuffer(json);
      return httpd_resp_send_500(req);
    }
  }
  if (offset + 2 >= kWebJsonBufferSize) {
    freeScratchBuffer(reply);
    freeScratchBuffer(json);
    return httpd_resp_send_500(req);
  }
  json[offset++] = '}';
  json[offset] = 0;

  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t rc = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
  freeScratchBuffer(reply);
  freeScratchBuffer(json);
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

#endif
