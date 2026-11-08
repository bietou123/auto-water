#include <Arduino.h>

// ==================== 接线定义 ====================
// STM32F103C6T6A        外设
// PA0 (ADC0)      →    土壤湿度传感器 A0（模拟量）
// PA1             →    土壤湿度传感器 D0（数字量）
// PA2             →    DHT11 DATA
// PA3 (PWM)       →    10kΩ 电阻 → LCD V0（对比度，建议并电容到 GND）
// PB0             →    继电器 IN
// PB6             →    LCD RS (第4脚)
// PB7             →    LCD EN (第6脚)
// PB8             →    LCD D4 (第11脚)
// PB9             →    LCD D5 (第12脚)
// PB10            →    LCD D6 (第13脚)
// PB11            →    LCD D7 (第14脚)
// PC13 (LED)      →    板载 LED (心跳)
// ==================================================
// LCD 硬接线（不经过 STM32）：
//   VSS(1)→GND  VDD(2)→3.3V  RW(5)→GND
//   A(15)→3.3V  K(16)→GND
// ==================================================

#define SOIL_AO     PA0
#define SOIL_DO     PA1
#define DHT11_PIN   PA2
#define LCD_V0      PA3
#define RELAY       PB0
#define LCD_RS      PB6
#define LCD_EN      PB7
#define LCD_D4      PB8
#define LCD_D5      PB9
#define LCD_D6      PB10
#define LCD_D7      PB11
#define LED_PIN     PC13

#define MOISTURE_THRESHOLD  30
#define WATERING_TIME_MS    3000
#define COOLDOWN_MS         10000
#define CONTRAST_PWM        120
#define ANIM_INTERVAL_MS     20000

// CGRAM 角色定义（5x8 点阵，8个槽位全部给猫脸）
static const uint8_t CAT_EYE_L[8]    = {0x00,0x0E,0x11,0x15,0x11,0x0E,0x00,0x00};  // 0 左眼
static const uint8_t CAT_EYE_R[8]    = {0x00,0x0E,0x11,0x15,0x11,0x0E,0x00,0x00};  // 1 右眼
static const uint8_t CAT_EYE_CL[8]   = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0x00};  // 2 闭眼
static const uint8_t CAT_NOSE[8]     = {0x00,0x00,0x00,0x04,0x0A,0x0A,0x04,0x00};  // 3 鼻子
static const uint8_t CAT_MOUTH_L[8]  = {0x00,0x00,0x00,0x00,0x10,0x0A,0x04,0x00};  // 4 W嘴左
static const uint8_t CAT_MOUTH_R[8]  = {0x00,0x00,0x00,0x00,0x01,0x0A,0x04,0x00};  // 5 W嘴右
static const uint8_t CAT_BLUSH[8]    = {0x00,0x00,0x00,0x00,0x04,0x0A,0x04,0x00};  // 6 腮红
static const uint8_t CAT_OPEN[8]     = {0x00,0x00,0x00,0x00,0x0E,0x11,0x0E,0x00};  // 7 张嘴(惊喜)

#define CGRAM_EYE_L   0
#define CGRAM_EYE_R   1
#define CGRAM_EYE_CL  2
#define CGRAM_NOSE    3
#define CGRAM_MOUTH_L 4
#define CGRAM_MOUTH_R 5
#define CGRAM_BLUSH   6
#define CGRAM_OPEN    7

// 动画状态
static unsigned long lastAnimTime = 0;
static bool animPending = false;
static bool animWink = false;

unsigned long lastWaterTime = 0;
bool watering = false;

// ==================== LCD 手动驱动 (4-bit, STM32 时序适配) ====================

static void lcdNibble(uint8_t data) {
  digitalWrite(LCD_D4, data & 0x01);
  digitalWrite(LCD_D5, (data >> 1) & 0x01);
  digitalWrite(LCD_D6, (data >> 2) & 0x01);
  digitalWrite(LCD_D7, (data >> 3) & 0x01);
  delayMicroseconds(2);
  digitalWrite(LCD_EN, HIGH);
  delayMicroseconds(3);
  digitalWrite(LCD_EN, LOW);
  delayMicroseconds(2);
}

