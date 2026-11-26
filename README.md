# NEX

> **依旧，献给我最爱的人。**

NEX 是一块为 M5Stack STOPWATCH（C152，ESP32-S3）重写的固件，以及配套的 Android 应用 **NEX_Connect**。它不是一块只会看时间的表：模拟表盘、秒表、姿态仪、倒计时、提醒事项、手机校时与壁纸上传，全部围绕“每天真的会用”来设计。

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.4-red?logo=espressif)
![ESP32-S3](https://img.shields.io/badge/ESP32--S3-240%20MHz-blue)
![LVGL](https://img.shields.io/badge/LVGL-9.5-9cf)
![Rust](https://img.shields.io/badge/Rust-xtensa-orange?logo=rust)
![Kotlin](https://img.shields.io/badge/Kotlin-2.2-purple?logo=kotlin)
![Jetpack Compose](https://img.shields.io/badge/Jetpack%20Compose-Material%203-green?logo=jetpackcompose)
![License](https://img.shields.io/badge/license-MIT%20%2F%20GPL--3.0-lightgrey)

## 功能

- **模拟表盘**：传统刻度、时针/分针、壁纸背景，底部低调电量显示（白色 / 低于 20% 黄色 / 充电闪电徽标）。
- **秒表**：KEYB 开始/继续，KEYA 暂停，触摸归零。
- **姿态仪**：Garmin 风格人工地平线、俯仰梯、坡度刻度，KEYA/KEYB 切换配色。
- **提醒事项**：手机推送单条到手表，手表端只保留清单；选中后双击 A+B 永久删除。
- **倒计时**：`+10秒` / `+1分`、开始/暂停、归零；到点扬声器蜂鸣，按键或超时自动停止。
- **手机联动（BLE）**：
  - 连接后自动校准手表时间（含时区）
  - 更换壁纸：手机圆形裁剪缩放 → 466×466 JPEG → BLE 上传
  - 提醒推送
- **系统设置**：时间与日期、系统信息、开发者选项、手机连接、返回表盘。

## 键位

| 操作 | 功能 |
|---|---|
| KEYA（左侧）短按 | 上移选择 / 秒表暂停 / 计时器归零 |
| KEYB（右侧）短按 | 下移选择 / 秒表开始 / 计时器开始 |
| KEYA + KEYB 短按 | 确认选项 |
| KEYA + KEYB 长按（≥1 秒） | 表盘进入应用 / 应用内返回表盘 |
| KEYA + KEYB 双击（提醒事项） | 删除当前选中的提醒 |

## 构建指南

### 固件

依赖：

- ESP-IDF **v5.5.4**（含 `idf.py`）
- Rust 的 `xtensa-esp32s3-espidf` 目标（`espup` 或 `esp-rs` 工具链）
- Git、CMake、Ninja、Python 3

```bash
git clone git@github.com:PR1TEA/NEX.git
cd NEX

# 拉取 M5GFX 等外部组件（LVGL 由组件管理器固定版本）
./tools/fetch_components.sh

# 首次构建
idf.py set-target esp32s3
idf.py build

# 烧录（按实际串口修改）
idf.py -p /dev/ttyACM0 flash monitor
```

生成物：

- `build/NEX.bin`：应用镜像
- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`

发布用合并镜像（从 0x0 开始）：

```bash
python -m esptool --chip esp32s3 merge_bin \
  -o NEX-flash.bin --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/NEX.bin
```

### Android

依赖：

- JDK **25**
- Android SDK **37**（build-tools 36.0.0）
- Gradle Wrapper 9.6.0（仓库自带）

```bash
cd android/NEX_Connect
./gradlew assembleDebug      # 调试包
./gradlew assembleRelease    # 未签名发布包
```

发布包使用 `apksigner` 签名：

```bash
apksigner sign --ks /path/to/nex-release.jks \
  --out NEX_Connect-release.apk \
  app/build/outputs/apk/release/app-release-unsigned.apk
```

签名密钥不随仓库分发；请自行妥善保管。

## 使用指南

1. 烧录固件后首次开机会自动格式化壁纸分区，表盘显示默认样式。
2. 手机打开蓝牙，安装并启动 **NEX_Connect**，点“连接设备”，选择 `NEX-Watch`。
3. 连接成功后自动校时；之后可：
   - 更换壁纸：选图 → 圆形取景 → 拖动/双指缩放 → 确认上传。
   - 提醒事项：输入内容 → 推送；手机端列表会清空，提醒只保留在手表。
4. 手表端：双键长按进入应用；在提醒事项中选中一条，快速双击 A+B 彻底删除。
5. 计时器：`+10秒` / `+1分` 调整时长，KEYB 开始，时间到会响铃；按 A 或 B 停止。
6. 充电时表盘底部电池左侧显示绿色闪电徽标。

## 仓库结构

```
main/                固件入口、UI、服务、BLE/GATT、音频
components/nex_rust/ Rust 状态与时间核心（C ABI）
android/NEX_Connect/ Android 应用（Kotlin + Compose）
protocol/            BLE 协议说明
docs/                工程与验证文档
releases/            版本发布物（flash 镜像、APK、说明、SHA256）
tools/               外部组件拉取等工具脚本
```

## 版权与许可证

- 固件（`main/`、`components/nex_rust/` 等）：**MIT License**，见根目录 `LICENSE`。
- Android 应用（`android/NEX_Connect/`）：**GNU General Public License v3.0**，见 `android/NEX_Connect/LICENSE`。
- 第三方组件（ESP-IDF、LVGL、M5GFX、M5Unified 参考、Jetpack Compose 等）遵循各自许可证。
- Copyright © 2026 **PR1TEA**. 保留所有权利。
- 本项目为独立作品，与 M5Stack 无附属关系；M5Stack 是其权利人的商标。

NEX 按“现状”提供，不附带任何明示或暗示的担保。
