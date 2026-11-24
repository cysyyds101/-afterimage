# 蓝发鲸鱼女仆 · ESP32-S3 AMOLED 动作项目

本项目对应 **Waveshare ESP32-S3-Touch-AMOLED-2.06**，屏幕 410×502。已经完成角色透明底素材、四组循环动画、电脑预览和 ESP-IDF 按钮切换程序。

## 先看效果

- `videos/all_actions.mp4`：四个动作合集，20 秒。
- `videos/idle.mp4`、`wave.mp4`、`jump.mp4`、`sleep.mp4`：单独的动作视频。
- `assets/character.png`：1254×1254 RGBA 透明底角色。
- `preview/actions.jpg`：四种动作的静态对照。

角色由原始截图复原，保留蓝色渐变长发、大眼睛、女仆头饰和鲸鱼尾巴；原图被字幕遮挡的裙摆、画面之外的腿和鞋由 AI 补绘。原始生成使用内置 imagegen；完整提示词保存在 `assets/prompts.json`。

| 动作 | 单次循环 | 主要变化 |
|---|---:|---|
| 待机眨眼 idle | 2.0 秒 | 轻微呼吸、眨眼、发梢摆动 |
| 挥手招呼 wave | 1.2 秒 | 抬起右手，向外挥动 |
| 开心跳跃 jump | 1.2 秒 | 蹲下、起跳、空中开心表情、落地 |
| 困倦点头 sleep | 3.2 秒 | 闭眼、低头、慢慢抬头 |

每个动作有 8 个绘制关键帧，采用分段停留的 2D 动画。MP4/AVI 的容器帧率为 20 fps，部分帧重复保持姿势；它不是每秒 20 张独立生成的新姿势。所有动作无声，背景为黑色，保留整个人物与尾巴。

## 板子上的操作

1. 开机默认播放待机眨眼。
2. **短按 BOOT**：待机 → 挥手 → 跳跃 → 困倦 → 待机。
3. **长按 BOOT 至少 0.8 秒**：回到待机。
4. **轻点屏幕**：切换到下一个动作。
5. 每个动作持续循环，直到再次操作。

## 实时语音对话

新版固件增加了按键对讲和首次联网配置：

1. 第一次启动时，屏幕显示 `Qiban-XXXX` 热点名称、密码 `qiban1234` 和配置地址。
2. 也可以先打开手机的 2.4 GHz 热点，从屏幕顶部向下滑，在 Wi-Fi 面板中点选热点并用屏幕键盘输入密码。面板会显示当前 SSID、IP、信号和附近网络。
3. 备用方式是连接板子的 `Qiban-XXXX` 热点并打开 `http://192.168.4.1`；这里还可填写语音桥 WebSocket 地址和设备令牌，保存后设备自动重启。
4. 连接完成后，按住 BOOT 说话，松开发送；屏幕依次显示 Listening、Thinking、Speaking。
5. 语音模式下 BOOT 专用于对讲，轻点屏幕仍可切换固定动作。
6. 要更换 Wi-Fi、语音桥或设备令牌时，长按屏幕约 1 秒；设备会清除联网配置并重新打开 `Qiban-XXXX` 配网热点，不需要 USB。

语音桥位于 `voice-bridge`。它把 16 kHz PCM 录音依次发送到平台统一配置的语音识别、用户选择的聊天大模型和平台统一配置的语音合成服务。设备只保存 Wi-Fi、桥地址和设备令牌，不保存任何模型密钥。

在未配置真实语音服务前，可以将语音桥设为 `MOCK_MODE=echo`。此模式会把板子刚录到的声音播放回来，用来检查麦克风、网络和扬声器。

BOOT 使用 GPIO0，低电平为按下，程序做了 35 ms 消抖。正常开机时松开 BOOT；按住它复位会进入 ESP32 下载模式。PWR 保持电源按钮功能。

## 烧录预编译固件

