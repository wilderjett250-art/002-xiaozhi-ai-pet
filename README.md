# Project 002 · XiaoZhi Cat AI Desktop Companion

[中文说明](README_zh.md) | [English Documentation](README_en.md)

## 中文

这是“100 个项目”计划中的第 **002** 个项目：一个基于 ESP32-S3 与 XiaoZhi ESP32 的猫形 AI 桌面伙伴。仓库包含设备固件、猫咪表情与语音交互、摄像头、ML307 Cat.1 4G / Wi-Fi 双网络，以及配套的 Companion Cloud 服务。

主要能力：

- “嗨喵喵 / 小喵小喵 / 你好小智”离线唤醒与流式语音对话
- 240 × 240 ST7789 显示、猫咪表情、音乐与主动问候
- OV3660 摄像头、设备状态遥测、OTA 与启动自检
- ML307 Cat.1 4G 主链路和 Wi-Fi 备用链路
- Companion Cloud 设备 API、Web 控制台、提醒与环境感知服务

完整的硬件说明、构建方法、云端部署和验证边界请阅读 [README_zh.md](README_zh.md)。

## English

This is project **002** in the “100 Projects” series: a cat-shaped AI desktop companion built on ESP32-S3 and XiaoZhi ESP32. The repository contains device firmware, cat expressions and voice interaction, camera support, ML307 Cat.1 4G / Wi-Fi dual networking, and the Companion Cloud service.

Highlights:

- Offline wake phrases and streaming voice conversation
- 240 × 240 ST7789 display, cat expressions, music, and proactive greetings
- OV3660 camera, device telemetry, OTA, and boot self-test
- ML307 Cat.1 4G primary link with Wi-Fi fallback
- Companion Cloud device APIs, web console, reminders, and ambient awareness

See [README_en.md](README_en.md) for hardware notes, build steps, cloud deployment, and validation scope.

## Repository Layout

```text
main/                ESP32 firmware and board implementations
main/assets/         Audio and cat-expression assets
companion-cloud/     Companion Cloud service, web UI, and tests
partitions/          Flash partition layouts
scripts/             Firmware build and asset tools
docs/                Upstream XiaoZhi protocol and hardware documentation
```

This project keeps the upstream MIT license. See [LICENSE](LICENSE).
