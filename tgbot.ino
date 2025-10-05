#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <ESP8266WebServer.h>
#include <FS.h>

#include "UniversalTelegramBot.h"
#include "ArduinoJson.h"
#include <U8g2lib.h>
#include <Wire.h>

// ------------------- Пины -------------------
#define LED_PIN    14        // Светодиод (D5 на NodeMCU)
#define BTN_RESET  0         // Кнопка сброса (D3 = GPIO0)
#define BTN_NEXT   2         // Кнопка "Следующая страница" (D4 = GPIO2)

// ------------------- Глобальные переменные -------------------
ESP8266WebServer server(80);
ESP8266WiFiMulti wifiMulti;
WiFiClientSecure secured_client;

String WIFI_SSID;
String WIFI_PASS;
String BOT_TOKEN;
String ALLOWED_CHATS;        // строка вида "123,456,789"

UniversalTelegramBot* bot = nullptr;
unsigned long bot_lasttime = 0;
const unsigned long BOT_MTBS = 1000; // интервал опроса бота

// OLED 128x32 I2C
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// Корневой сертификат Telegram
X509List cert(TELEGRAM_CERTIFICATE_ROOT);

// ------------------- Хранение сообщений -------------------
String currentMessage = "";
int currentOffset = 0;
const int MAX_LINES_PER_PAGE = 3;
const int LINE_HEIGHT = 9;

// ------------------- Кнопка с антидребезгом -------------------
const unsigned long DEBOUNCE_DELAY = 2000; // мс
unsigned long lastBtnNextPress = 0;
bool btnNextPrevState = HIGH;

