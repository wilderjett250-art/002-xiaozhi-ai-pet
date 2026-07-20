# 项目 002：小智猫形 AI 桌面伙伴

[返回双语首页](README.md) | [English](README_en.md)

## 项目简介

本项目基于开源 XiaoZhi ESP32 固件扩展，面向 ESP32-S3 N16R8 猫形桌面伙伴。设备端将语音对话、猫咪表情、摄像头、音乐、主动交互和 4G/Wi-Fi 双网络整合在一起；服务端 Companion Cloud 提供设备接入、提醒、环境感知和网页管理能力。

这是“100 个项目”计划中的第 **002** 个项目。提交历史按开发阶段组织，便于回看基础导入、硬件适配、交互功能、云端服务和文档完善的演进过程。

## 核心功能

- 支持“嗨喵喵”“小喵小喵”“你好小智”三组中文离线唤醒词。
- 支持流式 ASR、LLM、TTS 语音对话与多轮交互。
- 支持 240 × 240 ST7789 显示屏和一套猫咪状态表情资源。
- 支持 OV3660 摄像头、主动观察、设备状态上报和启动自检。
- 支持音乐播放、主动问候、提醒、专注计时和桌宠状态维护。
- 支持 ML307/ML307-DL Cat.1 4G 主链路与 Wi-Fi 备用链路。
- 支持 OTA、MQTT/WebSocket 通信和设备端 MCP 扩展。
- 提供 Companion Cloud API、Web 控制台、Docker 部署文件和自动化测试。

## 主要硬件

| 模块 | 配置 |
| --- | --- |
| 主控 | ESP32-S3 N16R8 |
| 显示 | ST7789，240 × 240；第二硬件路线支持 240 × 320 |
| 摄像头 | OV3660 |
| 麦克风 | INMP441 / ICS43434 类 I2S 数字麦克风 |
| 音频输出 | MAX98357A 类 I2S 功放与扬声器 |
| 蜂窝网络 | ML307R / ML307-DL Cat.1 4G |
| 网络策略 | 4G 主链路，Wi-Fi 备用链路 |

具体 GPIO 定义以对应目录中的 `config.h` 和 `sdkconfig.defaults.*` 为准，不应仅按通用接线图连接。

## 目录结构

```text
main/
├─ boards/                         板级实现与硬件引脚
│  ├─ bread-compact-wifi-s3cam/   猫形主设备与摄像头路线
│  ├─ bread-compact-ml307/        ML307 面包板路线
│  └─ lelian-mlr-s3-draft/        ESP32-S3 + ML307-DL 第二路线
├─ assets/                         提示音与猫咪表情
├─ audio/                          音频、唤醒词与音乐播放器
├─ companion_cloud.*               设备端 Companion Cloud 接入
└─ application.*                   设备主流程

companion-cloud/                   Python 服务、Web 控制台与测试
partitions/v2/                     v2 分区表
scripts/                           构建与资源生成工具
docs/                              XiaoZhi 上游协议与硬件文档
```

## 固件构建

建议使用 ESP-IDF 5.4 或更高版本。已开发环境使用 ESP-IDF 5.5.2。

```powershell
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

仓库提供多组项目配置参考：

- `sdkconfig.defaults.s3cam-local`：猫形主设备、240 × 240 屏幕与摄像头路线。
- `sdkconfig.defaults.s3-ml307dl-second`：第二套 ESP32-S3 + ML307-DL 路线。
- `sdkconfig.defaults.com13-ml307dl`：ML307-DL 专项调试参考。
- `sdkconfig.defaults.lelian-mlr-s3-draft`：乐联 MLR S3 草案板配置。

构建和烧录前，应在 `menuconfig` 中确认板型、分区表、网络参数和 Companion Cloud 地址。设备令牌属于部署配置，不应提交到 Git。

## Companion Cloud

```powershell
cd companion-cloud
Copy-Item .env.example .env
docker compose up -d --build
```

本地开发也可使用 Python 虚拟环境安装 `requirements.txt` 与 `requirements-dev.txt`。运行测试：

```powershell
python -m pytest -q
```

`.env.example` 只保存配置结构和示例值。正式部署时请在本机或服务器的 `.env` 中填写真实令牌、访问地址和服务参数。

## 当前验证范围

主设备路线已经覆盖 ESP32-S3、PSRAM、Flash、显示、LVGL、摄像头、麦克风、音频输出、猫咪资源、三组唤醒词、空闲交互、ML307 4G、SIM/PDP、HTTPS、OTA、MQTT 和网络切换等环节。

第二套 ESP32-S3 + ML307-DL 路线保留了独立板级配置，已经用于 4G XiaoZhi 链路开发；接入不同的私有 Companion Cloud 时，需要为该设备单独配置服务地址与令牌，并按实际硬件重新验证完整链路。

## 安全与仓库边界

仓库只包含可复现的源码、测试、文档和资源，不包含真实 `.env`、设备令牌、Flash/NVS 备份、运行数据库、虚拟环境、构建缓存和历史发布压缩包。

## 开源许可

项目沿用上游 MIT License，详情见 [LICENSE](LICENSE)。基于本项目发布产品时，也请遵守所使用的第三方组件和模型资源许可。
