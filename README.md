# KazepOS — STM32 桌面式多功能系统（FreeRTOS）

基于 **STM32F103ZE + FreeRTOS** 的桌面式多应用系统：一个"桌面"主界面，摇杆 + 按键切换多个应用，UI 采用**单写者（ui_task）**模型统一刷屏，数据与外设（W25Q128、FatFs、RTC）与显示解耦。

## 应用与功能

| 应用 | 说明 |
|---|---|
| 🖥️ Desktop 桌面 | 应用入口，图标网格 + 光标移动（摇杆） |
| 🎨 Paint 画板 | 画笔/橡皮擦画图，**保存/加载到 W25Q128 的 FAT 文件系统**（含免擦写优化、异常自检） |
| 🎵 Music 播放器 | 顺序/单曲/随机三种模式，内置曲谱，切歌、播放行与选中行解耦 |
| 📋 Logs 日志查看 | 系统运行日志应用：RAM **环形日志** + RTC 时间戳 + 全系统埋点 |
| 📁 Files 文件管理 | FatFs 文件浏览 |
| ⚙️ Settings 设置 | 系统设置（含修改密码） |
| 📊 Monitor 监控 | 系统运行状态监控 |
| 🔄 OTA | 伪 OTA 升级演示 |

系统服务：`ui_task`（UI 单写者）、`input_task`（输入）、`cursor`、`rtc_app`、`sys_log`（环形日志）、`sys_wdg`（看门狗心跳）、`sys_backlight`、`sys_cfg`、`sys_stats`、`fault_report`、`event` 事件流等。

## 硬件

- MCU：STM32F103ZE（HAL 库，CubeMX 生成工程）
- 屏幕：正点原子 ATK_MD0280（FSMC 接口）
- 存储：W25Q128 SPI Flash（挂 FatFs，自实现免擦写 diskio）
- 输入：摇杆 Joystick
- 内核：FreeRTOS（CMSIS-OS）

## 目录结构

```
KazepOS/
├── Core/            CubeMX 生成：main、中断、FreeRTOS 配置
├── App/             应用与系统服务（本项目的核心代码）
│   ├── Src/Inc/     ui_task 单写者、desktop、各 app、系统服务
│   ├── Dev/         摇杆驱动
│   └── FatFs/       FatFs + diskio（免擦优化）
├── Drivers/         BSP（ATK_MD0280 LCD）+ CMSIS + STM32F1 HAL
├── Middlewares/     FreeRTOS 内核
├── MDK-ARM/         Keil 工程（freertos_test.uvprojx）
├── freertos_test.ioc   CubeMX 工程文件
└── 开发日志.md        全开发过程记录（按功能、按坑）
```

## 如何打开

1. **CubeMX**：双击 `freertos_test.ioc` 可查看/重新生成外设与内核配置
2. **Keil MDK-ARM**：双击 `MDK-ARM/freertos_test.uvprojx` 打开编译下载（`App/` 下手写代码不参与 CubeMX 重新生成，放心生成）

## 版本历史规范

- 开发按**功能/修复**提交，一条提交对应一个改动点，用 `git log --oneline` 可回溯每一个版本的验收点：
  ```
  git log --oneline   # 每条提交 = 一个功能的完整记录
  git show 编号        # 看某次提交改了什么
  ```
- 提交信息格式：`模块: 做了什么（为什么）`，如 `Music: 修复切歌滞后...`
- 编译产物与本地配置不入库（见 `.gitignore`：`.o/.hex/.map/.lst`、`.uvguix`、JLink 日志等）