static void lcdByte(uint8_t data, bool isData) {
  digitalWrite(LCD_RS, isData ? HIGH : LOW);
  lcdNibble(data >> 4);
  lcdNibble(data & 0x0F);
  if (!isData && (data == 0x01 || data == 0x02)) delay(2);
  else delayMicroseconds(60);
}

static void lcdCmd(uint8_t c)   { lcdByte(c, false); }
static void lcdChar(uint8_t ch) { lcdByte(ch, true); }

static void lcdInit() {
  delay(100);
  pinMode(LCD_RS, OUTPUT); pinMode(LCD_EN, OUTPUT);
  pinMode(LCD_D4, OUTPUT); pinMode(LCD_D5, OUTPUT);
  pinMode(LCD_D6, OUTPUT); pinMode(LCD_D7, OUTPUT);
  digitalWrite(LCD_RS, LOW); digitalWrite(LCD_EN, LOW);
  delay(50);
  lcdNibble(0x03); delay(6);
  lcdNibble(0x03); delayMicroseconds(200);
  lcdNibble(0x03); delayMicroseconds(200);
  lcdNibble(0x02); delayMicroseconds(200);
  lcdCmd(0x28); lcdCmd(0x0C); lcdCmd(0x01); delay(2); lcdCmd(0x06);
}

static void lcdSetCursor(uint8_t col, uint8_t row) {
  lcdCmd(0x80 | (col + (row ? 0x40 : 0x00)));
}

static void lcdPrint(const char* s) { while (*s) lcdChar(*s++); }

static void lcdPrintNum(int n) {
  char buf[6]; int i = 0;
  if (n < 0) { n = -n; lcdChar('-'); }
  if (n == 0) { lcdChar('0'); return; }
  while (n > 0 && i < 5) { buf[i++] = '0' + (n % 10); n /= 10; }
  while (i > 0) lcdChar(buf[--i]);
}

static void lcdCreateChar(uint8_t loc, const uint8_t* pattern) {
  lcdCmd(0x40 | (loc << 3));
  for (uint8_t i = 0; i < 8; i++) lcdChar(pattern[i]);
}

static void loadAllChars() {
  lcdCreateChar(CGRAM_EYE_L,   CAT_EYE_L);
  lcdCreateChar(CGRAM_EYE_R,   CAT_EYE_R);
  lcdCreateChar(CGRAM_EYE_CL,  CAT_EYE_CL);
  lcdCreateChar(CGRAM_NOSE,    CAT_NOSE);
  lcdCreateChar(CGRAM_MOUTH_L, CAT_MOUTH_L);
  lcdCreateChar(CGRAM_MOUTH_R, CAT_MOUTH_R);
  lcdCreateChar(CGRAM_BLUSH,   CAT_BLUSH);
  lcdCreateChar(CGRAM_OPEN,    CAT_OPEN);
}

static void showCatFace(bool wink) {
  lcdCmd(0x01); delay(2);
  // 第0行：腮红+左眼+鼻子+右眼+腮红（铺满8字符宽，col 4 居中）
  lcdSetCursor(4, 0);
  lcdChar(CGRAM_BLUSH); lcdChar(CGRAM_BLUSH);
  lcdChar(CGRAM_EYE_L);
  lcdChar(CGRAM_NOSE);
  lcdChar(wink ? CGRAM_EYE_CL : CGRAM_EYE_R);
  lcdChar(CGRAM_BLUSH); lcdChar(CGRAM_BLUSH); lcdChar(CGRAM_BLUSH);
  // 第1行：腮红+W嘴×4+腮红（拉宽铺满）
  uint8_t mouth = wink ? CGRAM_OPEN : CGRAM_MOUTH_L;  // 眨眼时张嘴
  lcdSetCursor(4, 1);
  lcdChar(CGRAM_BLUSH); lcdChar(CGRAM_BLUSH);
  lcdChar(mouth); lcdChar(CGRAM_MOUTH_R);
  lcdChar(CGRAM_MOUTH_L); lcdChar(CGRAM_MOUTH_R);
  lcdChar(CGRAM_BLUSH); lcdChar(CGRAM_BLUSH);
}

