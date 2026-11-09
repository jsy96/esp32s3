/*
 * ============================================================
 *  ESP32-S3 + MPU-6050/MPU-6500 体感倾斜游戏手柄固件
 *  版本: v1.0.261109-1
 * ============================================================
 *  功能:
 *    - MPU-6050 (I2C, 200Hz) 采样 + Mahony 六轴姿态融合
 *    - 身体前倾/后仰 -> 手柄左摇杆 Y 轴
 *    - 身体左倾/右倾 -> 手柄左摇杆 X 轴
 *    - USB 原生口枚举为 DirectInput 游戏手柄 (TinyUSB HID)
 *    - BOOT 按键: 按住 = 手柄按键1; 长按 2 秒 = 重新回中(以当前姿势为中位)
 *    - USB 串口遥测协议 (配套 tools/tune_viewer.py 调参工具)
 *
 *  接线 (GY-521 / MPU-6050 模块):
 *      模块 VCC -> 开发板 3V3
 *      模块 GND -> 开发板 GND
 *      模块 SDA -> GPIO8
 *      模块 SCL -> GPIO9
 *      (AD0 引脚悬空, I2C 地址 0x68)
 *
 *  佩戴方向 (默认配置, 板子水平佩戴):
 *      传感器水平放置, 元件面朝上 (Z 轴朝上),
 *      模块丝印 X 轴指向身体正前方 (如绑在腰带前方)
 *      前倾 = 摇杆 Y 增大(前推), 右倾 = 摇杆 X 增大
 *      实际方向不对时改下方 INVERT_PITCH / INVERT_ROLL
 *
 *  Arduino IDE 设置 (重要):
 *      开发板:          ESP32S3 Dev Module
 *      USB Mode:        USB-OTG (TinyUSB)   <- 手柄功能必须
 *      USB CDC On Boot: Enabled             <- 串口遥测/日志必须
 *      Flash Size: 16MB, PSRAM: OPI PSRAM   <- N16R8
 *  烧录和使用手柄功能必须插开发板的 "USB" 口(原生USB), 不是 "UART/COM" 口
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include "USBHIDGamepad.h"

#define APP_VERSION "v1.0.261109-1"

/* ---------------- 引脚配置 ---------------- */
#define I2C_SDA_PIN   8      /* MPU-6050 SDA */
#define I2C_SCL_PIN   9      /* MPU-6050 SCL */
#define BUTTON_PIN    0      /* BOOT 按键 (低电平有效) */

/* ---------------- 安装方向重映射 ----------------
 * 默认: 传感器水平佩戴、元件面朝上、模块丝印X轴指向身体正前方
 *   机体坐标系约定: X=前, Y=左, Z=上
 * 若安装方向不同, 修改三个轴的来源 (0=丝印X轴 1=丝印Y轴 2=丝印Z轴)
 * 与符号 (+1.0/-1.0); 三个来源不能重复 */
#define AXIS_FORWARD_SRC   0
#define AXIS_LEFT_SRC      1
#define AXIS_UP_SRC        2
#define AXIS_FORWARD_SIGN  1.0f
#define AXIS_LEFT_SIGN     1.0f
#define AXIS_UP_SIGN       1.0f

/* ---------------- 手感参数 ---------------- */
#define TILT_DEADBAND_DEG  5.0f    /* 死区角度: 小于该角度摇杆回中 */
#define TILT_FULL_DEG     35.0f    /* 满偏角度: 达到该角度摇杆推满 */
#define AXIS_SMOOTH        0.35f   /* 输出平滑系数 0~1, 越小越平滑也越迟钝 */
#define INVERT_PITCH       0       /* 1 = 前后方向反转 */
#define INVERT_ROLL        0       /* 1 = 左右方向反转 */

/* ---------------- 姿态滤波参数 ---------------- */
#define MAHONY_TWO_KP   2.0f       /* 姿态收敛速度 (2x比例增益) */
#define MAHONY_TWO_KI   0.0f       /* 积分增益; 上电已做陀螺零偏校准, 保持 0 */

/* ---------------- 采样/发送 ---------------- */
#define SAMPLE_PERIOD_US   5000    /* 采样周期 5ms = 200Hz */
#define RECENTER_HOLD_MS   2000    /* BOOT 长按回中时间 */
#define SETTLE_MS          1500    /* 姿态收敛等待时间 */
#define CAL_SAMPLES        600     /* 陀螺零偏校准采样数 (600 x 5ms = 3s) */
#define CAL_STILL_DPS      0.35f   /* 校准时判静止的陀螺噪声阈值 (dps rms) */
#define CAL_MAX_RETRY      5       /* 静止检测最多重试次数, 之后带警告接受 */

