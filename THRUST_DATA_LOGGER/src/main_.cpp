// =============================================================================
// ESP32 Thrust-Data Logger — WiFi WebSocket Edition
// ---------------------------------------------------------------------------
// Hardware:
//   • ESP32 DevKit
//   • HX711 load-cell amplifier  (DOUT → GPIO 27, SCK → GPIO 26)
//   • Calibrated 40 kg load cell
//   • SD card module over SPI    (CS→5, SCK→18, MISO→19, MOSI→23)
//
// Dependencies (install via PlatformIO / Arduino Library Manager):
//   • bogde/HX711
//   • me-no-dev/ESPAsyncWebServer
//   • me-no-dev/AsyncTCP
//   • ArduinoJson  (bblanchon/ArduinoJson  v7.x)
//
// Access dashboard:
//   AP mode  → connect phone to "ThrustLogger" (pass: thrustlab)
//              then open  http://192.168.4.1
//   STA mode → set WIFI_SSID / WIFI_PASS below, board joins your router,
//              check Serial for IP then open that IP in browser.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <HX711.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// =============================================================================
// WiFi — choose ONE mode
// =============================================================================
// --- AP (Access Point) mode — phone connects directly to ESP32 ---
// #define WIFI_MODE_AP          // comment this out to use STA mode
// static const char* AP_SSID   = "ThrustLogger";
// static const char* AP_PASS   = "thrustlab";   // min 8 chars, or "" for open

// --- STA (Station) mode — ESP32 joins your home/lab router ---
#define WIFI_MODE_STA
static const char* WIFI_SSID = "Soham 1";
static const char* WIFI_PASS = "123456789";

// =============================================================================
// Pin definitions
// =============================================================================
static constexpr uint8_t HX711_DOUT_PIN = 27;
static constexpr uint8_t HX711_SCK_PIN  = 26;

static constexpr uint8_t SD_CS_PIN   = 5;
static constexpr uint8_t SD_SCK_PIN  = 18;
static constexpr uint8_t SD_MISO_PIN = 19;
static constexpr uint8_t SD_MOSI_PIN = 23;

// =============================================================================
// Calibration
// =============================================================================
static constexpr float CALIBRATION_SIGN   =  1.0f;
static constexpr float CALIBRATION_FACTOR =  CALIBRATION_SIGN * 68342.55f;
static constexpr float GRAVITY            =  9.80665f;

// =============================================================================
// Timing
// =============================================================================
static constexpr unsigned long SAMPLE_INTERVAL_MS = 20;    // 50 Hz
static constexpr unsigned long LOG_DURATION_MS    = 20000; // auto-stop 20 s
static constexpr unsigned long FLUSH_INTERVAL_MS  = 500;
static constexpr unsigned long SETTLE_TIME_MS     = 5000;
static constexpr unsigned long WS_BROADCAST_MS    = 20;    // 50 Hz to browser

// =============================================================================
// Globals
// =============================================================================
HX711             scale;
AsyncWebServer    server(80);
AsyncWebSocket    ws("/ws");
File              logFile;

bool     isLogging    = false;
bool     sdReady      = false;
uint16_t fileIndex    = 0;

unsigned long logStartMs    = 0;
unsigned long lastSampleMs  = 0;
unsigned long lastFlushMs   = 0;
unsigned long lastWsBcastMs = 0;

long  tareOffset  = 0;
float scaleFactor = CALIBRATION_FACTOR;

// Ring buffer for CSV download (stores last 10 s @ 100 Hz = 1000 rows)
static constexpr uint16_t CSV_RING_SIZE = 1200;
struct CsvRow { unsigned long ts; long raw; float kg; float grams; float forceN; };
CsvRow csvRing[CSV_RING_SIZE];
uint16_t csvHead = 0;
uint16_t csvCount = 0;

