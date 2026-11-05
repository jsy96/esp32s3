# ESP32-S3 体感倾斜游戏手柄

把 ESP32-S3 开发板 + MPU-6050 六轴传感器绑在身上，身体**前倾/后仰/左倾/右倾**实时变成游戏手柄摇杆，通过 USB 线插上电脑即被识别为 DirectInput 游戏手柄（免驱动）。

## 更新记录（最新在前）

| 日期 | 版本 | 内容 |
|------|------|------|
| 2026-11-05 | v1.0.261105-1 | 改用 arduino-cli 命令行环境（免装 Arduino IDE）：新增 `setup1.bat` 一键装工具链（乐鑫国内镜像+ghfast 加速，免代理）、`flash1.bat` 一键编译烧录；固件改用 esp32 核心 3.x 官方 `USBHIDGamepad` 类（摇杆轴改为 ±127/中位 0）；**固件已编译通过**（核心 3.3.12，403KB / 30% Flash） |
| 2026-11-04 | v1.0.261104-2 | 新增 `git1.bat` 一键推送脚本（自动设置 GitHub 代理 + 失败自动重试）；配置远程仓库 `github.com/jsy96/esp32s3` 并完成首次推送 |
| 2026-11-04 | v1.0.261104-1 | 初版：固件（MPU-6050 驱动 + Mahony 姿态融合 + USB HID 手柄 + BOOT 键回中）、串口遥测协议、调参工具 tune_viewer.py、项目文档 |

**当前验证状态（2026-11-05）**：
- ✅ **编译通过**（esp32 核心 3.3.12，403KB / 30% Flash，RAM 17%）
- ✅ **上板冒烟测试通过**：经 UART 口（CH343，COM7）烧录成功并校验；固件启动后原生 USB 口正常枚举出 **CDC 串口（COM9）+ HID 游戏手柄**（Windows 设备管理器出现 "HID-compliant game controller"）；串口日志按设计输出 I2C 无应答重试（MPU-6050 未接，属预期），板载 LED 红闪
- ⏳ 待 MPU-6050 到货后接线实测：姿态解算 → 摇杆映射 → joy.cpl 全流程

## 工作原理

```
[MPU-6050 六轴传感器]        [ESP32-S3 固件]                      [电脑]
 加速度计+陀螺仪   --I2C-->  200Hz 采样
  绑在腰前(水平)             Mahony 姿态融合(去重力/抗晃动)
                             相对"中位姿势"的倾角
                             前后倾->摇杆Y  左右倾->摇杆X
                             死区+满偏角度+平滑        --USB HID--> 识别为
                                                                        游戏手柄
```

关键设计：

1. **用"倾斜"而不是"平移"**：倾斜角来自重力方向，稳定、零漂移、游戏手感好；平移靠加速度积分，噪声大易漂移。
2. **中位可校准**：上电站稳后自动把"当前姿势"记为中位（不要求绝对水平站立）；游戏中长按 BOOT 键 2 秒可随时重新回中。
3. **偏航免疫**：倾角提取在数学上不受水平转身（偏航）影响，身体原地转向不会误触摇杆。
4. **回中后输出自动归零**，配合死区（默认 ±5°），站稳时摇杆不会漂。

## 硬件清单

| 物品 | 说明 | 状态 |
|------|------|------|
| ESP32-S3 N16R8 开发板 | 已购（配套资料在 `../ESP32-S3开发板N16R8焊接/`） | 已有 |
| MPU-6050 模块（GY-521） | 淘宝约 5~10 元，含稳压和 I2C 上拉 | 待购 |
| 杜邦线 母对母 ×4 | 连接 IMU | 待购 |
| USB 数据线 Type-C | 烧录+供电+手柄信号；**必须插开发板 "USB" 口（原生USB），不是 "UART/COM" 口** | 已有 |
| 佩戴方式 | 腰带/腰包/斜挎，保证传感器**水平、元件面朝上、丝印X轴指向身体正前方** | 自备 |

## 接线（4 根线）

| MPU-6050 模块 (GY-521) | ESP32-S3 开发板 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |

