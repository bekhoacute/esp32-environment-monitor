# ESP32 Environmental Monitor

PlatformIO/Arduino project for ESP32 DevKit V1, SCD41, Sharp GP2Y1010AU0F,
16x2 I2C LCD, and 2.8-inch SPI touch TFT matching the red `2.8 TFT SPI
240x320 V1.1` board in the supplied photo.

## Wiring assumed in firmware

- SCD41 SDA -> GPIO21; SCL -> GPIO22; power -> 3V3; GND -> GND.
- LCD 16x2 I2C backpack SDA -> GPIO21; SCL -> GPIO22; share GND with ESP32.
  Firmware detects LCD backpack addresses `0x27` and `0x3F` on startup. If your
  backpack uses another address, update the address selection in `src/main.cpp`.
- Many LCD backpacks pull SDA/SCL up to their VCC. Do not let a 5 V pull-up reach
  ESP32 GPIOs; power the backpack at 3.3 V only if its LCD works reliably there,
  otherwise add a bidirectional I2C level shifter between the 5 V LCD backpack and ESP32.
- GP2Y1010AU0F Vo -> GPIO34 through a voltage divider; sensor supply -> 5V.
- GP2Y LED control -> GPIO25, open-drain active-low. The sensor's specified LED drive circuit
  (150 ohm resistor and 220 uF capacitor) is required. Join grounds.
- Do not connect the GP2Y output directly to an ESP32 ADC until its voltage has
  been limited to the ESP32 input range. The firmware reports ADC raw and measured
  voltage; it does not claim a calibrated PM2.5 mass concentration.
- TFT VCC/GND -> board power/GND as specified by its vendor; all grounds must be common.
  The pictured module's manual specifies VCC 3.3-5 V and says its LED pin can be tied to
  3.3 V for always-on backlight. Never apply 5 V logic to ESP32 GPIOs; verify the module's
  signal-level compatibility or add level shifting if it does not accept 3.3 V logic.
- TFT SCK -> GPIO18, SDI/MOSI -> GPIO23, SDO/MISO -> GPIO19, CS -> GPIO27,
  DC -> GPIO26, RESET -> GPIO33, LED -> 3V3.
- Touch T_CLK -> GPIO18, T_DIN -> GPIO23, T_DO -> GPIO19, T_CS -> GPIO32,
  T_IRQ -> GPIO35. Display and touch share SPI clock/data but have separate CS pins.
  SD-card pins are unused. Avoid connecting the TFT module's SDO pin to the 16x2 LCD.

## Build and run

Open this folder in VS Code with the PlatformIO extension, select `esp32dev`, then
Build and Upload. Open the PlatformIO Serial Monitor at 115200 baud.

## LCD display

The TFT shows CO2, temperature, humidity, and dust ADC on its overview page. Touching
the screen switches to dust ADC/voltage details; touch again to return. Dust voltage
is a raw sensor signal, not calibrated PM2.5 concentration. The 16x2 LCD continues
to alternate between these readings over I2C.

The photo's silkscreen and layout match LCDWIKI's MSP2807-style 2.8-inch SPI board,
whose display controller is ILI9341. The code assumes its resistive touch controller
is XPT2046; the marking `HR20462112` alone did not identify that chip. If the TFT
lights but touch does not respond, confirm the controller marking and calibrate touch.

Power design is outside this firmware starter; provide stable board-appropriate
rails and common ground.
