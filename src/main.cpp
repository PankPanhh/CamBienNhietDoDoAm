#define MQTT_MAX_PACKET_SIZE 1024

#include "Arduino.h"
#include "DHT.h"
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ESP_Mail_Client.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// Khởi tạo màn hình LCD I2C
LiquidCrystal_I2C lcd(0x27, 16, 2);  // địa chỉ I2C và kích thước màn hình
int lcd_mode = 0; // 0: hiển thị nhiệt độ/độ ẩm, 1: hiển thị WiFi, 2: hiển thị trạng thái MQTT
unsigned long lastLcdUpdate = 0;  // Thêm biến để quản lý thời gian cập nhật LCD

IPAddress local_IP(192, 168, 31,130);
IPAddress gateway(192, 168, 31, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8); 

String version = "3.1.0"; // Nâng phiên bản

#define DHTPIN 14    // Chân D5 trên WeMos D1 mini
#define DHTTYPE DHT21   // AM2301 
DHT dht(DHTPIN, DHTTYPE);

WiFiEventHandler wifiDisconnectHandler;
WiFiClient espClient;
ESP8266WebServer server(8080);
WebSocketsServer webSocket(81); // Cổng WebSocket

String wifi_ssid = "";
String wifi_pass = "";
bool isTesting = false;

// BIẾN TOÀN CỤC CHO CẢM BIẾN
float t = 0.0;
float h = 0.0;
float t_min = 20.0;
float t_max = 25.0;
float h_min = 40.0;
float h_max = 70.0;

float t_warning = 30.0; 
float h_warning = 70.0; 

bool emailSentTemp = false; 
bool emailSentHum = false;  

// MQTT CONFIG
String mqtt_broker = "192.168.2.36";
int mqtt_port = 1883;
String mqtt_user = "itminh";
String mqtt_pass = "samho2024@";
String mqtt_topic = "TEMPHUMI/SERVERROOM/DATA";
String client_id = "TEST_ESP8266";

float temp_threshold = 0.1; 
int ac_setpoint = 18; 
bool ac_auto = true; 
String last_ac_action = "18"; 

unsigned long lastMqttPbMillis = 0;
unsigned long lastMqttReMillis = 0;
PubSubClient mqttClient(espClient);

// SMTP CONFIG
#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 587
#define AUTHOR_EMAIL "nguyenphuonganh061024@gmail.com"
#define AUTHOR_PASSWORD "ppahhmfhjfdatdga" 
#define RECIPIENT_EMAIL "npanh5006@gmail.com"
SMTPSession smtp;

#define MSG_BUFFER_SIZE (60)
char msg[MSG_BUFFER_SIZE];

unsigned long api_interval = 10000; 
unsigned long lastApiMillis = 0; 
unsigned long mqtt_interval = 10000;
unsigned long lastSensorMillis = 0; 
long lastMsg = 0;

// === BIẾN CHO STATE MACHINE WIFI MỚI ===
enum WifiState { WIFI_STATE_IDLE, WIFI_STATE_WAITING, WIFI_STATE_CONNECTING };
WifiState wifiState = WIFI_STATE_IDLE;
unsigned long wifiStateTs = 0;
const unsigned long WIFI_WAIT_INTERVAL = 5000UL;   // Chờ 5s trước khi thử lại
const unsigned long WIFI_CONNECT_TIMEOUT = 15000UL; // 15s timeout cho mỗi lần kết nối
// =======================================

void sendLog(String logText) {
  Serial.println(logText);
  if(WiFi.status() == WL_CONNECTED) {
    webSocket.broadcastTXT(logText); 
  }
}

String getLocalIpText() {
  if (WiFi.status() == WL_CONNECTED) {
    return WiFi.localIP().toString();
  }
  return local_IP.toString();
}

