#include <WiFi.h>
#include <WebServer.h>
#include <SPI.h>

// Firebase
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// =====================
// WiFi config
// =====================
const char* WIFI_SSID = "MINH";
const char* WIFI_PASS = "minh2003";

// =====================
// Firebase config
// =====================
#define API_KEY       "AIzaSyBROuJFSiU3D3i2Viu8MqkK4mfAqCzYhy0"
#define DATABASE_URL  "https://bh1750-c60fb-default-rtdb.asia-southeast1.firebasedatabase.app"

static const char* NODE_PATH = "/nodes/NODE_01";   // giữ y như bạn đang dùng

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
static bool fbReady = false;

// =====================
// LoRa (SX127x) pins
// =====================
static const int PIN_SS  = 27;
static const int PIN_RST = 32;

// =====================
// SX127x regs
// =====================
static const uint8_t REG_FIFO               = 0x00;
static const uint8_t REG_OP_MODE            = 0x01;
static const uint8_t REG_FRF_MSB            = 0x06;
static const uint8_t REG_FRF_MID            = 0x07;
static const uint8_t REG_FRF_LSB            = 0x08;
static const uint8_t REG_PA_CONFIG          = 0x09;
static const uint8_t REG_FIFO_ADDR_PTR      = 0x0D;
static const uint8_t REG_FIFO_TX_BASE_ADDR  = 0x0E;
static const uint8_t REG_FIFO_RX_BASE_ADDR  = 0x0F;
static const uint8_t REG_FIFO_RX_CURRENT    = 0x10;
static const uint8_t REG_IRQ_FLAGS          = 0x12;
static const uint8_t REG_RX_NB_BYTES        = 0x13;
static const uint8_t REG_PKT_RSSI_VALUE     = 0x1A;
static const uint8_t REG_MODEM_CONFIG_1     = 0x1D;
static const uint8_t REG_MODEM_CONFIG_2     = 0x1E;
static const uint8_t REG_PREAMBLE_MSB       = 0x20;
static const uint8_t REG_PREAMBLE_LSB       = 0x21;
static const uint8_t REG_PAYLOAD_LENGTH     = 0x22;
static const uint8_t REG_MODEM_CONFIG_3     = 0x26;
static const uint8_t REG_SYNC_WORD          = 0x39;
static const uint8_t REG_VERSION            = 0x42;

static const uint8_t IRQ_RX_DONE = 0x40;
static const uint8_t IRQ_TX_DONE = 0x08;
static const uint8_t IRQ_CRC_ERR = 0x20;

static const uint8_t MODE_LONG_RANGE = 0x80;
static const uint8_t MODE_SLEEP      = 0x00;
static const uint8_t MODE_STDBY      = 0x01;
static const uint8_t MODE_TX         = 0x03;
static const uint8_t MODE_RX_CONT    = 0x05;

static inline void csLow()  { digitalWrite(PIN_SS, LOW); }
static inline void csHigh() { digitalWrite(PIN_SS, HIGH); }

uint8_t readReg(uint8_t addr) {
  csLow(); SPI.transfer(addr & 0x7F);
  uint8_t v = SPI.transfer(0x00);
  csHigh(); return v;
}
void writeReg(uint8_t addr, uint8_t val) {
  csLow(); SPI.transfer(addr | 0x80); SPI.transfer(val);
  csHigh();
}
void burstWriteFIFO(const uint8_t* data, int n) {
  csLow(); SPI.transfer(REG_FIFO | 0x80);
  for (int i=0;i<n;i++) SPI.transfer(data[i]);
  csHigh();
}
void burstReadFIFO(uint8_t* buf, int n) {
  csLow(); SPI.transfer(REG_FIFO & 0x7F);
  for (int i=0;i<n;i++) buf[i] = SPI.transfer(0x00);
  csHigh();
}

void resetRadio() {
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_RST, LOW); delay(10);
  digitalWrite(PIN_RST, HIGH); delay(10);
}

void setFrequency(long hz) {
  uint64_t frf = ((uint64_t)hz << 19) / 32000000ULL;
  writeReg(REG_FRF_MSB, (uint8_t)(frf >> 16));
  writeReg(REG_FRF_MID, (uint8_t)(frf >> 8));
  writeReg(REG_FRF_LSB, (uint8_t)(frf >> 0));
}

int rssiDbm() { return -157 + (int)readReg(REG_PKT_RSSI_VALUE); }

