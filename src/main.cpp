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

IPAddress local_IP(192, 168, 31,130);
IPAddress gateway(192, 168, 31, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8); 

String version = "3.0.0"; // Phiên bản phần mềm

#define DHTPIN 5    // Chân D1 trên WeMos D1 mini

#define DHTTYPE DHT21   // AM2301 

DHT dht(DHTPIN, DHTTYPE);

WiFiEventHandler wifiDisconnectHandler;
WiFiClient espClient;
ESP8266WebServer server(8080);
WebSocketsServer webSocket(81); // Cổng WebSocket

// const char* ssid = "Juniper_Secured";
// const char* password = "vs353535";

String wifi_ssid = "";
String wifi_pass = "";

bool isTesting = false;

// SỬA: Biến toàn cục để lưu dữ liệu đồng bộ giữa loop() và handleData()
float t = 0.0;
float h = 0.0;

float t_min = 20.0;
float t_max = 25.0;
float h_min = 40.0;
float h_max = 70.0;

float t_warning = 30.0; // Ngưỡng cảnh báo nhiệt độ
float h_warning = 70.0; // Ngưỡng cảnh báo độ ẩm

bool emailSentTemp = false; // Biến để kiểm tra trạng thái gửi email
bool emailSentHum = false;  // Biến để kiểm tra trạng thái gửi email


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

#define SMTP_HOST "smtp.gmail.com"
#define SMTP_PORT 587
#define AUTHOR_EMAIL "nguyenphuonganh061024@gmail.com"
#define AUTHOR_PASSWORD "ppahhmfhjfdatdga" // Mật khẩu ứng dụng (App Password) thay vì mật khẩu tài khoản Gmail
#define RECIPIENT_EMAIL "npanh5006@gmail.com"

SMTPSession smtp;

#define MSG_BUFFER_SIZE (60)
char msg[MSG_BUFFER_SIZE];
unsigned long previousMillis = 0;
unsigned long reconnectInterval = 10000; // Thời gian chờ trước khi thử kết nối lại WiFi

unsigned long api_interval = 10000; 
unsigned long lastApiMillis = 0; 

unsigned long mqtt_interval = 10000;

unsigned long lastSensorMillis = 0; // Thêm biến quản lý thời gian đọc cảm biến riêng

long lastMsg = 0;
String ip = "";

void sendLog(String logText) {
  Serial.println(logText);
  webSocket.broadcastTXT(logText); // Gửi log đến tất cả các client WebSocket
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
  
  // Trích xuất cấu trúc dữ liệu đề xuất
  if (doc.containsKey("wifi")) {
    wifi_ssid = doc["wifi"]["ssid"].as<String>();
    wifi_pass = doc["wifi"]["password"].as<String>();
    sendLog("[Config] Da nap SSID: " + wifi_ssid);
  }

  // if (doc.containsKey("mqtt")) {
  //   mqtt_broker = doc["mqtt"]["broker"].as<String>();
  //   mqtt_port = doc["mqtt"]["port"].as<int>();
  //   mqtt_user = doc["mqtt"]["user"].as<String>();
  //   mqtt_pass = doc["mqtt"]["password"].as<String>();
  //   mqtt_topic = doc["mqtt"]["topic"].as<String>();
  //   // client_id = doc["mqtt"]["client_id"].as<String>();
  //   sendLog("[Config] Da nap thong so MQTT: " + mqtt_broker);
  // }

  

  if (doc.containsKey("alert")) {
    t_min = doc["alert"]["t_min"].as<float>();
    t_max = doc["alert"]["t_max"].as<float>();
    h_min = doc["alert"]["h_min"].as<float>();
    h_max = doc["alert"]["h_max"].as<float>();
    t_warning = t_max; // Gán đồng bộ cho luồng gửi email cũ
    h_warning = h_max;
  }

  if(doc.containsKey("timer")) {
    mqtt_interval = doc["timer"]["mqtt_interval"].as<unsigned long>() * 1000; // Chuyển sang mili giây
    api_interval = doc["timer"]["api_interval"].as<unsigned long>() * 1000; // Chuyển sang mili giây
  }
}

// void setup_wifi() {
//   delay(500);
//   Serial.println();

//   if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {  
//     Serial.println("STA Failed to configure");
//   }

//   WiFi.mode(WIFI_STA);
//   WiFi.begin(wifi_ssid, wifi_pass);

