# 002 XiaoZhi AI Pet

> An interactive AI hardware companion that connects voice dialogue, screen expressions, telemetry, and cloud control.

## Problem

Voice hardware often exposes only one interaction path, leaving device state, expressions, and cloud control disconnected.

## Demo

![Device and cloud flow](docs/mcp-based-graph.jpg)

Wake-up, dialogue, output, telemetry, and cloud control form one end-to-end device workflow.

## Highlights

- ESP32-S3 firmware with multi-board configuration.
- Offline wake-up, streaming voice, and cat-like expression feedback.
- Dual network access through Wi-Fi and ML307 Cat.1 4G.
- Companion Cloud API, a web console, reminders, and environment sensing.

## Tech

`ESP-IDF · ESP32-S3 · C/C++ · MQTT/WebSocket · Python · Cloud API`

## Reproduce from ZIP

1. Extract the ZIP and install ESP-IDF with the ESP32-S3 toolchain.
2. Choose the board and `sdkconfig` according to `docs/custom-board_zh.md`.
3. Run `idf.py set-target esp32s3` and `idf.py build` from the repository root.
4. Connect the board, run `idf.py flash monitor`, and configure network and cloud services as documented.

**Expected result:** After these steps, you should see the project's page, window, device output, or test result.

## Scope and Safety

This is a hardware project; verify the board, serial port, network credentials, and cloud endpoint belong to your own test environment before flashing.

## Contact

Open to technical exchange.
