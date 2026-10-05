# Waveshare ESP32-S3-Touch-LCD-1.85C 移植 Muse Gadget 固件 —— 完整教学

> 本文记录把 Meta 开源的 [muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
> 移植到 Waveshare ESP32-S3-Touch-LCD-1.85C 开发板并烧录运行的全过程。
> 适合有 Arduino 经验、想入门 ESP-IDF 嵌入式开发的同学。
> 所有引脚、寄存器、时序均来自 Waveshare 官方 Demo（ESP-IDF 版），可对照源码验证。

---

## 1. 背景知识

### 1.1 Muse Gadget 是什么

Muse 是 Meta 的 AI 语音助手硬件生态。`muse-gadget-sdk` 里有两部分：

- **Home Link**：负责配网（BLE）、Wi-Fi、连接 Muse 云服务；
- **Muse**：跑在带屏开发板上的语音助手 UI（LVGL 界面、推键说话、麦克风/喇叭、电池图标等）。

SDK 用**板级抽象层**支持不同开发板：每个板子提供一个 `muse_board_t` 结构体
（`esp32/components/muse/muse_board.h`），填上初始化函数、显示、触摸、音频、按键、
电源等回调，上层 UI/语音逻辑全部复用。

### 1.2 为什么要"移植"

SDK 官方支持列表里没有 1.85C 这块板子（只有 1.75C，屏幕驱动完全不同）。
所谓移植，就是照着官方硬件资料，把这块板的驱动填进板级接口里。

**核心原则：一切以厂商官方 Demo 为准。** 引脚定义、初始化时序、寄存器布局，
Waveshare 的 Demo（`ESP32-S3-Touch-LCD-1.85C-Test`）已经调通，照抄最稳。

### 1.3 需要什么账号/Token

- 固件编译、烧录、运行**不需要**任何付费服务；
- 配对需要免费的 SDK Token：在 <https://gadgets.muse.ai> → Account → SDK tokens
  申请（`mgst_...` 开头）；
- **Token 是密钥，不要提交到 git，不要发给任何人。** 它只写进本地构建目录的
  `sdkconfig`（该目录已被 `.gitignore` 忽略）。

---

## 2. 硬件清单（1.85C）

| 模块 | 芯片/接口 | 关键引脚 | 备注 |
|---|---|---|---|
| 主控 | ESP32-S3（240 MHz 双核） | — | 16 MB Flash，8 MB Octal PSRAM |
| 屏幕 | ST77916，360×360，16-bit | QSPI：SCK=40, D0=46, D1=45, D2=42, D3=41, CS=21 | 复位线在 TCA9554 的 P1 |
| 背光 | GPIO5，高电平亮 | LEDC PWM 13-bit 5 kHz | 占空比 = pct × 8191/100 |
| 触摸 | CST816，I2C 0x15 | SDA=11, SCL=10, INT=4（未用） | 复位线在 TCA9554 的 P0 |
| 扩展 IO | TCA9554PWR，I2C 0x20 | 同一条 I2C | 寄存器：输出 0x01，配置 0x03 |
| 喇叭 | PCM5101 I2S DAC | BCLK=48, WS=38, DOUT=47，无 MCLK | 纯播放，音量软件调节 |
| 麦克风 | 独立 I2S 口 | I2S_NUM_1：BCLK=15, WS=2, DIN=39 | Mono，32-bit slot，右声道 |
| 电池 | 分压电阻 → ADC1_CH7（GPIO8） | 衰减 DB_12 | 电池 mV = ADC校准mV × 3 / 0.9945 |
| 按键 | BOOT（GPIO0） | 低电平有效 | 唯一按键：talk + 唤醒 |

注意两个"坑点"（后面调试章节还会讲）：
- Demo 里 `TCA9554_EXIO1 = 0x01`、`TCA9554_EXIO2 = 0x02` 看起来是位掩码，
  但它们的 `Set_EXIO(Pin, State)` 把参数当**引脚号**用（`0x01 << (Pin-1)`），
  所以触摸复位实际是 TCA9554 的 **P0**，LCD 复位是 **P1**。
- 触摸芯片休眠/无触摸时，数据寄存器全 0；**必须检查触点数量字节，否则
  LVGL 会收到一个永远按在 (0,0) 的"幻影触摸"**。

---

## 3. 开发环境搭建（Windows）

### 3.1 安装 ESP-IDF v6.0.1

Muse SDK 只支持 ESP-IDF **v6.0.x**，其他版本编不过。

```powershell
# 下载离线安装包或 git clone，然后安装工具链（含 esp32s3 交叉编译器、Python 环境）
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git C:\Users\%USERNAME%\esp\esp-idf-v6.0.1
cd C:\Users\%USERNAME%\esp\esp-idf-v6.0.1
install.bat esp32s3
```

装完后每个新的 PowerShell 窗口都要 `export.ps1` 注入环境变量
（`$env:IDF_PATH\export.ps1`）。Git Bash 里要先 `Remove-Item Env:MSYSTEM`，
否则工具脚本报 "MSys not supported"。

### 3.2 检查清单

```powershell
idf.py --version          # ESP-IDF v6.0.1
python -c "import serial" # pyserial 已装（esptool 依赖）
```

### 3.3 常用包装脚本

工程里准备了两个 PowerShell 脚本（因为 Git Bash 会吃掉 `$env:` 变量，
统一用 `powershell -File` 调用）：

- `idf-conf-185c.ps1`：CMake 配置阶段（只需跑一次，或改 sdkconfig 后重跑）；
- `idf-ninja.ps1 -B <构建目录> -Jobs 4`：增量编译。**低并发 `-j 4` 很重要**，
  全核编译会让机器卡死（亲测黑屏过一次）。

---

## 4. 代码结构：SDK 怎么看

```
esp32/
├── components/muse/
│   ├── muse_board.h          # ★ 板级接口（muse_board_t 结构体）
│   ├── boards/               # ★ 每个开发板一个 .c
│   │   ├── board_guition_jc3248w535.c   # 本次移植的模板
│   │   ├── board_waveshare_s3_185c.c    # ★ 本次新增的板子
│   │   └── esp_lcd_st77916.c/.h         # ★ vendored 屏幕驱动（Espressif Apache-2.0）
│   ├── Kconfig               # 板子选项（choice MUSE_BOARD）
│   ├── CMakeLists.txt        # 按 CONFIG_MUSE_BOARD_* 选源文件
│   └── muse_ui.c / muse_audio.c / ...   # 上层通用逻辑，不用动
├── devices/
│   ├── sdkconfig.muse                    # 通用配置
│   └── sdkconfig.muse-waveshare-s3-185c  # ★ 本板的 sdkconfig overlay
└── main/main.c               # app_main：muse_app_run(muse_board_get())
```

### 4.1 注册一块新板要改三处（照抄现有板子的模式）

1. **`components/muse/Kconfig`**：`choice MUSE_BOARD` 里加
   `config MUSE_BOARD_WAVESHARE_S3_185C`；`MUSE_BOARD_ID` 字符串加
   `default "waveshare_s3_185c" if MUSE_BOARD_WAVESHARE_S3_185C`。
2. **`components/muse/CMakeLists.txt`**：板子选择链加
   `elseif(CONFIG_MUSE_BOARD_WAVESHARE_S3_185C) list(APPEND srcs ...)`
   （vendored 的 st77916 驱动也在这里一起编译）。
3. **`devices/sdkconfig.muse-waveshare-s3-185c`**：构建时用
   `-DSDKCONFIG_DEFAULTS` 叠加进来（见第 6 节）。

### 4.2 板级驱动逐段讲解（board_waveshare_s3_185c.c）

`muse_board_t` 要求实现这些回调（可选的留 NULL）：

| 回调 | 本次实现方式 | 要点 |
|---|---|---|
| `init` | I2C0 总线 + TCA9554（全输出、复位线拉高）+ BOOT 键 + ADC 校准 | 最先跑 |
| `display_start` | 起 LVGL 任务，里面完成屏幕初始化 | 见 4.3 |
| `display_lock/unlock` | 递归互斥锁 | 上层 UI 与 LVGL 任务共用 |
| `set_brightness` | LEDC 占空比 | 13-bit：pct × 8191/100 |
| `audio_init` | 双 I2S 通道 + 自定义 `audio_codec_data_if` | 见 4.4 |
| `poll_buttons` | `muse_gpio_button_poll` | 消抖由 SDK 做好 |
| `read_power` | ADC 读电池分压 | 无 USB 检测脚，用电压推断 |
| `power_off` | 关背光 + deep sleep，BOOT 键唤醒 | |
| `panel_sleep`/`display_pause` | **留 NULL**（第一版求稳） | 有 PMU 的板子才实现 |

### 4.3 显示：ST77916 + LVGL 直接模式

结构（与 guition 板相同，这是 SDK 里"裸驱动"的标准做法）：

1. **全屏帧缓冲在 PSRAM**（360×360×2 = 253 KB），LVGL 用
   `LV_DISPLAY_RENDER_MODE_DIRECT`——LVGL 只重绘变化区域，但回调里等
   `lv_display_flush_is_last()` 后**整帧**发出去；
2. **QSPI 分块发送**：SPI DMA 读 PSRAM 会抢不过双核绘图，所以把帧拆成
   20 行一块（14.4 KB）拷到内部 RAM，两块乒乓：`on_chunk_sent` 回调里
   释放信号量；
3. 颜色格式 `LV_COLOR_FORMAT_RGB565_SWAPPED`（SPI 屏要字节交换）；
4. 屏幕复位（TCA9554 P1）→ `spi_bus_initialize` → `esp_lcd_new_panel_io_spi`
   （32-bit 命令 + quad_mode）→ `esp_lcd_new_panel_st77916`（vendor_config
   传入 Demo 的初始化序列，**会替换驱动自带序列**）→ `esp_lcd_panel_init`；
5. **初始化序列**整段照抄 Demo 的 `vendor_specific_init_new`（约 180 条命令，
   含 gamma 表），结尾 `0x11` sleep-out（120 ms）+ `0x29` display-on。
   别自己编，这是厂商调好的。

触摸挂在 LVGL 上：`lv_indev_create()` + 手动 I2C 读 CST816。

### 4.4 音频：两个独立 I2S 口

SDK 的 `esp_codec_dev` 音频框架默认假设"一条双工 I2S 总线"，但 1.85C 的
喇叭（I2S_NUM_0）和麦克风（I2S_NUM_1）是**两条独立总线**，所以自己填
`audio_codec_dev` 的 data_if：

```c
static const audio_codec_data_if_t spk_if = { .enable = data_enable, .write = spk_write };
static const audio_codec_data_if_t mic_if = { .enable = data_enable, .read  = mic_read };
```

- 喇叭：16-bit 立体声 Philips 标准模式，`i2s_new_channel(I2S_NUM_0, &tx, NULL)`；
- 麦克风：mono、32-bit slot、右声道（Demo 的 `MIC_Speech.c` 用
  `I2S_STD_SLOT_RIGHT`），`i2s_new_channel(I2S_NUM_1, NULL, &rx)`；
- **32-bit 麦克风的缩放**（踩坑点，见 7.3）：语音数据在 32 位字的高位，
  Demo 用 `>>14` 降到 16 位；直接取高 16 位会满刻度削波。
  `mic_read` 里把每个 32 位采样 `>>14` 后**复制到两个 int16 半字**，
  这样上层不管取左声道还是右声道（`mic_slot`）都拿到同样的信号，
  再做 DC 去除 + 增益（防削波）。
- 音量在软件里调（`esp_codec_dev` 没有硬件 codec 可写）。

### 4.5 触摸：手动 I2C 轮询 CST816

```c
// 从寄存器 0x02 读 5 字节：[触点数量, X高(低4位), X低, Y高(低4位), Y低]
i2c_master_transmit_receive(s_tp, &reg=0x02, 1, buf, 5, 20);
if (err || buf[0] == 0) → RELEASED   // ★ 必须检查 buf[0]，否则幻影触摸
x = (buf[1] & 0x0F) << 8 | buf[2];
y = (buf[3] & 0x0F) << 8 | buf[4];
```

复位：TCA9554 P0 拉低 10 ms → 拉高 50 ms（与 Demo 一致），然后
`i2c_master_probe` 探测 0x15，失败只警告不致命（guition 模式，UI 照样跑）。

---

## 5. 配置文件（sdkconfig overlay）

`devices/sdkconfig.muse-waveshare-s3-185c` 的要点：

```ini
CONFIG_IDF_TARGET="esp32s3"
CONFIG_MUSE_BOARD_WAVESHARE_S3_185C=y     # 选板
CONFIG_HOMEHUB_BUTTON_GPIO=0              # BOOT 键
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y         # ★ 板载实测 16MB
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y      # 原生 USB 串口
CONFIG_SPIRAM=y                           # PSRAM 必须（帧缓冲 253KB 在里面）
CONFIG_SPIRAM_MODE_OCT=y / SPEED_80M / FETCH_INSTRUCTIONS / RODATA ...
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2         # 双核各一个 LVGL 绘制单元
```

**血泪教训**：别照抄 1.75C overlay 的 `FLASHSIZE_32MB`——那会导致启动时
`Detected size(16384k) smaller than header(32768k)` 断言重启。

Token 在**首次编译前**写进构建目录的 sdkconfig（不进 git）：

```bash
sed -i 's|^CONFIG_GADGET_SDK_TOKEN=""|CONFIG_GADGET_SDK_TOKEN="mgst_你的token"|' \
  build-muse-waveshare-s3-185c/sdkconfig
```

---

## 6. 构建与烧录

### 6.1 配置（一次）

```powershell
idf.py -B build-muse-waveshare-s3-185c `
    -DIDF_TARGET=esp32s3 `
    -DSDKCONFIG=build-muse-waveshare-s3-185c/sdkconfig `
    "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-waveshare-s3-185c" `
    reconfigure
```

`-DSDKCONFIG_DEFAULTS` 用分号叠加多个 overlay，后面的覆盖前面的。

### 6.2 编译

```powershell
ninja -C build-muse-waveshare-s3-185c -j 4     # 低并发！机器卡死过
```

中途超时被杀直接重跑即可（ninja 增量）。产物：
`build-.../muse-gadget.bin`（已签名）。

### 6.3 烧录

```powershell
idf.py -B build-muse-waveshare-s3-185c -p COM4 flash
```

- 板子是原生 USB（VID_303A），烧录时端口可能短暂消失再出现；
- `port busy` 先查谁占着：
  ```powershell
  [System.IO.Ports.SerialPort]::GetPortNames()
  Get-CimInstance Win32_Process | ? CommandLine -match 'COM4' | kill -Force
  ```
  （常见凶手：上次抓串口日志的 python 没退干净）；
- 烧录成功标志：`Hard resetting via RTS pin`。

### 6.4 看日志（最重要的调试手段）

```bash
python capture_com.py COM4 25        # 读 25 秒串口，高亮关键行
python capture_com.py COM4 14 reset  # 先拉 RTS 复位再看完整启动日志
```

启动自检要认这些行：

```
muse: board: Waveshare ESP32-S3-Touch-LCD-1.85C   ← 板子选对了
SDK token:  mgst_xxxx                              ← token 编进去了
muse_ui: UI up: 360x360                            ← 显示 OK
muse_audio: self-test @ 30 dB: mic L -28.7 dBFS ... ← 麦克风健康（-20~-40 dBFS 都正常，
                                                      接近 0 dBFS 是削波）
link.ble: advertising as MuseGadget-XXXXXX         ← BLE 在广播
```

---

## 7. 踩坑记录（本次真实踩过的）

### 7.1 扩展 IO 位号张冠李戴

现象：LCD 能初始化（碰巧），触摸 `i2c_master_probe` 失败。
原因：Demo 的 `TCA9554_EXIO1=0x01/EXIO2=0x02` 经 `Set_EXIO(Pin,...)`
按**引脚号**解析后对应 TCA9554 的 P0/P1，我当成位掩码用了 P1/P2。
教训：**别猜宏的语义，先看函数实现。**

### 7.2 幻影触摸（最隐蔽的一个）

现象：I2C 通信正常、探针通过、数据也读得到，但屏幕就是点不动。
原因：无触摸时芯片返回全 0，我的代码没检查触点数量字节，LVGL 收到了
一个**永远按在 (0,0)、永不松开**的触摸。按钮需要"按下-在同一对象上松开"
才算 click，于是整个 UI 对触摸无响应。
排查方法：在 `touch_read` 里加**限速调试日志**（每 2 秒最多一条 + 数据
非零即打），抓串口看到 `01 80 d7 00 b6` 这样的真实数据，一眼定位。
教训：轮询触摸必须判"有效位/数量位"；调试日志要限速，不然刷屏淹死串口。

### 7.3 麦克风 32-bit 数据削波

现象：自检 `mic L -0.1 dBFS (peak 32768)` —— 满刻度，一说话就破音。
原因：直接取 32-bit slot 的高 16 位，而这款麦的有效位更高。
修复：按 Demo 的 `>>14` 缩放后再进 16 位管道。
教训：**模拟链路的量纲要对齐厂商 Demo**，自检日志里的 dBFS 是好标尺。

### 7.4 Flash 容量配置

现象：`Detected size(16384k) smaller than header(32768k)` 启动断言循环重启。
原因：照抄了 1.75C overlay 的 32MB；这块板实测 16MB。
教训：SDKCONFIG 里的硬件参数以**实测**为准（启动日志开头有芯片上报值）。

### 7.5 串口被残留进程占用

现象：`Could not open COM4, the port is busy`。
原因：之前抓日志的 python 进程没退干净。
教训：抓串口脚本要设读取时长上限并保证退出；烧录前查进程。

---

## 8. 案例：给固件加一个新命令（audio.play_url）

这一章完整记录"从需求到真机验证"加一条 Muse 命令的全过程。
v1.1 的 `audio.play_url`（Muse 推送 MP3 的 URL，板子下载后从喇叭播出来）
就是按这个流程做的，已真机验证。学嵌入式最难的不是写代码，是
**在陌生代码库里找到该改哪、能复用什么**——本章重点讲这个。

### 8.1 需求与方案选择

需求：让 Muse 聊天里的 AI 能"开口"，板子要把一段语音播出来。

两个候选方案：

| 方案 | 做法 | 为什么没选/选了 |
|---|---|---|
| 板载 TTS | 固件接第三方 TTS API，把回复文本合成语音 | 要 API key、要 HTTPS 请求、S3 算力紧张；Muse 官方注释（`muse_chat_session.cpp` 的 `start_tts`）也只是留了插槽没实现 |
| **云端合成 + 板端播放**（选中） | Muse 把合成好的 MP3 通过 URL 推给板子，板子只管下载解码播放 | 板子零依赖，解码播放的轮子固件里全有 |

### 8.2 第一步永远是读规范

`esp32/AGENTS.md` 的 "Adding a command" 一节把规矩写得很死，
照做就不会踩架构上的坑：

1. 命令要出现在**两个同名的地方**：`noise_control.cpp` 的
   `build_register_json()`（`add_command()` 注册）和 `app.c` 的
   `on_ws_command()`（分发处理）；
2. **不许阻塞**：`on_ws_command()` 跑在 Noise 会话任务上。下载这种慢操作
   必须拷贝 `request_id` / `session_generation` / 参数到自己的任务，
   立即返回 `{"_async": true}`，任务完成后调用
   `noise_ctrl_send_command_result()` 回结果（成功失败都要回，不然
   Muse 干等超时）；
3. **参数自己校验**：固件不核对注册时声明的 required/optional，
   `params` 可能是 NULL，每个参数查存在性、类型、长度；
4. 需要特定硬件的命令用 **Kconfig 选项门控**，两处包同一个 `#if`；
5. `link.register` 总上限 **8 KB**，命令描述别写长篇大论；
6. 加一个 **host 测试**（`tests/`，纯源码检查即可，不需要板子）。

### 8.3 找轮子：三个现成模块拼出新功能

**下载** —— 照抄 `main/image_fetch.c`（`display.draw_url` 的下载器）：
`esp_http_client` + `esp_crt_bundle_attach`（证书校验）、手动跟随
重定向（禁自动跳转以固定协议）、总时限 + 单次超时、内部 RAM 下限保护
（低于阈值中止，保住 Noise 会话和 lwIP）。

**解码** —— `components/minimp3`（CC0 的 MP3 解码器，语音回复就在用）+
 `muse_chat_session.cpp` 里现成的解码循环（`mp3dec_init` /
`mp3dec_decode_frame` / 立体声转单声道 / Q16 定点线性插值重采样
`resampler_init`/`resample`）。这段代码注释明确写着"把 TTS 的 MP3
喂给 `tts_data()` 就行"——抄它的结构，别自己发明。

**播放 + UI** —— `muse_audio_write()`（16 kHz 单声道阻塞写，
音量走 UI 的设置）；屏显提示用
`muse_state_set_mode(MUSE_MODE_SPEAKING)` + `muse_state_set_caption()` +
播放时喂 `muse_state_set_level(muse_audio_level(...))`（音量条动画）。

### 8.4 实现清单（6 个文件）

1. **`main/audio_play.c/.h`** —— 核心模块，一个 48 KB **PSRAM 栈**任务
   完成 下载→解码→播放 全流程（TLS 握手和 minimp3 都吃栈；参考
   `muse_chat_session.cpp` 的会话任务同样用 48 KB PSRAM 栈跑 TLS）。
   要点：原子 `s_busy` 标志拒绝并发播放；MP3 缓冲 1 MB（PSRAM，
   够几分钟语音）；幂等调用 `muse_audio_power(true)`（编解码器休眠时
   自动上电）；**只在 IDLE 态接管屏幕**（正在按住 BOOT 对话时只出声、
   不抢 UI），播完恢复 IDLE + 空标题。
2. **`main/Kconfig.projbuild`** —— 新选项 `HOMEHUB_AUDIO_PLAY_COMMAND`，
   `default y if HOMEHUB_LED_BACKEND_MUSE && SPIRAM`（有完整 UI 的板子
   才有 codec 通路和 PSRAM，条件与 `HOMEHUB_DISPLAY_COMMANDS` 相同）。
3. **`main/noise_control.cpp`** —— `#if` 块里 `add_command()` 注册
   + `"timeout_ms": 120000`（下载+播放可能超过默认 30 s）。
4. **`main/app.c`** —— `#if` 块里分发：校验 url（http/https、≤1024
   字符）→ 拷贝上下文 → `audio_play_start()` → 成功返回 `_async`，
   失败返回 `command_error()`。
5. **`main/CMakeLists.txt`** —— 源文件加入 `GADGET_SRCS`，`minimp3`
   加进 `GADGET_REQUIRES`（main 本来就 `REQUIRES muse`，直接可调
   `muse_audio_*`/`muse_state_*`）。
6. **`tests/test_link_audio_play.py`** —— host 测试照
   `test_link_sensecap_sensors.py` 的"源码检查"模式：断言注册/分发
   在同一 `#if` 下、`_async` 存在、Kconfig 默认条件正确。

### 8.5 编译：两个 -Werror 陷阱

固件开了 `-Werror`，新增代码两个典型报错：

- `maybe-uninitialized`：重采样器只在"采样率变化"分支初始化，GCC
  证明不了必走该分支 → 定义时清零 `resampler_t rs = { 0 };`
  （语义不变，只是安抚编译器）；
- `format-zero-length`：`muse_state_set_caption("")` 是 printf 风格
  函数，空格式串非法 → 抄上游 `muse_app.c` 的清法：
  `muse_state_set_caption("%s", "")`。

### 8.6 验证流程（不用重新配对）

```bash
python -m unittest tests.test_link_audio_play     # 1. host 测试
powershell -File idf-ninja.ps1                    # 2. 编译（-j 4 低并发）
powershell -File idf-run.ps1 -B build-muse-waveshare-s3-185c -Port COM4 -Cmd flash
python capture_com.py COM4 25 reset               # 3. 抓启动日志
```

日志里要看到 `sent link.register (... bytes)`（远小于 8K 上限）和
`link.register acked; tunnel may open`，无 panic。烧录不清 NVS，
**Wi-Fi 和配对凭据都在**，Muse 重连后就能看到新命令，直接让它调
`audio.play_url` 真机验证。

### 8.7 本次的教训

- **改别人 SDK 先找"插槽"**：`start_tts` 的注释、`AGENTS.md` 的
  "Adding a command" 就是官方留的扩展点，顺着插槽走比重写整个链路稳。
- **异步命令的内存归属是头号坑**：`on_ws_command()` 一返回请求就被
  释放，参数和 ID 必须拷到自己的任务里（AGENTS.md 原话 +
  `camera.capture`/`device.discover` 范例）。
- **截断的 MP3 也能播**：下载中断时已缓冲部分照常解码，minimp3 遇到
  不完整帧自然结束——"播放到手的部分"比"失败重来"体验好。
- 上游 host 测试在 Windows 上有一批 GBK 编码的既有报错，与改动无关，
  对比 `git stash` 前后的基线即可确认。

---

## 9. 配对与使用

1. 手机装 Muse App → Settings → Devices → 打开 **Developer mode**；
2. Add Device → 选 `MuseGadget-XXXXXX`；
3. 提示确认时按一下板子的 **BOOT 键**；
4. 按提示配 Wi-Fi（**必须配，语音回复要走云端**，不配 Wi-Fi 它能听不能说）；
5. 状态变绿后，按住 BOOT 说话，松手等回复。

回退原厂：随时用 Waveshare 官方 Demo 重新烧录即可，不会变砖。

---

## 10. 后续升级建议

```bash
git remote add upstream https://github.com/facebookincubator/muse-gadget-sdk.git
git fetch upstream && git rebase upstream/main   # 我们的改动在独立文件里，冲突少
```

本次移植涉及的所有文件（都在本仓库）：

- `esp32/components/muse/boards/board_waveshare_s3_185c.c` —— 板级驱动
- `esp32/components/muse/boards/esp_lcd_st77916.c/.h` —— vendored 屏幕驱动
- `esp32/components/muse/Kconfig` / `CMakeLists.txt` —— 注册板子
- `esp32/devices/sdkconfig.muse-waveshare-s3-185c` —— 构建配置
- `esp32/main/audio_play.c/.h` —— v1.1 新增：audio.play_url 命令
- `esp32/tests/test_link_audio_play.py` —— 该命令的 host 测试
- `docs/waveshare-s3-185c-port-guide.md` —— 本文