`release` 内包含本次已编译的应用与动画资源。使用 ESP-IDF 终端或已安装 esptool 的 Python 环境执行：

```powershell
cd D:\work\whale-maid\release
python -m esptool --chip esp32s3 --port COM5 --baud 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-size 32MB --flash-freq 80m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 whale_maid.bin 0x410000 storage.bin
```

把 `COM5` 替换成设备实际端口。该命令会替换板上程序及本项目分区范围内的数据。必须同时烧录 `storage.bin`，否则没有动画资源。也可在 ESP-IDF 环境运行 `release/flash.ps1 -Port COM5`。

本次已于 2026-11-05 **烧录至 COM7 上的 ESP32-S3**，四个分区文件写入后校验通过。重启日志确认运行 `whale_maid`，PSRAM 自检和 LCD 初始化成功；18 秒日志观察期间未见应用报错或崩溃。屏幕的实际观感及实体按键/触摸操作仍需目视确认。烧录和启动记录位于 `release/flash-device.log`、`release/boot-device.log`。

实时语音版已于 2026-11-09 再次烧录至 COM7。启动日志确认 32 MB Flash、8 MB PSRAM、LCD、ES7210 双麦克风、ES8311 扬声器和 Wi-Fi 均正常初始化，并成功创建 `Qiban-1CC9` 配网热点。当前版本还确认以 AP+STA 双模式启动并扫描到 12 个附近网络；触屏下拉选网固件已写入，屏幕实际触摸手感仍需用户在板上目视确认。预编译文件与自动烧录脚本位于 `release-voice`。

## 从源码构建

源码在 `firmware`，本次使用本机 ESP-IDF 6.1 工具链构建，核心依赖固定为 Waveshare BSP 2.0.0、LVGL 9.5.0、esp_new_jpeg 1.0.2。首次在另一台电脑构建时，组件管理器需要联网下载依赖。

在已激活 ESP-IDF 的终端执行：

```powershell
cd D:\work\whale-maid\firmware
idf.py build
idf.py -p COM5 flash monitor
```

本机也可运行 `tools/build.ps1`。仅在显式传入 `-Flash -Port COM5` 时，该脚本才执行烧录。不要在 Windows PowerShell 的默认环境中直接运行未激活的 `idf.py`。

## 素材和播放器的关系

- 屏幕为 410×502；固件中的 JPEG 画布为 400×480，居中显示，边缘留黑。这一尺寸按 JPEG 解码块对齐。
- 当前固件逐帧解码 `firmware/spiffs` 中的 32 个 JPEG，播放节奏与视频完全一致，资源约 1.25 MiB，不需要 TF 卡。
- 解码缓冲和显示缓冲分离；写入画布时持有 LVGL 锁，避免同时读写画面。
- MP4 用于电脑/手机预览。AVI 为 MJPEG / YUV420 文件，便于以后接入视频播放器；不能直接把这组全尺寸 AVI 塞进官方限制 320×200 的原始示例。
- `assets/*_atlas.png` 是八帧动作图集，`assets/frames` 为对应的 400×480 PNG 帧。
- 修改动作后运行 `python tools/export_assets.py`，即可重新生成视频、JPEG、清单及 C 语言时序头文件，再重新构建固件。

## 已完成的验证

- 透明底主图具有真实 alpha 通道。
- 32 个 JPEG 全部为 baseline、4:2:0、400×480，单帧小于播放器 128 KiB 输入缓冲。
- 8 个单独 MP4/AVI 全部完整解码通过；分辨率、帧率、循环时长正确。
- 四组帧都有实际画面变化，四角保持黑色。
- 浏览器中检查了动作按钮、循环和暂停/继续。
- 固件编译和分区大小检查通过；详细素材验证结果见 `verification.json`。

## 官方参考

- [板卡规格、BOOT 按键说明](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06)
- [官方 ESP-IDF / Arduino 工程](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06)

本项目独立保存在 `D:\work\whale-maid`，现有 `ws206` 和旧素材工程不受本次修改影响。
