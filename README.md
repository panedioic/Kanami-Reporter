# Kanami-Reporter

Kanami-Reporter 是一个 OBS Studio 视频滤镜插件。它按固定频率读取来源画面，
在指定区域做图像模板匹配，通过状态机判断游戏当前所处阶段，并在状态变化时
播放对应的语音，用于替换游戏内的播报语音。

当前实现针对《三角洲行动》黑潮爆破模式的 `2560x1440` 画面（obs 需设置为 1920x1080）。

- 许可证：GPL-2.0-or-later
- 作者：Yiyan <me@y1yan.com>
- 目标平台：Windows 10 / 11（x64）
- OBS 版本：31.1.1

---

## 目录

- [工作原理](#工作原理)
- [项目结构](#项目结构)
- [各文件职责与接口](#各文件职责与接口)
- [识别状态列表](#识别状态列表)
- [资源文件](#资源文件)
- [构建](#构建)
- [安装](#安装)
- [在 OBS 中使用](#在-obs-中使用)
- [语音事件对照表](#语音事件对照表)
- [计时参考](#计时参考)
- [日志](#日志)
- [打包与交付](#打包与交付)
- [常见问题](#常见问题)
- [已知限制](#已知限制)
- [后续计划](#后续计划)

---

## 工作原理

```text
OBS 视频来源
      │
      ▼
reporter-capture      每 100ms 截取一帧，缩放为 1920x1080
      │
      ▼
帧队列（只保留最新帧）
      │
      ▼
reporter-worker       工作线程
      ├─ reporter-template   灰度转换、模板匹配
      ├─ reporter-detector   汇总每个状态的匹配结果与 ROI
      └─ reporter-statemachine  状态机，判断状态变化
                │
                ▼
        reporter-trigger   事件日志 + 播放 MP3
```

关键设计：

- 截图在 OBS 渲染线程执行，识别在独立工作线程执行，不阻塞渲染。
- 队列只保留最新帧，处理不过来时丢旧帧，避免延迟累积。
- 状态机只使用字符串状态名，判断器进行判断，命中即切换状态。
- 模板匹配使用归一化互相关，允许一像素偏移。

---

## 项目结构

```text
Kanami-Reporter/
├── CMakeLists.txt                  构建脚本
├── buildspec.json                  项目名、版本、OBS 依赖版本
├── build-install.ps1               一键编译、安装、打包
├── README.md
├── data/                           插件资源，安装到 OBS 的 data 目录
│   └── locale/
├── cmake/                          OBS 插件模板提供的构建辅助
└── src/
    ├── plugin-main.c               模块入口
    ├── voice-filter.c/.h           OBS 滤镜注册与生命周期
    ├── reporter-types.h            公共类型与常量
    ├── reporter-context.h          滤镜实例结构
    ├── reporter-states.h/.c        状态定义
    ├── reporter-statemachine.h/.c  状态机
    ├── reporter-detector.h/.c      状态判断器、ROI、识别占位函数
    ├── reporter-template.h/.c      模板文件读写、匹配、预览导出
    ├── reporter-capture.h/.c       截图与帧队列
    ├── reporter-settings.h/.c      按事件保存/恢复 ROI
    ├── reporter-properties.h/.c    OBS 滤镜属性界面
    ├── reporter-worker.h/.c        工作线程
    ├── reporter-trigger.h/.c       事件到语音的映射与播放
    └── reporter-import.h/.c        资源包导入
```

运行时目录：

```text
C:\Program Files\obs-studio\
├── obs-plugins\64bit\Kanami-Reporter.dll
└── data\obs-plugins\Kanami-Reporter\

%APPDATA%\obs-studio\plugin_config\Kanami-Reporter\
├── templates\*.krt                 识别模板
├── voices\*.mp3                    语音文件
└── roi-preview.bmp                 ROI 预览输出
```

---

## 各文件职责与接口

完整实现请查看对应源文件，本节只说明职责与对外符号。

### plugin-main.c

模块入口。注册滤镜，卸载时释放音频播放器。

| 函数 | 说明 |
|---|---|
| `obs_module_load()` | 注册 `kanami_reporter_filter` |
| `obs_module_unload()` | 调用 `reporter_audio_shutdown()` |

### voice-filter.c / voice-filter.h

OBS 滤镜本体。定义 `obs_source_info`，处理创建、销毁、参数更新和渲染回调。
渲染回调中按 `CAPTURE_NS` 间隔触发截图，并始终 `obs_source_skip_video_filter()`，
保证不影响原始画面。

`voice-filter.h` 导出 `kanami_reporter_filter`。

### reporter-types.h

公共类型与常量，被几乎所有模块引用。

| 名称 | 说明 |
|---|---|
| `FRAME_W` / `FRAME_H` | 识别基准分辨率，当前 `1920x1080`（obs 画面分辨率） |
| `FRAME_BYTES` | 单帧 BGRA 字节数 |
| `CAPTURE_NS` | 截图间隔，`100ms` |
| `MAX_TEMPLATE_PIXELS` | 单个模板面积上限，`262144` |
| `struct reporter_roi` | 识别区域 |
| `struct template_data` | 模板像素、ROI、均值、能量 |
| `struct frame_data` | 一帧图像及其灰度视图、时间戳、阈值 |
| `reporter_frame_free()` | 释放帧 |

### reporter-context.h

`struct kanami_reporter_data`：一个滤镜实例的全部运行时状态，包含截图资源、
任务队列、模板数组和各线程共享的标志。共享字段由 `CRITICAL_SECTION lock` 保护。

### reporter-states.h / reporter-states.c

状态枚举与名称表。

| 名称 | 说明 |
|---|---|
| `enum state_id` | 状态编号，顺序即判断顺序 |
| `STATE_COUNT` | 状态总数 |
| `state_names[]` | 状态字符串，同时用于模板文件名 |

### reporter-statemachine.h / reporter-statemachine.c

状态机本体。每帧更新游戏信息，触发射击类与计时类事件，然后按顺序尝试状态判断，
命中后切换到新状态并记录进入时间戳。

| 函数 | 说明 |
|---|---|
| `reporter_state_reset()` | 复位全部状态与内部计数 |
| `reporter_state_process_frame(void *argument)` | 处理一帧 |
| `reporter_state_get_current()` | 当前状态名 (deprecated) |
| `reporter_state_set_target_score()` | 设置目标分数 (deprecated) |

计时使用状态进入时间戳与截图时间戳的差值，不依赖每帧累加。

### reporter-detector.h / reporter-detector.c

状态判断与识别区域管理。`reporter_check_state()` 使用 `data->processing_frame`
的灰度图和 `data->models[state_id]` 执行匹配，并记录该状态对应的 ROI。

| 函数 | 说明 |
|---|---|
| `reporter_detector_begin_frame()` | 清空本帧匹配结果 |
| `reporter_check_state()` | 判断指定状态是否成立 |
| `reporter_detector_set_result()` | 写入匹配结果 |
| `reporter_set_state_roi()` / `reporter_get_state_roi()` | 读写状态 ROI |
| `reporter_get_player_kill()` | 本回合击杀数（占位） |
| `reporter_get_game_score()` | 双方比分（占位） |
| `reporter_get_target_score()` | 目标分数（占位） |
| `reporter_get_alive_players()` | 双方存活人数（占位） |
| `reporter_get_bomber_alive()` | 炸弹携带者是否存活（占位） |

标记为占位的函数目前返回固定值，接入 OCR 后只需替换这些函数，
状态机逻辑不需要修改。

### reporter-template.h / reporter-template.c

模板文件格式、读写、预览导出与图像比较。

| 函数 | 说明 |
|---|---|
| `reporter_valid_roi()` | ROI 合法性检查 |
| `reporter_template_path()` | 由状态编号生成模板路径 |
| `reporter_template_save()` / `reporter_template_load()` | KRT 读写 |
| `reporter_template_free()` | 释放模板 |
| `reporter_to_grayscale()` | BGRA 转灰度 |
| `reporter_template_score()` | 归一化互相关 |
| `reporter_save_roi_preview()` | 输出带红框的 BMP 预览 |

### reporter-capture.h / reporter-capture.c

在渲染线程中把来源画面缩放渲染到 `1920x1080`，回读像素并放入待处理队列。
队列满时丢弃旧帧并累加 `dropped` 计数。

| 函数 | 说明 |
|---|---|
| `reporter_capture_frame()` | 截取并提交一帧 |
| `reporter_frame_free()` | 释放帧 |

### reporter-settings.h / reporter-settings.c

按事件分别保存和恢复 ROI。切换事件时保存旧值、恢复新值的坐标；
若该事件尚无界面设置，则从已存在的模板文件中读取 ROI。

| 函数 | 说明 |
|---|---|
| `reporter_setting_int()` | 带范围限制的整数读取 |
| `reporter_settings_sync_event_roi()` | 同步当前事件 ROI |
| `reporter_settings_event_changed()` | 事件下拉框回调 |

### reporter-properties.h / reporter-properties.c

OBS 属性界面：启用开关、匹配阈值、事件选择、ROI 输入、
保存预览、保存模板、导入资源包。

| 函数 | 说明 |
|---|---|
| `reporter_defaults()` | 默认值 |
| `reporter_properties()` | 构建属性列表 |

### reporter-worker.h / reporter-worker.c

工作线程主循环，以及模板训练。

| 函数 | 说明 |
|---|---|
| `reporter_worker()` | 线程入口 |
| `train_template()` | 从当前帧保存模板 |

循环内依次处理：关闭信号、资源重载请求、预览请求、模板训练请求、
灰度转换、状态处理、性能统计。

### reporter-trigger.h / reporter-trigger.c

事件与语音文件的映射表，以及 MP3 播放。

| 名称 | 说明 |
|---|---|
| `struct reporter_voice_entry` | 事件名与候选语音文件名 |
| `reporter_voice_table[]` | 映射表 |
| `reporter_trigger()` | 记录日志并播放语音 |
| `reporter_audio_shutdown()` | 停止播放并释放设备 |

播放使用 Windows MCI：
- 单实例播放，新播报会中断上一条。
- 异步播放，不阻塞状态机。
- 语音文件从 `voices` 目录读取，支持中文文件名。

### reporter-import.h / reporter-import.c

从资源包导入 `templates` 与 `voices`。

| 函数 | 说明 |
|---|---|
| `reporter_import_resources_from_dialog()` | 打开文件选择器并导入 |

实现要点：
- 使用 Win32 `GetOpenFileNameW` 选择 zip。
- 使用系统自带 `tar.exe` 解压，无第三方依赖。
- 解压到临时目录，识别 `templates` / `voices` 的实际根目录。
- 复制到插件配置目录，导入完成后通知工作线程重载模板。
- 兼容“根目录直接是资源”和“根目录是 Kanami-Reporter”两种压缩包结构。

### voice-filter.h

声明 `kanami_reporter_filter`。

---

## 识别状态列表

间隔固定时间根据截图判断当前状态，若和上一时刻不同则切换状态。
所有状态定义于 `src/reporter-states.h`。

| 编号 | 状态名 | 含义 |
|---:|---|---|
| 0 | `game_choose_character` | 角色选择界面 |
| 1 | `game_start_attacker` | 进攻方开场 |
| 2 | `game_start_defender` | 防守方开场 |
| 3 | `game_switch_side` | 攻守互换 |
| 4 | `round_start` | 准备阶段 |
| 5 | `round_ingame` | 正式回合进行中 |
| 6 | `round_ingame_bomb_planted` | 炸弹已安装 |
| 7 | `round_end_win` | 回合胜利 |
| 8 | `round_end_lose` | 回合战败 |
| 9 | `round_win_five_kill` | 单人消灭对方全队 |
| 10 | `round_win_all_alive` | 胜利时我方全体存活 |
| 11 | `round_win_alive_1` | 胜利时我方仅存活 1 人 |
| 12 | `game_end_win` | 整场胜利 |
| 13 | `game_end_lose` | 整场失败 |
| 14 | `game_end_draw` | 平局 |

每个状态对应一个模板文件，文件名即状态名加 `.krt` 后缀。

---

## 资源文件

### 模板文件（KRT）

二进制格式，单文件对应一个状态：

| 偏移 | 长度 | 内容 |
|---:|---:|---|
| 0 | 4 | 魔数 `KRT1` |
| 4 | 4 | 版本号 `1` |
| 8 | 4 | 帧宽 `1920` |
| 12 | 4 | 帧高 `1080` |
| 16 | 4 | ROI X |
| 20 | 4 | ROI Y |
| 24 | 4 | ROI 宽 |
| 28 | 4 | ROI 高 |
| 32 | N | ROI 灰度像素，逐行存放 |

模板必须来自同分辨率的画面。分辨率不一致时会被拒绝加载。

### 语音文件

放在配置目录的 `voices` 子目录下，文件名必须与 `reporter-trigger.c` 中
映射表里的字符串完全一致，包括全角标点和扩展名。建议使用 `.mp3`。

### 配置文件

滤镜参数（启用开关、阈值、事件选择、各事件 ROI）由 OBS 保存在
场景集合中，不在插件配置目录内。

---

## 构建

### 环境要求

- Windows 10 / 11 x64
- Visual Studio 2022 或更高版本，含 C++ 桌面开发工作负载
- CMake 3.28 及以上
- OBS Studio 31.1.1（官方安装版，非 Microsoft Store 版）
- PowerShell 5.1 及以上

OBS 源码与依赖由 CMake 自动下载到 `.deps/` 目录，首次配置需要联网。

### 手动编译

```powershell
cmake -S . -B build -A x64 -DENABLE_FRONTEND_API=OFF -DENABLE_QT=OFF
cmake --build build --config RelWithDebInfo --parallel
```

---

## 安装

插件本体：

```text
build\RelWithDebInfo\Kanami-Reporter.dll
  → C:\Program Files\obs-studio\obs-plugins\64bit\

data\*
  → C:\Program Files\obs-studio\data\obs-plugins\Kanami-Reporter\
```

安装后完全退出并重新启动 OBS。

## 安装配置

将 `templates` 和 `voices` 放入：

```text
%APPDATA%\obs-studio\plugin_config\Kanami-Reporter\
```

也可以使用插件内的**资源包导入**功能自动完成。

---

## 在 OBS 中使用

1. 添加游戏采集或显示器采集来源。
2. 右键来源，选择**滤镜**。
3. 点击 `+`，选择 **Kanami Reporter**。
4. 保持 **Enable recognition** 关闭，先制作模板。
5. 选择 **Template event**，填写 ROI，点击 **Save ROI preview**。
6. 打开日志中的预览路径，确认红框准确圈住目标文字或图标。
7. 点击 **Save / replace event template** 保存该事件的模板。
8. 对所有需要的状态重复 5 至 7 步。
9. 勾选 **Enable recognition**。

### 属性说明

| 属性 | 说明 |
|---|---|
| Enable recognition | 是否执行识别 |
| Template match threshold | 匹配阈值，默认 0.90 |
| Template event | 当前正在编辑的事件 |
| Template X / Y | ROI 左上角坐标，基准 `1920x1080` |
| Template width / height | ROI 尺寸 |
| Save ROI preview | 输出带红框的预览图 |
| Save / replace event template | 保存当前事件的模板 |
| Import resource package (zip) | 导入资源包 |

### ROI 注意点

- 坐标以来源原始分辨率为基准，不是 OBS 输出分辨率，也不是缩放后的尺寸。
- 所有事件共用一组输入框，但每个事件会各自记住自己的坐标。
- 修改坐标只影响界面设置，点击保存模板后才会更新实际匹配区域。
- 模板区域应紧贴文字或图标，避免大块动态背景。

---

## 语音事件对照表

事件名必须与 `reporter-trigger.c` 中的映射表完全一致。

详见 `reporter-trigger.c` 。
