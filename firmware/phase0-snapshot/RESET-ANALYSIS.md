# 阶段 0 记录在案：`rst:0xc` 自发重启归因 + 花屏三条铁律核查

日期：2026-11-16 · 板子：Waveshare ESP32-S3-Touch-AMOLED-2.06 · 端口：COM7
对应计划：`gemini-code-1794823335133.md` 任务 0.1 / 0.2，验收标准「重启原因定位明确并已记录在案」

---

## 1. 结论先行（区分已证实与未证实）

| 项 | 状态 |
|---|---|
| `rst:0xc` 是**软件主动复位**，不是掉电/panic/看门狗 | **已证实** |
| 复位地址落在 `esp_restart_noos` | **已证实**（addr2line） |
| 全固件只有**一个** `esp_restart()` 执行点、**两条**触发路径 | **已证实**（源码穷举） |
| 两条路径中，只有「触屏长按」能在无人操作时自发触发 | **已证实**（逻辑排他） |
| 触屏产生了假长按事件 → 触发 `reset_config` | **未证实：三次带插桩浸泡均未复现** |
| 屏幕洋红横条 `0xF81F` 的硬件/驱动根因 | **未证实：日志无法判定视觉，需人眼 + 冷断电对照** |

**诚实说明**：计划要求「定位明确」。目前定位到的是**唯一可能的代码路径**，
而不是**已复现的物理事件**。下面第 3 节给出三次否证实验的原始数据。
不把它写成「已确认触屏误触」，因为证据不支持那个强度的结论。

---

## 2. 任务 0.1：地址解析与触发分支锁定

### 2.1 addr2line（已执行）

```
xtensa-esp32s3-elf-addr2line.exe -pfiaC -e build/whale_maid.elf 0x40385ae8 0x4038fc19
```

工具链：`C:/Espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin/`

| Saved PC | 解析结果 |
|---|---|
| `0x40385ae8` | `esp_restart_noos` at `system_internal.c:171` |
| `0x4038fc19` | `_xt_context_save` at `xtensa_context.S:169` |

`_xt_context_save` 是中断/异常入口的通用上下文保存，**不指向任何业务函数**；
真正有信息量的是 `esp_restart_noos` —— 它证明 CPU 是**被代码要求复位的**。

### 2.2 复位类型排除法

`rst:0xc (RTC_SW_CPU_RST)` = 软件 CPU 复位。据此排除：

- 掉电复位 `rst:0x1 (POWERON_RESET)` / 欠压 `rst:0xf (BROWNOUT_RESET)`
- 看门狗 `rst:0x7/0x8/0x9 (TG0/TG1_WDT_SYS_RST, RTC_WDT_SYS_RST)`
- panic `rst:0x3 (SW_CPU_RST)` 伴随 `Guru Meditation` / `abort()`

三次浸泡日志中 `Guru Meditation`、`abort() was called`、`Task watchdog got triggered`
出现次数均为 **0**（见第 3 节），与 `rst:0xc` 的软件复位定性一致。

### 2.3 唯一执行点 + 两条触发路径（源码穷举）

`grep -rn "esp_restart()"` 在 Phase 0 固件源码中只命中一处：

- `voice_client.c:317` → `restart_task()`，内部 `vTaskDelay(1200 ms)` 后 `esp_restart()`

创建 `restart_task` 的只有两处：

| 触发路径 | 位置 | 任务名 | 需要的外部条件 |
|---|---|---|---|
| A. HTTP `POST /save` | `voice_client.c:348`（`setup_save`） | `voice_restart` | 手机/电脑主动提交配网表单 |
| B. `voice_client_reset_config()` | `voice_client.c:953` | `setup_reset` | **LVGL `LONG_PRESSED` 事件**（`main.c:816/819` 的 `setup_hold_cb`） |

路径 A 需要有人主动发 HTTP 请求；浸泡期间无客户端连接，可排除。
**路径 B 是唯一能在「无人操作」前提下自发走到 `esp_restart()` 的分支。**

`setup_hold_cb` 挂在 `canvas` 与 `screen` 两个对象上（`main.c:815-819`），
事件源是 `bsp_display_indev_init()` → `bsp_touch_new()` 注册的 FT5x06 触摸输入设备。

### 2.4 一个必须纠正的误读

旧日志里复位前出现的

```
wifi:flush txq / wifi:stop sw txq / wifi:lmac stop hw txq
```

**不是 Wi-Fi 触发复位的证据**。`esp_restart()` 会执行 Wi-Fi 关闭钩子，
任何原因的主动复位都会打印这三行。此前把它们当作「射频干扰导致复位」的线索是错的。