//   Serial.println("Connecting to WiFi...");
//   Serial.print(wifi_ssid);

//   // giới hạn thời gian chờ tối đa 10s
//   int timeout = 0;
//   while (WiFi.status() != WL_CONNECTED && timeout < 20) { // 20 * 500ms = 10s
//     delay(500);
//     Serial.print(".");
//     timeout++;
//   }

//   randomSeed(micros());
//   if (WiFi.status() != WL_CONNECTED) {
//     Serial.println("\nFailed to connect to WiFi. Please check your credentials and network.");
//     Serial.println("Rebooting in 5 seconds...");
//     return;
//   }else {
//     Serial.println("\nWiFi connected successfully.");
//     Serial.print("IP address: ");
//     Serial.println(WiFi.localIP());
//   }
//   // Serial.println("");
//   // Serial.println("WiFi connected");
//   // Serial.print("IP address: ");
//   // Serial.println(WiFi.localIP());
//   // ip = WiFi.localIP().toString();

// }

void setup_wifi(){
  WiFi.mode(WIFI_STA);
  
  // Kiểm tra cấu hình trong bộ nhớ LittleFS
  if(wifi_ssid != "" && wifi_pass != "Juniper_Secured") {
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    sendLog("[WiFi] Dang ket noi vao cấu hình luu trong LittleFS: " + wifi_ssid);
  } 
  else {
    // Không khai báo lại IPAddress, sử dụng trực tiếp các biến toàn cục từ đầu file
    WiFi.config(local_IP, gateway, subnet, primaryDNS); 
    WiFi.begin("Juniper_Secured", "vs353535");
    sendLog("[WiFi] Dang ket noi vao WiFi mac dinh (IP Tĩnh): Juniper_Secured");
  }

  // Giới hạn thời gian chờ tối đa kết nối
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 40) { // Tăng lên 40 (20s) để mạch kịp nhận IP
    delay(500);
    Serial.print(".");
    timeout++;
  }

  if(WiFi.status() == WL_CONNECTED) {
    // SỬA LỖI EXCEPTION (28): Dùng lệnh in nối chuỗi an toàn thay vì toán tử cộng đối tượng tạm thời
    Serial.print("\nWiFi connected successfully. IP thuc te: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Khong ket noi duoc WiFi. Mach hoat dong che do Offline.");
  }
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
  
  sendLog("[WiFi] Bat dau thu nghiem ket noi ngam den: " + test_ssid);
  
  // 1. Phản hồi cho Trình duyệt web TRƯỚC để tránh bị lỗi "Không thể gửi lệnh"
  StaticJsonDocument<128> responseDoc;
  responseDoc["initiated"] = true;
  String out;
  serializeJson(responseDoc, out);
  server.send(200, "application/json", out);
  
  // 2. Chờ một chút rất nhỏ để gói tin HTTP kịp truyền về Laptop an toàn
  delay(100); 

  // 3. Cập nhật biến toàn cục và khóa luồng Reconnect
  wifi_ssid = test_ssid;
  wifi_pass = test_pass;
  isTesting = true; 
  previousMillis = millis(); // Cập nhật lại bộ đếm để kéo dài thời gian chờ trong loop()

  // 4. Tiến hành đổi mạng ngầm
  WiFi.disconnect();
  WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
}

void handleWifiSave() {
  // 1. Đọc khối WIFI chuẩn
  if (server.hasArg("ssid")) wifi_ssid = server.arg("ssid");
  if (server.hasArg("password")) wifi_pass = server.arg("password");

  if (server.hasArg("t_min")) t_min = server.arg("t_min").toFloat();
  if (server.hasArg("t_max")) t_max = server.arg("t_max").toFloat();
  if (server.hasArg("h_min")) h_min = server.arg("h_min").toFloat();
  if (server.hasArg("h_max")) h_max = server.arg("h_max").toFloat();
  
  if (server.hasArg("mqtt_int")) mqtt_interval = (unsigned long)server.arg("mqtt_int").toInt() * 1000; // SỬA: từ "interval" thành "mqtt_int"
  if (server.hasArg("api_int")) api_interval = (unsigned long)server.arg("api_int").toInt() * 1000;
  
  t_warning = t_max; 
  h_warning = h_max;

  // 4. Đóng gói lưu file config.json toàn vẹn
  StaticJsonDocument<1024> doc;
  doc["wifi"]["ssid"] = wifi_ssid;
  doc["wifi"]["password"] = wifi_pass;
  doc["alert"]["t_min"] = t_min;
  doc["alert"]["t_max"] = t_max;
  doc["alert"]["h_min"] = h_min;
  doc["alert"]["h_max"] = h_max;

  doc["timer"]["mqtt_interval"] = mqtt_interval / 1000; // Lưu chu kỳ MQTT dưới dạng giây
  doc["timer"]["api_interval"] = api_interval / 1000; // Lưu

  File wFile = LittleFS.open("/config.json", "w");
  if (!wFile) {
    server.send(500, "text/plain", "Loi ghi file");
    return;
  }
  serializeJson(doc, wFile);
  wFile.close();
  
  sendLog("[System] Cấu hình hệ thống đã được đồng bộ chuẩn xác vĩnh viễn!");
  server.send(200, "text/plain", "OK");
}