// =============================================================================
// Forward declarations
// =============================================================================
void   doTare();
void   startLogging();
void   stopLogging();
String nextFileName();
void   broadcastSample(unsigned long ts, long raw, float kg, float grams, float forceN);
void   handleWsMessage(AsyncWebSocket* server, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len);
void   sendCsvDownload(AsyncWebServerRequest* request);
String buildDashboardHTML();

// =============================================================================
// setup()
// =============================================================================
void setup()
{
    Serial.begin(115200);
    delay(800);
    Serial.println(F("\n[BOOT] ESP32 Thrust Logger — WiFi Edition"));

    // --- HX711 ---------------------------------------------------------------
    Serial.println(F("[HX711] Init ..."));
    scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);

    unsigned long t0 = millis();
    while (!scale.is_ready() && millis() - t0 < 10000) delay(100);
    Serial.println(scale.is_ready() ? F("[HX711] Ready.") : F("[HX711] TIMEOUT!"));

    scale.set_scale(scaleFactor);

    Serial.printf("[HX711] Settling %lu s ...\n", SETTLE_TIME_MS / 1000UL);
    delay(SETTLE_TIME_MS);
    doTare();

    // --- SD card -------------------------------------------------------------
    Serial.println(F("[SD] Init ..."));
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    sdReady = SD.begin(SD_CS_PIN, SPI);
    Serial.println(sdReady ? F("[SD] OK.") : F("[SD] FAILED (continuing without SD)."));

    if (sdReady) {
        while (fileIndex < 999 && SD.exists(nextFileName().c_str())) fileIndex++;
    }

    // --- WiFi ----------------------------------------------------------------
#ifdef WIFI_MODE_AP
    WiFi.disconnect(true);
    delay(100);
    WiFi.softAP(AP_SSID, AP_PASS[0] ? AP_PASS : nullptr);
    Serial.printf("[WiFi] AP  SSID: %s   IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
    Serial.printf("[WiFi] Open browser → http://%s\n",
                  WiFi.softAPIP().toString().c_str());
#else
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print(F("[WiFi] Connecting"));
    while (WiFi.status() != WL_CONNECTED) { Serial.print('.'); delay(500); }
    Serial.printf("\n[WiFi] Connected.  IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] Open browser → http://%s\n", WiFi.localIP().toString().c_str());
#endif

    // --- WebSocket handler ---------------------------------------------------
    ws.onEvent(handleWsMessage);
    server.addHandler(&ws);

    // --- HTTP routes ---------------------------------------------------------
    // Root — serve dashboard HTML
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html", buildDashboardHTML());
    });

    // CSV download endpoint
    server.on("/download", HTTP_GET, sendCsvDownload);

    // Status endpoint (for health checks)
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["logging"]  = isLogging;
        doc["sdReady"]  = sdReady;
        doc["tare"]     = tareOffset;
        doc["scale"]    = scaleFactor;
        String out;
        serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.begin();
    Serial.println(F("[HTTP] Server started."));
    Serial.println(F("\nReady. Open the dashboard URL on your phone or laptop."));
}

// =============================================================================
// loop()
// =============================================================================
void loop()
{
    ws.cleanupClients();

    unsigned long now = millis();

    if (now - lastSampleMs >= SAMPLE_INTERVAL_MS) {
        lastSampleMs = now;

        if (!scale.is_ready()) return;

        long  rawValue = scale.read();
        float kg       = scale.get_units(1);
        float grams    = kg * 1000.0f;
        float forceN   = kg * GRAVITY;

        // Store into ring buffer (always, even if not "logging")
        csvRing[csvHead] = { now, rawValue, kg, grams, forceN };
        csvHead = (csvHead + 1) % CSV_RING_SIZE;
        if (csvCount < CSV_RING_SIZE) csvCount++;

        // Broadcast to WebSocket clients (throttled to 50 Hz)
        if (now - lastWsBcastMs >= WS_BROADCAST_MS) {
            lastWsBcastMs = now;
            broadcastSample(now, rawValue, kg, grams, forceN);
        }

        // Write to SD if logging
        if (isLogging && logFile) {
            logFile.printf("%lu,%ld,%.4f,%.2f,%.4f\n",
                           now, rawValue, kg, grams, forceN);
        }
    }

    // Auto-stop logging after LOG_DURATION_MS
    if (isLogging && (now - logStartMs >= LOG_DURATION_MS)) {
        stopLogging();
    }

    // Flush SD periodically
    if (isLogging && logFile && (now - lastFlushMs >= FLUSH_INTERVAL_MS)) {
        lastFlushMs = now;
        logFile.flush();
    }
}