// ==================== DHT11 寄存器级读取 (STM32F103, PA2) ====================
// 旧版用 digitalRead/digitalWrite/micros 恒 DHT=ERR。
// 根因: stm32duino 的 pin 函数封装开销大、micros() 依赖 SysTick 中断，
//       DHT11 的 26~70µs 位宽测量被污染，读到的位乱 → 校验和失败。
// 新版: PA2 直接操作 GPIOA->CRL/IDR/BSRR/BRR，时序用 DWT->CYCCNT(72MHz)
//       周期计数，确定性不受中断影响。DHT11 位宽: '0'=26~28µs, '1'=70µs。
// 硬件注意: 建议在 PA2 对 3.3V 并 4.7~10kΩ 外置上拉（内部 ~40kΩ 偏弱）。

#define DHT_CRL_SHIFT   8            // PA2 在 CRL 的第 [9:8] 位
#define DHT_CRL_MASK    (0xFUL << DHT_CRL_SHIFT)
#define DHT_BIT         (1UL << 2)    // PA2

// 诊断: 读取失败阶段（串口 DHT=ERR(sN)）
//   1=无响应(等了200µs 传感器不拉低 → 查接线/上拉/供电)
//   2=响应低/高电平超时
//   3=数据位读取超时
//   4=校验和错误(位宽阈值判错 → 时序仍偏)
volatile uint8_t dhtErrStage = 0;

static inline uint32_t dhtTicks(void)      { return DWT->CYCCNT; }        // 72MHz 周期计数
static inline int     dhtRead(void)        { return (GPIOA->IDR & DHT_BIT) ? 1 : 0; }
static inline void    dhtInput(void)       { GPIOA->CRL = (GPIOA->CRL & ~DHT_CRL_MASK) | (0x1UL << DHT_CRL_SHIFT); }  // 输入, CNF=01 上拉/下拉, MODE=00
static inline void    dhtOutputLow(void)   { GPIOA->CRL = (GPIOA->CRL & ~DHT_CRL_MASK) | (0x3UL << DHT_CRL_SHIFT); GPIOA->BRR = DHT_BIT; }   // 推挽输出 50MHz, 拉低
static inline void    dhtOutputHigh(void)  { GPIOA->CRL = (GPIOA->CRL & ~DHT_CRL_MASK) | (0x3UL << DHT_CRL_SHIFT); GPIOA->BSRR = DHT_BIT; }  // 推挽输出, 拉高

static void dwtInit(void) {
  CoreDebug->DEMCR |= 0x01000000UL;   // TRCENA: 使能 DWT
  DWT->CYCCNT = 0;
  DWT->CTRL |= 0x00000001UL;          // CYCCNTENA
}

static void dhtBusDelay(uint32_t us) {
  uint32_t end = dhtTicks() + us * 72;
  while ((int32_t)(dhtTicks() - end) < 0) {}
}

// 等 PA2 变为 expect；超时 max_us 微秒返回 false
static bool dhtWaitUntil(int expect, uint32_t max_us) {
  uint32_t limit = dhtTicks() + max_us * 72;
  while (dhtRead() != expect) {
    if ((int32_t)(dhtTicks() - limit) >= 0) return false;
  }
  return true;
}