void reconnectMQTT(){
  if(WiFi.status() != WL_CONNECTED) return;
  // mqttClient.setServer(mqtt_broker.c_str(), mqtt_port);
  mqttClient.setServer(mqtt_broker.c_str(), mqtt_port);
  mqttClient.setBufferSize(1024);
  
  if (!mqttClient.connected()) {
    sendLog("[MQTT] Dang ket noi lai den broker: " + mqtt_broker);
    if (mqttClient.connect(client_id.c_str(), mqtt_user.c_str(), mqtt_pass.c_str())) {
      sendLog("[MQTT] Ket noi thanh cong den broker: " + mqtt_broker);
    } else {
      sendLog("[MQTT] Ket noi that bai. Ma loi: " + String(mqttClient.state()));
    }
  }
}

void publishSensorData() {

  if (isnan(t) || isnan(h)) {
    sendLog("[MQTT] -> Cảnh báo: Dữ liệu cảm biến không hợp lệ. Không publish.");
    return;
  }
  
  if (!mqttClient.connected()) return;

  String payload = "{";
  payload += "\"clientID\":\"" + client_id + "\",";
  payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  payload += "\"temp\":" + (isnan(t) ? "0.0" : String(t, 1)) + ",";
  payload += "\"humi\":" + (isnan(h) ? "0.0" : String(h, 1)) + ",";
  payload += "\"ac_setpoint\":" + String(ac_setpoint) + ",";
  payload += "\"ac_auto\":" + String(ac_auto ? "true" : "false") + ",";
  payload += "\"target_temp_min\":" + String(t_min, 1) + ",";
  payload += "\"target_temp_max\":" + String(t_max, 1) + ",";
  payload += "\"temp_threshold\":" + String(temp_threshold, 1) + ",";
  payload += "\"last_ac_action\":\"" + last_ac_action + "\",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"uptime\":" + String(millis() / 1000);
  payload += "}";
  
  if (mqttClient.publish(mqtt_topic.c_str(), payload.c_str())) {
    sendLog("[MQTT] -> Da publish message mau len topic: " + mqtt_topic);
  } else {
    sendLog("[MQTT] -> Publish packets THAT BAI!");
  }
}

int getQuality() {
  if (WiFi.status() != WL_CONNECTED)
    return -1;
  int dBm = WiFi.RSSI();
  if (dBm <= -100)
    return 0;
  if (dBm >= -50)
    return 100;
  return 2 * (dBm + 100);
}

void handleRestoreDefault() {
  if (LittleFS.exists("/config.json")) {
    LittleFS.remove("/config.json");
  }
  sendLog("[System] Da xoa file cau hinh! Dang khoi dong lai mạch...");
  server.send(200, "text/plain", "OK");
  delay(1000);
  ESP.restart();
}

// void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
//   Serial.println("Disconnected from WiFi access point. Reconnecting...");
//   WiFi.disconnect();
//   WiFi.begin(wifi_ssid, wifi_pass);
//   ip = local_IP.toString();
// }

void onWifiDisconnect(const WiFiEventStationModeDisconnected& event) {
  Serial.printf("[Sự kiện] Bị ngắt kết nối WiFi. Mã lý do: %d. Đang kết nối lại ngầm...\n", event.reason);
}

void handleRoot() {
  File htmlFile = LittleFS.open("/tem.html", "r");
  if (!htmlFile) {
    server.send(500, "text/plain; charset=utf-8", "Khong mo duoc file tem.html");
    return;
  }

  String html = htmlFile.readString();
  htmlFile.close();
  html.replace("{{LOCAL_IP}}", getLocalIpText());

  server.send(200, "text/html; charset=utf-8", html);
}

