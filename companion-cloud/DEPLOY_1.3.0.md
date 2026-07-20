# 喵伴云管 1.3.0 升级说明

本版本增加主动聆听、中文语音识别、回应决策、中文语音合成，以及管理页中的主动互动设置与事件记录。原有设备、聊天、照片、远程拍照、主动问候和 OTA 数据结构继续兼容。

## 升级前

1. 备份当前部署目录中的 `.env`。
2. 确认 Docker 数据卷 `pet-companion-cloud-data` 存在，不要删除或重新命名该数据卷。
3. 保留 Apache 现有 `/pet-companion/` 反向代理和证书配置。

```bash
docker volume inspect pet-companion-cloud-data
docker cp pet-companion-cloud:/app/data ./pet-companion-data-backup-$(date +%Y%m%d-%H%M%S)
```

## 部署

将压缩包解压到一个新目录，再把旧部署目录中的 `.env` 复制到新目录。不要使用 `.env.example` 覆盖服务器现有 `.env`。

检查 `.env` 中下列基础配置仍然存在：

```dotenv
PET_CLOUD_DATA_DIR=./data
PET_CLOUD_ROOT_PATH=/pet-companion
PET_CLOUD_AMBIENT_DEFAULT_ENABLED=true
PET_CLOUD_TIMEZONE=Asia/Shanghai
PET_CLOUD_SPEECH_THREADS=2
PET_CLOUD_TTS_SID=1
PET_CLOUD_TTS_SPEED=1.08
```

管理员令牌和设备令牌继续使用服务器原值。进入新目录后执行：

```bash
docker compose up -d --build
docker compose ps
docker compose logs --tail=100 pet-cloud
```

首次构建会下载并写入镜像的中文识别和语音合成模型，下载量约 350 MB。以后只修改应用代码时，Docker 会复用模型层缓存。

## 验证

```bash
curl -sS https://zkff.ai/pet-companion/health
```

健康检查应满足：

- `ok` 为 `true`。
- `version` 为 `1.3.0`。
- `speech.ready`、`speech.asr_ready`、`speech.tts_ready` 均为 `true`。

随后进入 `https://zkff.ai/pet-companion/`：

1. 输入原管理员令牌。
2. 确认设备列表和历史数据仍然存在。
3. 打开“主动互动”，设置启用状态、冷却时间、每小时上限和安静时段。
4. 桌宠下一次心跳后应显示主动聆听已启用；无需再次烧录。

## 运行规则

- 设备只在空闲状态启用本地 VAD，不影响三个唤醒词和小智正常对话。
- 只有检测到的人声短片段会上传；原始环境音频仅在内存中处理，不写入服务器磁盘。
- 服务器只保存转写文本、决策结果和回应依据。
- 语音模型不可用、后台关闭主动聆听、进入安静时段或触发频率上限时，桌宠不会主动回应。
- 公网服务仍为旧版本时，2.3.0 固件会自动关闭环境上传，原有功能继续运行。

## 回退

如需回退应用代码，进入旧部署目录并执行 `docker compose up -d --build`。继续使用同一个 `pet-companion-cloud-data` 数据卷和原 `.env`，历史数据不会因应用回退而丢失。
