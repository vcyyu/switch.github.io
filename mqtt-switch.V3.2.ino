#include <ESP8266WiFi.h>

// ⚠️⚠️⚠️ 必须在 PubSubClient.h 之前！
// PubSubClient 默认缓冲区只有 256 字节，装不下 OTA 分片，必须加大！
#define MQTT_MAX_PACKET_SIZE 4096

#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <Updater.h>

//====================【修改全部参数】====================
const char* WIFI_SSID     = "CMCC-eujw-sanlou";
const char* WIFI_PASS     = "gmw18479639832";
const char* MQTT_HOST     = "i11884f9.ala.cn-hangzhou.emqxsl.cn";
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "esp-switch01";
const char* MQTT_PASS     = "switch666888";
const char* CLIENT_ID     = "esp01_switch_001";

#define CMD_TOPIC    "light/switch/cmd"
#define STATE_TOPIC  "light/switch/state"
#define HEARTBEAT_TOPIC "device/heartbeat"

#define PULSE_CMD_TOPIC   "device/gpio5/pulse"
#define OTA_TOPIC         "device/ota"
#define OTA_DATA_TOPIC    "device/ota/data"
#define OTA_STATUS_TOPIC  "device/ota/status"

#define RELAY_PIN     2
#define LED_PIN       2
#define PULSE_PIN     5

#define HEARTBEAT_INTERVAL 30000UL
#define PULSE_LOW_MS       600UL
#define PULSE_PERIOD_MS    3600000UL

#define MQTT_IDLE_TIMEOUT_MS  (5UL * 60UL * 1000UL)
#define BOOT_MAX_LIFE_MS      (5UL * 60UL * 1000UL)

#define OTA_RECV_TIMEOUT_MS   30000UL   // 30秒没收到分片就中止
//============================================================

WiFiClientSecure espClient;
PubSubClient client(espClient);

bool switchState = false;
unsigned long bootStartTime = 0;
unsigned long lastMqttCmdTime = 0;
unsigned long lastHeartbeat = 0;
bool wifiStartFlag = false;

unsigned long ledLastTick = 0;
const unsigned long SLOW_BLINK_INTERVAL = 800;
bool wifiConnectedFlag = false;

unsigned long pulseStartTime = 0;
bool pulseActive = false;
unsigned long lastAutoPulseTime = 0;

// OTA
bool otaInProgress = false;
size_t otaTotalSize = 0;
size_t otaReceived = 0;
size_t otaChunkSize = 1024;
uint32_t otaNextSeq = 0;
unsigned long otaLastRecvTime = 0;

//--------------- LED ----------------
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

//--------------- 发送ACK ----------------
void publishOtaAck(uint32_t seq) {
  char buf[16];
  snprintf(buf, sizeof(buf), "ack:%u", seq);
  client.publish(OTA_STATUS_TOPIC, buf);
}