// SỬA: Sửa lại định dạng chuỗi JSON và sửa logic ngược isnan()
void handleData() {
  FSInfo fsInfo;
  LittleFS.info(fsInfo);

  String statusText = (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "DISCONNECTED";

  String json = "{";
  json += "\"wifi_status\":\"" + statusText + "\",";
  json += "\"wifi_ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"wifi_ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"wifi_quality\":" + String(getQuality()) + ",";
  json += "\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"free_ram\":" + String(ESP.getFreeHeap()) + ","; // RAM trống (Bytes)
  json += "\"flash_size\":" + String(ESP.getFlashChipRealSize()) + ","; // Tổng dung lượng Flash
  json += "\"fs_free\":" + String(fsInfo.totalBytes - fsInfo.usedBytes) + ","; // Bộ nhớ LittleFS còn trống
  json += "\"temperature\":" + (isnan(t) ? "\"NaN\"" : String(t, 1)) + ",";
  json += "\"humidity\":" + (isnan(h) ? "\"NaN\"" : String(h, 1)) + ",";
  // json += "\"t_warning\":" + String(t_warning, 1) + ",";
  // json += "\"h_warning\":" + String(h_warning, 1);

  json += "\"cfg_t_min\":" + String(t_min, 1) + ",";
  json += "\"cfg_t_max\":" + String(t_max, 1) + ",";
  json += "\"cfg_h_min\":" + String(h_min, 1) + ",";
  json += "\"cfg_h_max\":" + String(h_max, 1) + ",";

  json += "\"cfg_client_id\":\"" + client_id + "\",";
  json += "\"cfg_mq_broker\":\"" + mqtt_broker + "\",";
  json += "\"cfg_mq_port\":" + String(mqtt_port) + ",";
  json += "\"cfg_mq_user\":\"" + mqtt_user + "\",";
  json += "\"cfg_mq_pass\":\"" + mqtt_pass + "\",";
  json += "\"cfg_mq_topic\":\"" + mqtt_topic + "\""; 

  json += ",\"cfg_mqtt_interval\":" + String(mqtt_interval / 1000);
  json += ",\"cfg_api_interval\":"  + String(api_interval  / 1000);

  json += ",\"version\":\"" + version + "\"";
  json += "}";

  server.send(200, "application/json; charset=utf-8", json);
}

// void handleSetThreshold() {
//   if (server.hasArg("temp")) {
//     t_warning = server.arg("temp").toFloat();
//   }
//   if (server.hasArg("humi")) {
//     h_warning = server.arg("humi").toFloat();
//   }
//   Serial.printf("\n---> NHAN CONFIG MOI -> Nguong T: %.1f | Nguong H: %.1f\n", t_warning, h_warning);
//   server.send(200, "text/plain", "OK");
// }

void handleSetThreshold() {
  if (server.hasArg("temp")) t_max = server.arg("temp").toFloat();
  if (server.hasArg("humi")) h_max = server.arg("humi").toFloat();
  t_warning = t_max; h_warning = h_max;
  server.send(200, "text/plain", "OK");
}

void sendAlertEmail(String type, float val, float thresh) {
  // 1. Đọc file giao diện HTML từ bộ nhớ LittleFS
  File file = LittleFS.open("/email.html", "r");
  if (!file) {
    Serial.println("Loi: Khong tim thay file email.html trong LittleFS!");
    return;
  }
  String htmlTemplate = file.readString();
  file.close();

  // 2. Xác định màu sắc và đơn vị tương ứng từng loại cảm biến
  String color = (type == "NHIỆT ĐỘ") ? "#f43f5e" : "#06b6d4";
  String unit = (type == "NHIỆT ĐỘ") ? " °C" : " %";

  // 3. Thực hiện hoán đổi dữ liệu (Replace tokens) vào template mẫu
  htmlTemplate.replace("{{COLOR}}", color);
  htmlTemplate.replace("{{TYPE}}", type);
  htmlTemplate.replace("{{THRESHOLD}}", String(thresh, 1) + unit);
  htmlTemplate.replace("{{VALUE}}", String(val, 1) + unit);

  // 4. Cấu hình và tiến hành gửi SMTP Mail
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
  
  // Gán chuỗi HTML sau khi đã xử lý vào nội dung thư
  message.html.content = htmlTemplate.c_str();
  message.html.charSet = "utf-8";

  if (!smtp.connect(&config)) {
    Serial.printf("SMTP Connection failed: %s (Mã lỗi: %d)\n", smtp.errorReason().c_str(), smtp.statusCode());
    return;
  }

  if (!MailClient.sendMail(&smtp, &message)) {
    Serial.printf("Failed to send Email: %s\n", smtp.errorReason().c_str());
  } else {
    Serial.println("Email cảnh báo đã được gửi thành công!");
  }
}

void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  if(type == WStype_CONNECTED) {
    webSocket.sendTXT(num, "--- Da ket noi voi Web Console ESP8266 Realtime ---");
  }
}