void radioEnterRx() {
  writeReg(REG_IRQ_FLAGS, 0xFF);
  writeReg(REG_OP_MODE, MODE_LONG_RANGE | MODE_RX_CONT);
}

void radioInit() {
  writeReg(REG_OP_MODE, MODE_LONG_RANGE | MODE_SLEEP); delay(5);
  writeReg(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY); delay(5);

  setFrequency(433000000);

  writeReg(REG_MODEM_CONFIG_1, 0x70 | 0x02);         // BW125 + CR4/5
  writeReg(REG_MODEM_CONFIG_2, (7 << 4) | 0x04);     // SF7 + CRC ON
  writeReg(REG_MODEM_CONFIG_3, 0x04);                // AGC auto

  writeReg(REG_PREAMBLE_MSB, 0x00);
  writeReg(REG_PREAMBLE_LSB, 0x08);
  writeReg(REG_SYNC_WORD, 0x12);

  writeReg(REG_PA_CONFIG, 0x80 | (14 - 2));          // 14 dBm

  writeReg(REG_FIFO_RX_BASE_ADDR, 0x00);
  writeReg(REG_FIFO_ADDR_PTR, 0x00);

  radioEnterRx();
}

// =====================
// NON-BLOCKING LoRa TX (FIX LAG)
// =====================
static bool     txBusy = false;
static uint32_t txStartMs = 0;
static String   txCur = "";
static const uint32_t TX_TIMEOUT_MS = 1200;
static const uint32_t TX_GAP_MS     = 70;   // gap giữa các lệnh
static uint32_t lastTxDoneMs = 0;

// Coalesce slots (giữ lệnh mới nhất, tránh spam khi kéo slider)
static String pendPower  = "";
static String pendMode   = "";
static String pendBri    = "";
static String pendRgb    = "";
static String pendPreset = "";
static String pendStatus = "";  // "CMD:STATUS?"

static inline bool anyPending() {
  return pendPower.length() || pendMode.length() || pendBri.length() ||
         pendRgb.length() || pendPreset.length() || pendStatus.length();
}

static void txStart(const String& cmd) {
  // chuẩn bị TX FIFO
  const char* s = cmd.c_str();
  int n = strlen(s);
  if (n <= 0) return;
  if (n > 240) n = 240; // an toàn

  writeReg(REG_OP_MODE, MODE_LONG_RANGE | MODE_STDBY);
  writeReg(REG_IRQ_FLAGS, 0xFF);

  writeReg(REG_FIFO_TX_BASE_ADDR, 0x00);
  writeReg(REG_FIFO_ADDR_PTR, 0x00);
  burstWriteFIFO((const uint8_t*)s, n);
  writeReg(REG_PAYLOAD_LENGTH, (uint8_t)n);

  // vào TX
  writeReg(REG_OP_MODE, MODE_LONG_RANGE | MODE_TX);

  txBusy = true;
  txStartMs = millis();
  txCur = cmd;
}

static void txPoll() {
  if (!txBusy) return;

  uint8_t irq = readReg(REG_IRQ_FLAGS);
  uint32_t now = millis();

  if (irq & IRQ_TX_DONE) {
    writeReg(REG_IRQ_FLAGS, 0xFF);
    radioEnterRx();
    txBusy = false;
    lastTxDoneMs = now;
    // Serial.printf("[TX_OK] %s\n", txCur.c_str());
    txCur = "";
    return;
  }

  if (now - txStartMs > TX_TIMEOUT_MS) {
    writeReg(REG_IRQ_FLAGS, 0xFF);
    radioEnterRx();
    txBusy = false;
    lastTxDoneMs = now;
    Serial.printf("[TX_TIMEOUT] %s\n", txCur.c_str());
    txCur = "";
    return;
  }
}

static void scheduleCmd(const String& cmd) {
  // phân loại lệnh để coalesce
  if (cmd.startsWith("CMD:POWER:"))  pendPower  = cmd;
  else if (cmd.startsWith("CMD:MODE:"))   pendMode   = cmd;
  else if (cmd.startsWith("CMD:BRI:"))    pendBri    = cmd;
  else if (cmd.startsWith("CMD:RGB:"))    pendRgb    = cmd;
  else if (cmd.startsWith("CMD:PRESET:")) pendPreset = cmd;
  else if (cmd.startsWith("CMD:STATUS?")) pendStatus = cmd;
}

