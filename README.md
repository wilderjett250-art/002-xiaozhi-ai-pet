# 002 小智 AI 桌宠 | XiaoZhi AI Pet

> 让 ESP32-S3 设备拥有语音、表情、摄像头、网络连接和云端陪伴能力。
>
> **English:** A practical, runnable project with a documented workflow for the problem described above.

## 项目展示 / Demo

![设备与云端流程](docs/mcp-based-graph.jpg)

## 解决什么问题 / Problem

解决硬件设备只有单一交互入口的问题，把唤醒、对话、表情、遥测和云端控制整合到一个桌面伙伴中。

**English:** This project addresses the problem above with a reproducible local workflow.

## 有什么用 / Use

刷入固件后可进行语音对话、屏幕表情展示、设备状态上报、OTA 更新和 Companion Cloud 控制。

**English:** Run the workflow locally, inspect the output, and extend the project from the provided source.

## 高光亮点 / Highlights

- ESP32-S3 固件与多板卡适配
- 离线唤醒、流式语音和猫咪表情
- ML307 Cat.1 4G + Wi-Fi 双网络
- Companion Cloud API、Web 控制台、提醒与环境感知

## 技术名词 / Tech

`ESP-IDF · ESP32-S3 · C/C++ · MQTT/WebSocket · Python · Cloud API`

## 从 ZIP 开始复现 / Reproduce from ZIP

1. 下载 ZIP 并解压，安装 ESP-IDF 和对应工具链。
2. 按 docs/custom-board_zh.md 选择开发板和 sdkconfig。
3. 进入仓库根目录执行 idf.py set-target esp32s3。
4. 执行 idf.py build、idf.py flash monitor。
5. Companion Cloud 进入 companion-cloud，按其 README 安装依赖并配置环境变量。

**Expected result:** 设备启动后显示表情并进入语音交互；云端服务运行后可在控制台查看设备和提醒。

## 目录提示 / Notes

- 先阅读本 README，再按项目内更详细的中文/英文文档补充配置。
- 不要把真实密码、Token、数据库业务数据和本机运行结果提交回仓库。
- 下载 ZIP 后的第一次运行应使用测试数据或示例图片，确认链路正常后再接入自己的环境。

[English documentation](README.en.md)