void loadConfiguration() {
  if (!LittleFS.exists("/config.json")) {
    sendLog("[System] Chua co file config.json. Su dung mac dinh.");
    return;
  }
  File file = LittleFS.open("/config.json", "r");
  if (!file) {
    sendLog("[System] Khong the mo file config.json!");
    return;
  }
  
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, file);
  file.close();
  
  if (error) {
    sendLog("[System] Format JSON loi, khong the parse!");
    return;
  }
  
  if (doc.containsKey("wifi")) {
    wifi_ssid = doc["wifi"]["ssid"].as<String>();
    wifi_pass = doc["wifi"]["password"].as<String>();
    sendLog("[Config] Da nap SSID: " + wifi_ssid);
  }

  if (doc.containsKey("alert")) {
    t_min = doc["alert"]["t_min"].as<float>();
    t_max = doc["alert"]["t_max"].as<float>();
    h_min = doc["alert"]["h_min"].as<float>();
    h_max = doc["alert"]["h_max"].as<float>();
    t_warning = t_max; 
    h_warning = h_max;
  }

  if(doc.containsKey("timer")) {
    mqtt_interval = doc["timer"]["mqtt_interval"].as<unsigned long>() * 1000; 
    api_interval = doc["timer"]["api_interval"].as<unsigned long>() * 1000; 
  }
}

void setup_wifi(){
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false); // ĐỂ CODE TỰ QUẢN LÝ RECONNECT
  WiFi.persistent(false);

  if(wifi_ssid != "" && wifi_pass != "Juniper_Secured") {
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    sendLog("[WiFi] Dang ket noi vao cấu hình luu trong LittleFS: " + wifi_ssid);
  } 
  else {
    WiFi.config(local_IP, gateway, subnet, primaryDNS); 
    WiFi.begin("Juniper_Secured", "vs353535");
    sendLog("[WiFi] Dang ket noi vao WiFi mac dinh (IP Tĩnh): Juniper_Secured");
  }

  // Chờ tối đa kết nối ban đầu
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 40) { 
    delay(500);
    Serial.print(".");
    timeout++;
  }

  if(WiFi.status() == WL_CONNECTED) {
    Serial.print("\nWiFi connected successfully. IP thuc te: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Khong ket noi duoc WiFi. Mach hoat dong che do Offline.");
  }
}

// === HÀM QUẢN LÝ KẾT NỐI WIFI STATE MACHINE NON-BLOCKING ===
void manageWifi() {
  if (isTesting) return; // Nếu đang test từ giao diện web thì bỏ qua
  
  unsigned long now = millis();
  int status = WiFi.status();

  switch (wifiState) {
    // ── IDLE: Trạng thái bình thường đang có mạng ──
    case WIFI_STATE_IDLE:
      if (status != WL_CONNECTED) {
        sendLog("\n[WiFi] Mat ket noi mang!");
        wifiState = WIFI_STATE_WAITING;
        wifiStateTs = now;
      }
      break;

    // ── WAITING: Đợi 5s trước khi thử kết nối lại ──
    case WIFI_STATE_WAITING:
      if (status == WL_CONNECTED) {
        wifiState = WIFI_STATE_IDLE; // Vô tình tự có mạng lại
      } else if (now - wifiStateTs >= WIFI_WAIT_INTERVAL) {
        sendLog("[WiFi] Bat dau thu reconnect...");
        WiFi.disconnect(false);
        delay(100);
        
        if(wifi_ssid != "" && wifi_pass != "Juniper_Secured") {
          WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
        } else {
          WiFi.begin("Juniper_Secured", "vs353535");
        }
        
        wifiState = WIFI_STATE_CONNECTING;
        wifiStateTs = now;
      }
      break;

    // ── CONNECTING: Đang chờ module đàm phán với Router ──
    case WIFI_STATE_CONNECTING:
      if (status == WL_CONNECTED) {
        sendLog("[WiFi] Reconnect THANH CONG! IP: " + WiFi.localIP().toString());
        wifiState = WIFI_STATE_IDLE;
      } else if (now - wifiStateTs >= WIFI_CONNECT_TIMEOUT) {
        sendLog("[WiFi] Timeout! Xoa cache kenh song va thu lai sau 15s...");
        
        // --- HARD RESET ĐỂ TRỊ LỖI KẸT CACHE KHI ROUTER RESTART ---
        WiFi.mode(WIFI_OFF);     // Tắt hoàn toàn sóng RF
        delay(50);               
        WiFi.mode(WIFI_STA);     // Bật lại
        WiFi.disconnect(true);   // Ép xóa sạch cache BSSID/Channel
        delay(50);
        // ---------------------------------------------------------
        
        wifiState = WIFI_STATE_WAITING;
        wifiStateTs = now;
      }
      break;
  }
}

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  Serial.printf("[Sự kiện ngắt SDK] Mã lý do: %d. \n", event.reason);
}

