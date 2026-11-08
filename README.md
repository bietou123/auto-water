# 自动浇水系统 · STM32F103C6T6A

土壤湿度检测 + DHT11 温湿度 + 1602 液晶显示 + 继电器水泵的自动浇水控制器。固件基于 PlatformIO / Arduino 框架，所有外设驱动自行实现，不依赖第三方库。

## 硬件清单

| 元件 | 说明 |
|------|------|
| STM32F103C6T6A | Blue Pill 最小系统板（72 MHz） |
| 土壤湿度传感器 | 模拟量 A0 + 数字量 D0 双输出 |
| DHT11 | 温湿度传感器（单总线） |
| LCD1602 | 并口 4 位模式，5×8 点阵 |
| 继电器模块 | 高电平触发，控制水泵 |
| 水泵 | 5 V / 12 V，视继电器规格 |
| 电阻 | 10 kΩ（LCD 对比度）、4.7~10 kΩ（DHT11 上拉） |

## 接线

| STM32 | 外设 |
|-------|------|
| PA0 (ADC0) | 土壤湿度 AO（模拟量） |
| PA1 | 土壤湿度 DO（数字量） |
| PA2 | DHT11 DATA（建议对 3.3 V 并 4.7~10 kΩ 上拉） |
| PA3 (PWM) | 10 kΩ 电阻 → LCD V0（对比度，建议并电容到 GND） |
| PB0 | 继电器 IN |
| PB6 | LCD RS（第 4 脚） |
| PB7 | LCD EN（第 6 脚） |
| PB8 | LCD D4（第 11 脚） |
| PB9 | LCD D5（第 12 脚） |
| PB10 | LCD D6（第 13 脚） |
| PB11 | LCD D7（第 14 脚） |
| PC13 | 板载 LED（心跳） |

LCD 硬接线：VSS(1)→GND，VDD(2)→3.3 V，RW(5)→GND，A(15)→3.3 V，K(16)→GND。

## 功能

- 每 2 秒采样土壤湿度（ADC 映射 0~100%）与 DHT11 温湿度
- 湿度低于阈值（默认 30%）且距上次浇水超过冷却时间（默认 10 秒）时，开启水泵 3 秒
- LCD 第一行显示温度 / 湿度，第二行显示土壤湿度与状态（OK / DRY! / WATERING）
- 每 20 秒插播一帧猫脸表情动画，眨眼与张嘴交替
- 串口 9600 输出原始 ADC、湿度百分比、温湿度与水泵动作日志

## 技术要点

- **DHT11 寄存器级驱动**：`digitalRead`/`micros()` 在 stm32duino 上开销大且受 SysTick 中断干扰，位宽测量失真。改为直接操作 `GPIOA->CRL/IDR/BSRR/BRR`，用 `DWT->CYCCNT`（72 MHz 周期计数）测量时序，稳定可靠。
- **PWM 调 LCD 对比度**：用 TIM2 在 PA3 输出约 50 kHz PWM，占空比映射到 `CONTRAST_PWM`（默认 120/255），完全无可见波纹；比电位器更省元件。
- **自带 4 位并口 LCD 驱动**：不使用 LiquidCrystal 库，直接实现初始化、命令 / 数据写入与 CGRAM 自定义字符。
- **CGRAM 猫脸动画**：8 个自定义字符槽全部用于眼睛、鼻子、W 嘴与腮红，拼出 8×2 的点阵表情。

## 构建与烧录

```bash
pip install platformio
pio run                 # 编译
pio run -t upload       # 烧录（先在 platformio.ini 配置串口）
pio device monitor      # 查看串口日志（9600）
```

## 调参

| 参数 | 位置 | 说明 |
|------|------|------|
| `MOISTURE_THRESHOLD` | `src/main.cpp` | 浇水阈值，默认 30% |
| `WATERING_TIME_MS` | `src/main.cpp` | 单次浇水时长，默认 3000 ms |
| `COOLDOWN_MS` | `src/main.cpp` | 两次浇水最小间隔，默认 10000 ms |
| `CONTRAST_PWM` | `src/main.cpp` | LCD 对比度，0~255 |
| `map(raw, 1030, 485, 0, 100)` | `src/main.cpp` | 湿度标定，需按实际传感器替换干 / 湿 ADC 值 |

## 已知限制

- 土壤湿度传感器的干湿 ADC 值随型号、供电与土壤类型变化，必须自行标定
- DHT11 精度有限（温度 ±2 °C，湿度 ±5%），且两次读取间隔不应短于 1 秒
- 继电器默认高电平触发，低电平触发模块需改 `digitalWrite` 极性

## 许可

MIT License，见 [LICENSE](LICENSE)。
