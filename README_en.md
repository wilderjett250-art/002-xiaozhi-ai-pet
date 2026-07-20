# Project 002: XiaoZhi Cat AI Desktop Companion

[Bilingual Home](README.md) | [中文](README_zh.md)

## Overview

This project extends the open-source XiaoZhi ESP32 firmware for an ESP32-S3 N16R8 cat-shaped desktop companion. The device combines voice conversation, cat expressions, camera input, music, proactive interaction, and 4G/Wi-Fi dual networking. Its Companion Cloud service provides device connectivity, reminders, ambient awareness, and a browser-based management console.

It is project **002** in the “100 Projects” series. The Git history is intentionally organized as a development progression: upstream baseline, hardware support, interaction features, cloud service, and documentation.

## Features

- Three offline Chinese wake phrases: “Hai Miao Miao,” “Xiao Miao Xiao Miao,” and “Ni Hao Xiao Zhi.”
- Streaming ASR, LLM, and TTS voice interaction with multi-turn conversation.
- 240 × 240 ST7789 display with a complete cat-expression asset set.
- OV3660 camera, proactive observation, device telemetry, and boot self-test.
- Music playback, proactive greetings, reminders, focus timers, and pet-state management.
- ML307/ML307-DL Cat.1 4G primary networking with Wi-Fi fallback.
- OTA, MQTT/WebSocket communication, and device-side MCP extensions.
- Companion Cloud APIs, web console, Docker deployment, and automated tests.

## Hardware

| Module | Configuration |
| --- | --- |
| MCU | ESP32-S3 N16R8 |
| Display | ST7789, 240 × 240; the second hardware track supports 240 × 320 |
| Camera | OV3660 |
| Microphone | INMP441 / ICS43434-class I2S digital microphone |
| Audio output | MAX98357A-class I2S amplifier and speaker |
| Cellular modem | ML307R / ML307-DL Cat.1 4G |
| Network policy | 4G primary, Wi-Fi fallback |

Always use the `config.h` and `sdkconfig.defaults.*` files for the selected board as the authoritative GPIO source.

## Structure

```text
main/
├─ boards/                         Board implementations and GPIO definitions
│  ├─ bread-compact-wifi-s3cam/   Primary cat device and camera track
│  ├─ bread-compact-ml307/        ML307 breadboard track
│  └─ lelian-mlr-s3-draft/        Second ESP32-S3 + ML307-DL track
├─ assets/                         Prompt audio and cat expressions
├─ audio/                          Audio pipeline, wake words, and music player
├─ companion_cloud.*               Device-side Companion Cloud client
└─ application.*                   Main device workflow

companion-cloud/                   Python service, web console, and tests
partitions/v2/                     Version 2 flash partition tables
scripts/                           Build and asset-generation tools
docs/                              Upstream XiaoZhi protocol and hardware docs
```

## Firmware Build

ESP-IDF 5.4 or later is recommended. The development environment used ESP-IDF 5.5.2.

```powershell
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

Project-specific configuration references include:

- `sdkconfig.defaults.s3cam-local` for the primary 240 × 240 cat device and camera track.
- `sdkconfig.defaults.s3-ml307dl-second` for the second ESP32-S3 + ML307-DL track.
- `sdkconfig.defaults.com13-ml307dl` for focused ML307-DL integration work.
- `sdkconfig.defaults.lelian-mlr-s3-draft` for the Lelian MLR S3 draft board.

Before building or flashing, confirm the board type, partition table, networking, and Companion Cloud settings in `menuconfig`. Device tokens are deployment secrets and must remain outside Git.

## Companion Cloud

```powershell
cd companion-cloud
Copy-Item .env.example .env
docker compose up -d --build
```

For local development, create a Python virtual environment and install `requirements.txt` plus `requirements-dev.txt`. Run the test suite with:

```powershell
python -m pytest -q
```

`.env.example` documents the configuration shape with example values. Put real tokens, URLs, and deployment settings only in the local or server-side `.env` file.

## Validation Scope

The primary device track has covered ESP32-S3, PSRAM, flash, display, LVGL, camera, microphone, audio output, cat assets, all three wake phrases, idle interaction, ML307 4G, SIM/PDP, HTTPS, OTA, MQTT, and network failover paths.

The second ESP32-S3 + ML307-DL track has its own board configuration and has been used for 4G XiaoZhi link development. Connecting it to a different private Companion Cloud requires a device-specific service URL and token followed by full validation on that hardware.

## Repository and Security Boundary

The repository contains reproducible source code, tests, documentation, and assets. Real `.env` files, device tokens, flash/NVS backups, runtime databases, virtual environments, build caches, and historical release archives are excluded.

## License

This project retains the upstream MIT License. See [LICENSE](LICENSE). Product distributions must also comply with the licenses of all included third-party components and model assets.