/* ---------------- 遥测协议 (USB 串口) ----------------
 * 主机 -> 固件: 's'=开启遥测  'x'=停止遥测  'r'=远程回中  'v'=查询版本
 * 固件 -> 主机: #T <ms> <roll_deg> <pitch_deg> <outX> <outY> <ax> <ay> <az> <state>
 *   state: 0=陀螺校准 1=姿态收敛 2=运行 */

/* ---------------- 摇杆数值范围 (官方手柄描述符为 8 位轴) ---------------- */
#define AXIS_MAX      127     /* 摇杆满偏值, 中位 0 */

/* ============================================================
 *  LED (板载 RGB, 无 RGB 的板子自动退化为纯串口提示)
 * ============================================================ */
#ifdef RGB_BUILTIN
  #define LED_SUPPORTED 1
#else
  #define LED_SUPPORTED 0
#endif

static void ledSet(int r, int g, int b) {
#if LED_SUPPORTED
  neopixelWrite(RGB_BUILTIN, r, g, b);
#else
  (void)r; (void)g; (void)b;
#endif
}

/* ============================================================
 *  MPU-6050/MPU-6500 寄存器级 I2C 驱动
 * ============================================================ */
#define MPU_ADDR              0x68
#define REG_SMPLRT_DIV        0x19
#define REG_CONFIG            0x1A
#define REG_GYRO_CONFIG       0x1B
#define REG_ACCEL_CONFIG      0x1C
#define REG_ACCEL_CONFIG2     0x1D       /* MPU-6500 only */
#define REG_ACCEL_XOUT_H      0x3B
#define REG_USER_CTRL         0x6A
#define REG_PWR_MGMT_1        0x6B
#define REG_SIGNAL_PATH_RESET 0x68
#define REG_WHO_AM_I          0x75

#define ACCEL_LSB_PER_G       16384.0f   /* 量程 +-2g */
#define GYRO_LSB_PER_DPS      65.5f      /* 量程 +-500dps */

static float gyroBias[3] = { 0, 0, 0 };      /* 陀螺零偏 (重映射后的机体轴, dps) */
static bool gyroBiasValid = false;

static bool mpuWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool mpuReadRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static uint8_t mpuWho = 0;

/* 写入后读回确认, 避免通信失败或配置未生效时仍报告初始化成功 */
static bool mpuConfigure(uint8_t reg, uint8_t value) {
  uint8_t actual = 0;
  return mpuWrite(reg, value) && mpuReadRegs(reg, &actual, 1) && actual == value;
}

/* 返回: 0=成功, -1=I2C无应答, -2=配置失败, -3=未知型号(含ID=0) */
static int mpuInit(void) {
  if (!mpuReadRegs(REG_WHO_AM_I, &mpuWho, 1)) return -1;
  if (mpuWho != 0x68 && mpuWho != 0x70) return -3;

  if (!mpuWrite(REG_PWR_MGMT_1, 0x80)) return -2;  /* DEVICE_RESET */
  delay(100);
  if (!mpuWrite(REG_SIGNAL_PATH_RESET, 0x07)) return -2;
  delay(100);
  if (!mpuConfigure(REG_PWR_MGMT_1, 0x01)) return -2; /* PLL时钟, 唤醒 */
  delay(50);
  if (!mpuConfigure(REG_SMPLRT_DIV, 4) ||      /* 1kHz/(1+4) = 200Hz */
      !mpuConfigure(REG_CONFIG, 0x03) ||       /* 陀螺DLPF: 6050=42Hz, 6500=41Hz */
      !mpuConfigure(REG_GYRO_CONFIG, 0x08) ||  /* +-500dps, 6500 FCHOICE_B=0 */
      !mpuConfigure(REG_ACCEL_CONFIG, 0x00))  /* +-2g, 两型号比例系数相同 */
    return -2;
  /* 6500 的加速度DLPF独立配置; 6050没有此寄存器, 不可写入 */
  if (mpuWho == 0x70 && !mpuConfigure(REG_ACCEL_CONFIG2, 0x03)) return -2;
  delay(50);
  return 0;
}

