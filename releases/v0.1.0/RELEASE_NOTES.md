# NEX v0.1.0

**首个正式发布版本** — 2026-11-26

依旧，献给我最爱的人。

## 内容

| 文件 | 说明 |
|---|---|
| `NEX-v0.1.0-flash.bin` | ESP32-S3 完整刷写镜像（合并 bootloader + 分区表 + 应用），从 `0x0` 写入 |
| `NEX_Connect-v0.1.0.apk` | 已签名的 Android 应用（NEX_Connect 0.1.0） |
| `SHA256SUMS.txt` | 上述两个文件的 SHA-256 |

## 固件

- 模拟表盘：刻度、时针/分针、壁纸背景
- 表盘底部电量：白色 / 低于 20% 黄色 / 充电绿色闪电徽标
- 秒表：KEYB 开始、KEYA 暂停、触摸归零
- 姿态仪：人工地平线，KEYA/KEYB 切换配色
- 提醒事项：手机推送，手表端双击 A+B 彻底删除
- 倒计时：+10 秒 / +1 分，扬声器提醒
- BLE：`NEX-Watch`，自动校时、壁纸上传、提醒推送
- 设置：时间与日期、系统信息、开发者选项、手机连接

## Android

- Material 3 界面
- BLE 连接与校时
- 圆形裁剪 / 缩放壁纸上传（466×466 JPEG）
- 提醒推送（推送成功后手机端清空）
- 深色 / 浅色 / 跟随系统

## 刷写

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 NEX-v0.1.0-flash.bin
```

首次开机自动格式化壁纸分区；壁纸与提醒为空。

APK 签名证书 SHA-256：

```
d59318d65bff08a3a9abec7eb3fdc2d60eef86b6ff381892462d5ed8b795870e
```

## 许可证

- 固件：GPL-3.0
- Android 应用：GPL-3.0
- Copyright © 2026 PR1TEA
