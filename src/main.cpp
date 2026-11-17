#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <string.h>
#include <SensirionI2cScd4x.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>

// ESP32 DevKit V1 wiring.
constexpr uint8_t PIN_I2C_SDA = 21;
constexpr uint8_t PIN_I2C_SCL = 22;
constexpr uint8_t PIN_DUST_LED = 25;     
constexpr uint8_t PIN_DUST_ANALOG = 34; // ADC1 input-only
constexpr uint8_t PIN_SPI_SCK = 18;
constexpr uint8_t PIN_SPI_MISO = 19;
constexpr uint8_t PIN_SPI_MOSI = 23;
constexpr uint8_t PIN_TFT_CS = 27;
constexpr uint8_t PIN_TFT_DC = 26;
constexpr uint8_t PIN_TFT_RESET = 33;
constexpr uint8_t PIN_TOUCH_CS = 32;
constexpr uint8_t PIN_TOUCH_IRQ = 35;
constexpr uint8_t LCD_COLS = 16;
constexpr uint8_t LCD_ROWS = 2;
constexpr uint16_t COLOR_BG = 0x0861;   
constexpr uint16_t COLOR_CARD = 0x10A3; 

SensirionI2cScd4x scd4x;
Adafruit_ILI9341 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RESET);
XPT2046_Touchscreen touch(PIN_TOUCH_CS, PIN_TOUCH_IRQ);

struct DustReading {
  uint16_t raw = 0;
  float volts = 0.0f;
  float ugm3 = 0.0f; // Bổ sung giá trị ug/m3
};

LiquidCrystal_I2C* lcd = nullptr;
uint32_t lastDustSampleMs = 0;
uint32_t lastScdPollMs = 0;
uint32_t lastLcdUpdateMs = 0;
uint32_t lastTftUpdateMs = 0;
uint32_t lastTftTitleMs = 0;
uint32_t lastTouchMs = 0;
uint32_t lastReadingMs = 0;
uint8_t lcdAddress = 0;
uint8_t lcdPage = 0;
uint8_t tftPage = 0;
uint8_t drawnTftPage = 255;
char lastTftValues[4][24] = {};
char lastLcdLines[2][LCD_COLS + 1] = {};
bool touchWasDown = false;
bool hasDustSample = false;
uint16_t latestCo2 = 0;
float latestTemperature = 0.0f;
float latestHumidity = 0.0f;
DustReading latestDust;

// --- ĐÃ SỬA LỖI LẤY MẪU BỤI ---
DustReading readDustPulse() {
  digitalWrite(PIN_DUST_LED, LOW); // Bật LED IR
  delayMicroseconds(280);          // Chờ đúng 280us theo Datasheet

  // Chỉ lấy mẫu ADC 1 lần duy nhất bằng milivolt
  const uint32_t mVolts = analogReadMilliVolts(PIN_DUST_ANALOG);
  
  delayMicroseconds(40);           // Đủ 320us tổng chu kỳ xung
  digitalWrite(PIN_DUST_LED, HIGH);// Tắt LED IR

  DustReading reading;
  reading.volts = mVolts / 1000.0f;
  reading.raw = (mVolts * 4095) / 3300; // Quy đổi tương đương RAW 12-bit

  // Tính nồng độ bụi ug/m3 (Giả định V_zero ~ 0.6V)
  if (reading.volts > 0.6f) {
    reading.ugm3 = (reading.volts - 0.6f) * 200.0f;
  } else {
    reading.ugm3 = 0.0f;
  }

  return reading;
}

void printScdError(const char* operation, int16_t error) {
  char message[64];
  errorToString(error, message, sizeof(message));
  Serial.printf("SCD41 %s failed: %s\n", operation, message);
}

void scanI2cBus() {
  Serial.println("Scanning I2C bus...");
  uint8_t found = 0;
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    const uint8_t error = Wire.endTransmission();
    if (error == 0) {
      Serial.printf("I2C device found at 0x%02X%s\n", address,
                    address == SCD41_I2C_ADDR_62 ? " (SCD41 address)" : "");
      if (address == 0x27 || address == 0x3F) lcdAddress = address;
      ++found;
    }
  }
  if (found == 0) Serial.println("No I2C devices found; check the SCD41/LCD bus.");
}

void initializeLcd() {
  if (lcdAddress == 0) {
    Serial.println("LCD 16x2 not found at 0x27 or 0x3F; disabled.");
    return;
  }
  lcd = new LiquidCrystal_I2C(lcdAddress, LCD_COLS, LCD_ROWS);
  lcd->init();
  lcd->backlight();
  lcd->clear();
  lcd->print("Env Monitor");
  lcd->setCursor(0, 1);
  lcd->printf("LCD I2C 0x%02X", lcdAddress);
}