// =============================================================================
// broadcastSample() — send JSON over WebSocket to all clients
// =============================================================================
void broadcastSample(unsigned long ts, long raw, float kg, float grams, float forceN)
{
    if (ws.count() == 0) return;

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"t\":%lu,\"r\":%ld,\"k\":%.4f,\"g\":%.1f,\"n\":%.4f,\"l\":%d}",
             ts, raw, kg, grams, forceN, isLogging ? 1 : 0);
    ws.textAll(buf);
}

// =============================================================================
// handleWsMessage() — receive commands from dashboard
// =============================================================================
void handleWsMessage(AsyncWebSocket* svr, AsyncWebSocketClient* client,
                     AwsEventType type, void* arg, uint8_t* data, size_t len)
{
    if (type != WS_EVT_DATA) return;

    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index != 0 || info->len != len) return;  // multi-frame guard
    if (info->opcode != WS_TEXT) return;

    String msg = String((char*)data).substring(0, len);

    if (msg == "tare")  { doTare();      client->text("{\"ack\":\"tared\"}"); }
    else if (msg == "start") { startLogging(); client->text("{\"ack\":\"started\"}"); }
    else if (msg == "stop")  { stopLogging();  client->text("{\"ack\":\"stopped\"}"); }
    else if (msg == "ping")  { client->text("{\"ack\":\"pong\"}"); }
}

// =============================================================================
// doTare()
// =============================================================================
void doTare()
{
    Serial.println(F("[TARE] Taring (20 samples)..."));
    scale.tare(20);
    tareOffset = scale.get_offset();
    Serial.printf("[TARE] Offset: %ld | Verify: %.4f kg\n",
                  tareOffset, scale.get_units(5));

    // Notify connected WS clients
    ws.textAll("{\"event\":\"tared\"}");
}

// =============================================================================
// startLogging() / stopLogging()
// =============================================================================
void startLogging()
{
    if (isLogging) return;

    if (sdReady) {
        String fname = nextFileName();
        logFile = SD.open(fname.c_str(), FILE_WRITE);
        if (logFile) {
            logFile.println(F("time_ms,raw,kg,grams,force_N"));
            logFile.flush();
            Serial.printf("[LOG] Started → %s\n", fname.c_str());
            fileIndex++;
        } else {
            Serial.println(F("[LOG] Failed to open file on SD!"));
        }
    } else {
        Serial.println(F("[LOG] No SD — logging to RAM ring buffer only."));
    }

    // Reset ring buffer for a clean recording session
    csvHead  = 0;
    csvCount = 0;

    isLogging  = true;
    logStartMs = millis();
    lastFlushMs = logStartMs;

    ws.textAll("{\"event\":\"started\"}");
}

void stopLogging()
{
    if (logFile) { logFile.flush(); logFile.close(); }
    isLogging = false;
    Serial.println(F("[LOG] Stopped."));
    ws.textAll("{\"event\":\"stopped\"}");
}

// =============================================================================
// nextFileName()
// =============================================================================
String nextFileName()
{
    char buf[20];
    snprintf(buf, sizeof(buf), "/test%03u.csv", fileIndex);
    return String(buf);
}

