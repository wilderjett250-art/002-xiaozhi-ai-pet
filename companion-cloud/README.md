# 喵伴云管

喵伴云管与现有小智对话服务并行运行，用于保存设备聊天文本、保存明确触发的照片、向在线设备下发远程拍照和主动问候命令，并通过双 OTA 分区远程更新桌宠固件。1.3.1 包含主动聆听和可控调试采样：设备本地 VAD 只截取短人声，云端完成中文转写、回应决策与语音合成；管理员可在排查期间保存对应的原始 CAP1 包和可播放 WAV。

设备心跳同步当前链路、4G 主网络、Wi-Fi 备用网络、自动故障切换开关和链路连接状态，管理页集中显示双网运行策略。

## 数据规则

- 聊天记录保存用户识别文本与桌宠回复文本，不保存原始麦克风音频。
- 调试采样默认关闭。关闭时主动聆听的 Opus 片段仅在内存中解码，处理后不落盘；后台保留转写、决策、回应依据、设备网络快照、音量指标和处理耗时。
- 管理员为指定设备打开调试采样后，服务器额外保存原始 CAP1 和可播放 WAV；默认保留 3 天、每设备上限 200 MB，超期或超额时自动从旧记录开始清理。
- 调试音频只能通过管理员令牌读取、下载或删除。关闭调试采样不会删除已有诊断文字，管理页可单独清空音频或删除整条记录。
- 管理页可设置主动聆听开关、互动冷却、每小时上限、安静时段、调试音频开关、保留天数和容量上限。语音引擎不可用时，设备不会开启环境上传，普通唤醒对话保持不变。
- 照片仅接受 `voice`（用户明确要求拍照）和 `admin`（管理页按钮拍照）来源。
- 自动观察 `auto` 不进入照片接口，服务端类型校验也会拒绝该来源。
- 后台远程拍照会在设备屏幕显示拍照状态，形成可见提示。
- 后台“打招呼”按钮让在线桌宠本地播报“你在干嘛呀？”，随后自动进入聆听。
- 设备上传的聊天和照片时间会与服务器接收时间核对，异常时区偏差会自动纠正。

## 本地启动

```powershell
cd G:\xiaozhiai\xiaozhi-esp32-official\companion-cloud
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe scripts\download_speech_models.py --target .\models
$env:PET_CLOUD_ADMIN_TOKEN = "your-admin-token"
$env:PET_CLOUD_DEVICE_TOKEN = "your-device-token"
$env:PET_CLOUD_DATA_DIR = "./data"
$env:PET_CLOUD_ROOT_PATH = "/pet-companion"
$env:PET_CLOUD_SPEECH_MODEL_DIR = ".\models"
.\.venv\Scripts\python.exe -m uvicorn app:app --host 0.0.0.0 --port 8765
```

浏览器访问 `http://127.0.0.1:8765`，输入 `PET_CLOUD_ADMIN_TOKEN` 后即可进入管理页。

## 公网运行

4G 设备需要可公网访问的 HTTPS 地址。反向代理应将 `/pet-companion` 请求转发到本服务，并启用有效 TLS 证书。设备固件中的服务地址配置为该 HTTPS 根地址，设备令牌配置为 `PET_CLOUD_DEVICE_TOKEN`。

运行数据位于 `companion-cloud/data`：SQLite 数据库保存设备、对话、照片和主动互动元数据，`data/photos` 保存原图，`data/speech` 暂存带随机签名的桌宠回应语音并在 7 天后清理，`data/ambient-debug` 保存管理员明确开启的调试音频。部署时应备份整个数据目录。

### Apache 与 ML307R 兼容配置

ML307R 的 HTTPS 客户端不会发送 TLS SNI。Apache 服务器必须把 `zkff.ai:443` 对应的 SSL 虚拟主机设为该地址的默认虚拟主机，否则设备会收到 `421 Misdirected Request`，浏览器访问仍可能正常。

1. 将 `zkff.ai` 的 `<VirtualHost *:443>` 配置文件调整为同一 IP 的首个 443 虚拟主机，常见做法是把文件命名为 `000-zkff-ssl.conf` 并优先启用。
2. 在该虚拟主机中保留 `ServerName zkff.ai`、现有证书配置和 `/pet-companion` 反向代理规则。
3. 运行 `apachectl configtest`，确认返回 `Syntax OK` 后执行平滑重载。
4. 使用不带 SNI、但带 `Host: zkff.ai` 的 TLS 请求检查 `/pet-companion/health`，状态应为 `200`；桌宠串口日志中的心跳状态也应从 `421` 变为 `200`。

不要把云端地址降级为 HTTP。设备令牌、聊天记录、远程拍照和 OTA 命令必须继续通过 HTTPS 传输。

## Docker 部署

1. 首次部署时将 `.env.example` 复制为 `.env`，设置两项随机令牌，并保留 `PET_CLOUD_DATA_DIR=./data` 和 `PET_CLOUD_ROOT_PATH=/pet-companion`。升级部署时保留服务器原有 `.env`，不要用示例文件覆盖真实令牌。
2. 在当前目录运行 `docker compose up -d --build`。
3. 运行 `docker compose ps`，确认服务状态为 `healthy`。
4. 使用 Nginx 或 Caddy 将公网 HTTPS 域名的 `/pet-companion` 路径反向代理到 `127.0.0.1:8765`，请求体上限设置为 6 MB，并传递原始协议头。
5. 访问 `https://你的域名/pet-companion/health`，应返回包含 `"version":"1.3.1"` 且 `speech.ready` 为 `true` 的健康状态。

首次 Docker 构建会下载约 350 MB 的 SenseVoice int8 中文识别模型和 VITS 中文语音模型。模型写入镜像，容器重启不会重复下载。`PET_CLOUD_LLM_*` 三项为空时使用本地规则判断是否主动回应；接入 OpenAI 兼容模型后会自动切换为语义决策。

## 远程固件更新

1. 设备正常联网并在管理页显示在线。
2. 点击设备行的“升级”，选择本项目编译得到的 `xiaozhi.bin`，填写版本号并确认。
3. 服务端校验 ESP32 固件头、文件大小与 SHA-256 后下发更新命令。
4. 设备下载到备用 OTA 分区，核对 SHA-256，通过后切换分区并自动重启；下载或校验失败时继续运行原固件。

当前 ESP32-S3 16 MB Flash 布局为两个 5 MiB 应用槽和一个 6016 KiB 资源分区。远程更新只上传 `xiaozhi.bin`，不要上传合并镜像、资源镜像或 ZIP 文件。

Docker 部署的数据保存在 `pet-companion-cloud-data` 数据卷中；直接运行 Python 时则保存在 `PET_CLOUD_DATA_DIR` 指定的目录。令牌仅保存在服务器 `.env` 与设备固件配置中，不应放入公开代码仓库。

## 接口验证

```powershell
.\.venv\Scripts\python.exe -m pip install -r requirements-dev.txt
.\.venv\Scripts\python.exe -m pytest -q
```
