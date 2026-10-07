#include <Arduino.h>

/*
 * 16-битная ячейка памяти. Arduino UNO + KY-040 + 2 кнопки + 6 LED.
 *
 * Четыре LED расположены слева направо от старшего бита к младшему:
 *   страница 00:  3  2  1  0       страница 01:  7  6  5  4
 *   страница 10: 11 10  9  8       страница 11: 15 14 13 12
 *
 * Кнопка 1 — следующая страница. Кнопка 2 — инверсия выбранного бита.
 * Обе кнопки непрерывно 2 секунды — однократная инверсия всех 16 бит.
 * Одиночное нажатие обрабатывается при отпускании: это исключает
 * случайную смену страницы или бита перед распознаванием двух кнопок.
 *
 * Значение 1: ШИМ около 50%; выбранный LED: 100%; значение 0: выключен.
 * Выделение не изменяет память. После записи снимается только выделение:
 * LED возвращается к 0% или 50% в соответствии с новым значением бита.
 *
 * Комментарии — UTF-8. Сообщения Serial — русские слова латиницей.
 * Сторонние библиотеки не нужны. SW энкодера не используется.
 */

namespace Config {
    constexpr uint8_t DATA_LED_PINS[4] = {5, 6, 9, 10};
    constexpr uint8_t PAGE_MSB_PIN = 11;
    constexpr uint8_t PAGE_LSB_PIN = 12;
    constexpr uint8_t ENCODER_CLK_PIN = 2;
    constexpr uint8_t ENCODER_DT_PIN = 3;
    constexpr uint8_t PAGE_BUTTON_PIN = 7;
    constexpr uint8_t EDIT_BUTTON_PIN = 8;

    constexpr uint8_t NORMAL_BRIGHTNESS = 128;
    constexpr uint8_t SELECTED_BRIGHTNESS = 255;
    constexpr uint16_t INITIAL_MEMORY = 0x1248;  // 0001001001001000₂ = 4680.
    constexpr uint32_t SERIAL_BAUD = 115200;
    constexpr uint32_t DEBOUNCE_MS = 30;
    constexpr uint32_t LONG_PRESS_MS = 2000;
    constexpr uint32_t TEST_STEP_MS = 500;
    constexpr uint32_t TEST_BLANK_MS = 250;
    constexpr uint32_t ENCODER_DEBOUNCE_US = 1500;

    // Для нестандартного физического энкодера направление можно поменять.
    constexpr bool REVERSE_ENCODER = false;
}

// Неблокирующий антидребезг. Одно отпускание даёт ровно одно событие.
class DebouncedButton {
public:
    explicit DebouncedButton(uint8_t pin) : pin_(pin) {}

    void begin(uint32_t now) {
        pinMode(pin_, INPUT_PULLUP);
        rawPressed_ = digitalRead(pin_) == LOW;
        stablePressed_ = rawPressed_;
        changedAt_ = now;
        released_ = false;
    }

    void update(uint32_t now) {
        released_ = false;
        const bool reading = digitalRead(pin_) == LOW;

        if (reading != rawPressed_) {
            rawPressed_ = reading;
            changedAt_ = now;
        }

        // Беззнаковая разность корректна и при переполнении millis().
        if (stablePressed_ != rawPressed_ &&
            uint32_t(now - changedAt_) >= Config::DEBOUNCE_MS) {
            stablePressed_ = rawPressed_;
            released_ = !stablePressed_;
        }
    }

    bool isPressed() const { return stablePressed_; }
    bool wasReleased() const { return released_; }

private:
    const uint8_t pin_;
    bool rawPressed_ = false;
    bool stablePressed_ = false;
    bool released_ = false;
    uint32_t changedAt_ = 0;
};

DebouncedButton pageButton(Config::PAGE_BUTTON_PIN);
DebouncedButton editButton(Config::EDIT_BUTTON_PIN);

uint16_t memoryCell = Config::INITIAL_MEMORY;
uint8_t currentPage = 0;
uint8_t selectedLed = 0;
bool selectionActive = false;
bool cursorHasPosition = false;

bool startupComplete = false;
uint32_t startupStartedAt = 0;
int8_t previousTestLedCount = -1;

bool ignoreButtonsUntilRelease = false;
bool comboActive = false;
bool comboTimerRunning = false;
bool comboAlreadyExecuted = false;
uint32_t comboStartedAt = 0;

// Энкодер ловится аппаратным прерыванием по спаду CLK.
// Это не позволяет симулятору "проглотить" короткий импульс между
// двумя проходами loop(). В обработчике только запоминается направление;
// вся логика интерфейса выполняется уже в основном цикле.
volatile int8_t pendingEncoderSteps = 0;
volatile uint32_t lastEncoderInterruptUs = 0;

uint8_t bitIndexForLed(uint8_t ledIndex) {
    return currentPage * 4 + (3 - ledIndex);
}