// ------------------- HTML -------------------
const char* index_html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Настройка ESP8266</title>
  <style>
    :root { --bg:#f0f4f8; --card:#fff; --accent:#007BFF; --text:#333; }
    * { box-sizing: border-box; }
    body { margin:0; background:var(--bg); font-family:system-ui, -apple-system, Segoe UI, Roboto, Arial, sans-serif; display:flex; align-items:center; justify-content:center; min-height:100vh; }
    .card { width:min(480px, 92vw); background:var(--card); padding:28px; border-radius:16px; box-shadow:0 10px 30px rgba(0,0,0,.12); }
    h1 { font-size:20px; margin:0 0 14px; color:var(--text); text-align:center; }
    p.hint { margin:0 0 18px; color:#666; font-size:14px; text-align:center; }
    label { display:block; margin:12px 0 6px; font-weight:600; color:#444; }
    input[type="text"], input[type="password"] { width:100%; padding:12px 14px; border:1px solid #d0d7de; border-radius:10px; font-size:14px; outline:none; }
    .row { display:flex; gap:10px; }
    .row > * { flex:1; }
    .chat-list { margin-top:8px; }
    .chat-field { position:relative; margin-bottom:8px; }
    .remove { position:absolute; right:6px; top:6px; border:none; background:#eee; padding:6px 8px; border-radius:8px; cursor:pointer; }
    .actions { display:flex; gap:10px; margin-top:18px; }
    .btn { flex:1; border:none; padding:12px; border-radius:12px; font-weight:600; cursor:pointer; }
    .btn-primary { background:var(--accent); color:#fff; }
    .btn-ghost { background:#eef3ff; color:#184fdb; }
    .note { margin-top:8px; font-size:12px; color:#666; text-align:center; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Настройка TelegramPager</h1>
    <p class="hint">Введите Wi-Fi, токен Telegram и один или несколько Chat ID</p>
    <form method="POST" action="/save">
      <label for="ssid">Wi-Fi SSID</label>
      <input type="text" id="ssid" name="ssid" placeholder="Введите SSID" required>

      <label for="password">Пароль Wi-Fi</label>
      <input type="password" id="password" name="password" placeholder="Введите пароль" required>

      <label for="token">Telegram Token</label>
      <input type="text" id="token" name="token" placeholder="Например: 123456:ABC-DEF..." required>

      <label>Telegram Chat ID</label>
      <div id="chat-container" class="chat-list">
        <div class="chat-field">
          <input type="text" name="chatid[]" placeholder="Введите Chat ID">
          <button type="button" class="remove" onclick="removeField(this)">✕</button>
        </div>
      </div>
      <div class="actions">
        <button type="button" class="btn btn-ghost" onclick="addChat()">➕ Добавить ещё Chat ID</button>
        <button type="submit" class="btn btn-primary">💾 Сохранить</button>
      </div>
      <div class="note">После сохранения устройство перезапустится автоматически</div>
    </form>
  </div>

<script>
function addChat(value="") {
  const container = document.getElementById('chat-container');
  const wrap = document.createElement('div');
  wrap.className = 'chat-field';
  wrap.innerHTML = '<input type="text" name="chatid[]" placeholder="Введите Chat ID" value="'+String(value).replace(/"/g,'&quot;')+'">'+
                   '<button type="button" class="remove" onclick="removeField(this)">✕</button>';
  container.appendChild(wrap);
}
function removeField(btn){
  const field = btn.parentElement;
  const container = document.getElementById('chat-container');
  if (container.children.length > 1) container.removeChild(field);
}
</script>
</body>
</html>
)rawliteral";

// ------------------- Функции -------------------
void blinkLed(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(delayMs);
    digitalWrite(LED_PIN, LOW);  delay(delayMs);
  }
}

bool isAllowed(const String& id) {
  String needle = "," + id + ",";
  String hay = "," + ALLOWED_CHATS + ",";
  return hay.indexOf(needle) >= 0;
}

// ------------------- SPIFFS -------------------
bool loadConfig() {
  if (!SPIFFS.begin()) return false;
  if (!SPIFFS.exists("/config.txt")) return false;
  File f = SPIFFS.open("/config.txt", "r");
  if (!f) return false;

  WIFI_SSID     = f.readStringUntil('\n'); WIFI_SSID.trim();
  WIFI_PASS     = f.readStringUntil('\n'); WIFI_PASS.trim();
  BOT_TOKEN     = f.readStringUntil('\n'); BOT_TOKEN.trim();
  ALLOWED_CHATS = f.readStringUntil('\n'); ALLOWED_CHATS.trim();
  f.close();
  return (WIFI_SSID.length() && BOT_TOKEN.length());
}

void saveConfig(const String& ssid, const String& pass, const String& token, const String& csvChats) {
  File f = SPIFFS.open("/config.txt", "w");
  if (!f) return;
  f.println(ssid);
  f.println(pass);
  f.println(token);
  f.println(csvChats);
  f.close();
}

void resetConfigFile() {
  if (SPIFFS.begin()) {
    if (SPIFFS.exists("/config.txt")) SPIFFS.remove("/config.txt");
  }
}

// ------------------- OLED прокрутка -------------------
void displayPage(const String& text, int startLine) {
  u8g2.clearBuffer();
  int lineCount = 0;
  int y = LINE_HEIGHT;
  String line = "";
  int lineIndex = 0;

  for (int i = 0; i < text.length(); ) {
    uint8_t c = text[i];
    int charLen = 1;
    if ((c & 0xE0) == 0xC0) charLen = 2;
    else if ((c & 0xF0) == 0xE0) charLen = 3;
    else if ((c & 0xF8) == 0xF0) charLen = 4;

    String nextChar = text.substring(i, i + charLen);
    String testLine = line + nextChar;

    if (nextChar == "\n" || u8g2.getUTF8Width(testLine.c_str()) > 128) {
      if (lineIndex >= startLine && lineCount < MAX_LINES_PER_PAGE) {
        u8g2.drawUTF8(0, y, line.c_str());
        y += LINE_HEIGHT;
        lineCount++;
      }
      line = (nextChar == "\n") ? "" : nextChar;
      lineIndex++;
      if (lineCount >= MAX_LINES_PER_PAGE) break;
    } else {
      line = testLine;
    }
    i += charLen;
  }

  if (lineIndex >= startLine && lineCount < MAX_LINES_PER_PAGE && line.length() > 0) {
    u8g2.drawUTF8(0, y, line.c_str());
  }

  u8g2.sendBuffer();
}

// ------------------- Telegram -------------------
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String fromId = bot->messages[i].from_id;
    if (!isAllowed(fromId)) continue;
    currentMessage = bot->messages[i].text;
    currentOffset = 0;
    displayPage(currentMessage, currentOffset);
    blinkLed(5, 120);
  }
}

// ------------------- WebServer -------------------
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", index_html);
}

void handleSave() {
  String csv;
  for (uint8_t i = 0; i < server.args(); i++) {
    if (server.argName(i) == "chatid[]") {
      String v = server.arg(i); v.trim();
      if (v.length()) {
        if (csv.length()) csv += ",";
        csv += v;
      }
    }
  }

  saveConfig(server.arg("ssid"), server.arg("password"), server.arg("token"), csv);

  server.send(200, "text/html; charset=utf-8",
    "<!DOCTYPE html><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='3'>"
    "<style>body{font-family:Arial;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;background:#f0f4f8}"
    ".box{background:#fff;padding:24px 28px;border-radius:14px;box-shadow:0 10px 30px rgba(0,0,0,.12);text-align:center}</style>"
    "<div class='box'><h2>✅ Настройки сохранены</h2><p>TelegramPager перезагрузится автоматически…</p></div>"
  );

  delay(1000);
  ESP.restart();
}

// ------------------- Setup -------------------
void enterConfigMode(const String& msg) {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("TelegramPager");
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("AP IP: " + WiFi.softAPIP().toString());
  displayPage(msg + "\nAP: TelegramPager", 0);
}

void setup() {
  Serial.begin(9600);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BTN_RESET, INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);

  u8g2.begin();
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);

  if (digitalRead(BTN_RESET) == LOW) {
    resetConfigFile();
    enterConfigMode("Config удалён.\nНастройте заново.");
    return;
  }

  if (!loadConfig()) {
    enterConfigMode("Нет конфигурации.\nНастройте Wi-Fi.");
    return;
  }

  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID.c_str(), WIFI_PASS.c_str());
  Serial.print("Подключение к Wi-Fi");
  unsigned long t0 = millis();
  while (wifiMulti.run() != WL_CONNECTED) {
    delay(400); Serial.print(".");
    if (millis() - t0 > 20000) break;
  }

  if (WiFi.status() != WL_CONNECTED) {
    enterConfigMode("Wi-Fi ошибка.\nНастройте заново.");
    return;
  }

  Serial.println();
  Serial.println("Wi-Fi OK: " + WiFi.localIP().toString());

  secured_client.setTrustAnchors(&cert);
  bot = new UniversalTelegramBot(BOT_TOKEN, secured_client);
  //bot->last_message_received = 0;

  configTime(0, 0, "pool.ntp.org");
  time_t now = time(nullptr);
  int tries = 0;
  while (now < 24 * 3600 && tries < 50) { delay(150); now = time(nullptr); tries++; }
  Serial.println("Время синхронизировано");
  displayPage("Готово.\nОжидаю\nсообщения…", 0);
}

// ------------------- Loop -------------------
void loop() {
  if (bot == nullptr || WIFI_SSID.length() == 0) {
    server.handleClient();
  } else {
    if (millis() - bot_lasttime > BOT_MTBS) {
      int numNewMessages = bot->getUpdates(bot->last_message_received + 1);
      while (numNewMessages) {
        handleNewMessages(numNewMessages);
        numNewMessages = bot->getUpdates(bot->last_message_received + 1);
      }
      bot_lasttime = millis();
    }
  }

  // Кнопка сброса
  if (digitalRead(BTN_RESET) == LOW) {
    delay(50);
    if (digitalRead(BTN_RESET) == LOW) {
      displayPage("Сброс настроек…", 0);
      resetConfigFile();
      delay(600);
      ESP.restart();
    }
  }

  // Кнопка "Следующая страница" с антидребезгом
  bool btnState = digitalRead(BTN_NEXT);
  if (btnState != btnNextPrevState) {
    lastBtnNextPress = millis();
  }
  if (btnState == LOW && (millis() - lastBtnNextPress) > DEBOUNCE_DELAY) {
    lastBtnNextPress = millis();

    // Подсчёт строк
    int totalLines = 0;
    for (int i = 0; i < currentMessage.length(); ) {
      uint8_t c = currentMessage[i];
      int charLen = 1;
      if ((c & 0xE0) == 0xC0) charLen = 2;
      else if ((c & 0xF0) == 0xE0) charLen = 3;
      else if ((c & 0xF8) == 0xF0) charLen = 4;
      String nextChar = currentMessage.substring(i, i + charLen);
      String line = "";
      while (i < currentMessage.length() && nextChar != "\n" && u8g2.getUTF8Width((line + nextChar).c_str()) <= 128) {
        line += nextChar;
        i += charLen;
        if (i < currentMessage.length()) {
          c = currentMessage[i];
          charLen = 1;
          if ((c & 0xE0) == 0xC0) charLen = 2;
          else if ((c & 0xF0) == 0xE0) charLen = 3;
          else if ((c & 0xF8) == 0xF0) charLen = 4;
          nextChar = currentMessage.substring(i, i + charLen);
        }
      }
      totalLines++;
      if (i < currentMessage.length() && currentMessage[i] == '\n') i++;
    }

    currentOffset += MAX_LINES_PER_PAGE;
    if (currentOffset >= totalLines) currentOffset = 0;

    displayPage(currentMessage, currentOffset);
  }
  btnNextPrevState = btnState;
}