void callAPI(){
  if(WiFi.status() != WL_CONNECTED) return;
  if(isnan(t) || isnan(h)){
    sendLog("[API] -> Cảnh báo: Dữ liệu cảm biến không hợp lệ. Không gọi API.");
    return;
  }

  WiFiClient apiclient;
  String host = "192.168.1.13";
  String url = "/test/arduino/inserttempandhump"
             "?temp=" + String(t, 1) +
             "&hum="  + String(h, 1) +
             "&ip_machine=" + WiFi.localIP().toString();
  if (!apiclient.connect(host.c_str(), 80)) {
    sendLog("[API] -> Khong the ket noi den API: " + host); 
    return;
  }

  apiclient.print(String("GET ") + url + " HTTP/1.1\r\n" +
                  "Host: " + host + "\r\n" +
                  "Connection: close\r\n\r\n");

  unsigned long t0 = millis();
  while (apiclient.available() == millis() - t0 < 2000) delay(10);

  String statusLine = apiclient.readStringUntil('\n');
  sendLog("[API] -> Da goi API: " + url + " | Trang thai: " + statusLine);
  apiclient.stop();
}

void handleSetMqttInterval(){
  if (!server.hasArg("interval")) {
    server.send(400, "text/plain", "Thieu tham so: interval (don vi: giay)");
    return;
  }
  int sec = server.arg("interval").toInt();
  if (sec < 1 || sec > 3600) {
    server.send(400, "text/plain", "Gia tri interval phai tu 1 den 3600 giay");
    return;
  }

  mqtt_interval = (unsigned long)sec * 1000; // Chuyển sang mili giây
  sendLog("[MQTT] -> Da cap nhat khoang thoi gian publish: " + String(sec) + " giay");
  server.send(200, "text/plain", "OK");
}

  void handleSetApiInterval() {
    if (!server.hasArg("interval")) {
      server.send(400, "text/plain", "Thieu tham so: interval (don vi: giay)");
      return;
    }
    int sec = server.arg("interval").toInt();
    if (sec < 1 || sec > 3600) {
      server.send(400, "text/plain", "Gia tri interval phai tu 1 den 3600 giay");
      return;
    }

    api_interval = (unsigned long)sec * 1000; // Chuyển sang mili giây
    sendLog("[API] -> Da cap nhat khoang thoi gian goi API: " + String(sec) + " giay");
    server.send(200, "text/plain", "OK");
  }

void setup() 
{
  Serial.begin(9600); 
  Serial.println("DHTxx test!");

  if (!LittleFS.begin()) {
    Serial.println("Failed to mount file system");
  }

  loadConfiguration(); // Nạp cấu hình WiFi từ LittleFS nếu có
  setup_wifi(); // Kết nối WiFi dựa trên cấu hình đã nạp
  dht.begin();

  wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
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
  server.onNotFound(handleRoot);
  ElegantOTA.begin(&server); // Bắt đầu ElegantOTA với server ESP8266WebServer
  ElegantOTA.setAuth("itminh", "samho2024@"); // Tùy chọn: đặt tên người dùng và mật khẩu cho OTA
  server.begin();

  webSocket.begin();
  webSocket.onEvent(onWebSocketEvent);

  Serial.println("HTTP Web Server da khoi dong tai dia chi:");
  String currentIP = WiFi.localIP().toString();
  Serial.print("http://" + currentIP + ":8080 | Phiên bản: " + version + "\n");
  
  // Serial.printf("http://%s:8080\n | Phiên bản: %s\n", WiFi.localIP().toString().c_str());
}