bool memoryBitIsSet(uint8_t bitIndex) {
    return ((memoryCell >> bitIndex) & 1U) != 0;
}

void updateDisplay() {
    // Левый индикатор — старший бит номера страницы, правый — младший.
    digitalWrite(Config::PAGE_MSB_PIN, (currentPage & 2U) ? HIGH : LOW);
    digitalWrite(Config::PAGE_LSB_PIN, (currentPage & 1U) ? HIGH : LOW);

    for (uint8_t ledIndex = 0; ledIndex < 4; ++ledIndex) {
        uint8_t brightness = memoryBitIsSet(bitIndexForLed(ledIndex))
            ? Config::NORMAL_BRIGHTNESS : 0;

        if (selectionActive && ledIndex == selectedLed) {
            brightness = Config::SELECTED_BRIGHTNESS;
        }
        analogWrite(Config::DATA_LED_PINS[ledIndex], brightness);
    }
}

void printState(const __FlashStringHelper* eventName) {
    Serial.println();
    Serial.print(F("Sobytiye: "));
    Serial.println(eventName);
    Serial.print(F("Pamyat-BIN: "));
    for (int8_t bitIndex = 15; bitIndex >= 0; --bitIndex) {
        Serial.print(memoryBitIsSet(bitIndex) ? '1' : '0');
    }
    Serial.print(F(" | Pamyat-DEC: "));
    Serial.println(memoryCell, DEC);

    Serial.print(F("Fragment: "));
    Serial.print((currentPage >> 1) & 1U);
    Serial.print(currentPage & 1U);
    Serial.print(F(" | Razryady-sleva-napravo: "));
    for (uint8_t ledIndex = 0; ledIndex < 4; ++ledIndex) {
        Serial.print(bitIndexForLed(ledIndex));
        Serial.print(ledIndex < 3 ? ' ' : '\n');
    }

    Serial.print(F("Bity-fragmenta: "));
    for (uint8_t ledIndex = 0; ledIndex < 4; ++ledIndex) {
        Serial.print(memoryBitIsSet(bitIndexForLed(ledIndex)) ? '1' : '0');
    }
    Serial.print(F(" | Aktivnyi-bit: "));
    if (selectionActive) {
        Serial.print(bitIndexForLed(selectedLed));
        Serial.print(F(" | Znachenie-bita: "));
        Serial.println(memoryBitIsSet(bitIndexForLed(selectedLed)) ? 1 : 0);
    } else {
        Serial.println(F("net"));
    }
}

// Вызывается на каждом спаде CLK энкодера.
void onEncoderStep() {
    const uint32_t nowUs = micros();
    if (uint32_t(nowUs - lastEncoderInterruptUs) < Config::ENCODER_DEBOUNCE_US) {
        return;
    }
    lastEncoderInterruptUs = nowUs;

    int8_t direction = digitalRead(Config::ENCODER_DT_PIN) == HIGH ? 1 : -1;
    if (Config::REVERSE_ENCODER) {
        direction = -direction;
    }

    // Ограничиваем очередь, чтобы случайный дребезг не накопил сотни шагов.
    if (direction > 0 && pendingEncoderSteps < 12) {
        ++pendingEncoderSteps;
    } else if (direction < 0 && pendingEncoderSteps > -12) {
        --pendingEncoderSteps;
    }
}

void processEncoder() {
    noInterrupts();
    int8_t steps = pendingEncoderSteps;
    pendingEncoderSteps = 0;
    interrupts();

    if (steps == 0) {
        return;
    }

    // Во время комбинации кнопок поворот не меняет выбранный бит.
    if (comboActive || ignoreButtonsUntilRelease) {
        return;
    }

    bool changed = false;

    while (steps != 0) {
        const int8_t direction = steps > 0 ? 1 : -1;
        steps += direction > 0 ? -1 : 1;

        if (!cursorHasPosition) {
            selectedLed = direction > 0 ? 0 : 3;
            cursorHasPosition = true;
            changed = true;
            continue;
        }

        const int8_t nextLed = int8_t(selectedLed) + direction;
        if (nextLed >= 0 && nextLed <= 3) {
            selectedLed = uint8_t(nextLed);
            changed = true;
        }
    }

    // Даже если упёрлись в край, выбранный бит остаётся подсвеченным.
    selectionActive = true;

    if (changed) {
        updateDisplay();
        printState(F("vybor-bita"));
    } else {
        updateDisplay();
    }
}

void switchPage() {
    currentPage = (currentPage + 1) % 4;
    selectionActive = false;
    cursorHasPosition = false;
    updateDisplay();
    printState(F("smena-fragmenta"));
}