// =============================================================================
// sendCsvDownload() — serve ring buffer as CSV file download
// =============================================================================
void sendCsvDownload(AsyncWebServerRequest* request)
{
    // Build CSV string from ring buffer
    String csv = "time_ms,raw,kg,grams,force_N\n";
    uint16_t start = (csvCount < CSV_RING_SIZE) ? 0 : csvHead;
    for (uint16_t i = 0; i < csvCount; i++) {
        uint16_t idx = (start + i) % CSV_RING_SIZE;
        char row[80];
        snprintf(row, sizeof(row), "%lu,%ld,%.4f,%.2f,%.4f\n",
                 csvRing[idx].ts, csvRing[idx].raw,
                 csvRing[idx].kg, csvRing[idx].grams, csvRing[idx].forceN);
        csv += row;
    }

    AsyncWebServerResponse* response =
        request->beginResponse(200, "text/csv", csv);
    response->addHeader("Content-Disposition",
                        "attachment; filename=\"thrust_data.csv\"");
    request->send(response);
}

// =============================================================================
// buildDashboardHTML() — returns the full single-page dashboard
// =============================================================================
String buildDashboardHTML()
{
    // The full HTML/CSS/JS dashboard is embedded as a raw string.
    // Kept in a separate file in your repo for readability;
    // here it is inlined for single-file deployment.
    static const char HTML[] PROGMEM = R"HTMLEOF(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0">
<title>Thrust Logger</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.2/dist/chart.umd.min.js"></script>
<style>
  :root {
    --bg: #0a0c10; --panel: #12161e; --border: #1e2530;
    --accent: #00e5ff; --accent2: #ff4b6e; --accent3: #39ff6a;
    --text: #e8eaf0; --muted: #5a6278;
    --font: 'Courier New', monospace;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { background: var(--bg); color: var(--text); font-family: var(--font);
         min-height: 100vh; padding: 12px; }
  h1 { font-size: 1.1rem; letter-spacing: 0.2em; color: var(--accent);
       text-transform: uppercase; margin-bottom: 14px; text-align: center; }
  h1 span { color: var(--muted); font-size: 0.7rem; display: block; margin-top: 2px; }

  .status-bar { display: flex; gap: 8px; justify-content: center;
                margin-bottom: 14px; flex-wrap: wrap; }
  .pill { padding: 3px 12px; border-radius: 20px; font-size: 0.7rem;
          letter-spacing: 0.1em; border: 1px solid var(--border); }
  .pill.green  { border-color: var(--accent3); color: var(--accent3); }
  .pill.red    { border-color: var(--accent2); color: var(--accent2); }
  .pill.cyan   { border-color: var(--accent);  color: var(--accent);  }
  .pill.muted  { border-color: var(--muted);   color: var(--muted);  }

  .metrics { display: grid; grid-template-columns: repeat(3,1fr);
             gap: 8px; margin-bottom: 14px; }
  .metric { background: var(--panel); border: 1px solid var(--border);
            border-radius: 8px; padding: 10px 8px; text-align: center; }
  .metric .val { font-size: 1.5rem; font-weight: bold; color: var(--accent); }
  .metric .val.force { color: var(--accent2); }
  .metric .val.gram  { color: var(--accent3); }
  .metric .lbl { font-size: 0.6rem; color: var(--muted);
                 letter-spacing: 0.1em; text-transform: uppercase; margin-top: 3px; }

  .chart-wrap { background: var(--panel); border: 1px solid var(--border);
                border-radius: 8px; padding: 10px; margin-bottom: 14px; }
  .chart-wrap canvas { max-height: 220px; }

  .controls { display: grid; grid-template-columns: 1fr 1fr; gap: 8px;
              margin-bottom: 14px; }
  button { padding: 14px 8px; border-radius: 8px; border: 1px solid;
           font-family: var(--font); font-size: 0.8rem; letter-spacing: 0.1em;
           text-transform: uppercase; cursor: pointer; transition: all 0.15s;
           background: transparent; }
  .btn-tare  { border-color: var(--accent);  color: var(--accent); }
  .btn-start { border-color: var(--accent3); color: var(--accent3); }
  .btn-stop  { border-color: var(--accent2); color: var(--accent2); }
  .btn-dl    { border-color: var(--muted);   color: var(--muted); grid-column: span 2; }
  button:active { opacity: 0.6; transform: scale(0.97); }
  button:disabled { opacity: 0.3; cursor: not-allowed; }

  .log { background: var(--panel); border: 1px solid var(--border);
         border-radius: 8px; padding: 8px; max-height: 110px;
         overflow-y: auto; font-size: 0.68rem; color: var(--muted); }
  .log div { padding: 1px 0; border-bottom: 1px solid var(--border); }
  .log div:last-child { border: none; }
  .log .ev { color: var(--accent); }
  .log .warn { color: var(--accent2); }

  @media (min-width: 600px) {
    body { max-width: 700px; margin: 0 auto; padding: 20px; }
    .metrics { grid-template-columns: repeat(3,1fr); }
    .controls { grid-template-columns: repeat(4,1fr); }
    .btn-dl { grid-column: span 4; }
  }
</style>
</head>
<body>
<h1>⚡ THRUST LOGGER <span id="ip-label"></span></h1>

<div class="status-bar">
  <div class="pill muted" id="pill-ws">WS: CONNECTING</div>
  <div class="pill muted" id="pill-sd">SD: —</div>
  <div class="pill muted" id="pill-rec">● IDLE</div>
  <div class="pill cyan"  id="pill-hz">— Hz</div>
</div>

<div class="metrics">
  <div class="metric"><div class="val" id="val-kg">—</div><div class="lbl">kg</div></div>
  <div class="metric"><div class="val gram" id="val-g">—</div><div class="lbl">grams</div></div>
  <div class="metric"><div class="val force" id="val-n">—</div><div class="lbl">force N</div></div>
</div>

<div class="chart-wrap">
  <canvas id="chart"></canvas>
</div>

<div class="controls">
  <button class="btn-tare"  onclick="sendCmd('tare')">Tare</button>
  <button class="btn-start" id="btn-start" onclick="sendCmd('start')">▶ Start</button>
  <button class="btn-stop"  id="btn-stop"  onclick="sendCmd('stop')" disabled>■ Stop</button>
  <button class="btn-dl"    onclick="downloadCSV()">⬇ Download CSV</button>
</div>

<div class="log" id="log"><div>Initialising...</div></div>

<script>
const MAX_PTS = 300; // last 6 seconds shown at 50 Hz
let ws, reconnTimer;
let lastTs = 0, hz = 0, hzSamples = 0, hzTimer;

// Chart setup
const ctx = document.getElementById('chart').getContext('2d');
const chart = new Chart(ctx, {
  type: 'line',
  data: {
    labels: [],
    datasets: [
      { label: 'kg',      data: [], borderColor: '#00e5ff', borderWidth: 1.5,
        pointRadius: 0, tension: 0.3, yAxisID: 'y' },
      { label: 'Force N', data: [], borderColor: '#ff4b6e', borderWidth: 1.5,
        pointRadius: 0, tension: 0.3, yAxisID: 'y2' }
    ]
  },
  options: {
    animation: false,
    responsive: true,
    maintainAspectRatio: true,
    interaction: { mode: 'index', intersect: false },
    plugins: { legend: { labels: { color: '#5a6278', font: { size: 10 } } } },
    scales: {
      x:  { ticks: { color: '#5a6278', maxTicksLimit: 6, font: { size: 9 } },
             grid: { color: '#1e2530' } },
      y:  { ticks: { color: '#00e5ff', font: { size: 9 } },
             grid: { color: '#1e2530' }, position: 'left', title: { display: true, text: 'kg', color:'#00e5ff', font:{size:9} } },
      y2: { ticks: { color: '#ff4b6e', font: { size: 9 } },
             grid: { drawOnChartArea: false }, position: 'right',
             title: { display: true, text: 'N', color:'#ff4b6e', font:{size:9} } }
    }
  }
});

function addPoint(t, kg, n) {
  const label = (t / 1000).toFixed(2);
  chart.data.labels.push(label);
  chart.data.datasets[0].data.push(kg);
  chart.data.datasets[1].data.push(n);
  if (chart.data.labels.length > MAX_PTS) {
    chart.data.labels.shift();
    chart.data.datasets[0].data.shift();
    chart.data.datasets[1].data.shift();
  }
  chart.update('none');
}

function log(msg, cls) {
  const el = document.getElementById('log');
  const d = document.createElement('div');
  d.textContent = new Date().toISOString().slice(11,19) + ' ' + msg;
  if (cls) d.className = cls;
  el.prepend(d);
  while (el.children.length > 60) el.removeChild(el.lastChild);
}

function connect() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(proto + '://' + location.host + '/ws');

  ws.onopen = () => {
    document.getElementById('pill-ws').textContent = 'WS: LIVE';
    document.getElementById('pill-ws').className = 'pill green';
    log('WebSocket connected', 'ev');
    clearTimeout(reconnTimer);
    // Kick-off Hz meter
    hzTimer = setInterval(() => {
      document.getElementById('pill-hz').textContent = hz.toFixed(0) + ' Hz';
      hz = 0;
    }, 1000);
  };

  ws.onclose = () => {
    document.getElementById('pill-ws').textContent = 'WS: OFF';
    document.getElementById('pill-ws').className = 'pill red';
    clearInterval(hzTimer);
    reconnTimer = setTimeout(connect, 2000);
  };

  ws.onmessage = (e) => {
    try {
      const d = JSON.parse(e.data);

      // Sample data packet
      if ('k' in d) {
        hz++;
        document.getElementById('val-kg').textContent = d.k.toFixed(3);
        document.getElementById('val-g').textContent  = d.g.toFixed(1);
        document.getElementById('val-n').textContent  = d.n.toFixed(3);
        addPoint(d.t, d.k, d.n);
        // Recording indicator
        const pill = document.getElementById('pill-rec');
        if (d.l) { pill.textContent = '● REC'; pill.className = 'pill red'; }
        else     { pill.textContent = '● IDLE'; pill.className = 'pill muted'; }
      }
      // Event packets
      if (d.event === 'tared')   { log('Scale tared', 'ev'); }
      if (d.event === 'started') {
        log('Recording started', 'ev');
        document.getElementById('btn-start').disabled = true;
        document.getElementById('btn-stop').disabled  = false;
      }
      if (d.event === 'stopped') {
        log('Recording stopped', 'ev');
        document.getElementById('btn-start').disabled = false;
        document.getElementById('btn-stop').disabled  = true;
      }
      if (d.ack) log('ACK: ' + d.ack);

    } catch(err) { log('Parse error: ' + err, 'warn'); }
  };
}

function sendCmd(cmd) {
  if (!ws || ws.readyState !== WebSocket.OPEN) {
    log('Not connected!', 'warn'); return;
  }
  ws.send(cmd);
  log('CMD → ' + cmd);
}

function downloadCSV() {
  log('Downloading CSV...', 'ev');
  const a = document.createElement('a');
  a.href = '/download';
  a.download = 'thrust_data.csv';
  a.click();
}

// Fetch initial status
fetch('/status').then(r => r.json()).then(s => {
  document.getElementById('pill-sd').textContent = 'SD: ' + (s.sdReady ? 'OK' : 'NO');
  document.getElementById('pill-sd').className   = 'pill ' + (s.sdReady ? 'green' : 'red');
}).catch(() => {});

document.getElementById('ip-label').textContent = location.host;
connect();
</script>
</body>
</html>
)HTMLEOF";

    return String(HTML);
}