void loop() {
  // 1. Luôn lắng nghe trình duyệt truy cập
  server.handleClient();
  webSocket.loop(); // Lắng nghe các kết nối WebSocket
  ElegantOTA.loop();
  unsigned long currentMillis = millis();


  if(WiFi.status() == WL_CONNECTED) {
    if(!mqttClient.connected()) {
      if (currentMillis - lastMqttReMillis >= 10000) { // Thử kết nối lại mỗi 10 giây
        lastMqttReMillis = currentMillis;
        reconnectMQTT();
      }
    } else {
      mqttClient.loop();
    }
  }
  // 2. Tự động kết nối lại WiFi khi mất sóng
  if ((WiFi.status() != WL_CONNECTED) && (isTesting == false)){
    if (currentMillis - previousMillis >= reconnectInterval) {
      previousMillis = currentMillis;
      Serial.println("Reconnecting to WiFi...");
      WiFi.disconnect();
      WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    }
  }

  // 3. ĐỌC CẢM BIẾN VÀ XỬ LÝ GỬI MAIL (Đúng chu kỳ 2 giây ổn định của AM2301)
  if (currentMillis - lastSensorMillis >= 2000) {
    lastSensorMillis = currentMillis;
    
    float new_h = dht.readHumidity();
    float new_t = dht.readTemperature();
    
    // Chỉ xử lý nếu cảm biến trả về con số hợp lệ công nhận
    if (!isnan(new_t) && !isnan(new_h)) {
      t = new_t; // Gán vào biến toàn cục để trả về JSON cho Web
      h = new_h; // Gán vào biến toàn cục để trả về JSON cho Web
      
      // --- KIỂM TRA NGƯỠNG VÀ GỬI MAIL NGAY TẠI ĐÂY ---
      // Kiểm tra Nhiệt độ
      if (t >= t_warning) {
        if (!emailSentTemp) {
          Serial.println("\n[!] Nhiệt độ vượt ngưỡng! Tiến hành gửi Mail...");
          sendAlertEmail("NHIỆT ĐỘ", t, t_warning);
          emailSentTemp = true; 
        }
      } else {
        emailSentTemp = false; 
      }

      // Kiểm tra Độ ẩm
      if (h >= h_warning) {
        if (!emailSentHum) {
          Serial.println("\n[!] Độ ẩm vượt ngưỡng! Tiến hành gửi Mail...");
          sendAlertEmail("ĐỘ ẨM", h, h_warning);
          emailSentHum = true;
        }
      } else {
        emailSentHum = false;
      }
      
    } else {
      // Nếu lượt đọc này bị lỗi, giữ nguyên giá trị t và h cũ, không gán bậy để tránh rớt web
      Serial.println("[Báo lỗi] Cảm biến AM2301 phản hồi chậm, đang đợi chu kỳ kế tiếp...");
    }
  }

  if (mqttClient.connected() && (currentMillis - lastMqttPbMillis >= mqtt_interval)) {
    lastMqttPbMillis = currentMillis;
    publishSensorData();
  }

  // GỌI HTTP API ĐỊNH KỲ
  if (WiFi.status() == WL_CONNECTED && (currentMillis - lastApiMillis >= api_interval)) {
    lastApiMillis = currentMillis;
    callAPI();
  }

  // 4. In log lên Serial Monitor định kỳ 1 giây để theo dõi
  if (currentMillis - lastMsg > 1000) {
    lastMsg = currentMillis;
    int Qal = getQuality();
    if(WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI: %3d%%] | TEMP: %5.1f°C (Ngưỡng: %5.1f°C) | HUMI: %5.1f%% (Ngưỡng: %5.1f%%)\n", 
              getQuality(), t, t_warning, h, h_warning);
    } else {
      Serial.printf("Log -> Đang mất mạng WiFi! Mạch vẫn chạy Offline | T: %.1f *C | H: %.1f %%\n", t, h);
    }

    char webLogBuf[128];
    snprintf(webLogBuf, sizeof(webLogBuf), "[WIFI: %3d%%] | TEMP: %5.1f°C (Ngưỡng: %5.1f°C) | HUMI: %5.1f%% (Ngưỡng: %5.1f%%)", 
              Qal, t, t_warning, h, h_warning);
    sendLog(String(webLogBuf));
    // sendLog("Log -> RSSI: " + String(WiFi.RSSI()) + " dBm | T: " + String(t, 1) + " *C | H: " + String(h, 1) + " %");
    // digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
}