void toggleSelectedBit() {
    if (!selectionActive) {
        printState(F("snachala-vyberite-bit-enkoderom"));
        return;
    }

    const uint8_t bitIndex = bitIndexForLed(selectedLed);
    memoryCell ^= (uint16_t(1) << bitIndex);
    selectionActive = false;
    // Позиция запоминается: следующий поворот продолжает движение от неё.
    updateDisplay();
    printState(F("bit-invertirovan"));
}

void invertAllBits() {
    memoryCell ^= uint16_t(0xFFFF);
    selectionActive = false;
    updateDisplay();
    printState(F("vse-16-bit-invertirovany"));
}

void processButtons(uint32_t now) {
    const bool pagePressed = pageButton.isPressed();
    const bool editPressed = editButton.isPressed();
    const bool bothPressed = pagePressed && editPressed;

    // Зажатые при запуске кнопки сначала нужно отпустить.
    if (ignoreButtonsUntilRelease) {
        if (!pagePressed && !editPressed) {
            ignoreButtonsUntilRelease = false;
        }
        return;
    }

    if (bothPressed && !comboActive) {
        comboActive = true;
        comboTimerRunning = true;
        comboAlreadyExecuted = false;
        comboStartedAt = now;
    }

    if (comboActive) {
        if (bothPressed) {
            if (!comboTimerRunning) {
                comboTimerRunning = true;
                comboStartedAt = now;
            }
            if (!comboAlreadyExecuted &&
                uint32_t(now - comboStartedAt) >= Config::LONG_PRESS_MS) {
                invertAllBits();
                comboAlreadyExecuted = true;
            }
        } else {
            // Отпускание любой кнопки прерывает непрерывный отсчёт.
            comboTimerRunning = false;
            if (!pagePressed && !editPressed) {
                comboActive = false;
                comboAlreadyExecuted = false;
            }
        }
        // До отпускания ОБЕИХ кнопок одиночные действия запрещены.
        // Долгое удержание не запускает повторную инверсию каждые 2 с.
        return;
    }

    if (pageButton.wasReleased()) {
        switchPage();
    }
    if (editButton.wasReleased()) {
        toggleSelectedBit();
    }
}

void updateStartup(uint32_t now) {
    const uint32_t elapsed = uint32_t(now - startupStartedAt);
    const uint32_t testDuration = 4 * Config::TEST_STEP_MS;

    if (elapsed < testDuration + Config::TEST_BLANK_MS) {
        const int8_t litLedCount = elapsed < testDuration
            ? int8_t(elapsed / Config::TEST_STEP_MS + 1) : 0;
        if (litLedCount != previousTestLedCount) {
            previousTestLedCount = litLedCount;
            for (uint8_t ledIndex = 0; ledIndex < 4; ++ledIndex) {
                analogWrite(Config::DATA_LED_PINS[ledIndex],
                    ledIndex < litLedCount ? Config::SELECTED_BRIGHTNESS : 0);
            }
        }
        return;
    }

    // Всё, что могло накопиться во время стартового теста, отбрасываем.
    noInterrupts();
    pendingEncoderSteps = 0;
    lastEncoderInterruptUs = micros();
    interrupts();

    pageButton.begin(now);
    editButton.begin(now);
    ignoreButtonsUntilRelease = pageButton.isPressed() || editButton.isPressed();
    startupComplete = true;
    updateDisplay();
    Serial.println(F("Gotovo. K1: fragment. K2: invertirovat-vybrannyi-bit."));
    Serial.println(F("K1+K2: derzhat-2-sekundy. Potom-otpustit-obe-knopki."));
    printState(F("start"));
}

void setup() {
    Serial.begin(Config::SERIAL_BAUD);
    for (uint8_t ledIndex = 0; ledIndex < 4; ++ledIndex) {
        pinMode(Config::DATA_LED_PINS[ledIndex], OUTPUT);
        analogWrite(Config::DATA_LED_PINS[ledIndex], 0);
    }
    pinMode(Config::PAGE_MSB_PIN, OUTPUT);
    pinMode(Config::PAGE_LSB_PIN, OUTPUT);
    digitalWrite(Config::PAGE_MSB_PIN, LOW);
    digitalWrite(Config::PAGE_LSB_PIN, LOW);
    pinMode(Config::ENCODER_CLK_PIN, INPUT_PULLUP);
    pinMode(Config::ENCODER_DT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(Config::ENCODER_CLK_PIN), onEncoderStep, FALLING);

    startupStartedAt = millis();
    pageButton.begin(startupStartedAt);
    editButton.begin(startupStartedAt);
    Serial.println(F("Test-LED: po-ocheredi-cherez-500-ms, zatem-vse-vyklyucheny."));
    updateStartup(startupStartedAt);
}

void loop() {
    const uint32_t now = millis();
    pageButton.update(now);
    editButton.update(now);

    if (!startupComplete) {
        updateStartup(now);
        return;
    }

    processButtons(now);
    processEncoder();
}