void drawStaticCard(int16_t x, int16_t y, int16_t w, int16_t h,
                    const char* title, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, 9, COLOR_CARD);
  tft.drawRoundRect(x, y, w, h, 9, color);
  tft.setTextSize(1);
  tft.setTextColor(color, COLOR_CARD);
  tft.setCursor(x + 10, y + 10);
  tft.print(title);
  tft.drawFastHLine(x + 10, y + 25, w - 20, 0x2945);
}

void drawScrollingTitle(uint32_t now) {
  constexpr int16_t TITLE_X = 10;
  constexpr int16_t TITLE_WIDTH = 300;
  constexpr int16_t TEXT_WIDTH = 288;
  constexpr int16_t GAP = 50;
  constexpr int16_t CYCLE = TEXT_WIDTH + GAP;
  constexpr uint32_t STEP_MS = 60; // Tăng thời gian delay để giảm giật màn hình
  if (now - lastTftTitleMs < STEP_MS) return;
  lastTftTitleMs = now;

  const int16_t x = TITLE_X + TITLE_WIDTH -
                    static_cast<int16_t>((now / STEP_MS) % CYCLE);
  tft.fillRect(TITLE_X, 4, TITLE_WIDTH, 22, COLOR_BG);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_CYAN, COLOR_BG);
  tft.setCursor(x - CYCLE, 8);
  tft.print("Khoa Hoc Lap Trinh Nhung");
  tft.setCursor(x, 8);
  tft.print("Khoa Hoc Lap Trinh Nhung");
  tft.setCursor(x + CYCLE, 8);
  tft.print("Khoa Hoc Lap Trinh Nhung");
}

void drawTftLayout() {
  tft.fillScreen(COLOR_BG);
  for (uint8_t i = 0; i < 4; ++i) lastTftValues[i][0] = '\0';
  tft.drawFastHLine(10, 29, 300, 0x2945);

  if (tftPage == 0) {
    drawStaticCard(8, 38, 148, 80, "CO2", ILI9341_CYAN);
    drawStaticCard(164, 38, 148, 80, "TEMP", ILI9341_ORANGE);
    drawStaticCard(8, 126, 148, 80, "HUMIDITY", ILI9341_GREEN);
    drawStaticCard(164, 126, 148, 80, "DUST (ug/m3)", ILI9341_YELLOW);
  } else {
    drawStaticCard(10, 50, 145, 68, "RAW ADC", ILI9341_YELLOW);
    drawStaticCard(165, 50, 145, 68, "OUTPUT VOLTAGE", ILI9341_GREEN);
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_LIGHTGREY);
    tft.setCursor(20, 145);
    tft.print("GP2Y1010AU0F");
    tft.setCursor(20, 161);
    tft.print("Raw sensor output; calibration needed");
  }

  tft.fillRoundRect(96, 218, 128, 18, 7, 0x2148);
  tft.setTextSize(1);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(107, 224);
  tft.print(tftPage == 0 ? "TOUCH TO VIEW DUST" : "TOUCH FOR OVERVIEW");
  drawnTftPage = tftPage;
}

void drawValueIfChanged(uint8_t slot, int16_t x, int16_t y, int16_t w,
                        const char* value) {
  if (strcmp(lastTftValues[slot], value) == 0) return;
  tft.fillRect(x, y, w, 30, COLOR_CARD);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE, COLOR_CARD);
  tft.setCursor(x, y);
  tft.print(value);
  strncpy(lastTftValues[slot], value, sizeof(lastTftValues[slot]) - 1);
  lastTftValues[slot][sizeof(lastTftValues[slot]) - 1] = '\0';
}

void updateTft(uint32_t now) {
  constexpr uint32_t TFT_REFRESH_MS = 250;
  if (drawnTftPage != tftPage) drawTftLayout();
  drawScrollingTitle(now);
  if (now - lastTftUpdateMs < TFT_REFRESH_MS) return;
  lastTftUpdateMs = now;
  char value[24];
  if (tftPage == 0) {
    snprintf(value, sizeof(value), "%u ppm", latestCo2);
    drawValueIfChanged(0, 18, 72, 128, value);
    snprintf(value, sizeof(value), "%.1f C", latestTemperature);
    drawValueIfChanged(1, 174, 72, 128, value);
    snprintf(value, sizeof(value), "%.0f %%", latestHumidity);
    drawValueIfChanged(2, 18, 160, 128, value);
    snprintf(value, sizeof(value), "%.1f", latestDust.ugm3); // Hiển thị ug/m3
    drawValueIfChanged(3, 174, 160, 128, value);
  } else {
    snprintf(value, sizeof(value), "%u", latestDust.raw);
    drawValueIfChanged(0, 20, 80, 125, value);
    snprintf(value, sizeof(value), "%.3f V", latestDust.volts);
    drawValueIfChanged(1, 175, 80, 125, value); 
  }
}