### 2.5 支持路径 B 的间接证据（来自未插桩的旧日志）

`firmware/p0-scripted-630s.log`（旧固件，无 `log_touch` 插桩）中出现 2 次复位：

1. 复位前有一段 `idle → wave → jump → sleep` 的连续 `CMD_NEXT` 突发。
   `CMD_NEXT` 只做 `action = (action + 1) % 4`，**只有 `tap_cb`（`LV_EVENT_CLICKED`）会产生这个顺序**；
   测试串口与 `voice_action_changed` 都无法产生。→ 说明当时确实有触摸事件被派发。
2. 第二次复位的时间差：`Wi-Fi scan` 在 5064 ms，复位在 10431 ms，间隔 5367 ms，
   与「3 s 长按判定 + 1.2 s `restart_task` 延时 + 事件处理开销」量级吻合。

这两条是**一致性证据**，不是**复现证据**。

---

## 3. 否证实验：三次带插桩浸泡（全部未复现）

插桩内容（已在烧录的 Phase 0 固件中生效，非仅存在于源码）：

- `main.c:101` `log_touch()` → `ESP_LOGW(TAG, "Touch %s at (%d,%d) event=%d", ...)`，
  由 `tap_cb`（CLICKED）与 `setup_hold_cb`（LONG_PRESSED）共同调用
- `voice_client.c:932` → `ESP_LOGW(TAG, "Config reset requested; erasing NVS and restarting into AP mode")`

即：**只要触屏派发任何事件、或复位分支被走到，日志必然留痕。**

| 日志 | 时长 | 刺激 | `Touch` 行 | `Config reset` 行 | 自发复位 | abort | 看门狗 | DMA 失败 |
|---|---|---|---|---|---|---|---|---|
| `p0-touch-240s.log` | 240 s | 静置 idle | 0 | 0 | 0 | 0 | 0 | 0 |
| `p0-touch-630s.log` | 622 s | 脚本 60 次切换 | 0 | 0 | 0 | 0 | 0 | 0 |
| `p0-touch-900s.log` | **898.5 s** | 脚本 **87 次切换** | **0** | **0** | **0** | **0** | **0** | **0** |

`p0-touch-900s.log` 唯一 boot marker 是第 3 行
`rst:0x15 (USB_UART_CHIP_RESET)` —— 那是**烧录器自己造成的复位**，不是自发重启。

累计 **1760 s / 147 次状态切换**，零触摸事件、零复位分支命中。

### 3.1 由此得到的归因判定

- 「FT5x06 间歇性假触摸 → 长按 → `reset_config`」是**目前唯一未被排除的假设**，
  但**三次实验都没有抓到它**。它可能是低概率事件（需要特定射频/温度/I2C 时序条件），
  也可能旧日志里的两次复位另有原因（例如当时有人碰过屏幕、或当时的固件版本与快照不同）。
- **不能声称已定位物理根因。** 计划验收的「定位明确」在代码路径层面达成，
  在物理事件层面**未达成**，此处如实记录。

### 3.2 为什么这仍然不阻塞阶段 1

阶段 1 按计划**彻底删除触屏**：不调用 `bsp_display_start_with_config()`
（它无条件走 `bsp_display_indev_init()` → `bsp_touch_new()`），改为自行组合
`lvgl_port_init` + `bsp_display_new` + `lvgl_port_add_disp_rgb` + `bsp_display_brightness_init`。
结果：**LVGL 没有任何 indev，`tap_cb` / `setup_hold_cb` 不复存在**。

复位路径只剩两条，且都需要人的明确动作：
- HTTP `POST /save`（手机配网提交）
- **BOOT 键按住 ≥ 3.0 s**（`main.c` `button_task`，`RESET_HOLD_MS 3000`）

也就是说：**无论假触摸假设成立与否，触发源都被结构性移除了。**
这是「按设计消除」，不是「已修好一个已证实的 bug」——两者区别在此写明。

---

## 4. 任务 0.2：花屏三条铁律核查（代码层面已完成）