/* 扫描 I2C 总线打印应答地址 (仅接线排错用) */
static void i2cScan(void) {
  Serial.print("#I2C 扫描到地址:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", a);
    }
  }
  Serial.println();
}

/* 读取一次数据, 单位: 加速度 g, 角速度 dps
 * 先按安装方向重映射, 再扣除陀螺零偏 (零偏存于重映射后的机体轴) */
static bool imuRead(float acc[3], float gyr[3]) {
  uint8_t b[14];
  if (!mpuReadRegs(REG_ACCEL_XOUT_H, b, 14)) return false;

  int16_t ax = (int16_t)((b[0] << 8) | b[1]);
  int16_t ay = (int16_t)((b[2] << 8) | b[3]);
  int16_t az = (int16_t)((b[4] << 8) | b[5]);
  int16_t gx = (int16_t)((b[8] << 8) | b[9]);
  int16_t gy = (int16_t)((b[10] << 8) | b[11]);
  int16_t gz = (int16_t)((b[12] << 8) | b[13]);

  float a[3] = { ax / ACCEL_LSB_PER_G, ay / ACCEL_LSB_PER_G, az / ACCEL_LSB_PER_G };
  float g[3] = { gx / GYRO_LSB_PER_DPS, gy / GYRO_LSB_PER_DPS, gz / GYRO_LSB_PER_DPS };

  const uint8_t src[3] = { AXIS_FORWARD_SRC, AXIS_LEFT_SRC, AXIS_UP_SRC };
  const float  sgn[3]  = { AXIS_FORWARD_SIGN, AXIS_LEFT_SIGN, AXIS_UP_SIGN };
  for (int i = 0; i < 3; i++) {
    acc[i] = a[src[i]] * sgn[i];
    gyr[i] = g[src[i]] * sgn[i];
  }

  if (gyroBiasValid) {
    gyr[0] -= gyroBias[0];
    gyr[1] -= gyroBias[1];
    gyr[2] -= gyroBias[2];
  }
  return true;
}

/* ============================================================
 *  Mahony 六轴姿态融合 (角速度单位 rad/s, 加速度任意单位, 内部归一化)
 * ============================================================ */
static float q[4] = { 1.0f, 0.0f, 0.0f, 0.0f };   /* 机体->世界 姿态四元数 */
static float integralFBx = 0, integralFBy = 0, integralFBz = 0;

static float invSqrt(float x) {
  return 1.0f / sqrtf(x);   /* ESP32-S3 带 FPU, 直接算精度最好 */
}

static void mahonyUpdate(float gx, float gy, float gz,
                         float ax, float ay, float az, float dt) {
  float recipNorm;
  float halfvx, halfvy, halfvz;
  float qa, qb, qc;

  if (!(ax == 0.0f && ay == 0.0f && az == 0.0f)) {
    /* 用加速度计 (重力方向) 修正姿态 */
    recipNorm = invSqrt(ax * ax + ay * ay + az * az);
    ax *= recipNorm; ay *= recipNorm; az *= recipNorm;

    halfvx = q[1] * q[3] - q[0] * q[2];
    halfvy = q[0] * q[1] + q[2] * q[3];
    halfvz = q[0] * q[0] - 0.5f + q[3] * q[3];

    float ex = ay * halfvz - az * halfvy;
    float ey = az * halfvx - ax * halfvz;
    float ez = ax * halfvy - ay * halfvx;

    if (MAHONY_TWO_KI > 0.0f) {
      integralFBx += MAHONY_TWO_KI * ex * dt;
      integralFBy += MAHONY_TWO_KI * ey * dt;
      integralFBz += MAHONY_TWO_KI * ez * dt;
      gx += integralFBx; gy += integralFBy; gz += integralFBz;
    }
    gx += MAHONY_TWO_KP * ex;
    gy += MAHONY_TWO_KP * ey;
    gz += MAHONY_TWO_KP * ez;
  }

  /* 积分四元数微分方程 */
  gx *= 0.5f * dt; gy *= 0.5f * dt; gz *= 0.5f * dt;
  qa = q[0]; qb = q[1]; qc = q[2];
  q[0] += (-qb * gx - qc * gy - q[3] * gz);
  q[1] += ( qa * gx + qc * gz - q[3] * gy);
  q[2] += ( qa * gy - qb * gz + q[3] * gx);
  q[3] += ( qa * gz + qb * gy - qc * gx);

  recipNorm = invSqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  q[0] *= recipNorm; q[1] *= recipNorm; q[2] *= recipNorm; q[3] *= recipNorm;
}