static void txScheduler() {
  if (txBusy) return;

  uint32_t now = millis();
  if (now - lastTxDoneMs < TX_GAP_MS) return; // giữ nhịp

  // ưu tiên lệnh điều khiển trước, STATUS? sau
  String next = "";
  if (pendPower.length())  { next = pendPower;  pendPower = ""; }
  else if (pendMode.length())   { next = pendMode;   pendMode = ""; }
  else if (pendBri.length())    { next = pendBri;    pendBri = ""; }
  else if (pendRgb.length())    { next = pendRgb;    pendRgb = ""; }
  else if (pendPreset.length()) { next = pendPreset; pendPreset = ""; }
  else if (pendStatus.length()) { next = pendStatus; pendStatus = ""; }

  if (next.length()) txStart(next);
}

bool sendPacketQueued(const String& cmd) {
  // không block nữa: nếu rảnh thì start ngay, không thì queue/coalesce
  if (!txBusy && (millis() - lastTxDoneMs >= TX_GAP_MS)) {
    txStart(cmd);
    return true; // started
  }
  scheduleCmd(cmd);
  return false; // queued
}

// =====================
// Web server
// =====================
WebServer server(80);
String lastAck = "";

// =====================
// Local mirrored state
// =====================
String st_mode   = "MANUAL";
bool   st_power  = true;
int    st_bri    = 120;
int    st_r      = 255;
int    st_g      = 0;
int    st_b      = 0;
String st_preset = "STATIC";
float  st_lux    = NAN;
int    st_rssi   = 0;
int    st_wifi_rssi = 0;

// =====================
// Firebase push control
// =====================
uint32_t fb_last_push = 0;
static const uint32_t FB_PUSH_MIN_MS = 500;   // nới ra chút cho nhẹ
static bool fb_dirty = true;

uint32_t last_stat_req_ms = 0;
static const uint32_t STATUS_POLL_INTERVAL_MS = 1200;

static inline bool fbCan() { return fbReady && Firebase.ready(); }

void fbPushBaselineOnce() {
  if (!fbCan()) return;

  FirebaseJson j;
  j.set("mode", st_mode);
  j.set("power", st_power);
  j.set("bri", st_bri);
  j.set("r", st_r);
  j.set("g", st_g);
  j.set("b", st_b);
  j.set("preset", st_preset);
  j.set("lux", 0);
  j.set("rssi", 0);
  j.set("wifi_rssi", WiFi.RSSI());
  j.set("ts_ms", 0);
  j.set("last_ack", "");
  j.set("online", true);

  Firebase.RTDB.updateNode(&fbdo, NODE_PATH, &j);
}

void fbPushState(bool force=false) {
  if (!fbCan()) return;

  uint32_t now = millis();
  if (!force) {
    if (!fb_dirty) return;
    if (now - fb_last_push < FB_PUSH_MIN_MS) return;
  }

  st_wifi_rssi = WiFi.RSSI();

  FirebaseJson j;
  j.set("mode", st_mode);
  j.set("power", st_power);
  j.set("bri", st_bri);
  j.set("r", st_r);
  j.set("g", st_g);
  j.set("b", st_b);
  j.set("preset", st_preset);
  j.set("rssi", st_rssi);
  j.set("wifi_rssi", st_wifi_rssi);
  j.set("ts_ms", (int)now);
  j.set("last_ack", lastAck);
  j.set("online", true);
  if (!isnan(st_lux)) j.set("lux", st_lux);

  bool ok = Firebase.RTDB.updateNode(&fbdo, NODE_PATH, &j);
  if (!ok) {
    Serial.print("FB update FAIL: ");
    Serial.println(fbdo.errorReason());
  }

  fb_last_push = now;
  fb_dirty = false;
}

// =====================
// STAT parsing from Pico
// =====================
static bool parseCsvRGB(const String& s, int &r, int &g, int &b) {
  int p1 = s.indexOf(',');
  if (p1 < 0) return false;
  int p2 = s.indexOf(',', p1+1);
  if (p2 < 0) return false;
  r = s.substring(0, p1).toInt();
  g = s.substring(p1+1, p2).toInt();
  b = s.substring(p2+1).toInt();
  return true;
}