AD0 引脚悬空（I2C 地址 0x68）。改了引脚的话同步改固件顶部 `I2C_SDA_PIN / I2C_SCL_PIN`。

## 烧录步骤（arduino-cli 命令行，主路径）

环境只用命令行，不装 Arduino IDE；下载全部走国内镜像，**不需要开代理**。

**一次性环境安装（已在本机完成）：**

1. 下载 arduino-cli（Windows 64bit zip）：`https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip`，解压出 `arduino-cli.exe` 放到 `tools/arduino-cli/` 目录
2. 双击 `setup1.bat`：自动更新板包索引 → `tools/fetch_esp32_core.py` 走国内镜像（乐鑫 dl.espressif.com + ghfast.top）下载全部工具链约 1.9GB 并校验 SHA256 → 离线安装 esp32 核心

**日常编译烧录：**

1. 开发板插 **"USB" 口**（原生 USB，不是 UART 口）
2. 双击 `flash1.bat`（自动检测串口；也可 `flash1.bat COM7` 指定）——编译+烧录+自动重启
3. 首次烧录若连不上：**按住 BOOT 不放、按一下 RST、松开 BOOT** 进下载模式再跑一次
4. 验证：`Win+R` → `joy.cpl` 出现手柄，晃动测试摇杆

固件编译参数（`flash1.bat` 内置，改板子才需要动）：
`esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,UploadMode=cdc,FlashSize=16M,PSRAM=opi`
（注意 `USBMode=default` 就是 USB-OTG(TinyUSB)，`hwcdc` 反而是硬件 CDC——别被名字骗了）

<details><summary>备选：Arduino IDE 烧录（图形界面路径）</summary>

1. 开发板管理器安装 esp32 by Espressif Systems（3.x）
2. 打开 `firmware/tilt_gamepad/tilt_gamepad.ino`
3. 工具菜单：开发板 `ESP32S3 Dev Module`、USB Mode `USB-OTG (TinyUSB)`、USB CDC On Boot `Enabled`、Flash Size `16MB`、PSRAM `OPI PSRAM`
4. 选端口点上传；失败就 BOOT 进下载模式重试

</details>

## 使用方法

1. 把开发板+传感器**水平**绑在腰前（丝印 X 轴指向身体正前方，元件面朝上）。
2. 插 USB，**保持静止约 5 秒**：LED 红闪=陀螺校准 → 黄色常亮=姿态收敛 → 绿色微亮=就绪（此时姿势自动记为中位）。
3. 进游戏：前倾=摇杆前推，后仰=后拉，右倾=右，左倾=左。
4. **BOOT 键**：按住=手柄按键1（可当确认键）；长按 2 秒=以当前姿势重新回中（换姿势/重新佩戴后用，LED 绿闪确认）。
5. 测试手柄：`joy.cpl` → 属性 → 晃身体看十字光标。

## 轴映射

| 手柄控件 | 来源 | 数值 |
|---|---|---|
| 左摇杆 X | 身体左/右倾（右倾增大） | -127~127，中位 0 |
| 左摇杆 Y | 身体前/后倾（前倾增大） | -127~127，中位 0 |
| Z / Rz / Rx / Ry | 预留，恒居中 | 0 |
| 帽键(HAT) | 预留，恒居中 | — |
| 按键 | BUTTON_A = BOOT 键，其余预留 | — |

## 调参（改固件顶部宏后重新烧录）

| 参数 | 默认 | 作用 | 什么时候调 |
|---|---|---|---|
| `TILT_DEADBAND_DEG` | 5° | 死区角度 | 站稳时摇杆仍漂 → 调大；小动作没反应 → 调小 |
| `TILT_FULL_DEG` | 35° | 摇杆推满所需倾角 | 觉得要倾太多才满偏 → 调小（如 25） |
| `AXIS_SMOOTH` | 0.35 | 输出平滑（0~1） | 抖动大 → 调小；觉得迟钝 → 调大 |
| `INVERT_PITCH` / `INVERT_ROLL` | 0 | 前后/左右方向反转 | 佩戴方向导致轴向颠倒时 |
| `AXIS_*_SRC` / `AXIS_*_SIGN` | 恒等 | 安装方向重映射 | 没法水平佩戴、需换面/换向绑时（问我也行） |
| `MAHONY_TWO_KP` | 2.0 | 姿态收敛速度 | 一般不动 |

