# 喵伴云管 1.3.1 升级说明

本版本可从公网现有 1.2.2 直接升级，包含 1.3.0 的主动聆听、中文语音识别与语音合成，并增加可控调试采样、浏览器试听、诊断指标、自动清理和管理员删除功能。ESP32-S3 的 2.3.0 固件已经兼容本版本，无需重新烧录。

## 升级前

1. 备份当前部署目录中的 `.env`。
2. 确认 Docker 数据卷 `pet-companion-cloud-data` 存在，不要删除或重新命名。
3. 保留 Apache 现有证书、默认 443 虚拟主机和 `/pet-companion/` 反向代理配置。

```bash
docker volume inspect pet-companion-cloud-data
docker cp pet-companion-cloud:/app/data ./pet-companion-data-backup-$(date +%Y%m%d-%H%M%S)
```

## 部署

将压缩包解压到新目录，再把服务器原 `.env` 复制到新目录。管理员令牌和设备令牌继续使用原值，不要使用 `.env.example` 覆盖真实 `.env`。

在 `.env` 末尾增加：

```dotenv
PET_CLOUD_AMBIENT_DEBUG_AUDIO=false
PET_CLOUD_AMBIENT_DEBUG_RETENTION_DAYS=3
PET_CLOUD_AMBIENT_DEBUG_MAX_MB=200
```

进入新目录执行：

```bash
docker compose up -d --build
docker compose ps
docker compose logs --tail=150 pet-cloud
```

首次部署语音引擎会下载并写入镜像的 SenseVoice 和 VITS 模型。Docker 已把模型下载层放在应用源码层之前，后续代码升级会优先复用模型缓存。

## 验证

```bash
curl -sS https://zkff.ai/pet-companion/health
```

返回结果应满足：

- `ok` 为 `true`。
- `version` 为 `1.3.1`。
- `speech.ready`、`speech.asr_ready`、`speech.tts_ready` 均为 `true`。

进入 `https://zkff.ai/pet-companion/` 并使用原管理员令牌登录：

1. 确认原设备、聊天和照片仍然存在。
2. 打开设备的“聆听”设置。
3. 开启“保存调试音频”，保持 3 天和 200 MB 的初始限制。
4. 设备下一次心跳后，主动互动页面应出现转写、音量指标和处理耗时。
5. 有 WAV 的记录可以直接试听；原始包用于核对设备上传内容。

## 数据与权限

- 调试采样默认关闭，只能由管理员为指定设备开启。
- 设备继续只上传本地 VAD 判断为人声的短片段，不上传连续环境音频。
- 调试文件保存在 Docker 数据卷的 `/app/data/ambient-debug`。
- WAV 和原始 CAP1 下载接口都要求管理员令牌，设备令牌不能读取。
- 超过保留天数或容量上限时，从最旧音频开始自动清理；转写和诊断元数据继续保留。
- 管理页可以清空某台设备的全部调试音频，也可以删除单条完整记录。

## 回退

进入旧部署目录并执行 `docker compose up -d --build` 即可回退应用。继续使用同一个 `.env` 和 `pet-companion-cloud-data` 数据卷，原设备、聊天、照片及主动互动记录都会保留。