//--------------- base64解码 ----------------
int base64Decode(const char* in, int inLen, uint8_t* out, int outMax) {
  static const char* b64chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int val = 0, valb = -8, idx = 0;
  for (int i = 0; i < inLen; i++) {
    char c = in[i];
    if (c == '=') break;
    if (c == '\0') break;
    const char* p = strchr(b64chars, c);
    if (!p) continue;
    val = (val << 6) + (int)(p - b64chars);
    valb += 6;
    if (valb >= 0) {
      if (idx >= outMax) return -1;
      out[idx++] = (uint8_t)((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  return idx;
}

//--------------- 处理OTA消息 ----------------
void handleOtaMessage(const String& topic, const String& msg) {
  // ---- 开始 / 结束 ----
  if (topic == OTA_TOPIC) {
    if (msg.indexOf("\"ota_start\"") >= 0) {
      int sizeIdx  = msg.indexOf("\"size\":");
      int chunkIdx = msg.indexOf("\"chunkSize\":");
      if (sizeIdx > 0) {
        int comma = msg.indexOf(',', sizeIdx);
        if (comma < 0) comma = msg.indexOf('}', sizeIdx);
        otaTotalSize = msg.substring(sizeIdx + 7, comma).toInt();
      }
      if (chunkIdx > 0) {
        int brace = msg.indexOf('}', chunkIdx);
        otaChunkSize = msg.substring(chunkIdx + 12, brace).toInt();
      }
      if (otaChunkSize == 0) otaChunkSize = 1024;

      Serial.printf("OTA开始: 总大小=%u, 分片=%u\n", otaTotalSize, otaChunkSize);

      if (Update.begin(otaTotalSize)) {
        otaInProgress = true;
        otaReceived = 0;
        otaNextSeq = 0;
        otaLastRecvTime = millis();
        publishOtaProgress(0);
      } else {
        Serial.println("Update.begin 失败");
        client.publish(OTA_STATUS_TOPIC, "error");
      }
    } else if (msg.indexOf("\"ota_end\"") >= 0) {
      if (otaInProgress) {
        if (otaReceived != otaTotalSize) {
          Serial.printf("OTA字节数不符: 收到%u, 期望%u\n", otaReceived, otaTotalSize);
          client.publish(OTA_STATUS_TOPIC, "error");
          Update.end(false);
          otaInProgress = false;
          return;
        }
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
    return;
  }

  // ---- 分片数据 ----
  if (topic == OTA_DATA_TOPIC) {
    if (!otaInProgress) return;
    otaLastRecvTime = millis();

    int seqIdx  = msg.indexOf("\"seq\":");
    int dataIdx = msg.indexOf("\"data\":\"");
    if (seqIdx < 0 || dataIdx < 0) return;

    int seqComma = msg.indexOf(',', seqIdx);
    uint32_t seq = msg.substring(seqIdx + 6, seqComma).toInt();

    int dataStart = dataIdx + 8;
    int dataEnd   = msg.indexOf('"', dataStart);
    if (dataEnd < 0) return;

    String b64 = msg.substring(dataStart, dataEnd);

    // 重复包：重发 ACK 即可
    if (seq < otaNextSeq) {
      Serial.printf("OTA重复包 seq=%u, 重发ACK\n", seq);
      publishOtaAck(seq);
      return;
    }
    // 乱序：先忽略
    if (seq > otaNextSeq) {
      Serial.printf("OTA乱序: 期望%u, 收到%u\n", otaNextSeq, seq);
      return;
    }

    int b64Len = b64.length();
    int outMax = (b64Len * 3) / 4 + 4;
    uint8_t* outBuf = (uint8_t*)malloc(outMax);
    if (!outBuf) {
      Serial.println("OTA内存不足");
      client.publish(OTA_STATUS_TOPIC, "error");
      otaInProgress = false;
      return;
    }

    int outLen = base64Decode(b64.c_str(), b64Len, outBuf, outMax);
    if (outLen <= 0) {
      Serial.println("base64解码失败");
      free(outBuf);
      client.publish(OTA_STATUS_TOPIC, "error");
      otaInProgress = false;
      return;
    }

    size_t written = Update.write(outBuf, outLen);
    free(outBuf);

    if (written != (size_t)outLen) {
      Serial.printf("OTA写入失败: 期望%u, 实际%u\n", outLen, written);
      client.publish(OTA_STATUS_TOPIC, "error");
      otaInProgress = false;
      return;
    }

    otaReceived += written;
    otaNextSeq++;

    Serial.printf("OTA写入 seq=%u 大小=%u 累计=%u/%u\n",
                  seq, outLen, otaReceived, otaTotalSize);

    publishOtaAck(seq);

    if (otaNextSeq % 10 == 0 || otaReceived >= otaTotalSize) {
      int percent = (otaTotalSize > 0)
                    ? (int)(otaReceived * 100 / otaTotalSize)
                    : 0;
      publishOtaProgress(percent);
    }
  }
}

//--------------- MQTT回调 ----------------
void callback(char* topic, byte* payload, unsigned int length) {
  // ✅ 关键调试日志：能看到 ESP 有没有收到分片，以及分片多少字节
  Serial.printf("收到消息: topic=%s, length=%u\n", topic, length);

  lastMqttCmdTime = millis();
  payload[length] = '\0';
  String msg = String((char*)payload);
  msg.trim();
  String topicStr = String(topic);

  Serial.print("收到MQTT [");
  Serial.print(topicStr);
  Serial.print("]: ");
  Serial.println(msg);

  if (topicStr == OTA_TOPIC || topicStr == OTA_DATA_TOPIC) {
    handleOtaMessage(topicStr, msg);
    return;
  }

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

  if (topicStr == PULSE_CMD_TOPIC) {
    if (!pulseActive) {
      Serial.println("触发GPIO5脉冲（MQTT）");
      digitalWrite(PULSE_PIN, LOW);
      pulseStartTime = millis();
      pulseActive = true;
    }
    return;
  }
}

//--------------- WiFi ----------------
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
  digitalWrite(PULSE_PIN, HIGH);

  bootStartTime = millis();
  lastMqttCmdTime = millis();
  lastAutoPulseTime = millis();

  espClient.setInsecure();
  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setCallback(callback);

  // ✅ 双保险：显式设置 MQTT 缓冲区大小
  client.setBufferSize(4096);

  wifiStartFlag = false;
  otaInProgress = false;
}

//--------------- loop ----------------
void loop() {
  if (!wifiStartFlag) {
    startWifi();
    wifiStartFlag = true;
  }

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

  if (client.connected() && (now - lastHeartbeat >= HEARTBEAT_INTERVAL)) {
    client.publish(HEARTBEAT_TOPIC, "online");
    lastHeartbeat = now;
  }

  // GPIO5 脉冲
  if (pulseActive && (now - pulseStartTime >= PULSE_LOW_MS)) {
    digitalWrite(PULSE_PIN, HIGH);
    pulseActive = false;
    Serial.println("GPIO5脉冲结束，恢复高电平");
  }
  if (now - lastAutoPulseTime >= PULSE_PERIOD_MS) {
    lastAutoPulseTime = now;
    if (!pulseActive) {
      Serial.println("自动周期触发GPIO5脉冲");
      digitalWrite(PULSE_PIN, LOW);
      pulseStartTime = now;
      pulseActive = true;
    }
  }

  // OTA 超时保护
  if (otaInProgress && (now - otaLastRecvTime > OTA_RECV_TIMEOUT_MS)) {
    Serial.println("OTA接收超时，中止");
    Update.end(false);
    otaInProgress = false;
    client.publish(OTA_STATUS_TOPIC, "error");
  }

  // 休眠判断
  bool bootTimeout = (now - bootStartTime >= BOOT_MAX_LIFE_MS);
  bool mqttIdleTimeout = (client.connected() &&
                          (now - lastMqttCmdTime >= MQTT_IDLE_TIMEOUT_MS));
  if (bootTimeout || mqttIdleTimeout) {
    if (bootTimeout) {
      // Serial.println("【开机兜底超时5分钟，进入休眠】");
    } else {
      Serial.println("【MQTT空闲5分钟未收到指令，进入休眠】");
    }
    // enterDeepSleepOnlyResetWakeup();
  }

  yield();
}