#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Updater.h>          // OTA 核心库（ESP8266 core 自带）

//====================【修改全部参数】====================
const char* WIFI_SSID     = "CMCC-eujw";
const char* WIFI_PASS     = "gmw18479639832";
const char* MQTT_HOST     = "i11884f9.ala.cn-hangzhou.emqxsl.cn";
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "esp-switch01";
const char* MQTT_PASS     = "switch666888";
const char* CLIENT_ID     = "esp01_switch_001"; // 务必保证唯一

#define CMD_TOPIC    "light/switch/cmd"    // 接收开关指令
#define STATE_TOPIC  "light/switch/state"  // 上报当前状态
#define HEARTBEAT_TOPIC "device/heartbeat"

// 新增：GPIO5脉冲 & OTA主题
#define PULSE_CMD_TOPIC   "device/gpio5/pulse"
#define OTA_TOPIC         "device/ota"        // 开始/结束指令
#define OTA_DATA_TOPIC    "device/ota/data"   // 分片数据
#define OTA_STATUS_TOPIC  "device/ota/status" // 上报进度

#define RELAY_PIN     0   // GPIO0 (D3)
#define LED_PIN       2   // 板载LED
#define PULSE_PIN     5   // GPIO5 脉冲输出

#define HEARTBEAT_INTERVAL 1000UL   // 1秒上报一次心跳
#define PULSE_LOW_MS       600UL    // 低电平持续时间
#define PULSE_PERIOD_MS    3600000UL // 1小时周期

#define MQTT_IDLE_TIMEOUT_MS  (5UL * 60UL * 1000UL)
#define BOOT_MAX_LIFE_MS      (5UL * 60UL * 1000UL)
//============================================================

WiFiClientSecure espClient;
PubSubClient client(espClient);

bool switchState = false;
unsigned long bootStartTime = 0;
unsigned long lastMqttCmdTime = 0;
unsigned long lastHeartbeat = 0;
bool wifiStartFlag = false;

// LED
unsigned long ledLastTick = 0;
const unsigned long SLOW_BLINK_INTERVAL = 800;
bool wifiConnectedFlag = false;

// 脉冲相关
unsigned long pulseStartTime = 0;       // 低电平开始时间（0表示当前无脉冲）
bool pulseActive = false;
unsigned long lastAutoPulseTime = 0;    // 上次自动脉冲触发时间

// OTA相关
bool otaInProgress = false;
size_t otaTotalSize = 0;
size_t otaReceived = 0;
size_t otaChunkSize = 0;
uint32_t otaNextSeq = 0;

//--------------- LED工具 ----------------
void ledFastBlink(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW);  delay(100);
    digitalWrite(LED_PIN, HIGH); delay(100);
  }
}

//--------------- 发布OTA进度 ----------------
void publishOtaProgress(int percent) {
  char buf[16];
  snprintf(buf, sizeof(buf), "progress:%d", percent);
  client.publish(OTA_STATUS_TOPIC, buf);
}