| 铁律 | 核查结果 | 证据位置 |
|---|---|---|
| 绘制缓冲必须在**内部 DMA RAM**，禁止 QSPI 从 PSRAM 直接 DMA | **已落实**：开机一次性 `heap_caps_aligned_alloc(16, 16400, MALLOC_CAP_INTERNAL\|MALLOC_CAP_DMA\|MALLOC_CAP_8BIT)` 并 `lv_display_set_buffers(..., PARTIAL)`；`disp_cfg.flags.buff_dma=false / buff_spiram=false` | `main/main.c` `app_main`；日志 `LVGL draw buffer pinned to internal DMA (16400 bytes)` |
| QSPI `pclk_hz` = **40 MHz** | **已确认**（BSP 宏内硬编码，两处均为 40 MHz，无需改 `main.c`） | `managed_components/waveshare__esp_lcd_sh8601/include/esp_lcd_sh8601.h:90,102` |
| LVGL rounder **2 像素对齐**（x1/y1 偶，x2/y2 奇） | **已落实**，与 BSP 原语义逐字一致 | `main/main.c:371-378` `rounder_event_cb` |

补充纠正计划中的两处器件名笔误（不影响执行）：
- 屏驱是 **SH8601**，不是计划写的 `CO5300`
- 触摸是 **espressif__esp_lcd_touch_ft5x06**，不是计划写的 `FT3168`
- `BSP_LCD_PCLK` 是 **GPIO_NUM_11 引脚号**，不是时钟频率；真正的时钟在上面那个宏里

### 4.1 任务 0.2 未完成部分（需要用户动手，且有一个真实阻塞）

1. **冷断电 10 秒 + 刷回 `release-voice` 预编译固件**：需要人拔插电源，我无法执行。
2. **阻塞：`release-voice/storage.bin` 与当前分区表不兼容。**

   ```
   release-voice/storage.bin        = 8,388,608 B (8 MB)
   firmware/partitions.csv storage  = 4M
   ```

   直接按当前分区表烧 8 MB 的 storage 镜像会越界覆盖后面的 `model` 分区。
   `release-voice/partition-table.bin` 也是另一套布局。
   **要做这个对照实验，必须连 `release-voice/partition-table.bin` 一起烧，
   并且实验后重新烧回当前 `firmware/build` 的全套镜像。** 未经确认不擅自执行。
3. **洋红横条 / 撕裂的目视判定**：日志无法判定视觉。三次浸泡期间屏幕内容需人眼确认。
    analyzer 中该行固定输出 `MANUAL`，不会被静默计为 PASS。

---

## 5. 阶段 0 期间已证实并修掉的另一个问题（与复位无关）

**帧卡顿根因已证实**：所有 `Display cost` 超时事件的耗时都落在 `lvgl_lock=` 字段，
且与运行期 SPIFFS `open()` 的 29–122 ms 开销同时发生。

- `p0-touch-900s.log` 最差单帧 `max=114286 us`，
  同窗 `Decode cost idle_00: lock=10 open=115865 read=26190 decode=60631 total=202696 us`
- 纯 idle 静置浸泡（`p0-touch-240s.log`）最大帧仅 1296 us

→ **卡顿唯一来源是「切换动作时在 LVGL 锁内读盘 + 解码」**，不是刷屏本身、不是射频。
阶段 1 的 8 帧开机预解码常驻 PSRAM 正是消除这一项：运行期不再有任何 SPIFFS 访问。

稳态内部 SRAM 基线（**AP+STA 双模配网态**，非纯 STA）：
`p0-touch-900s.log` `Heap trend: internal=44851..48739 B`，`dma_block` 最小 24576 B。
板子当前**没有保存 Wi-Fi 凭据**，日志第 134 行 `wifi:mode : sta + softAP`、
第 140 行 `Provisioning AP Qiban-1CC9 started` 可证。
所以计划里 `44,927 B` 这个基线是**双模最差态**，阶段 1 的「纯 STA > 80 KB」需要在配网完成后才测得准。

---

## 6. 原始证据文件清单

```
firmware/p0-touch-240s.log      240 s 静置
firmware/p0-touch-630s.log      622 s 脚本 60 切换
firmware/p0-touch-900s.log      898.5 s 脚本 87 切换（最终归因实验）
firmware/p0-scripted-630s.log   旧固件、无插桩、含 2 次复位（间接证据来源）
firmware/phase0-snapshot/       Phase 0 的 main.c / voice_client.c / animation_manifest.h 原样快照
tools/analyze_log.py            判定脚本（已对齐阶段 1 DoD；MANUAL 项不计 PASS）
```

复现命令：

```bash
C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe tools/analyze_log.py firmware/p0-touch-900s.log
grep -cE "Touch |Config reset requested" firmware/p0-touch-900s.log   # → 0
grep -nE "rst:0x|Guru|abort\(\)|Task watchdog" firmware/p0-touch-900s.log
```