void parseStatLine(const String& stat) {
  int start = 0;
  while (start < (int)stat.length()) {
    int sp = stat.indexOf(' ', start);
    if (sp < 0) sp = stat.length();
    String tok = stat.substring(start, sp);
    start = sp + 1;
    tok.trim();
    if (!tok.length()) continue;

    int eq = tok.indexOf('=');
    if (eq < 0) continue;

    String k = tok.substring(0, eq);
    String v = tok.substring(eq+1);
    k.trim(); v.trim();

    if (k == "MODE") st_mode = v;
    else if (k == "POWER") st_power = (v == "ON");
    else if (k == "BRI") st_bri = v.toInt();
    else if (k == "RGB") { int rr,gg,bb; if (parseCsvRGB(v, rr, gg, bb)) { st_r=rr; st_g=gg; st_b=bb; } }
    else if (k == "PRESET") st_preset = v;
    else if (k == "LUX") { if (v == "NA") st_lux = NAN; else st_lux = v.toFloat(); }
  }
  fb_dirty = true;
}

// =====================
// RX polling (ACK/STAT)
// =====================
void pollRx() {
  uint8_t irq = readReg(REG_IRQ_FLAGS);
  if (!(irq & IRQ_RX_DONE)) return;

  bool crcErr = (irq & IRQ_CRC_ERR);
  uint8_t n = readReg(REG_RX_NB_BYTES);
  uint8_t addr = readReg(REG_FIFO_RX_CURRENT);
  writeReg(REG_IRQ_FLAGS, 0xFF);

  if (crcErr || n == 0 || n > 255) {
    Serial.println("RX_BAD(CRC/len)");
    return;
  }

  writeReg(REG_FIFO_ADDR_PTR, addr);
  uint8_t buf[256];
  burstReadFIFO(buf, n);
  buf[n] = 0;

  String s = (char*)buf;
  st_rssi = rssiDbm();

  // Serial.print("RX "); Serial.print(s);
  // Serial.print(" RSSI="); Serial.println(st_rssi);

  if (s.startsWith("ACK:")) {
    lastAck = s;
    fb_dirty = true;
  }
  if (s.startsWith("STAT:")) {
    String body = s.substring(5);
    body.trim();
    parseStatLine(body);
    fbPushState(true);
  }
}

// =====================
// HTTP helpers
// =====================
static inline void corsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,OPTIONS");
}

void replyText200(const String& s) {
  corsHeaders();
  server.send(200, "text/plain", s);
}

String replyFor(const String& cmd, bool started) {
  // started=đã bắt đầu TX ngay; false=queued
  String out = cmd + (started ? " (TX_START)" : " (QUEUED)");
  if (lastAck.length()) out += " | " + lastAck;
  return out;
}

void sendCmdHttpQueued(const String& cmd) {
  bool started = sendPacketQueued(cmd);
  replyText200(replyFor(cmd, started));
}