static bool readDHT11(float &t, float &h) {
  uint8_t data[5] = {0};

  dhtOutputLow();                      // 开始信号: 拉低 >=18ms
  delay(20);
  dhtOutputHigh();                     // 拉高 20~40µs 后释放
  dhtBusDelay(30);
  dhtInput();                          // 释放进输入，ODR 已是 1 → 上拉使能
  GPIOA->BSRR = DHT_BIT;

  // 1) 等 DHT 拉低响应（80µs 低）
  if (!dhtWaitUntil(0, 200)) { dhtErrStage = 1; return false; }
  // 2) 等响应低电平结束 → 变高（80µs 高）
  if (!dhtWaitUntil(1, 200)) { dhtErrStage = 2; return false; }
  // 3) 等响应高电平结束 → 变低（即第 0 位的前导低）
  if (!dhtWaitUntil(0, 200)) { dhtErrStage = 2; return false; }

  // 读 40 位（每位: 低 50µs + 高 26/70µs）
  for (uint8_t i = 0; i < 5; i++) {
    for (uint8_t j = 0; j < 8; j++) {
      // 等本位的低电平结束 → 线变高
      if (!dhtWaitUntil(1, 150)) { dhtErrStage = 3; return false; }
      // 测高电平宽度
      uint32_t hstart = dhtTicks();
      if (!dhtWaitUntil(0, 150)) { dhtErrStage = 3; return false; }
      uint32_t hwidth_us = (dhtTicks() - hstart) / 72;
      data[i] <<= 1;
      if (hwidth_us > 45) data[i] |= 1;
    }
  }

  if ((uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) { dhtErrStage = 4; return false; }

  h = data[0];  // DHT11 小数部分始终为 0
  t = data[2];
  return true;
}

// ==================== 主程序 ====================

void setup() {
  Serial.begin(9600);
  dwtInit();
  pinMode(SOIL_AO, INPUT_ANALOG);
  pinMode(SOIL_DO, INPUT_PULLUP);
  pinMode(RELAY, OUTPUT); digitalWrite(RELAY, LOW);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);

  // 高频率 PWM (~50kHz)，消除可见波纹
  pinMode(LCD_V0, OUTPUT);
  GPIOA->CRL &= ~(0xF << 12);
  GPIOA->CRL |= (0xB << 12);           // PA3 = AF push-pull 50MHz
  RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
  TIM2->PSC = 0;
  TIM2->ARR = 1440;                    // 72MHz / 1440 = 50kHz
  TIM2->CCR4 = (CONTRAST_PWM * 1440UL) / 255;
  TIM2->CCMR2 |= TIM_CCMR2_OC4M_1 | TIM_CCMR2_OC4M_2 | TIM_CCMR2_OC4PE;
  TIM2->CCER |= TIM_CCER_CC4E;
  TIM2->CR1 |= TIM_CR1_CEN;

  lcdInit();
  loadAllChars();
  lastAnimTime = millis();

  digitalWrite(LED_PIN, LOW);
  lcdSetCursor(0, 0); lcdPrint("AutoWater v1.1");
  lcdSetCursor(0, 1); lcdPrint("Init OK");
  Serial.println("LCD Init done.");
  delay(2000);
  lcdCmd(0x01); delay(2);
}

void loop() {
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));

  int raw = analogRead(SOIL_AO);
  int soilPct = map(raw, 1030, 485, 0, 100);  // 干~1030 湿~490
  soilPct = constrain(soilPct, 0, 100);
  bool dryDigital = (digitalRead(SOIL_DO) == LOW);

  float temp = 0, hum = 0;
  bool dhtOk = readDHT11(temp, hum);

  Serial.print("Soil="); Serial.print(soilPct); Serial.print("%");
  Serial.print(" raw="); Serial.print(raw);
  Serial.print(" dry="); Serial.print(dryDigital ? "Y" : "N");
  if (dhtOk) { Serial.print(" T="); Serial.print(temp,1); Serial.print("C H="); Serial.print(hum,1); Serial.print("%"); }
  else { Serial.print(" DHT=ERR(s"); Serial.print(dhtErrStage); Serial.print(")"); }
  Serial.println();

  unsigned long now = millis();

  if (animPending) {
    showCatFace(animWink);
    animPending = false;
    animWink = !animWink;
    lastAnimTime = now;
  } else {
    lcdSetCursor(0, 0);
    if (dhtOk) {
      lcdPrint("T:"); lcdPrintNum((int)temp); lcdChar(0xDF); lcdPrint("C");
    } else {
      lcdPrint("T:--C");
    }
    lcdPrint("  H:");
    if (dhtOk) { lcdPrintNum((int)hum); lcdPrint("%   "); }
    else lcdPrint("--%   ");

    lcdSetCursor(0, 1);
    lcdPrint("Soil:"); lcdPrintNum(soilPct); lcdPrint("% ");
    if (watering) lcdPrint("WATERING");
    else if (soilPct < MOISTURE_THRESHOLD) lcdPrint("DRY!    ");
    else lcdPrint("OK      ");
  }

  if (now - lastAnimTime > ANIM_INTERVAL_MS) animPending = true;

  bool needWater = (soilPct < MOISTURE_THRESHOLD);
  if (!watering && needWater && (now - lastWaterTime > COOLDOWN_MS)) {
    Serial.println(">> PUMP ON");
    digitalWrite(RELAY, HIGH); watering = true; lastWaterTime = now;
  }
  if (watering && (now - lastWaterTime > WATERING_TIME_MS)) {
    Serial.println(">> PUMP OFF");
    digitalWrite(RELAY, LOW); watering = false;
  }
  delay(2000);
}