//--------------- 处理OTA消息 ----------------
void handleOtaMessage(const String& topic, const String& msg) {
  if (topic == OTA_TOPIC) {
    // 控制指令
    if (msg.indexOf("\"ota_start\"") >= 0) {
      // 解析 size / chunkSize（简易解析，够用）
      int sizeIdx = msg.indexOf("\"size\":");
      int chunkIdx = msg.indexOf("\"chunkSize\":");
      if (sizeIdx > 0 && chunkIdx > 0) {
        otaTotalSize = msg.substring(sizeIdx + 7, msg.indexOf(',', sizeIdx)).toInt();
        otaChunkSize = msg.substring(chunkIdx + 12, msg.indexOf('}', chunkIdx)).toInt();
      }
      if (otaChunkSize == 0) otaChunkSize = 512;

      Serial.printf("OTA开始: 总大小=%u, 分片=%u\n", otaTotalSize, otaChunkSize);
      // 开始更新
      if (Update.begin(otaTotalSize)) {
        otaInProgress = true;
        otaReceived = 0;
        otaNextSeq = 0;
        publishOtaProgress(0);
      } else {
        Serial.println("Update.begin 失败");
        client.publish(OTA_STATUS_TOPIC, "error");
      }
    } else if (msg.indexOf("\"ota_end\"") >= 0) {
      if (otaInProgress) {
        if (Update.end(true)) {
          Serial.println("OTA 完成，重启...");
          client.publish(OTA_STATUS_TOPIC, "done");
          delay(500);
          ESP.restart();
        } else {
          Serial.println("Update.end 失败");
          client.publish(OTA_STATUS_TOPIC, "error");
          otaInProgress = false;
        }
      }
    }
  } else if (topic == OTA_DATA_TOPIC) {
    if (!otaInProgress) return;
    // 解析 seq 和 base64 data
    int seqIdx = msg.indexOf("\"seq\":");
    int dataIdx = msg.indexOf("\"data\":\"");
    if (seqIdx < 0 || dataIdx < 0) return;
    uint32_t seq = msg.substring(seqIdx + 6, msg.indexOf(',', seqIdx)).toInt();
    int dataStart = dataIdx + 8;
    int dataEnd = msg.indexOf('"', dataStart);
    String b64 = msg.substring(dataStart, dataEnd);
    b64.replace("\\/", "/"); // 防止转义

    // 简单校验顺序（可选）
    if (seq != otaNextSeq) {
      Serial.printf("OTA序号乱序: 期望%u, 实际%u\n", otaNextSeq, seq);
      // 这里可以选择忽略或重传，简单起见忽略乱序包
      return;
    }

    // base64 解码
    int len = b64.length();
    int pad = 0;
    if (len > 0 && b64[len-1] == '=') pad++;
    if (len > 1 && b64[len-2] == '=') pad++;
    int outLen = (len * 3) / 4 - pad;

    uint8_t* outBuf = (uint8_t*)malloc(outLen);
    if (!outBuf) {
      Serial.println("OTA 内存不足");
      client.publish(OTA_STATUS_TOPIC, "error");
      otaInProgress = false;
      return;
    }

    // 自定义 base64 解码
    const char* b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val = 0, valb = -8, idx = 0;
    for (int i = 0; i < len; i++) {
      char c = b64[i];
      if (c == '=') break;
      const char* p = strchr(b64chars, c);
      if (!p) continue;
      val = (val << 6) + (p - b64chars);
      valb += 6;
      if (valb >= 0) {
        outBuf[idx++] = (uint8_t)((val >> valb) & 0xFF);
        valb -= 8;
      }
    }

    if (idx != outLen) outLen = idx; // 修正

    // 写入 flash
    size_t written = Update.write(outBuf, outLen);
    free(outBuf);

    if (written != outLen) {
      Serial.println("OTA 写入失败");
      client.publish(OTA_STATUS_TOPIC, "error");
      otaInProgress = false;
      return;
    }

    otaReceived += written;
    otaNextSeq++;

    // 上报进度（每10片或最后一片报一次）
    if (otaNextSeq % 10 == 0 || otaReceived >= otaTotalSize) {
      int percent = (otaTotalSize > 0) ? (otaReceived * 100 / otaTotalSize) : 0;
      publishOtaProgress(percent);
      Serial.printf("OTA进度: %d%% (%u/%u)\n", percent, otaReceived, otaTotalSize);
    }
  }
}

//--------------- MQTT回调 ----------------
void callback(char* topic, byte* payload, unsigned int length) {
  lastMqttCmdTime = millis();

  payload[length] = '\0';
  String msg = String((char*)payload);
  msg.trim();
  String topicStr = String(topic);

  Serial.print("收到MQTT [");
  Serial.print(topicStr);
  Serial.print("]: ");
  Serial.println(msg);

  // OTA 消息
  if (topicStr == OTA_TOPIC || topicStr == OTA_DATA_TOPIC) {
    handleOtaMessage(topicStr, msg);
    return;
  }

  // 开关指令
  if (topicStr == CMD_TOPIC) {
    if (msg == "ON") {
      switchState = true;
      digitalWrite(RELAY_PIN, LOW);
    } else if (msg == "OFF") {
      switchState = false;
      digitalWrite(RELAY_PIN, HIGH);
    }
    client.publish(STATE_TOPIC, switchState ? "ON" : "OFF");
    return;
  }

  // GPIO5脉冲指令
  if (topicStr == PULSE_CMD_TOPIC) {
    // 支持 "pulse:600" 或纯 "pulse"，统一用固定时间
    if (!pulseActive) {
      Serial.println("触发GPIO5脉冲（MQTT）");
      digitalWrite(PULSE_PIN, LOW);   // 低电平
      pulseStartTime = millis();
      pulseActive = true;
    }
    return;
  }
}