// =====================
// Routes
// =====================
const char HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>ESP32 API</title>
<style>
body{font-family:system-ui;background:#0b0f15;color:#e7eefc;margin:18px}
code{background:#111827;padding:2px 6px;border-radius:8px}
</style></head><body>
<h2>ESP32 API READY</h2>
<ul>
<li><code>/api/power?on=1|0</code></li>
<li><code>/api/mode?name=AUTO|MANUAL</code></li>
<li><code>/api/bri?v=0..255</code></li>
<li><code>/api/rgb?r=0..255&g=0..255&b=0..255</code></li>
<li><code>/api/preset?name=STATIC|NIGHT|MUSIC</code></li>
<li><code>/api/status_local</code></li>
<li><code>/api/health</code></li>
</ul>
<p>FIX: LoRa TX non-blocking (web không lag khi bấm nhiều).</p>
</body></html>
)HTML";

void setupRoutes() {
  server.on("/", [](){ server.send(200, "text/html", HTML); });

  server.onNotFound([](){ replyText200("404"); });

  server.on("/api/power", [](){
    String on = server.hasArg("on") ? server.arg("on") : "0";
    st_power = (on == "1");
    fb_dirty = true;
    String cmd = String("CMD:POWER:") + (st_power ? "ON" : "OFF");
    sendCmdHttpQueued(cmd);
    fbPushState();
  });

  server.on("/api/mode", [](){
    String name = server.hasArg("name") ? server.arg("name") : "MANUAL";
    name.toUpperCase();
    if (name != "AUTO" && name != "MANUAL") name = "MANUAL";
    st_mode = name;
    fb_dirty = true;
    String cmd = "CMD:MODE:" + name;
    sendCmdHttpQueued(cmd);
    fbPushState();
  });

  server.on("/api/bri", [](){
    int iv = server.hasArg("v") ? server.arg("v").toInt() : 120;
    if (iv < 0) iv = 0;
    if (iv > 255) iv = 255;
    st_bri = iv;
    fb_dirty = true;
    String cmd = "CMD:BRI:" + String(iv);
    sendCmdHttpQueued(cmd);
    fbPushState();
  });

  server.on("/api/rgb", [](){
    int r = server.hasArg("r") ? server.arg("r").toInt() : 0;
    int g = server.hasArg("g") ? server.arg("g").toInt() : 0;
    int b = server.hasArg("b") ? server.arg("b").toInt() : 0;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    st_r = r; st_g = g; st_b = b;
    fb_dirty = true;
    String cmd = "CMD:RGB:" + String(r) + "," + String(g) + "," + String(b);
    sendCmdHttpQueued(cmd);
    fbPushState();
  });

  server.on("/api/preset", [](){
    String name = server.hasArg("name") ? server.arg("name") : "STATIC";
    name.toUpperCase();
    if (name != "STATIC" && name != "NIGHT" && name != "MUSIC") name = "STATIC";
    st_preset = name;
    fb_dirty = true;
    String cmd = "CMD:PRESET:" + name;
    sendCmdHttpQueued(cmd);
    fbPushState();
  });

  server.on("/api/status_local", [](){
    String luxs = isnan(st_lux) ? "NA" : String(st_lux, 1);
    String out =
      "MODE=" + st_mode +
      " POWER=" + String(st_power ? "ON" : "OFF") +
      " BRI=" + String(st_bri) +
      " RGB=" + String(st_r) + "," + String(st_g) + "," + String(st_b) +
      " PRESET=" + st_preset +
      " LUX=" + luxs +
      " RSSI=" + String(st_rssi) +
      " WIFI_RSSI=" + String(WiFi.RSSI());
    replyText200(out);
  });

  server.on("/api/health", [](){
    String out =
      "wifi=" + String(WiFi.status() == WL_CONNECTED ? "OK" : "DOWN") +
      " ip=" + WiFi.localIP().toString() +
      " fbReady=" + String(fbReady ? "1" : "0") +
      " fbCan=" + String(fbCan() ? "1" : "0") +
      " txBusy=" + String(txBusy ? "1" : "0") +
      " pend=" + String(anyPending() ? "1" : "0");
    replyText200(out);
  });

  // preflight
  server.on("/api", HTTP_OPTIONS, [](){ corsHeaders(); server.send(204); });
}

// =====================
// Setup / Loop
// =====================
void setup() {
  Serial.begin(115200);
  delay(800);

  pinMode(PIN_SS, OUTPUT);
  csHigh();

  // SPI: SCK=18, MISO=19, MOSI=23, SS=PIN_SS
  SPI.begin(18, 19, 23, PIN_SS);
  resetRadio();

  Serial.print("REG_VERSION=0x");
  Serial.println(readReg(REG_VERSION), HEX);

  radioInit();
  Serial.println("LoRa OK.");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\nWiFi connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("WiFi RSSI: "); Serial.println(WiFi.RSSI());

  // Firebase init (anonymous)
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  if (Firebase.signUp(&config, &auth, "", "")) {
    Serial.println("Firebase signup OK (anonymous).");
    fbReady = true;
  } else {
    Serial.print("Firebase signup FAIL: ");
    Serial.println(config.signer.signupError.message.c_str());
    fbReady = false;
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  setupRoutes();
  server.begin();
  Serial.println("Web server started.");

  if (fbCan()) {
    fbPushBaselineOnce();
    fbPushState(true);
    Serial.println("Firebase baseline pushed.");
  } else {
    Serial.println("Firebase not ready yet (local control vẫn chạy bình thường).");
  }
}

void loop() {
  // 1) web luôn mượt: handleClient không bị block nữa
  server.handleClient();

  // 2) poll LoRa RX
  pollRx();

  // 3) non-blocking TX: poll done + schedule next
  txPoll();
  txScheduler();

  // 4) STATUS? chỉ gửi khi rảnh (không chen vào lúc bạn đang bấm)
  uint32_t now = millis();
  if (!txBusy && !anyPending() && (now - last_stat_req_ms >= STATUS_POLL_INTERVAL_MS)) {
    last_stat_req_ms = now;
    sendPacketQueued("CMD:STATUS?");
  }

  // 5) push Firebase nhẹ nhàng
  fbPushState(false);

  delay(1);
}
