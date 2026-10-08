一个基于 ESP8266 + MQTT + Web 的远程继电器控制项目，支持网页控制继电器开关、GPIO5 脉冲输出、远程 OTA 固件升级、心跳在线检测。
📖 项目简介
本项目由 ESP8266 固件 和 网页控制面板 两部分组成，通过 EMQX Cloud (MQTT over TLS/WebSocket) 进行公网通信，实现以下功能：

功能	说明
🔌 继电器远程开关	网页一键控制，GPIO0（或自定义引脚）输出高低电平
⚡ GPIO5 脉冲输出	手动触发 + 1 小时自动周期触发，低电平持续 600ms
📦 Web OTA 公网远程升级	网页选择 .bin 文件，通过 MQTT 分片传输固件，无需 USB
💓 心跳在线检测	ESP 定时上报心跳，网页实时显示设备在线/离线状态
💡 LED 状态指示	未联网慢闪，联网后熄灭
项目/
├── esp8266_switch.ino      # ESP8266 固件代码（Arduino IDE）
└── index.html              # 网页控制面板
<img width="594" height="856" alt="7556ad90-60df-4816-ac25-aed58a3c2382" src="https://github.com/user-attachments/assets/72af0646-8988-4951-b36b-61c6ef827581" />