//--------------- WiFi启动 ----------------
void startWifi() {
  Serial.println("WiFi开始连接...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

//--------------- MQTT重连 ----------------
void mqttReconnect() {
  while (!client.connected()) {
    Serial.println("尝试连接MQTT服务器");
    if (client.connect(CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println("MQTT连接成功！");
      client.subscribe(CMD_TOPIC);
      client.subscribe(PULSE_CMD_TOPIC);
      client.subscribe(OTA_TOPIC);
      client.subscribe(OTA_DATA_TOPIC);
      client.publish(STATE_TOPIC, switchState ? "ON" : "OFF");
      lastMqttCmdTime = millis();
    } else {
      Serial.print("连接失败，错误码：");
      Serial.println(client.state());
      delay(3000);
    }
  }
}

//--------------- 休眠 ----------------
void enterDeepSleepOnlyResetWakeup() {
  ledFastBlink(3);
  Serial.println("======================");
  Serial.println("即将进入深度睡眠！");
  Serial.println("仅RST复位可以唤醒");
  Serial.println("======================");
  Serial.flush();
  delay(200);
  ESP.deepSleep(0);
}

//--------------- setup ----------------
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(PULSE_PIN, OUTPUT);
  digitalWrite(PULSE_PIN, HIGH);   // 脉冲空闲时为高电平

  bootStartTime = millis();
  lastMqttCmdTime = millis();
  lastAutoPulseTime = millis();

  espClient.setInsecure();
  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setCallback(callback);

  wifiStartFlag = false;
  otaInProgress = false;
}

//--------------- loop ----------------
void loop() {
  if (!wifiStartFlag) {
    startWifi();
    wifiStartFlag = true;
  }

  // WiFi慢闪
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnectedFlag = false;
    unsigned long nowTmp = millis();
    if (nowTmp - ledLastTick > SLOW_BLINK_INTERVAL) {
      ledLastTick = nowTmp;
      digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    }
  } else {
    if (!wifiConnectedFlag) {
      Serial.println("\nWiFi连接成功 IP:" + WiFi.localIP().toString());
      wifiConnectedFlag = true;
      ledFastBlink(2);
      digitalWrite(LED_PIN, HIGH);
    }
  }

  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    mqttReconnect();
  }
  client.loop();

  unsigned long now = millis();

  // 心跳
  if (client.connected() && (now - lastHeartbeat >= HEARTBEAT_INTERVAL)) {
    client.publish(HEARTBEAT_TOPIC, "online");
    lastHeartbeat = now;
  }

  // ---- GPIO5 脉冲处理 ----
  // 1) 结束当前脉冲
  if (pulseActive && (now - pulseStartTime >= PULSE_LOW_MS)) {
    digitalWrite(PULSE_PIN, HIGH);
    pulseActive = false;
    Serial.println("GPIO5脉冲结束，恢复高电平");
  }
  // 2) 自动周期触发（1小时一次）
  if (now - lastAutoPulseTime >= PULSE_PERIOD_MS) {
    lastAutoPulseTime = now;
    if (!pulseActive) {
      Serial.println("自动周期触发GPIO5脉冲");
      digitalWrite(PULSE_PIN, LOW);
      pulseStartTime = now;
      pulseActive = true;
    }
  }

  // ---- 休眠判断（保持原逻辑） ----
  bool bootTimeout = (now - bootStartTime >= BOOT_MAX_LIFE_MS);
  bool mqttIdleTimeout = (client.connected() && (now - lastMqttCmdTime >= MQTT_IDLE_TIMEOUT_MS));
  if (bootTimeout || mqttIdleTimeout) {
    if (bootTimeout) Serial.println("【开机兜底超时5分钟，进入休眠】");
    else Serial.println("【MQTT空闲5分钟未收到指令，进入休眠】");
    // enterDeepSleepOnlyResetWakeup();  // 如需休眠取消注释
  }

  yield();
}