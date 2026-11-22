// Arduino Uno. D2..D7: LED1..LED6, D8: индикатор режима.
#include <Arduino.h>

const byte ledPins[] = {2, 3, 4, 5, 6, 7, 8};
const byte valueMask = 0b111111;
const byte modeLed = 1 << 6;
const unsigned long longPressMs = 600;
const unsigned int frameUs = 2500;  // Общий программный ШИМ для семи выходов.

struct Button {
  byte pin;
  bool raw;
  bool down;
  unsigned long changedAt;

  // +1 при устойчивом нажатии, -1 при отпускании, иначе 0.
  int update(unsigned long now) {
    bool sample = digitalRead(pin) == LOW;
    if (sample != raw) {
      raw = sample;
      changedAt = now;
    }
    if (raw != down && now - changedAt >= 25) {
      down = raw;
      return down ? 1 : -1;
    }
    return 0;
  }
};

Button plusButton = {9}, minusButton = {10}, modeButton = {11};
byte value = 0b101111;
bool shiftMode = false;
bool inverted = false;
unsigned long pressedAt;
unsigned long startedAt;

// Один кадр ШИМ. Меняем только выбранные светодиоды.
void writeLeds(byte mask) {
  static byte previous = 0;
  for (byte i = 0; i < 7; ++i) {
    if (bitRead(previous ^ mask, i))
      digitalWrite(ledPins[i], bitRead(mask, i));
  }
  previous = mask;
}

void showFrame(byte mask) {
  int level = analogRead(A0);
  if (inverted) level = 1023 - level;
  unsigned int onUs = (unsigned long)frameUs * level / 1023;
  if (onUs > 0) {
    writeLeds(mask);
    delayMicroseconds(onUs);
  }
  if (onUs < frameUs) {
    writeLeds(0);
    delayMicroseconds(frameUs - onUs);
  }
}

void printValue(byte displayed) {
  static int last = -1;
  if (last == displayed) return;
  last = displayed;
  for (int i = 5; i >= 0; --i) Serial.print(bitRead(displayed, i));
  Serial.print(' ');
  Serial.println(displayed);
}

void setup() {
  for (byte pin : ledPins) pinMode(pin, OUTPUT);
  pinMode(plusButton.pin, INPUT_PULLUP);
  pinMode(minusButton.pin, INPUT_PULLUP);
  pinMode(modeButton.pin, INPUT_PULLUP);
  Serial.begin(9600);
  startedAt = millis();
}

void loop() {
  unsigned long now = millis();
  static bool testing = true;
  if (testing) {
    unsigned long step = (now - startedAt) / 500;
    if (step < 14) {
      // Шаги 0..6 добавляют LED1..7, шаги 7..13 гасят их в том же порядке.
      byte mask = step < 7 ? (1U << (step + 1)) - 1
                           : (0x7FU << (step - 6)) & 0x7F;
      showFrame(mask);
      return;
    }
    testing = false;
  }

  int plusEvent = plusButton.update(now);
  int minusEvent = minusButton.update(now);
  int modeEvent = modeButton.update(now);
  if (modeEvent == 1) pressedAt = now;
  if (modeButton.down && now - pressedAt >= longPressMs) inverted = true;
  if (modeEvent == -1) {
    if (!inverted) shiftMode = !shiftMode;
    inverted = false;
  }

  if (!inverted && plusEvent == 1) {
    if (shiftMode) value = ((value << 1) | (value >> 5)) & valueMask;
    else value = (value + 1) & valueMask;
  }
  if (!inverted && minusEvent == 1) {
    if (shiftMode) value = (value >> 1) | ((value & 1) << 5);
    else value = (value + valueMask) & valueMask;
  }

  byte displayed = inverted ? value ^ valueMask : value;
  printValue(displayed);
  showFrame(displayed | (shiftMode ? modeLed : 0));
}