static void quatConj(const float in[4], float out[4]) {
  out[0] =  in[0];
  out[1] = -in[1];
  out[2] = -in[2];
  out[3] = -in[3];
}

static void quatMul(const float a[4], const float b[4], float out[4]) {
  float r[4];
  r[0] = a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3];
  r[1] = a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2];
  r[2] = a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
  r[3] = a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
  out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = r[3];
}

/* ============================================================
 *  USB HID 游戏手柄 (esp32 核心 3.x 官方 USBHIDGamepad 类)
 *  8 位摇杆轴 (中位 0) + 帽键 + 32 按键, DirectInput 即插即用
 *  X = 左右倾, Y = 前后倾, BUTTON_A = BOOT 键
 * ============================================================ */
static USBHIDGamepad Gamepad;

/* ============================================================
 *  运行状态
 * ============================================================ */
enum WorkState { ST_GYRO_CAL = 0, ST_SETTLE = 1, ST_RUN = 2 };
static WorkState state = ST_GYRO_CAL;

static float qRef[4] = { 1, 0, 0, 0 };   /* 回中时 captured 中位姿态 */
static float smX = 0, smY = 0;           /* 平滑后的摇杆偏移 (-1~1) */
static bool  btnOut = false;             /* 按键1 输出状态 */
static bool  telemetryOn = false;        /* 主机 's'/'x' 控制 */
static uint32_t lastTickUs = 0;
static uint32_t lastTelemetryMs = 0;
static uint32_t settleStartMs = 0;
static uint32_t recenterFlashMs = 0;

/* 陀螺校准累计器 */
static float calSum[3] = { 0, 0, 0 };
static float calSumSq[3] = { 0, 0, 0 };
static int    calCount = 0;
static int    calRetry = 0;

/* I2C 连续失败计数 */
static int i2cFails = 0;

/* 倾角输出: 超出死区线性增长, 达到满偏角度输出满值; 保留符号 */
static float tiltToAxis(float deg) {
  float a = fabsf(deg);
  float f;
  if (a <= TILT_DEADBAND_DEG)       f = 0.0f;
  else if (a >= TILT_FULL_DEG)      f = 1.0f;
  else f = (a - TILT_DEADBAND_DEG) / (TILT_FULL_DEG - TILT_DEADBAND_DEG);
  return (deg < 0.0f) ? -f : f;
}

