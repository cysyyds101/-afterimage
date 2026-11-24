# 栖伴语音桥

设备通过 WebSocket 发送 16 kHz、单声道、16 位 PCM。语音桥调用平台统一配置的语音识别和语音合成服务；聊天模型由设备所属用户配置。设备只持有设备令牌，不会接触平台语音密钥或用户的大模型密钥。

## 本地回声测试

复制 `.env.example` 为 `.env`，设置 `DEVICE_TOKEN` 和 `MOCK_MODE=echo`，然后运行：

```powershell
npm install
.\start.ps1
```

回声模式不调用外部 API。板子说话结束后，会把刚才的录音播放回来，用于验证麦克风、Wi-Fi、WebSocket 和扬声器整条链路。

## 平台模式

部署者配置 `STT_*` 和 `TTS_*`，再设置：

```dotenv
PLATFORM_API_URL=https://你的栖伴平台域名
PLATFORM_BRIDGE_KEY=与平台端相同的高强度随机密钥
```

板子发来设备令牌后，语音桥通过 `POST /api/v1/voice/session` 验证设备并临时取得该用户的聊天模型设置。聊天密钥在平台数据库中以 AES-GCM 加密保存，响应只允许持有 `PLATFORM_BRIDGE_KEY` 的语音桥读取。**每轮对话开始前都会重新向平台取一次配置**，因此在平台 studio 保存的模型/人设修改下一轮即生效，板子无需重连；刷新失败时沿用上一次的有效配置，不中断对话。

接口约定采用 OpenAI 兼容路径：

- `POST /audio/transcriptions`
- `POST /chat/completions`
- `POST /audio/speech`

当前版本是按住说话的半双工模式，可以避免扬声器声音被麦克风再次录入。单设备本地开发时可以留空 `PLATFORM_API_URL`，继续使用 `DEVICE_TOKEN` 与 `CHAT_*`。

## 控制台在平台 studio（唯一入口）

语音桥本身不再提供任何配置页面。聊天模型、人设提示词的编辑，以及设备实时状态
（待机/正在听/思考中/说话中、对话回合数、最近使用的模型），都在栖伴平台
character-cloud 的 studio「设备管理 / API 接入」里完成，页面已自适应手机。

语音桥在每次状态变化、每轮对话结束和设备断开时，向平台
`POST /api/v1/voice/status` 上报（设备令牌 + `PLATFORM_BRIDGE_KEY` 双重鉴权）。
上报失败只记录不含密钥的告警，绝不影响语音对话本身。

本地 `GET /health` 仍可用于部署探活，返回连接数与 STT/TTS 配置状态，不含任何密钥。

测试：`node --test platform-auth.test.mjs`（含每回合重取配置、保存即生效、状态上报、密钥不外泄）。