## 实时调参工具 tune_viewer.py

插好 "USB" 口后运行（本机 Python 走 uv）：

```bash
uv run --no-project --with pyserial python tools/tune_viewer.py
# 或指定串口
uv run --no-project --with pyserial python tools/tune_viewer.py --port COM7
```

- 实时显示横滚/俯仰角度条形图、摇杆输出值、原始加速度，用于核对佩戴方向和选取死区/满偏参数。
- 运行中：`r`=远程回中，`q`=退出。固件日志（校准进度、错误）直接透传显示。
- 也可用任意串口终端（115200）手动发命令：`s`=开遥测，`x`=停，`r`=回中，`v`=查版本。

## 游戏兼容性

- Windows 直接识别为 **DirectInput** 手柄：`joy.cpl`、大多数模拟器（RetroArch 等）、老游戏可直接用。
- Steam 游戏（多认 XInput）：Steam 里 手柄厂商支持/控制器设置 中勾选 generic controller 支持即可映射；个别只认 Xbox 手柄的游戏，后续可加 viGEm 桥接版（PC 端小程序转 XInput），需要再说。

## 常见问题排查

| 现象 | 原因与处理 |
|---|---|
| 电脑识别不到手柄 | Arduino 没设 `USB Mode: USB-OTG (TinyUSB)`；或插的是 "UART" 口而不是 "USB" 口 |
| 串口没输出/工具连不上 | 没设 `USB CDC On Boot: Enabled`；或被其他串口程序占用 |
| 上电一直红闪不进运行 | 陀螺校准期间在动，保持静止即可（最多重试 5 次后会带警告继续） |
| 串口报 I2C 无应答 | SDA/SCL 接反或没接好；固件会自动扫描并打印总线上的地址，据此排查 |
| 方向反了（前倾变后拉等） | 改 `INVERT_PITCH` / `INVERT_ROLL` |
| 角度漂移、站姿不稳 | 重新长按 BOOT 回中；检查佩戴是否松动晃荡 |
| 遥测显示角度对但游戏没反应 | 游戏不认 DirectInput，见"游戏兼容性" |

## 一键推送 git1.bat

改完代码后双击 `git1.bat`（或命令行运行）即可完成 add → commit → push 到 GitHub（`https://github.com/jsy96/esp32s3.git`，main 分支）：

- 有改动时提示输入提交信息，直接回车用默认 `update`
- 自动把 github.com 的 git 流量指到本机代理 `127.0.0.1:7890`（直连被墙；Clash 端口变了改脚本顶部 `PROXY_PORT`）
- 当前代理线路不稳定（实测约一半连接被重置），脚本内置**失败自动重试 5 次**；连续失败说明该节点不可用，换 Clash 节点后重跑

## 目录结构

```
esp32s3-tilt-gamepad/
├── README.md                          本文档
├── git1.bat                           一键 add/commit/push 到 GitHub（带代理+重试）
├── setup1.bat                         一次性环境安装（国内镜像下载工具链，免代理）
├── flash1.bat                         一键编译+烧录（自动检测串口，失败自动重试）
├── firmware/
│   └── tilt_gamepad/
│       └── tilt_gamepad.ino           固件主程序（Arduino 框架, ESP32-S3）
└── tools/
    ├── tune_viewer.py                 串口实时调参工具（Python, pyserial）
    ├── fetch_esp32_core.py            工具链国内镜像下载脚本（乐鑫镜像+ghfast 回退）
    └── arduino-cli/                   arduino-cli.exe 所在目录（不入库）
```

## 版本说明

版本号集中在固件 `tilt_gamepad.ino` 顶部 `APP_VERSION` 常量（格式 `v主.次.YYMMDD-当日序号`），上电时打印到串口，调参工具发 `v` 也可查询；每次修改后同步更新本 README 的更新记录。