static int8_t axisToI8(float f) {
  float v = f * AXIS_MAX;
  if (v >  (float)AXIS_MAX) v =  AXIS_MAX;
  if (v < -(float)AXIS_MAX) v = -AXIS_MAX;
  return (int8_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

/* 以当前姿态为新中位 */
static void doRecenter(void) {
  if (state != ST_RUN) return;
  qRef[0] = q[0]; qRef[1] = q[1]; qRef[2] = q[2]; qRef[3] = q[3];
  smX = 0; smY = 0;
  recenterFlashMs = millis();
  Serial.println("#E 已回中 (当前姿势 = 中位)");
}

static void calibStart(void) {
  state = ST_GYRO_CAL;
  calSum[0] = calSum[1] = calSum[2] = 0;
  calSumSq[0] = calSumSq[1] = calSumSq[2] = 0;
  calCount = 0;
  gyroBiasValid = false;
  Serial.println("#E 开始陀螺零偏校准, 请保持静止...");
}

static void enterSettle(void) {
  state = ST_SETTLE;
  settleStartMs = millis();
  Serial.println("#E 陀螺校准完成, 姿态收敛中...");
}

static void enterRun(void) {
  qRef[0] = q[0]; qRef[1] = q[1]; qRef[2] = q[2]; qRef[3] = q[3];
  smX = 0; smY = 0;
  state = ST_RUN;
  recenterFlashMs = millis();
  Serial.println("#E 就绪: 当前姿势已设为中位, 长按BOOT可重新回中");
}

/* ============================================================
 *  各状态的单次采样处理 (200Hz 调用)
 * ============================================================ */
static void tickGyroCal(const float gyr[3]) {
  for (int i = 0; i < 3; i++) {
    calSum[i]   += gyr[i];
    calSumSq[i] += gyr[i] * gyr[i];
  }
  calCount++;

  if (calCount < CAL_SAMPLES) return;

  /* 计算均值与噪声, 判断校准期间是否真的静止 */
  float mean[3], rms[3];
  bool still = true;
  for (int i = 0; i < 3; i++) {
    mean[i] = calSum[i] / calCount;
    float var = calSumSq[i] / calCount - mean[i] * mean[i];
    rms[i] = (var > 0) ? sqrtf(var) : 0;
    if (rms[i] > CAL_STILL_DPS) still = false;
  }

  if (!still && calRetry < CAL_MAX_RETRY) {
    calRetry++;
    Serial.printf("#E 校准期间有晃动 (噪声 rms %.2f dps), 第 %d 次重试, 请保持静止\n",
                  rms[0] + rms[1] + rms[2], calRetry);
    calSum[0] = calSum[1] = calSum[2] = 0;
    calSumSq[0] = calSumSq[1] = calSumSq[2] = 0;
    calCount = 0;
    return;
  }
  if (!still) {
    Serial.println("#E 警告: 多次检测到晃动, 使用带噪声的零偏继续 (建议稍后断电重试)");
  }

  gyroBias[0] = mean[0];
  gyroBias[1] = mean[1];
  gyroBias[2] = mean[2];
  gyroBiasValid = true;
  Serial.printf("#E 陀螺零偏 (dps): %.3f %.3f %.3f\n",
                gyroBias[0], gyroBias[1], gyroBias[2]);
  enterSettle();
}

static void tickSettle(const float acc[3], const float gyr[3]) {
  mahonyUpdate(gyr[0] * DEG_TO_RAD, gyr[1] * DEG_TO_RAD, gyr[2] * DEG_TO_RAD,
               acc[0], acc[1], acc[2], 0.005f);
  if (millis() - settleStartMs >= SETTLE_MS) enterRun();
}

static void tickRun(const float acc[3], const float gyr[3], float dt) {
  mahonyUpdate(gyr[0] * DEG_TO_RAD, gyr[1] * DEG_TO_RAD, gyr[2] * DEG_TO_RAD,
               acc[0], acc[1], acc[2], dt);

  /* 相对中位姿态的残余旋转: qRel = conj(qRef) x q
   * 其俯仰/横滚即"偏离中位的倾角", 且不受水平转向(偏航漂移)影响 */
  float qc[4], qRel[4];
  quatConj(qRef, qc);
  quatMul(qc, q, qRel);

  float roll  = atan2f(2.0f * (qRel[0] * qRel[1] + qRel[2] * qRel[3]),
                       1.0f - 2.0f * (qRel[1] * qRel[1] + qRel[2] * qRel[2]));
  float pitch = asinf(fminf(1.0f, fmaxf(-1.0f,
                       2.0f * (qRel[0] * qRel[2] - qRel[3] * qRel[1]))));
  float rollDeg  = roll  * RAD_TO_DEG;
  float pitchDeg = pitch * RAD_TO_DEG;
#if INVERT_ROLL
  rollDeg = -rollDeg;
#endif
#if INVERT_PITCH
  pitchDeg = -pitchDeg;
#endif

  smX += AXIS_SMOOTH * (tiltToAxis(rollDeg)  - smX);   /* 右倾 -> X 增大 */
  smY += AXIS_SMOOTH * (tiltToAxis(pitchDeg) - smY);   /* 前倾 -> Y 增大 */

  /* 发送 USB HID 报文 (8 位轴, 中位 0; BOOT 键映射为 BUTTON_A) */
  int8_t outX8 = axisToI8(smX);
  int8_t outY8 = axisToI8(smY);
  Gamepad.send(outX8, outY8, 0, 0, 0, 0, HAT_CENTER,
               btnOut ? ((uint32_t)1 << BUTTON_A) : 0);

  /* 遥测 (20Hz) */
  uint32_t nowMs = millis();
  if (telemetryOn && nowMs - lastTelemetryMs >= 50) {
    lastTelemetryMs = nowMs;
    Serial.printf("#T %lu %.2f %.2f %d %d %.3f %.3f %.3f %d\n",
                  (unsigned long)nowMs, rollDeg, pitchDeg,
                  (int)outX8, (int)outY8, acc[0], acc[1], acc[2], (int)state);
  }
}

static void sampleTick(float dt) {
  float acc[3], gyr[3];
  if (!imuRead(acc, gyr)) {
    if (++i2cFails > 100) {
      Serial.println("#E 错误: IMU 读取连续失败, 尝试重新初始化...");
      ledSet(80, 0, 0);
      int r = mpuInit();
      if (r != 0) { i2cScan(); i2cFails = 0; return; }
      i2cFails = 0;
      calibStart();
    }
    return;
  }
  i2cFails = 0;

  switch (state) {
    case ST_GYRO_CAL: tickGyroCal(gyr);     break;
    case ST_SETTLE:   tickSettle(acc, gyr); break;
    case ST_RUN:      tickRun(acc, gyr, dt); break;
  }
}

/* ============================================================
 *  BOOT 按键: 短按/按住 = 手柄按键1, 长按2秒 = 回中
 * ============================================================ */
static void buttonTick(void) {
  static bool prevDown = false;
  static uint32_t pressStart = 0;
  static bool recenterArmed = false;

  bool down = (digitalRead(BUTTON_PIN) == LOW);
  uint32_t now = millis();

  if (down && !prevDown) {
    pressStart = now;
    recenterArmed = false;
  }
  if (down && !recenterArmed && (now - pressStart >= RECENTER_HOLD_MS)) {
    recenterArmed = true;
    doRecenter();
  }
  btnOut = down && !recenterArmed;   /* 长按回中期间不输出按键1 */
  prevDown = down;
}

/* ============================================================
 *  串口协议处理
 * ============================================================ */
static void serialTick(void) {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    switch (c) {
      case 's': telemetryOn = true;  Serial.println("#E 遥测开启"); break;
      case 'x': telemetryOn = false; Serial.println("#E 遥测停止"); break;
      case 'r': doRecenter(); break;
      case 'v': Serial.printf("#V tilt_gamepad %s\n", APP_VERSION); break;
      default: break;
    }
  }
}

/* ============================================================
 *  setup / loop
 * ============================================================ */
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 400000);
  Gamepad.begin();
  ledSet(0, 0, 0);

  Serial.println();
  Serial.println("==============================================");
  Serial.printf("  tilt_gamepad  %s\n", APP_VERSION);
  Serial.println("  ESP32-S3 + MPU-6050/MPU-6500 体感倾斜 USB 游戏手柄");
  Serial.println("  遥测: 串口发 s 开始 / x 停止 | 回中: r");
  Serial.println("==============================================");

  int r = mpuInit();
  while (r != 0) {
    if (r == -1)      Serial.println("#E 错误: I2C 无应答, 检查接线/地址 (SDA=8 SCL=9)");
    else if (r == -2) Serial.println("#E 错误: IMU 寄存器配置写入/读回失败");
    else              Serial.printf("#E 错误: 不支持的 WHO_AM_I=0x%02X (支持 0x68/0x70)\n", mpuWho);
    i2cScan();
    for (int i = 0; i < 10; i++) { ledSet(80, 0, 0); delay(100); ledSet(0, 0, 0); delay(100); }
    delay(1000);
    r = mpuInit();
  }
  Serial.printf("#E %s 初始化成功 (WHO_AM_I=0x%02X, 200Hz, +/-2g, +/-500dps)\n",
                mpuWho == 0x70 ? "MPU-6500-compatible" : "MPU-6050", mpuWho);

  delay(200);          /* 丢弃上电瞬间的脏数据 */
  calibStart();
}

void loop() {
  serialTick();
  buttonTick();

  uint32_t now = micros();
  if (now - lastTickUs >= SAMPLE_PERIOD_US) {
    float dt = (now - lastTickUs) * 1e-6f;
    if (dt > 0.05f) dt = 0.05f;
    lastTickUs = now;
    sampleTick(dt);
  }

  /* LED 状态指示 */
  uint32_t ms = millis();
  bool blink = (ms / 200) % 2 == 0;
  switch (state) {
    case ST_GYRO_CAL: ledSet(blink ? 60 : 0, 0, 0); break;              /* 红闪=校准 */
    case ST_SETTLE:   ledSet(50, 40, 0); break;                          /* 黄=收敛 */
    case ST_RUN:
      if (ms - recenterFlashMs < 600) ledSet(0, blink ? 100 : 0, 0);    /* 绿闪=刚回中 */
      else                            ledSet(0, btnOut ? 40 : 5, 0);    /* 微绿=运行 */
      break;
  }

  delay(1);
}
