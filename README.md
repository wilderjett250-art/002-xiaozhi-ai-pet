# 002 小智 AI 桌宠 / XiaoZhi AI Pet

> 把语音对话、屏幕表情、设备遥测和云端控制连接成一个可交互的 AI 硬件伙伴。
>
> **English:** An interactive AI hardware companion that connects voice dialogue, screen expressions, telemetry, and cloud control.

## 解决什么问题 / Problem

语音硬件常只有单一入口，设备状态、表情和云端控制彼此割裂，调试与扩展成本高。

**English:** Voice hardware often exposes only one interaction path, leaving device state, expressions, and cloud control disconnected.

## 项目展示 / Demo

![设备与云端流程 / Device and cloud flow](docs/mcp-based-graph.jpg)

从唤醒到输出是一条完整的设备—云端工作流。

**English:** Wake-up, dialogue, output, telemetry, and cloud control form one end-to-end device workflow.

## 高光亮点 / Highlights

- ESP32-S3 固件与多板卡配置。
  **English:** ESP32-S3 firmware with multi-board configuration.
- 离线唤醒、流式语音和猫咪表情反馈。
  **English:** Offline wake-up, streaming voice, and cat-like expression feedback.
- Wi-Fi 与 ML307 Cat.1 4G 双网络接入。
  **English:** Dual network access through Wi-Fi and ML307 Cat.1 4G.
- Companion Cloud API、Web 控制台、提醒和环境感知。
  **English:** Companion Cloud API, a web console, reminders, and environment sensing.

## 技术名词 / Tech

`ESP-IDF · ESP32-S3 · C/C++ · MQTT/WebSocket · Python · Cloud API`

## 从 ZIP 开始复现 / Reproduce from ZIP

1. 解压 ZIP，安装 ESP-IDF 及 ESP32-S3 工具链。
2. 按 `docs/custom-board_zh.md` 选择开发板和 `sdkconfig`。
3. 在仓库根目录执行 `idf.py set-target esp32s3`、`idf.py build`。
4. 连接开发板后执行 `idf.py flash monitor`，再按文档配置网络和云端服务。

**Expected result:** 完成上述步骤后，应能看到项目的页面、窗口、设备输出或测试结果。

**Expected result:** After these steps, you should see the project's page, window, device output, or test result.

## 范围与安全 / Scope and Safety

这是硬件项目；刷写前必须确认板卡、串口、网络凭据和云端接口均属于自己的测试环境。

**English:** This is a hardware project; verify the board, serial port, network credentials, and cloud endpoint belong to your own test environment before flashing.

## 交流 / Contact

欢迎交流技术。

Open to technical exchange.

[English full version](README.en.md)
