#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <cstring>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>

#include "wifi_config.h"

namespace {

constexpr uint8_t MOTOR_A_IN1_PIN = D6;
constexpr uint8_t MOTOR_A_IN2_PIN = D5;
constexpr uint8_t MOTOR_B_IN1_PIN = D0;
constexpr uint8_t MOTOR_B_IN2_PIN = D7;
constexpr uint8_t SLOT_COUNT = 3;
constexpr uint8_t TANK_COUNT = 2;
constexpr uint16_t MIN_RUN_MS = 100;
constexpr uint16_t MAX_RUN_MS = 10000;
constexpr uint8_t MIN_MOTOR_SPEED_PERCENT = 20;
constexpr uint8_t MAX_MOTOR_SPEED_PERCENT = 100;
constexpr uint16_t EEPROM_SIZE = 128;
constexpr uint32_t SETTINGS_MAGIC = 0x46454544;
constexpr time_t MIN_VALID_EPOCH = 1704067200;
constexpr bool WIFI_ENABLED = true;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 15000;
constexpr uint32_t STATUS_INTERVAL_MS = 10000;
constexpr uint32_t SCHEDULE_CHECK_INTERVAL_MS = 1000;
constexpr char TIME_ZONE[] = "IST-5:30";

struct Settings {
  uint16_t slotMinute[SLOT_COUNT];
  uint8_t slotTankMask[SLOT_COUNT];
  uint16_t runDurationMs[TANK_COUNT];
  uint8_t motorSpeedPercent[TANK_COUNT];
};

struct MotorState {
  bool running;
  uint32_t stopAtMs;
};

ESP8266WebServer server(80);
RTC_DS3231 rtc;
Settings settings = {};
MotorState motors[TANK_COUNT] = {};
bool wifiConfigured = false;
bool timeConfigured = false;
bool rtcFound = false;
bool rtcReady = false;
bool rtcSyncedFromNtp = false;
bool eepromReady = false;
bool wasWiFiConnected = false;
uint32_t lastWiFiAttemptMs = 0;
uint32_t lastStatusMs = 0;
uint32_t lastScheduleCheckMs = 0;
uint32_t lastDayKey = 0;
uint8_t fedMask = 0;
volatile uint32_t wifiDisconnectCount = 0;

bool isWiFiConnected() {
  return WIFI_ENABLED && WiFi.status() == WL_CONNECTED;
}

struct StoredState {
  uint32_t magic;
  Settings settings;
  uint32_t dayKey;
  uint8_t fedMask;
  uint8_t padding[3];
  uint32_t checksum;
};

const char PAGE_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#07131f">
<title>Bluefin | Fish Feeder</title>
<style>
*{box-sizing:border-box}
:root{color-scheme:dark;--bg:#07131f;--panel:#0d1d2c;--panel2:#112538;--line:#1f3a4d;--text:#eaf5fb;--muted:#94adbb;--cyan:#48d6d1;--blue:#48a9ff;--green:#99efb3;--red:#ff8c8c}
body{margin:0;min-height:100vh;background:radial-gradient(ellipse at 12% 0%,#123b4c 0,transparent 36%),radial-gradient(ellipse at 100% 30%,#102b48 0,transparent 34%),var(--bg);color:var(--text);font:15px/1.5 Inter,ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}
main{width:min(100% - 32px,1040px);margin:0 auto;padding:34px 0 48px}
.topbar{display:flex;align-items:center;justify-content:space-between;gap:16px;margin-bottom:26px}
.brand{display:flex;align-items:center;gap:12px}.brandmark{display:grid;place-items:center;width:44px;height:44px;border-radius:15px;background:linear-gradient(145deg,#51ddd3,#328df4);box-shadow:0 8px 28px #1faec533;color:#062131;font-size:23px}
.eyebrow{margin:0;color:var(--cyan);font-size:11px;font-weight:800;letter-spacing:.18em;text-transform:uppercase}
h1{margin:0;font-size:24px;letter-spacing:-.04em}.topnote{color:var(--muted);font-size:13px}
.hero{display:flex;justify-content:space-between;align-items:end;gap:20px;margin:0 0 20px}.hero h2{margin:3px 0 0;font-size:clamp(27px,5vw,39px);line-height:1.12;letter-spacing:-.05em}.hero p{margin:8px 0 0;color:var(--muted)}
.status-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:22px 0}
.stat,.card{border:1px solid var(--line);background:linear-gradient(145deg,#102438ed,#0a1927f2);border-radius:18px;box-shadow:0 14px 40px #020a1180}
.stat{padding:15px 17px;min-width:0}.stat-label{color:var(--muted);font-size:12px}.stat-value{margin-top:5px;font-size:15px;font-weight:700;overflow-wrap:anywhere}.live{color:var(--green)}.offline{color:var(--red)}
.layout{display:grid;grid-template-columns:1fr 1fr;gap:16px}.card{padding:21px}
.card-head{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;margin-bottom:17px}
h3{margin:0;font-size:18px;letter-spacing:-.02em}.sub{margin:4px 0 0;color:var(--muted);font-size:13px}
.feed-grid{display:grid;grid-template-columns:1fr 1fr;gap:11px}.tank{padding:15px;border:1px solid var(--line);border-radius:14px;background:#0a1926}.tank-title{font-size:13px;color:var(--muted)}.tank-state{margin:4px 0 14px;font-size:17px;font-weight:750}
button{font:inherit;border:0;cursor:pointer;transition:transform .15s,filter .15s}button:hover{filter:brightness(1.09);transform:translateY(-1px)}button:active{transform:translateY(0)}
.feed-button,.save-button{width:100%;padding:11px 14px;border-radius:11px;background:linear-gradient(100deg,#54d8d0,#64b4ff);color:#062131;font-weight:800}
.both-button{width:100%;margin-top:10px;padding:10px;border:1px solid #31516a;border-radius:11px;background:#112b40;color:var(--text);font-weight:700}
.notice{min-height:22px;margin:10px 0 0;color:var(--green);font-size:13px}
.speed{margin-top:17px;padding-top:16px;border-top:1px solid var(--line)}.speed-head{display:flex;justify-content:space-between;gap:10px;align-items:center;font-size:13px}.speed-value{color:var(--cyan);font-weight:800;font-variant-numeric:tabular-nums}
input[type=range]{width:100%;margin:12px 0 3px;accent-color:var(--cyan)}.range-ends{display:flex;justify-content:space-between;color:var(--muted);font-size:11px}
.slots{display:grid;gap:10px}.slot{display:grid;grid-template-columns:75px minmax(100px,1fr) auto;gap:10px;align-items:center;padding:12px;border:1px solid var(--line);border-radius:13px;background:#0a1926}.slot-name{font-size:13px;font-weight:750}
input[type=time],input[type=number]{min-width:0;padding:9px 10px;border:1px solid #29465b;border-radius:9px;background:#071522;color:var(--text);font:inherit}
.checks{display:flex;gap:9px;align-items:center;color:var(--muted);font-size:12px}.checks label{display:flex;gap:4px;align-items:center;white-space:nowrap}input[type=checkbox]{accent-color:var(--cyan)}
.duration-grid{display:grid;grid-template-columns:1fr 1fr;gap:11px;margin-top:16px}.field label{display:block;margin-bottom:6px;color:var(--muted);font-size:12px}.field input{width:100%}
.save-row{display:flex;align-items:center;gap:14px;margin-top:16px}.save-row .save-button{width:auto;min-width:175px}.save-note{margin:0;color:var(--green);font-size:13px}
.footer{margin:18px 3px 0;color:#7792a2;font-size:12px}
@media(max-width:700px){main{width:min(100% - 22px,560px);padding-top:22px}.layout{grid-template-columns:1fr}.status-grid{grid-template-columns:1fr 1fr}.topnote{display:none}.card{padding:17px}}
@media(max-width:420px){.slot{grid-template-columns:58px 1fr}.checks{grid-column:2;justify-content:flex-start}.feed-grid{gap:8px}.tank{padding:12px}}
</style></head>
<body><main>
<header class="topbar"><div class="brand"><div class="brandmark">≈</div><div><p class="eyebrow">Bluefin control</p><h1>Fish feeder</h1></div></div><div class="topnote">Smart feeding, right on schedule</div></header>
<section class="hero"><div><p class="eyebrow">Your aquarium at a glance</p><h2>Feeding dashboard</h2><p>Manage feed cycles, motor speed, and daily timing.</p></div></section>
<section class="status-grid">
<div class="stat"><div class="stat-label">Wi-Fi</div><div class="stat-value" id="wifi">Connecting…</div></div>
<div class="stat"><div class="stat-label">Feeder address</div><div class="stat-value" id="ip">—</div></div>
<div class="stat"><div class="stat-label">Clock · India</div><div class="stat-value" id="clock">Waiting…</div></div>
<div class="stat"><div class="stat-label">Uptime</div><div class="stat-value" id="uptime">—</div></div>
</section>
<div class="layout">
<section class="card"><div class="card-head"><div><h3>Manual feed</h3><p class="sub">Start a feed cycle whenever you need.</p></div></div>
<div class="feed-grid">
<div class="tank"><div class="tank-title">TANK 01</div><div class="tank-state" id="motor1">Ready</div><button class="feed-button" onclick="feed(1)">Feed tank 1</button></div>
<div class="tank"><div class="tank-title">TANK 02</div><div class="tank-state" id="motor2">Ready</div><button class="feed-button" onclick="feed(2)">Feed tank 2</button></div>
</div><button class="both-button" onclick="feed(3)">Feed both tanks</button>
<div id="notice" class="notice" aria-live="polite"></div>
<div class="speed"><div class="card-head"><div><h3>Motor speed</h3><p class="sub">PWM speed during each feed cycle.</p></div></div>
<div class="speed-head"><label for="speed1">Tank 1 speed</label><span class="speed-value"><span id="speed1Value">100</span>%</span></div><input id="speed1" type="range" min="20" max="100" value="100" oninput="showSpeed(1)">
<div class="speed-head"><label for="speed2">Tank 2 speed</label><span class="speed-value"><span id="speed2Value">100</span>%</span></div><input id="speed2" type="range" min="20" max="100" value="100" oninput="showSpeed(2)">
<div class="range-ends"><span>20% · slower</span><span>100% · full</span></div>
</div></section>
<section class="card"><div class="card-head"><div><h3>Daily schedule</h3><p class="sub">Choose a time and which tanks feed. Empty slots stay off.</p></div></div>
<div id="slots" class="slots"></div>
<div class="duration-grid"><div class="field"><label for="run1">Tank 1 run time (ms)</label><input id="run1" type="number" min="100" max="10000"></div><div class="field"><label for="run2">Tank 2 run time (ms)</label><input id="run2" type="number" min="100" max="10000"></div></div>
<div class="save-row"><button class="save-button" onclick="saveSettings()">Save settings</button><p id="saveNotice" class="save-note" aria-live="polite"></p></div>
</section></div>
<p class="footer">Use only on a trusted local network. Keep the motor supply disconnected while checking wiring and direction.</p>
</main>
<script>
const $=id=>document.getElementById(id);
function slotHtml(i,time,mask){return `<div class="slot"><div class="slot-name">Slot 0${i+1}</div><input type="time" id="time${i}" value="${time}"><div class="checks"><label><input type="checkbox" id="tank1_${i}" ${(mask&1)?"checked":""}>T1</label><label><input type="checkbox" id="tank2_${i}" ${(mask&2)?"checked":""}>T2</label></div></div>`}
function showSpeed(tank){$('speed'+tank+'Value').textContent=$('speed'+tank).value}
async function refresh(){try{const r=await fetch('/api/status');const d=await r.json();$('wifi').textContent=d.wifi==='connected'?'Connected':d.wifi;$('wifi').className='stat-value '+(d.wifi==='connected'?'live':'offline');$('ip').textContent=d.ip;$('clock').textContent=d.time;$('uptime').textContent=`${d.uptime}s · ${d.clock}`;$('motor1').textContent=d.motor1;$('motor2').textContent=d.motor2;if(!$('time0')){d.slots.forEach((s,i)=>$('slots').insertAdjacentHTML('beforeend',slotHtml(i,s.time,s.mask)));$('run1').value=d.run1;$('run2').value=d.run2;$('speed1').value=d.speed1;$('speed2').value=d.speed2;showSpeed(1);showSpeed(2)}}catch(e){$('wifi').textContent='Unavailable';$('wifi').className='stat-value offline';$('clock').textContent='Cannot reach feeder'}}
async function feed(tank){$('notice').textContent='Starting feed…';try{const r=await fetch('/api/feed',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:`tank=${tank}`});const d=await r.json();$('notice').textContent=d.message}catch(e){$('notice').textContent='Feed request failed; check the feeder connection.'}}
async function saveSettings(){const p=new URLSearchParams();for(let i=0;i<3;i++){p.set(`time${i}`, $('time'+i).value);p.set(`mask${i}`,($(`tank1_${i}`).checked?1:0)+($(`tank2_${i}`).checked?2:0))}p.set('run1',$('run1').value);p.set('run2',$('run2').value);p.set('speed1',$('speed1').value);p.set('speed2',$('speed2').value);$('saveNotice').textContent='Saving…';try{const r=await fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:p});const d=await r.json();$('saveNotice').textContent=d.message;if(r.ok){$('speed1').value=p.get('speed1');$('speed2').value=p.get('speed2');showSpeed(1);showSpeed(2)}}catch(e){$('saveNotice').textContent='Save failed; check the feeder connection.'}}
refresh();setInterval(refresh,3000);
</script></body></html>
)rawliteral";

bool validScheduleSettings(const Settings& value) {
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    if (value.slotMinute[i] >= 24 * 60 ||
        (value.slotTankMask[i] & ~0x03U) != 0) {
      return false;
    }
  }
  for (uint8_t i = 0; i < TANK_COUNT; ++i) {
    if (value.runDurationMs[i] < MIN_RUN_MS ||
        value.runDurationMs[i] > MAX_RUN_MS) {
      return false;
    }
  }
  return true;
}

bool validSettings(const Settings& value) {
  if (!validScheduleSettings(value)) {
    return false;
  }
  for (uint8_t i = 0; i < TANK_COUNT; ++i) {
    if (value.motorSpeedPercent[i] < MIN_MOTOR_SPEED_PERCENT ||
        value.motorSpeedPercent[i] > MAX_MOTOR_SPEED_PERCENT) {
      return false;
    }
  }
  return true;
}

uint32_t stateChecksum(const Settings& value, uint32_t dayKey, uint8_t mask,
                       bool includeMotorSpeed) {
  uint32_t checksum =
      SETTINGS_MAGIC ^ value.slotMinute[0] ^
      (static_cast<uint32_t>(value.slotMinute[1]) << 1) ^
      (static_cast<uint32_t>(value.slotMinute[2]) << 2) ^
      (static_cast<uint32_t>(value.slotTankMask[0]) << 3) ^
      (static_cast<uint32_t>(value.slotTankMask[1]) << 4) ^
      (static_cast<uint32_t>(value.slotTankMask[2]) << 5) ^
      (static_cast<uint32_t>(value.runDurationMs[0]) << 6) ^
      (static_cast<uint32_t>(value.runDurationMs[1]) << 7) ^ dayKey ^ mask;
  if (includeMotorSpeed) {
    checksum ^= static_cast<uint32_t>(value.motorSpeedPercent[0]) << 8;
    checksum ^= static_cast<uint32_t>(value.motorSpeedPercent[1]) << 9;
  }
  return checksum;
}

bool savePersistentState();

void loadSettings() {
  settings.slotMinute[0] = 8 * 60;
  settings.slotMinute[1] = 15 * 60;
  settings.slotMinute[2] = 20 * 60;
  settings.runDurationMs[0] = 500;
  settings.runDurationMs[1] = 500;
  settings.motorSpeedPercent[0] = MAX_MOTOR_SPEED_PERCENT;
  settings.motorSpeedPercent[1] = MAX_MOTOR_SPEED_PERCENT;
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    settings.slotTankMask[i] = 0;
  }

  EEPROM.begin(EEPROM_SIZE);
  eepromReady = true;
  StoredState stored = {};
  EEPROM.get(0, stored);
  const bool validHeader = stored.magic == SETTINGS_MAGIC &&
                           validScheduleSettings(stored.settings);
  const bool hasCurrentSettings =
      validHeader && validSettings(stored.settings) &&
      stored.checksum ==
          stateChecksum(stored.settings, stored.dayKey, stored.fedMask, true);
  const bool hasLegacySettings =
      validHeader &&
      stored.checksum ==
          stateChecksum(stored.settings, stored.dayKey, stored.fedMask, false);
  if (hasCurrentSettings) {
    settings = stored.settings;
    lastDayKey = stored.dayKey;
    fedMask = stored.fedMask;
  } else if (hasLegacySettings) {
    settings = stored.settings;
    settings.motorSpeedPercent[0] = MAX_MOTOR_SPEED_PERCENT;
    settings.motorSpeedPercent[1] = MAX_MOTOR_SPEED_PERCENT;
    lastDayKey = stored.dayKey;
    fedMask = stored.fedMask;
    if (!savePersistentState()) {
      Serial.println("ERROR: Could not migrate saved settings for motor speed.");
    }
  }
}

bool savePersistentState() {
  if (!eepromReady) {
    return false;
  }
  StoredState stored = {};
  stored.magic = SETTINGS_MAGIC;
  stored.settings = settings;
  stored.dayKey = lastDayKey;
  stored.fedMask = fedMask;
  stored.checksum =
      stateChecksum(stored.settings, stored.dayKey, stored.fedMask, true);
  EEPROM.put(0, stored);
  return EEPROM.commit();
}

void stopMotor(uint8_t tank) {
  if (tank == 0) {
    analogWrite(MOTOR_A_IN1_PIN, 0);
    digitalWrite(MOTOR_A_IN1_PIN, LOW);
    digitalWrite(MOTOR_A_IN2_PIN, LOW);
  } else {
    analogWrite(MOTOR_B_IN1_PIN, 0);
    digitalWrite(MOTOR_B_IN1_PIN, LOW);
    digitalWrite(MOTOR_B_IN2_PIN, LOW);
  }
  motors[tank].running = false;
}

void startMotor(uint8_t tank) {
  if (tank >= TANK_COUNT || motors[tank].running) {
    return;
  }
  const uint8_t inputPin = tank == 0 ? MOTOR_A_IN1_PIN : MOTOR_B_IN1_PIN;
  const uint8_t otherInputPin =
      tank == 0 ? MOTOR_A_IN2_PIN : MOTOR_B_IN2_PIN;
  const uint16_t duty =
      static_cast<uint16_t>(settings.motorSpeedPercent[tank]) * 1023U / 100U;
  analogWrite(inputPin, duty);
  digitalWrite(otherInputPin, LOW);
  motors[tank].running = true;
  motors[tank].stopAtMs = millis() + settings.runDurationMs[tank];
  Serial.printf("Tank %u motor started at %u%% for %u ms.\n",
                static_cast<unsigned>(tank + 1),
                static_cast<unsigned>(settings.motorSpeedPercent[tank]),
                static_cast<unsigned>(settings.runDurationMs[tank]));
}

bool getLocalTime(struct tm& localTime) {
  if (rtcReady) {
    const DateTime now = rtc.now();
    if (now.year() < 2024) {
      return false;
    }
    localTime = {};
    localTime.tm_year = now.year() - 1900;
    localTime.tm_mon = now.month() - 1;
    localTime.tm_mday = now.day();
    localTime.tm_hour = now.hour();
    localTime.tm_min = now.minute();
    localTime.tm_sec = now.second();
    return true;
  }

  const time_t now = time(nullptr);
  return now >= MIN_VALID_EPOCH && localtime_r(&now, &localTime) != nullptr;
}

bool parseTime(const String& text, uint16_t& minute) {
  if (text.length() != 5 || text[2] != ':' ||
      !isDigit(text[0]) || !isDigit(text[1]) ||
      !isDigit(text[3]) || !isDigit(text[4])) {
    return false;
  }
  const uint8_t hour =
      static_cast<uint8_t>((text[0] - '0') * 10 + text[1] - '0');
  const uint8_t mins =
      static_cast<uint8_t>((text[3] - '0') * 10 + text[4] - '0');
  if (hour > 23 || mins > 59) {
    return false;
  }
  minute = static_cast<uint16_t>(hour * 60 + mins);
  return true;
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

void handleStatus() {
  struct tm localTime = {};
  char timeText[24] = "waiting for RTC/NTP";
  const bool hasTime = getLocalTime(localTime);
  if (hasTime &&
      strftime(timeText, sizeof(timeText), "%Y-%m-%d %H:%M:%S",
               &localTime) == 0) {
    snprintf(timeText, sizeof(timeText), "time unavailable");
  }
  const char* clockSource =
      rtcReady ? "DS3231" : (hasTime ? "NTP (no RTC)" : "waiting");

  char json[512];
  snprintf(json, sizeof(json),
           "{\"wifi\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"uptime\":%lu,"
           "\"disconnects\":%lu,\"clock\":\"%s\",\"time\":\"%s\",\"motor1\":\"%s\","
           "\"motor2\":\"%s\",\"run1\":%u,\"run2\":%u,"
           "\"speed1\":%u,\"speed2\":%u,\"slots\":["
           "{\"time\":\"%02u:%02u\",\"mask\":%u},"
           "{\"time\":\"%02u:%02u\",\"mask\":%u},"
           "{\"time\":\"%02u:%02u\",\"mask\":%u}]}",
           WIFI_ENABLED ? (isWiFiConnected() ? "connected" : "disconnected")
                        : "disabled",
           WIFI_ENABLED ? WiFi.localIP().toString().c_str() : "0.0.0.0",
           WIFI_ENABLED ? WiFi.RSSI() : 0,
           static_cast<unsigned long>(millis() / 1000UL),
           static_cast<unsigned long>(wifiDisconnectCount), clockSource,
           timeText,
           motors[0].running ? "running" : "ready",
           motors[1].running ? "running" : "ready",
           static_cast<unsigned>(settings.runDurationMs[0]),
           static_cast<unsigned>(settings.runDurationMs[1]),
           static_cast<unsigned>(settings.motorSpeedPercent[0]),
           static_cast<unsigned>(settings.motorSpeedPercent[1]),
           settings.slotMinute[0] / 60, settings.slotMinute[0] % 60,
           static_cast<unsigned>(settings.slotTankMask[0]),
           settings.slotMinute[1] / 60, settings.slotMinute[1] % 60,
           static_cast<unsigned>(settings.slotTankMask[1]),
           settings.slotMinute[2] / 60, settings.slotMinute[2] % 60,
           static_cast<unsigned>(settings.slotTankMask[2]));
  server.send(200, "application/json", json);
}

void handleFeed() {
  if (!server.hasArg("tank")) {
    server.send(400, "application/json", "{\"message\":\"Missing tank selection.\"}");
    return;
  }
  const String tankArg = server.arg("tank");
  uint8_t mask = 0;
  if (tankArg == "1") {
    mask = 0x01;
  } else if (tankArg == "2") {
    mask = 0x02;
  } else if (tankArg == "3") {
    mask = 0x03;
  } else {
    server.send(400, "application/json", "{\"message\":\"Invalid tank selection.\"}");
    return;
  }
  for (uint8_t tank = 0; tank < TANK_COUNT; ++tank) {
    if ((mask & (1U << tank)) != 0 && motors[tank].running) {
      server.send(409, "application/json", "{\"message\":\"A selected motor is already running.\"}");
      return;
    }
  }
  for (uint8_t tank = 0; tank < TANK_COUNT; ++tank) {
    if ((mask & (1U << tank)) != 0) {
      startMotor(tank);
    }
  }
  server.send(202, "application/json", "{\"message\":\"Feed started.\"}");
}

void handleSettings() {
  Settings updated = settings;
  for (uint8_t i = 0; i < SLOT_COUNT; ++i) {
    char timeKey[8];
    char maskKey[8];
    snprintf(timeKey, sizeof(timeKey), "time%u", i);
    snprintf(maskKey, sizeof(maskKey), "mask%u", i);
    if (!server.hasArg(timeKey) || !server.hasArg(maskKey) ||
        !parseTime(server.arg(timeKey), updated.slotMinute[i])) {
      server.send(400, "application/json", "{\"message\":\"Invalid schedule time.\"}");
      return;
    }
    const long tankMask = server.arg(maskKey).toInt();
    if (tankMask < 0 || tankMask > 3) {
      server.send(400, "application/json", "{\"message\":\"Invalid tank assignment.\"}");
      return;
    }
    updated.slotTankMask[i] = static_cast<uint8_t>(tankMask);
  }

  const long run1 = server.arg("run1").toInt();
  const long run2 = server.arg("run2").toInt();
  const long speed1 = server.arg("speed1").toInt();
  const long speed2 = server.arg("speed2").toInt();
  if (run1 < MIN_RUN_MS || run1 > MAX_RUN_MS ||
      run2 < MIN_RUN_MS || run2 > MAX_RUN_MS) {
    server.send(400, "application/json", "{\"message\":\"Run duration must be 100–10000 ms.\"}");
    return;
  }
  if (!server.hasArg("speed1") || !server.hasArg("speed2") ||
      speed1 < MIN_MOTOR_SPEED_PERCENT ||
      speed1 > MAX_MOTOR_SPEED_PERCENT ||
      speed2 < MIN_MOTOR_SPEED_PERCENT ||
      speed2 > MAX_MOTOR_SPEED_PERCENT) {
    server.send(400, "application/json",
                "{\"message\":\"Motor speed must be 20–100%.\"}");
    return;
  }
  updated.runDurationMs[0] = static_cast<uint16_t>(run1);
  updated.runDurationMs[1] = static_cast<uint16_t>(run2);
  updated.motorSpeedPercent[0] = static_cast<uint8_t>(speed1);
  updated.motorSpeedPercent[1] = static_cast<uint8_t>(speed2);
  if (!validSettings(updated) || !eepromReady) {
    server.send(500, "application/json", "{\"message\":\"Could not save settings.\"}");
    return;
  }

  const Settings previous = settings;
  settings = updated;
  if (!savePersistentState()) {
    settings = previous;
    server.send(500, "application/json", "{\"message\":\"Settings write failed.\"}");
    return;
  }
  server.send(200, "application/json", "{\"message\":\"Schedule saved.\"}");
}

void serviceSchedule() {
  const uint32_t nowMs = millis();
  if (nowMs - lastScheduleCheckMs < SCHEDULE_CHECK_INTERVAL_MS) {
    return;
  }
  lastScheduleCheckMs = nowMs;

  struct tm localTime = {};
  if (!getLocalTime(localTime)) {
    return;
  }

  const uint32_t dayKey =
      static_cast<uint32_t>((localTime.tm_year + 1900) * 10000 +
                            (localTime.tm_mon + 1) * 100 + localTime.tm_mday);
  if (dayKey != lastDayKey) {
    lastDayKey = dayKey;
    fedMask = 0;
    if (!savePersistentState()) {
      Serial.println("ERROR: Could not save new daily schedule state.");
    }
  }

  const uint16_t currentMinute =
      static_cast<uint16_t>(localTime.tm_hour * 60 + localTime.tm_min);
  for (uint8_t slot = 0; slot < SLOT_COUNT; ++slot) {
    const uint8_t selected = settings.slotTankMask[slot];
    for (uint8_t tank = 0; tank < TANK_COUNT; ++tank) {
      const uint8_t bit = 1U << (slot * TANK_COUNT + tank);
      if (settings.slotMinute[slot] != currentMinute ||
          (selected & (1U << tank)) == 0 || (fedMask & bit) != 0 ||
          motors[tank].running) {
        continue;
      }
      fedMask |= bit;
      if (!savePersistentState()) {
        fedMask &= static_cast<uint8_t>(~bit);
        Serial.println("Scheduled feed skipped: could not save completion state.");
        continue;
      }
      Serial.printf("Scheduled slot %u started for Tank %u.\n",
                    static_cast<unsigned>(slot + 1),
                    static_cast<unsigned>(tank + 1));
      startMotor(tank);
    }
  }
}

void startWiFi() {
  if (strlen(WIFI_SSID) == 0 || strlen(WIFI_PASSWORD) == 0 ||
      strcmp(WIFI_SSID, "YOUR_WIFI_SSID") == 0 ||
      strcmp(WIFI_PASSWORD, "YOUR_WIFI_PASSWORD") == 0) {
    Serial.println("ERROR: Set Wi-Fi credentials in include/wifi_config.h.");
    return;
  }

  wifiConfigured = true;
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptMs = millis();
  Serial.printf("Connecting to Wi-Fi network: %s\n", WIFI_SSID);
  Serial.println("IP configuration: DHCP (router assigned).");
}

void serviceWiFiAndNtp() {
  if (!WIFI_ENABLED || !wifiConfigured) {
    return;
  }
  const uint32_t nowMs = millis();
  const bool connected = isWiFiConnected();
  if (connected) {
    if (!timeConfigured) {
      setenv("TZ", TIME_ZONE, 1);
      tzset();
      configTime(TIME_ZONE, "pool.ntp.org", "time.google.com");
      timeConfigured = true;
      Serial.printf("Wi-Fi connected; open http://%s/\n",
                    WiFi.localIP().toString().c_str());
      Serial.println("NTP time synchronization started.");
    }
    if (rtcFound && !rtcSyncedFromNtp) {
      const time_t now = time(nullptr);
      if (now >= MIN_VALID_EPOCH) {
        struct tm localTime = {};
        if (localtime_r(&now, &localTime) != nullptr) {
          rtc.adjust(DateTime(localTime.tm_year + 1900, localTime.tm_mon + 1,
                              localTime.tm_mday, localTime.tm_hour,
                              localTime.tm_min, localTime.tm_sec));
          rtcReady = true;
          rtcSyncedFromNtp = true;
          Serial.println("DS3231 synchronized from NTP (India local time).");
        }
      }
    }
    wasWiFiConnected = true;
    return;
  }

  timeConfigured = false;
  if (wasWiFiConnected) {
    wasWiFiConnected = false;
    ++wifiDisconnectCount;
    Serial.printf("Wi-Fi disconnected; disconnect count %lu.\n",
                  static_cast<unsigned long>(wifiDisconnectCount));
  }
  if (nowMs - lastWiFiAttemptMs >= WIFI_RETRY_INTERVAL_MS) {
    lastWiFiAttemptMs = nowMs;
    Serial.printf("Wi-Fi disconnected (status %d); retrying.\n",
                  static_cast<int>(WiFi.status()));
    WiFi.reconnect();
  }
}

void serviceMotors() {
  const uint32_t now = millis();
  for (uint8_t tank = 0; tank < TANK_COUNT; ++tank) {
    if (motors[tank].running &&
        static_cast<int32_t>(now - motors[tank].stopAtMs) >= 0) {
      stopMotor(tank);
      Serial.printf("Tank %u motor stopped.\n",
                    static_cast<unsigned>(tank + 1));
    }
  }
}

void printStatus() {
  const uint32_t nowMs = millis();
  if (nowMs - lastStatusMs < STATUS_INTERVAL_MS) {
    return;
  }
  lastStatusMs = nowMs;
  Serial.printf("STATUS | uptime %lu s | Wi-Fi %s | disconnects %lu",
                static_cast<unsigned long>(nowMs / 1000UL),
                !WIFI_ENABLED ? "disabled"
                              : (isWiFiConnected() ? "connected"
                                                   : "disconnected"),
                static_cast<unsigned long>(wifiDisconnectCount));
  if (isWiFiConnected()) {
    Serial.printf(" | DHCP IP %s | RSSI %d dBm",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  struct tm localTime = {};
  if (getLocalTime(localTime)) {
    Serial.printf(" | India time %04d-%02d-%02d %02d:%02d:%02d | clock %s",
                  localTime.tm_year + 1900, localTime.tm_mon + 1,
                  localTime.tm_mday, localTime.tm_hour, localTime.tm_min,
                  localTime.tm_sec, rtcReady ? "DS3231" : "NTP");
  } else {
    Serial.print(" | waiting for RTC/NTP time");
  }
  Serial.println();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("NodeMCU feeder starting; reset reason: %s\n",
                ESP.getResetReason().c_str());
  Serial.println("DRV8833 pins: IN1 D6, IN2 D5, IN3 D0, IN4 D7.");

  pinMode(MOTOR_A_IN1_PIN, OUTPUT);
  pinMode(MOTOR_A_IN2_PIN, OUTPUT);
  pinMode(MOTOR_B_IN1_PIN, OUTPUT);
  pinMode(MOTOR_B_IN2_PIN, OUTPUT);
  analogWriteRange(1023);
  analogWriteFreq(1000);
  digitalWrite(MOTOR_A_IN1_PIN, LOW);
  digitalWrite(MOTOR_A_IN2_PIN, LOW);
  digitalWrite(MOTOR_B_IN1_PIN, LOW);
  digitalWrite(MOTOR_B_IN2_PIN, LOW);

  setenv("TZ", TIME_ZONE, 1);
  tzset();
  Wire.begin(D2, D1);
  rtcFound = rtc.begin();
  if (!rtcFound) {
    Serial.println("ERROR: DS3231 not detected; will use NTP time when available.");
  } else if (rtc.lostPower()) {
    Serial.println("DS3231 lost power; waiting for NTP to set its clock.");
  } else {
    const DateTime rtcTime = rtc.now();
    if (rtcTime.year() >= 2024) {
      rtcReady = true;
      Serial.println("DS3231 clock is valid; schedules can run without Wi-Fi.");
    } else {
      Serial.println("DS3231 time is not set; waiting for NTP.");
    }
  }
  loadSettings();

  if (WIFI_ENABLED) {
    startWiFi();
  } else {
    WiFi.persistent(false);
    WiFi.setAutoConnect(false);
    WiFi.setAutoReconnect(false);
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi disabled for DS3231 offline test.");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/feed", HTTP_POST, handleFeed);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  Serial.println("HTTP feeder control server started.");
}

void loop() {
  serviceWiFiAndNtp();
  if (isWiFiConnected()) {
    server.handleClient();
  }
  serviceMotors();
  serviceSchedule();
  printStatus();
  delay(5);
}