void handleWifiScan(){
  int n = WiFi.scanNetworks();
  StaticJsonDocument<1024> doc;
  JsonArray networks = doc.to<JsonArray>();

  for (int i = 0; i < n; ++i) {
    JsonObject item = networks.createNestedObject();
    item["ssid"] = WiFi.SSID(i);
    item["rssi"] = WiFi.RSSI(i);
  }
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json; charset=utf-8", response);
}

void handleWifiTest() {
  if (!server.hasArg("ssid") || !server.hasArg("password")) {
    server.send(400, "text/plain", "Thieu tham so");
    return;
  }
  String test_ssid = server.arg("ssid");
  String test_pass = server.arg("password");

  isTesting = true;

  WiFi.disconnect(false);
  delay(100);
  WiFi.begin(test_ssid.c_str(), test_pass.c_str());

  StaticJsonDocument<200> doc;
  doc["initiated"] = true;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json; charset=utf-8", response);

  delay(10000);
  isTesting = false;
}

int getQuality() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int rssi = WiFi.RSSI();
  return (rssi + 100) * 2;
}

void handleSetThreshold() {
  if (!server.hasArg("type") || !server.hasArg("value")) {
    server.send(400, "text/plain", "Thieu tham so");
    return;
  }
  
  String type = server.arg("type");
  float value = server.arg("value").toFloat();
  
  if (type == "t_min") t_min = value;
  else if (type == "t_max") t_max = value;
  else if (type == "h_min") h_min = value;
  else if (type == "h_max") h_max = value;
  
  server.send(200, "text/plain", "OK");
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return; // Không kết nối WiFi thì không thử MQTT

  espClient.setTimeout(500);

  mqttClient.setServer(mqtt_broker.c_str(), mqtt_port);
  mqttClient.setBufferSize(1024); // Tăng kích thước buffer để gửi dữ liệu lớn hơn
  
  if (mqttClient.connect(client_id.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
    sendLog("[MQTT] Da ket noi thanh cong");
    mqttClient.subscribe(mqtt_topic.c_str());
  } else {
    sendLog("[MQTT] Ket noi that bai, code: " + String(mqttClient.state()));
  }
}

// void publishSensorData() {
//   if (!mqttClient.connected()) return;
  
//   StaticJsonDocument<256> doc;
//   doc["temperature"] = t;
//   doc["humidity"] = h;
//   doc["wifi_quality"] = getQuality();
//   doc["ip"] = WiFi.localIP().toString();
  
//   char payload[256];
//   serializeJson(doc, payload);
  
//   mqttClient.publish(mqtt_topic.c_str(), payload);
//   sendLog("[MQTT] Da gui du lieu");
// }

void publishSensorData() {
  if (isnan(t) || isnan(h)) return;
  if (!mqttClient.connected()) return;
  
  StaticJsonDocument<512> doc;
  doc["clientID"] = client_id;
  doc["ip"] = WiFi.localIP().toString();
  doc["temp"] = serialized(String(t, 1));
  doc["humi"] = serialized(String(h, 1));
  doc["ac_setpoint"] = ac_setpoint;
  doc["ac_auto"] = ac_auto ? true : false;
  doc["target_temp_min"] = serialized(String(t_min, 1));
  doc["target_temp_max"] = serialized(String(t_max, 1));
  doc["temp_threshold"] = serialized(String(temp_threshold, 1));
  doc["last_ac_action"] = last_ac_action;
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  
  char payload[512];
  serializeJson(doc, payload);
  
  if (mqttClient.publish(mqtt_topic.c_str(), payload)) {
    sendLog("[MQTT] -> Da publish data topic: " + mqtt_topic);
  } else {
    sendLog("[MQTT] -> Publish packets THAT BAI!");
  }
}

// === HANDLER /data - TRỰC TIẾP TRẢ VỀ TẤT CẢ DỮ LIỆU ===
void handleData() {
  StaticJsonDocument<512> doc;
  
  // Dữ liệu cảm biến (luôn trả về, kể cả là NaN)
  if (!isnan(t)) {
    doc["temperature"] = serialized(String(t, 1));
  } else {
    doc["temperature"] = "NaN";
  }
  
  if (!isnan(h)) {
    doc["humidity"] = serialized(String(h, 1));
  } else {
    doc["humidity"] = "NaN";
  }
  
  // Dữ liệu WiFi
  if (WiFi.status() == WL_CONNECTED) {
    doc["wifi_status"] = "CONNECTED";
    doc["wifi_ip"] = WiFi.localIP().toString();
    doc["wifi_ssid"] = WiFi.SSID();
    doc["rssi"] = WiFi.RSSI();
  } else {
    doc["wifi_status"] = "DISCONNECTED";
    doc["wifi_ip"] = "N/A";
    doc["wifi_ssid"] = "N/A";
    doc["rssi"] = "N/A";
  }
  
  // Dữ liệu hệ thống
  doc["free_ram"] = ESP.getFreeHeap();
  
  FSInfo fs_info;
  LittleFS.info(fs_info);
  doc["fs_free"] = fs_info.totalBytes - fs_info.usedBytes;
  doc["fs_total"] = fs_info.totalBytes;
  
  // Cấu hình hiện tại
  doc["cfg_t_min"] = t_min;
  doc["cfg_t_max"] = t_max;
  doc["cfg_h_min"] = h_min;
  doc["cfg_h_max"] = h_max;
  doc["cfg_mqtt_interval"] = mqtt_interval / 1000;
  doc["cfg_api_interval"] = api_interval / 1000;
  doc["cfg_mq_broker"] = mqtt_broker;
  doc["cfg_mq_port"] = mqtt_port;
  doc["cfg_mq_user"] = mqtt_user;
  doc["cfg_mq_pass"] = mqtt_pass;
  doc["cfg_mq_topic"] = mqtt_topic;
  doc["cfg_lcd_mode"] = lcd_mode;
  doc["version"] = version;
  
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json; charset=utf-8", response);
}

void handleRoot() {
  if (LittleFS.exists("/tem.html")) {
    File file = LittleFS.open("/tem.html", "r");
    if (file) {
      // Gửi toàn bộ nội dung file tem.html về trình duyệt với định nghĩa text/html
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
  }
  
  // Nếu không tìm thấy file trong LittleFS, mạch mới báo lỗi để bạn biết
  server.send(404, "text/plain", "Error: Giao dien /tem.html khong ton tai trong LittleFS! Hay chay Upload Filesystem Image.");
}
void handleWifiSave() {
  StaticJsonDocument<512> doc;
  
  if (server.hasArg("ssid")) {
    doc["wifi"]["ssid"] = server.arg("ssid");
    doc["wifi"]["password"] = server.arg("password");
    wifi_ssid = server.arg("ssid");
    wifi_pass = server.arg("password");
  }
  
  if (server.hasArg("broker")) {
    doc["mqtt"]["broker"] = server.arg("broker");
    doc["mqtt"]["port"] = server.arg("port").toInt();
    doc["mqtt"]["user"] = server.arg("user");
    doc["mqtt"]["pass"] = server.arg("mq_pass");
    doc["mqtt"]["topic"] = server.arg("topic");
    
    mqtt_broker = server.arg("broker");
    mqtt_port = server.arg("port").toInt();
    mqtt_user = server.arg("user");
    mqtt_pass = server.arg("mq_pass");
    mqtt_topic = server.arg("topic");
  }
  
  if (server.hasArg("t_min")) {
    doc["alert"]["t_min"] = server.arg("t_min").toFloat();
    doc["alert"]["t_max"] = server.arg("t_max").toFloat();
    doc["alert"]["h_min"] = server.arg("h_min").toFloat();
    doc["alert"]["h_max"] = server.arg("h_max").toFloat();
    
    t_min = server.arg("t_min").toFloat();
    t_max = server.arg("t_max").toFloat();
    h_min = server.arg("h_min").toFloat();
    h_max = server.arg("h_max").toFloat();
  }
  
  if (server.hasArg("mqtt_int")) {
    doc["timer"]["mqtt_interval"] = server.arg("mqtt_int").toInt();
    doc["timer"]["api_interval"] = server.arg("api_int").toInt();
    
    mqtt_interval = server.arg("mqtt_int").toInt() * 1000;
    api_interval = server.arg("api_int").toInt() * 1000;
  }
  
  File file = LittleFS.open("/config.json", "w");
  if (file) {
    serializeJson(doc, file);
    file.close();
    sendLog("[Config] Da luu config.json thanh cong");
  }
  
  server.send(200, "text/plain", "OK");
}

void handleRestoreDefault() {
  LittleFS.remove("/config.json");
  sendLog("[System] Da xoa config.json. Thiet lap mac dinh.");
  server.send(200, "text/plain", "OK");
}

void sendAlertEmail(String type, float val, float thresh) {
  String htmlTemplate = "<h2>CẢNH BÁO HỆ THỐNG</h2>";
  htmlTemplate += "<p>Loại cảnh báo: {{TYPE}}</p>";
  htmlTemplate += "<p>Giá trị hiện tại: {{VALUE}}</p>";
  htmlTemplate += "<p>Ngưỡng cảnh báo: {{THRESHOLD}}</p>";
  
  String color = "#f43f5e";
  String unit = (type == "NHIỆT ĐỘ") ? "°C" : "%";
  
  htmlTemplate.replace("{{COLOR}}", color);
  htmlTemplate.replace("{{TYPE}}", type);
  htmlTemplate.replace("{{THRESHOLD}}", String(thresh, 1) + unit);
  htmlTemplate.replace("{{VALUE}}", String(val, 1) + unit);

  Session_Config config;
  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = AUTHOR_EMAIL;
  config.login.password = AUTHOR_PASSWORD;
  config.login.user_domain = "";
  config.certificate.verify = false; 
  config.secure.startTLS = true;

  SMTP_Message message;
  message.sender.name = "Hệ Thống Nhiệt độ & Độ ẩm";
  message.sender.email = AUTHOR_EMAIL;
  message.subject = "[CẢNH BÁO] THÔNG SỐ VƯỢT NGƯỠNG AN TOÀN!";
  message.addRecipient("User", RECIPIENT_EMAIL);
  
  message.html.content = htmlTemplate.c_str();
  message.html.charSet = "utf-8";

  if (smtp.connect(&config)) {
    MailClient.sendMail(&smtp, &message);
  }
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if(type == WStype_CONNECTED) {
    webSocket.sendTXT(num, "--- Da ket noi voi Web Console ESP8266 ---");
  }
}

void callAPI(){
  if(WiFi.status() != WL_CONNECTED) return;
  if(isnan(t) || isnan(h)) return;

  WiFiClient apiclient;
  String host = "192.168.1.13";
  String url = "/test/arduino/inserttempandhump?temp=" + String(t, 1) + "&hum="  + String(h, 1) + "&ip_machine=" + WiFi.localIP().toString();
  
  apiclient.setTimeout(300);

  if (!apiclient.connect(host.c_str(), 80)) {
    Serial.println("[API] Khong the ket noi den API: " + host);
    return;
  }

  apiclient.print(String("GET ") + url + " HTTP/1.1\r\n" +
                  "Host: " + host + "\r\n" +
                  "Connection: close\r\n\r\n");

  apiclient.stop();
}

void handleSetMqttInterval(){
  if (!server.hasArg("interval")) {
    server.send(400, "text/plain", "Thieu tham so");
    return;
  }
  int sec = server.arg("interval").toInt();
  if (sec < 1 || sec > 3600) {
    server.send(400, "text/plain", "Ngoai pham vi");
    return;
  }
  mqtt_interval = (unsigned long)sec * 1000; 
  server.send(200, "text/plain", "OK");
}

void handleSetApiInterval() {
  if (!server.hasArg("interval")) {
    server.send(400, "text/plain", "Thieu tham so");
    return;
  }
  int sec = server.arg("interval").toInt();
  if (sec < 1 || sec > 3600) {
    server.send(400, "text/plain", "Ngoai pham vi");
    return;
  }
  api_interval = (unsigned long)sec * 1000; 
  server.send(200, "text/plain", "OK");
}

void handleSetLCDMode(){
  if(!server.hasArg("mode")) {
    server.send(400, "text/plain", "Thieu tham so");
    return;
  }

  lcd_mode = server.arg("mode").toInt();
  lcd.clear();
  sendLog("[LCD] Da chuyen che do hien thi sang: " + String(lcd_mode));
  server.send(200, "text/plain", "OK");
}

// === HÀM CẬP NHẬT LCD THEO CHẾ ĐỘ ===
void updateLCDDisplay() {
  unsigned long currentMillis = millis();
  
  // Cập nhật LCD mỗi 500ms để tránh flicker
  if (currentMillis - lastLcdUpdate < 500) {
    return;
  }
  lastLcdUpdate = currentMillis;
  
  switch(lcd_mode) {
    case 0:  // Chế độ 1: Nhiệt độ & Độ ẩm
      if (!isnan(t) && !isnan(h)) {
        lcd.setCursor(0, 0);
        lcd.printf("T:%5.1fC H:%5.1f%%", t, h);
      } else {
        lcd.setCursor(0, 0);
        lcd.print("T:--C  H:---  ");
      }
      break;
      
    case 1:  // Chế độ 2: WiFi & IP
      {
        int quality = getQuality();
        lcd.setCursor(0, 0);
        
        if (WiFi.status() == WL_CONNECTED) {
          lcd.printf("WiFi: %3d%%  ", quality);
          lcd.setCursor(0, 1);
          String ip = WiFi.localIP().toString();
          lcd.printf("IP:%-13s", ip.c_str());
        } else {
          lcd.print("WiFi: Offline  ");
          lcd.setCursor(0, 1);
          lcd.print("Reconnecting... ");
        }
      }
      break;
      
    case 2:  // Chế độ 3: MQTT Status
      lcd.setCursor(0, 0);
      if (WiFi.status() == WL_CONNECTED) {
        if (mqttClient.connected()) {
          lcd.print("MQTT: CONNECTED ");
        } else {
          lcd.print("MQTT: OFFLINE   ");
        }
      } else {
        lcd.print("MQTT: NO WIFI   ");
      }
      lcd.setCursor(0, 1);
      lcd.printf("Broker:%-11s", mqtt_broker.substring(0, 11).c_str());
      break;
      
    default:
      lcd.setCursor(0, 0);
      lcd.print("Mode Not Set    ");
      break;
  }
}

void setup() {
  Serial.begin(9600); 

  mqttClient.setBufferSize(1024);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Starting...");
  
  if (!LittleFS.begin()) {
    Serial.println("Failed to mount LittleFS");
  }

  loadConfiguration(); 
  setup_wifi(); 
  dht.begin();

  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);  // Quantifying endpoint /data
  server.on("/set_threshold", HTTP_POST, handleSetThreshold);
  server.on("/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/wifi/test", HTTP_POST, handleWifiTest);
  server.on("/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/system/restore", HTTP_POST, handleRestoreDefault);
  server.on("/system/restart", HTTP_POST, []() {
    server.send(200, "text/plain", "OK");
    delay(1000);
    ESP.restart();
  });
  server.on("/set_mqtt_interval", HTTP_POST, handleSetMqttInterval);
  server.on("/set_api_interval", HTTP_POST, handleSetApiInterval);
  server.on("/set_lcd_mode", HTTP_POST, handleSetLCDMode);
  server.onNotFound(handleRoot);
  ElegantOTA.begin(&server); 
  ElegantOTA.setAuth("itminh", "samho2024@"); 
  server.begin();

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  Serial.println("HTTP Web Server da khoi dong tai dia chi:");
  Serial.print("http://" + WiFi.localIP().toString() + ":8080 | Phiên bản: " + version + "\n");
}


void loop() {
  server.handleClient();
  webSocket.loop(); 
  ElegantOTA.loop();
  
  unsigned long currentMillis = millis();

  // 1. CHẠY BỘ QUẢN LÝ RECONNECT WIFI STATE MACHINE
  manageWifi();

  // 2. CHẠY MQTT (Chỉ chạy khi WiFi đang có)
  if(WiFi.status() == WL_CONNECTED) {
    if(!mqttClient.connected()) {
      if (currentMillis - lastMqttReMillis >= 10000) { 
        lastMqttReMillis = currentMillis;
        reconnectMQTT();
      }
    } else {
      mqttClient.loop();
    }
  }

  // 3. ĐỌC CẢM BIẾN (Chạy Offline 100% không bị treo)
  if (currentMillis - lastSensorMillis >= 2000) {
    lastSensorMillis = currentMillis;
    
    float new_h = dht.readHumidity();
    float new_t = dht.readTemperature();
    
    if (!isnan(new_t) && !isnan(new_h)) {
      t = new_t; 
      h = new_h; 

      // Kiểm tra Nhiệt độ
      if (t >= t_warning) {
        if (!emailSentTemp) {
          sendAlertEmail("NHIỆT ĐỘ", t, t_warning);
          emailSentTemp = true; 
        }
      } else { emailSentTemp = false; }

      // Kiểm tra Độ ẩm
      if (h >= h_warning) {
        if (!emailSentHum) {
          sendAlertEmail("ĐỘ ẨM", h, h_warning);
          emailSentHum = true;
        }
      } else { emailSentHum = false; }
      
    } else {
      Serial.println("[Báo lỗi] Cảm biến AM2301 phản hồi chậm...");
    }
  }

  // 4. PUBLISH MQTT
  if (WiFi.status() == WL_CONNECTED && mqttClient.connected() && (currentMillis - lastMqttPbMillis >= mqtt_interval)) {
    lastMqttPbMillis = currentMillis;
    publishSensorData();
  }

  // 5. GỌI HTTP API ĐỊNH KỲ
  if (WiFi.status() == WL_CONNECTED && (currentMillis - lastApiMillis >= api_interval)) {
    lastApiMillis = currentMillis;
    callAPI();
  }

  // 6. IN LOG MÀN HÌNH SERIAL & WEB
  if (currentMillis - lastMsg > 1000) {
    lastMsg = currentMillis;
    int Qal = getQuality();
    if(WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI: %3d%%] | TEMP: %5.1f°C | HUMI: %5.1f%%\n", Qal, t, h);
    } else {
      Serial.printf("Log -> Đang mất mạng WiFi! Mạch vẫn chạy Offline\n");
    }

    char webLogBuf[128];
    snprintf(webLogBuf, sizeof(webLogBuf), "[WIFI: %3d%%] | TEMP: %5.1f°C | HUMI: %5.1f%%", Qal, t, h);
    sendLog(String(webLogBuf));
  }

  // 7. CẬP NHẬT LCD THEO CHẾ ĐỘ (Chạy luôn, không phụ thuộc vào cảm biến)
  updateLCDDisplay();
}