void pollTouch(uint32_t now) {
  const bool isDown = touch.touched();
  if (isDown && !touchWasDown && now - lastTouchMs > 350) {
    tftPage = (tftPage + 1) % 2;
    lastTouchMs = now;
    lastTftUpdateMs = 0;
  }
  touchWasDown = isDown;
}

void updateLcd(uint32_t now) {
  if (lcd == nullptr || now - lastLcdUpdateMs < 1000) return;
  lastLcdUpdateMs = now;
  lcdPage = (now / 5000) % 2;
  char line0[17] = {};
  char line1[17] = {};
  if (lcdPage == 0) {
    if (latestCo2 != 0) snprintf(line0, sizeof(line0), "CO2:%u ppm", latestCo2);
    else snprintf(line0, sizeof(line0), "CO2: waiting...");
    if (lastReadingMs != 0) snprintf(line1, sizeof(line1), "T%.1fC RH%.0f%%", latestTemperature, latestHumidity);
    else snprintf(line1, sizeof(line1), "SCD41 waiting");
  } else {
    snprintf(line0, sizeof(line0), "Dust:%.1fug/m3", latestDust.ugm3);
    snprintf(line1, sizeof(line1), "Vo:%.2f V", latestDust.volts);
  }

  char* lines[] = {line0, line1};
  for (uint8_t row = 0; row < 2; ++row) {
    char padded[LCD_COLS + 1];
    memset(padded, ' ', LCD_COLS);
    padded[LCD_COLS] = '\0';
    const size_t lineLength = strlen(lines[row]);
    memcpy(padded, lines[row], lineLength < LCD_COLS ? lineLength : LCD_COLS);
    if (strcmp(padded, lastLcdLines[row]) == 0) continue;
    lcd->setCursor(0, row);
    lcd->print(padded);
    memcpy(lastLcdLines[row], padded, sizeof(padded));
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  
  // --- ĐÃ SỬA: Dùng chế độ OUTPUT tiêu chuẩn cho DUST LED ---
  pinMode(PIN_DUST_LED, OUTPUT);
  digitalWrite(PIN_DUST_LED, HIGH);
  
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_DUST_ANALOG, ADC_11db);

  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
  tft.begin();
  tft.setRotation(1);
  Serial.printf("TFT logical size: %d x %d\n", tft.width(), tft.height());
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(18, 100);
  tft.print("Starting sensors...");
  touch.begin();
  touch.setRotation(1);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  scanI2cBus();
  initializeLcd();
  
  scd4x.begin(Wire, SCD41_I2C_ADDR_62);
  int16_t error = scd4x.stopPeriodicMeasurement();
  if (error) printScdError("stopPeriodicMeasurement", error);
  delay(500);
  error = scd4x.startPeriodicMeasurement();
  if (error) printScdError("startPeriodicMeasurement", error);
  Serial.println("Environment monitor starting...");
}

void loop() {
  const uint32_t now = millis();
  
  if (now - lastDustSampleMs >= 10) {
    lastDustSampleMs = now;
    const DustReading sample = readDustPulse();
    if (!hasDustSample) {
      latestDust = sample;
      hasDustSample = true;
    } else {
      latestDust.raw = static_cast<uint16_t>((3UL * latestDust.raw + sample.raw + 2) / 4);
      latestDust.volts = latestDust.volts * 0.75f + sample.volts * 0.25f;
      latestDust.ugm3 = latestDust.ugm3 * 0.75f + sample.ugm3 * 0.25f;
    }
  }

  if (now - lastScdPollMs >= 1000) {
    lastScdPollMs = now;
    bool dataReady = false;
    int16_t error = scd4x.getDataReadyStatus(dataReady);
    
    if (error) {
      printScdError("getDataReadyStatus", error);
    } else if (dataReady) {
      uint16_t co2 = 0;
      float temperature = 0.0f;
      float humidity = 0.0f;
      error = scd4x.readMeasurement(co2, temperature, humidity);
      if (error) {
        printScdError("readMeasurement", error);
      } else if (co2 != 0) {
        latestCo2 = co2;
        latestTemperature = temperature;
        latestHumidity = humidity;
        lastReadingMs = now;
        Serial.printf("SCD41 CO2=%u ppm, T=%.2f C, RH=%.2f %%\n", co2, temperature, humidity);
      }
    }
  }

  pollTouch(now);
  updateTft(now);
  updateLcd(now);
}
