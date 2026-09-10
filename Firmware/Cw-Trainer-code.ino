/* 
Cw Trainer. Designed to run on a esp32-c3 SuperMini

Warning: Code was made with help of Ia mind non huma written code.

https://github.com/Nallopro/Cw-Trainer/

*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

// Pin configs

// Jack paddle: each contact connects its GPIO to GND.
constexpr uint8_t DIT_PIN = 0;  // Jack TIP by default
constexpr uint8_t DAH_PIN = 1;  // Jack RING by default

// Internal BOOT button on the ESP32-C3 Super Mini.
constexpr uint8_t MENU_BUTTON_PIN = 9;

// OLED I2C: SDA GPIO10, SCL GPIO20.
constexpr uint8_t OLED_SDA_PIN = 10;
constexpr uint8_t OLED_SCL_PIN = 20;
constexpr uint8_t OLED_ADDRESS = 0x3C;

// Passive buzzer / piezo on GPIO21.
constexpr uint8_t BUZZER_PIN = 21;
constexpr bool ACTIVE_BUZZER = false;

/*
  Two CW LEDs driven from GPIO8 with PWM.

  The LEDs turn on only while a CW mark is being generated and
  Output is set to LIGHT or BOTH.

*/
constexpr uint8_t CW_LED_PIN = 8;
constexpr uint32_t CW_LED_PWM_FREQUENCY_HZ = 1000;
constexpr uint8_t CW_LED_PWM_RESOLUTION_BITS = 8;
constexpr uint8_t CW_LED_PWM_DUTY = 45;  //  0-255

// Arduino-ESP32 2.x needs fixed LEDC channels.
#if ESP_ARDUINO_VERSION_MAJOR < 3
constexpr uint8_t BUZZER_LEDC_CHANNEL = 0;
constexpr uint8_t CW_LED_LEDC_CHANNEL = 1;
#endif

// ============================== OLED ===================================

constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int OLED_RESET = -1;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledAvailable = false;
bool screenDirty = true;
uint32_t lastScreenDraw = 0;

// ============================= Settings =================================

enum OutputMode : uint8_t {
  OUTPUT_SOUND = 0,
  OUTPUT_LIGHT = 1,
  OUTPUT_BOTH = 2
};

enum KeyMode : uint8_t {
  KEY_PIN7_DIT_PIN8_DAH = 0,
  KEY_PIN7_DAH_PIN8_DIT = 1,
  KEY_STRAIGHT = 2
};

struct TrainerSettings {
  uint8_t wpm = 20;         // 10, 15, 20, 25, 30, 35 or 40
  uint8_t dahRatio10 = 30;  // 20=2.0, 25=2.5, 30=3.0
  uint16_t toneHz = 650;    // 300...1200
  uint8_t keyMode = KEY_PIN7_DIT_PIN8_DAH;
  uint8_t outputMode = OUTPUT_BOTH;
  uint8_t invertDisplay = 0;   // 0=OFF, 1=180 degrees
  uint8_t brightness = 100;    // 1, 25, 50, 75 or 100 percent
};

TrainerSettings settings;
Preferences preferences;

// Non-zero while Sound uses its independent game speed.
// This changes keyer/decoder timing without modifying settings.wpm.
uint8_t cwTimingOverrideWpm = 0;

bool isValidWpm(uint8_t value) {
  return value == 10 || value == 15 || value == 20 || value == 25 || value == 30 || value == 35 || value == 40;
}

uint8_t nearestValidWpm(uint8_t value) {
  constexpr uint8_t VALID_WPM[] = {
    10, 15, 20, 25, 30, 35, 40
  };

  uint8_t nearest = VALID_WPM[0];
  uint8_t nearestDistance = abs(static_cast<int>(value) - static_cast<int>(nearest));

  for (uint8_t candidate : VALID_WPM) {
    uint8_t distance =
      abs(static_cast<int>(value) - static_cast<int>(candidate));

    if (distance < nearestDistance) {
      nearest = candidate;
      nearestDistance = distance;
    }
  }

  return nearest;
}

bool isValidBrightness(uint8_t value) {
  return value == 1 || value == 25 || value == 50 ||
         value == 75 || value == 100;
}

uint8_t nearestValidBrightness(uint8_t value) {
  constexpr uint8_t VALUES[] = {1, 25, 50, 75, 100};

  uint8_t nearest = VALUES[0];
  uint8_t nearestDistance =
      abs(static_cast<int>(value) - static_cast<int>(nearest));

  for (uint8_t candidate : VALUES) {
    uint8_t distance =
        abs(static_cast<int>(value) - static_cast<int>(candidate));

    if (distance < nearestDistance) {
      nearest = candidate;
      nearestDistance = distance;
    }
  }

  return nearest;
}

uint8_t brightnessPercentToContrast(uint8_t percent) {
  // SSD1306 has an 8-bit contrast/current setting: 0...255.
  // Keep 1% non-zero so the display never becomes completely invisible.
  uint16_t contrast =
      (static_cast<uint16_t>(percent) * 255U + 50U) / 100U;

  return static_cast<uint8_t>(max<uint16_t>(1U, contrast));
}

void applyDisplayBrightness() {
  if (!oledAvailable) {
    return;
  }

  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(
      brightnessPercentToContrast(settings.brightness));
  screenDirty = true;
}

void clampSettings() {
  if (!isValidWpm(settings.wpm)) {
    settings.wpm = nearestValidWpm(settings.wpm);
  }

  if (settings.dahRatio10 != 20 && settings.dahRatio10 != 25 && settings.dahRatio10 != 30) {
    settings.dahRatio10 = 30;
  }

  settings.toneHz = constrain(settings.toneHz, 300, 1200);
  settings.keyMode = constrain(settings.keyMode, 0, 2);
  settings.outputMode = constrain(settings.outputMode, 0, 2);
  settings.invertDisplay = settings.invertDisplay ? 1 : 0;

  if (!isValidBrightness(settings.brightness)) {
    settings.brightness =
        nearestValidBrightness(settings.brightness);
  }
}

void loadSettings() {
  preferences.begin("cwtrainer", true);
  settings.wpm = preferences.getUChar("wpm", 20);

  // Compatibility with older versions that stored 2 or 3.
  uint8_t storedRatio = preferences.getUChar("ratio", 30);
  if (storedRatio == 2) {
    storedRatio = 20;
  } else if (storedRatio == 3) {
    storedRatio = 30;
  }
  settings.dahRatio10 = storedRatio;

  settings.toneHz = preferences.getUShort("tone", 650);
  settings.keyMode =
    preferences.getUChar("keymode", KEY_PIN7_DIT_PIN8_DAH);
  settings.outputMode = preferences.getUChar("output", OUTPUT_BOTH);
  settings.invertDisplay = preferences.getUChar("invert", 0);
  settings.brightness = preferences.getUChar("bright", 100);
  preferences.end();
  clampSettings();
}

void saveSettings() {
  preferences.begin("cwtrainer", false);
  preferences.putUChar("wpm", settings.wpm);
  preferences.putUChar("ratio", settings.dahRatio10);
  preferences.putUShort("tone", settings.toneHz);
  preferences.putUChar("keymode", settings.keyMode);
  preferences.putUChar("output", settings.outputMode);
  preferences.putUChar("invert", settings.invertDisplay);
  preferences.putUChar("bright", settings.brightness);
  preferences.end();
}

void applyDisplayRotation() {
  display.setRotation(settings.invertDisplay ? 2 : 0);
  screenDirty = true;
}

// Un punto estandar dura 1200/WPM milisegundos.
uint32_t ditTimeMs() {
  uint8_t effectiveWpm =
      cwTimingOverrideWpm != 0 ? cwTimingOverrideWpm : settings.wpm;
  return 1200UL / effectiveWpm;
}

/*
  Relacion raya:punto:
  - 3:1 = Morse estandar.
  - 2:1 = raya mas corta para practicar.
*/
uint32_t markTimeMs(bool isDah) {
  if (!isDah) {
    return ditTimeMs();
  }

  return (ditTimeMs() * settings.dahRatio10) / 10UL;
}

uint32_t elementGapMs() {
  return ditTimeMs();
}

const char* outputModeName(uint8_t mode) {
  switch (mode) {
    case OUTPUT_SOUND: return "SOUND";
    case OUTPUT_LIGHT: return "LIGHT";
    default: return "BOTH";
  }
}

String ratioText(uint8_t ratio10) {
  switch (ratio10) {
    case 20: return "2";
    case 25: return "2.5";
    default: return "3";
  }
}

const char* keyModeName(uint8_t mode) {
  switch (mode) {
    case KEY_PIN7_DIT_PIN8_DAH:
      return "T=. R=-";
    case KEY_PIN7_DAH_PIN8_DIT:
      return "T=- R=.";
    default:
      return "STRAIGHT";
  }
}

const char* keyModeFullName(uint8_t mode) {
  switch (mode) {
    case KEY_PIN7_DIT_PIN8_DAH:
      return "TIP=. RING=-";
    case KEY_PIN7_DAH_PIN8_DIT:
      return "TIP=- RING=.";
    default:
      return "STRAIGHT KEY";
  }
}

const char* keyModeTopLabel(uint8_t mode) {
  switch (mode) {
    case KEY_PIN7_DIT_PIN8_DAH:
      return ".-";
    case KEY_PIN7_DAH_PIN8_DIT:
      return "-.";
    default:
      return "ST";
  }
}

class DebouncedInput {
public:
  void begin(uint8_t pinNumber, uint32_t debounceMilliseconds = 5) {
    pin = pinNumber;
    debounceMs = debounceMilliseconds;
    pinMode(pin, INPUT_PULLUP);

    bool initial = digitalRead(pin) == LOW;
    rawPressed = initial;
    stablePressed = initial;
    lastRawChange = millis();
  }

  void update(uint32_t now) {
    pressedEdgeFlag = false;
    releasedEdgeFlag = false;

    bool reading = digitalRead(pin) == LOW;

    if (reading != rawPressed) {
      rawPressed = reading;
      lastRawChange = now;
    }

    if ((now - lastRawChange >= debounceMs) && (stablePressed != rawPressed)) {
      stablePressed = rawPressed;

      if (stablePressed) {
        pressedEdgeFlag = true;
      } else {
        releasedEdgeFlag = true;
      }
    }
  }

  bool pressed() const {
    return stablePressed;
  }

  bool pressedEdge() const {
    return pressedEdgeFlag;
  }

  bool releasedEdge() const {
    return releasedEdgeFlag;
  }

private:
  uint8_t pin = 0;
  uint32_t debounceMs = 5;
  uint32_t lastRawChange = 0;
  bool rawPressed = false;
  bool stablePressed = false;
  bool pressedEdgeFlag = false;
  bool releasedEdgeFlag = false;
};

DebouncedInput ditInput;
DebouncedInput dahInput;

// ========================== Menu button ==============================

enum ButtonEvent : uint8_t {
  BUTTON_NONE,
  BUTTON_SHORT,
  BUTTON_LONG,
  BUTTON_DOUBLE
};

class MultiClickButton {
public:
  void begin(uint8_t pinNumber) {
    pin = pinNumber;
    pinMode(pin, INPUT_PULLUP);

    bool initial = digitalRead(pin) == LOW;
    rawPressed = initial;
    stablePressed = initial;
    lastRawChange = millis();

    if (initial) {
      pressStarted = millis();
    }
  }

  ButtonEvent update(uint32_t now) {
    bool reading = digitalRead(pin) == LOW;

    if (reading != rawPressed) {
      rawPressed = reading;
      lastRawChange = now;
    }

    if ((now - lastRawChange >= debounceMs) && (stablePressed != rawPressed)) {
      stablePressed = rawPressed;

      if (stablePressed) {
        pressStarted = now;
      } else {
        uint32_t heldMs = now - pressStarted;

        if (heldMs >= longPressMs) {
          waitingSecondClick = false;
          return BUTTON_LONG;
        }

        if (waitingSecondClick && (now - firstReleaseTime <= doubleClickMs)) {
          waitingSecondClick = false;
          return BUTTON_DOUBLE;
        }

        waitingSecondClick = true;
        firstReleaseTime = now;
      }
    }

    if (waitingSecondClick && !stablePressed && (now - firstReleaseTime > doubleClickMs)) {
      waitingSecondClick = false;
      return BUTTON_SHORT;
    }

    return BUTTON_NONE;
  }

  bool pressed() const {
    return stablePressed;
  }

  void reset(uint32_t now) {
    bool initial = digitalRead(pin) == LOW;

    rawPressed = initial;
    stablePressed = initial;
    lastRawChange = now;
    pressStarted = initial ? now : 0;
    firstReleaseTime = 0;
    waitingSecondClick = false;
  }

private:
  uint8_t pin = 0;

  static constexpr uint32_t debounceMs = 25;
  static constexpr uint32_t longPressMs = 700;
  static constexpr uint32_t doubleClickMs = 320;

  uint32_t lastRawChange = 0;
  uint32_t pressStarted = 0;
  uint32_t firstReleaseTime = 0;

  bool rawPressed = false;
  bool stablePressed = false;
  bool waitingSecondClick = false;
};

MultiClickButton menuButton;

/*
  Gesture detector for a straight key while navigating menus.
  The signal is already debounced by ditInput/dahInput, so this class
  only implements short / long / double timing.
*/

class MultiClickSignal {
public:
  void reset(bool pressedNow, uint32_t now) {
    stablePressed = pressedNow;
    pressStarted = pressedNow ? now : 0;
    firstReleaseTime = 0;
    waitingSecondClick = false;
  }

  ButtonEvent update(bool pressedNow, uint32_t now) {
    if (pressedNow != stablePressed) {
      stablePressed = pressedNow;

      if (stablePressed) {
        pressStarted = now;
      } else {
        uint32_t heldMs = now - pressStarted;

        if (heldMs >= longPressMs) {
          waitingSecondClick = false;
          return BUTTON_LONG;
        }

        if (waitingSecondClick && (now - firstReleaseTime <= doubleClickMs)) {
          waitingSecondClick = false;
          return BUTTON_DOUBLE;
        }

        waitingSecondClick = true;
        firstReleaseTime = now;
      }
    }

    if (waitingSecondClick && !stablePressed && (now - firstReleaseTime > doubleClickMs)) {
      waitingSecondClick = false;
      return BUTTON_SHORT;
    }

    return BUTTON_NONE;
  }

private:
  static constexpr uint32_t longPressMs = 700;
  static constexpr uint32_t doubleClickMs = 320;

  uint32_t pressStarted = 0;
  uint32_t firstReleaseTime = 0;
  bool stablePressed = false;
  bool waitingSecondClick = false;
};

MultiClickSignal straightMenuButton;
bool straightMenuControlWasActive = false;
bool straightMenuInputArmed = false;


uint32_t bothPaddlesStarted = 0;
bool bothPaddlesMenuTriggered = false;
bool paddleComboLatched = false;
constexpr uint32_t BOTH_PADDLES_MENU_MS = 1200;


constexpr uint32_t PADDLE_CHORD_WINDOW_MS = 70;

enum PendingPaddleMenuAction : uint8_t {
  PADDLE_MENU_NONE,
  PADDLE_MENU_DIT,
  PADDLE_MENU_DAH
};

PendingPaddleMenuAction pendingPaddleMenuAction = PADDLE_MENU_NONE;
uint32_t pendingPaddleMenuStarted = 0;
bool paddleMenuGestureLatched = false;

// =========================== CW output ================================

bool cwLedPwmAttached = false;
void writeCwLed(bool on);

void initializeCwLed() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  cwLedPwmAttached = ledcAttach(
    CW_LED_PIN,
    CW_LED_PWM_FREQUENCY_HZ,
    CW_LED_PWM_RESOLUTION_BITS);

  if (!cwLedPwmAttached) {
    // Safe fallback: leave the LEDs OFF rather than driving them at 100%.
    pinMode(CW_LED_PIN, OUTPUT);
    digitalWrite(CW_LED_PIN, LOW);
  }
#else
  ledcSetup(
    CW_LED_LEDC_CHANNEL,
    CW_LED_PWM_FREQUENCY_HZ,
    CW_LED_PWM_RESOLUTION_BITS);
  ledcAttachPin(CW_LED_PIN, CW_LED_LEDC_CHANNEL);
  cwLedPwmAttached = true;
#endif

  writeCwLed(false);
}

void writeCwLed(bool on) {
  if (!cwLedPwmAttached) {
    digitalWrite(CW_LED_PIN, LOW);
    return;
  }

  uint8_t duty = on ? CW_LED_PWM_DUTY : 0;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(CW_LED_PIN, duty);
#else
  ledcWrite(CW_LED_LEDC_CHANNEL, duty);
#endif
}

bool cwBuzzerPwmAttached = false;

void initializeBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);

  if (ACTIVE_BUZZER) {
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  cwBuzzerPwmAttached =
      ledcAttach(BUZZER_PIN, settings.toneHz, 8);

  if (cwBuzzerPwmAttached) {
    ledcWriteTone(BUZZER_PIN, 0);
  } else {
    // Safe fallback: never try to drive an unattached LEDC output.
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  }
#else
  double actualFrequency =
      ledcSetup(BUZZER_LEDC_CHANNEL, settings.toneHz, 8);

  cwBuzzerPwmAttached = actualFrequency > 0.0;

  if (cwBuzzerPwmAttached) {
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
    ledcWriteTone(BUZZER_LEDC_CHANNEL, 0);
  } else {
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
  }
#endif
}

void writeBuzzer(bool on) {
  if (ACTIVE_BUZZER) {
    digitalWrite(BUZZER_PIN, on ? HIGH : LOW);
    return;
  }

  if (!cwBuzzerPwmAttached) {
    digitalWrite(BUZZER_PIN, LOW);
    return;
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER_PIN, on ? settings.toneHz : 0);
#else
  ledcWriteTone(BUZZER_LEDC_CHANNEL, on ? settings.toneHz : 0);
#endif
}

void setCwOutputs(bool on) {
  bool useSound =
    settings.outputMode == OUTPUT_SOUND || settings.outputMode == OUTPUT_BOTH;

  bool useLight =
    settings.outputMode == OUTPUT_LIGHT || settings.outputMode == OUTPUT_BOTH;

  writeBuzzer(on && useSound);
  writeCwLed(on && useLight);
}

// ========================= Mose decodification =========================

struct MorseEntry {
  const char* code;
  char character;
};

const MorseEntry MORSE_TABLE[] = {
  { ".-", 'A' }, { "-...", 'B' }, { "-.-.", 'C' }, { "-..", 'D' }, { ".", 'E' }, { "..-.", 'F' }, { "--.", 'G' }, { "....", 'H' }, { "..", 'I' }, { ".---", 'J' }, { "-.-", 'K' }, { ".-..", 'L' }, { "--", 'M' }, { "-.", 'N' }, { "---", 'O' }, { ".--.", 'P' }, { "--.-", 'Q' }, { ".-.", 'R' }, { "...", 'S' }, { "-", 'T' }, { "..-", 'U' }, { "...-", 'V' }, { ".--", 'W' }, { "-..-", 'X' }, { "-.--", 'Y' }, { "--..", 'Z' },

  { "-----", '0' },
  { ".----", '1' },
  { "..---", '2' },
  { "...--", '3' },
  { "....-", '4' },
  { ".....", '5' },
  { "-....", '6' },
  { "--...", '7' },
  { "---..", '8' },
  { "----.", '9' },

  { ".-.-.-", '.' },
  { "--..--", ',' },
  { "..--..", '?' },
  { "-..-.", '/' },
  { "-....-", '-' },
  { "-.--.", '(' },
  { "-.--.-", ')' },
  { ".-.-.", '+' },
  { "-...-", '=' },
  { ".--.-.", '@' }
};

constexpr size_t MORSE_ENTRY_COUNT =
  sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]);

String currentMorse;
String decodedText;

// Games can temporarily capture decoded characters instead of adding
// them to the normal trainer text.
bool gameInputCaptureActive = false;   // Sound
bool writeInputCaptureActive = false;  // Write

void handleGameDecodedCharacter(char character);
void handleWriteDecodedCharacter(char character);

uint32_t lastElementFinished = 0;
uint32_t lastKeyActivity = 0;
constexpr uint32_t AUTO_CLEAR_MS = 20000;

bool decoderKeyerBusy = false;

bool characterAlreadyDecoded = false;
bool wordSpaceAlreadyAdded = false;

char decodeMorse(const String& code) {
  for (const MorseEntry& entry : MORSE_TABLE) {
    if (code.equals(entry.code)) {
      return entry.character;
    }
  }
  return '#';
}

const char* encodeMorse(char character) {
  character = static_cast<char>(
    toupper(static_cast<unsigned char>(character)));

  for (const MorseEntry& entry : MORSE_TABLE) {
    if (entry.character == character) {
      return entry.code;
    }
  }

  return nullptr;
}

String textToMorse(const char* message) {
  String result;

  if (message == nullptr) {
    return result;
  }

  bool firstToken = true;

  for (size_t index = 0; message[index] != '\0'; ++index) {
    char character = message[index];

    if (character == ' ') {
      if (!result.endsWith(" / ")) {
        result += " / ";
      }
      firstToken = true;
      continue;
    }

    const char* code = encodeMorse(character);
    if (code == nullptr) {
      continue;
    }

    if (!firstToken && !result.endsWith(" / ")) {
      result += ' ';
    }

    result += code;
    firstToken = false;
  }

  return result;
}

void drawStartupMorseText(const char* message) {
  String morse = textToMorse(message);

  constexpr uint8_t CHARS_PER_LINE = 21;
  constexpr uint8_t MAX_LINES = 8;

  String lines[MAX_LINES];
  uint8_t lineCount = 0;
  String currentLine;

  // Wrap at spaces so Morse groups are not split.
  int startIndex = 0;
  while (startIndex < static_cast<int>(morse.length()) && lineCount < MAX_LINES) {
    int separator = morse.indexOf(' ', startIndex);
    String token;

    if (separator < 0) {
      token = morse.substring(startIndex);
      startIndex = morse.length();
    } else {
      token = morse.substring(startIndex, separator);
      startIndex = separator + 1;
    }

    if (token.isEmpty()) {
      continue;
    }

    String candidate =
      currentLine.isEmpty() ? token : currentLine + " " + token;

    if (candidate.length() <= CHARS_PER_LINE) {
      currentLine = candidate;
    } else {
      lines[lineCount++] = currentLine;
      currentLine = token;
    }
  }

  if (!currentLine.isEmpty() && lineCount < MAX_LINES) {
    lines[lineCount++] = currentLine;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setTextWrap(false);

  int16_t blockHeight = lineCount * 8;
  int16_t startY = max<int16_t>(0, (SCREEN_HEIGHT - blockHeight) / 2);

  for (uint8_t line = 0; line < lineCount; ++line) {
    int16_t lineWidth = lines[line].length() * 6;
    int16_t x = max<int16_t>(0, (SCREEN_WIDTH - lineWidth) / 2);

    display.setCursor(x, startY + line * 8);
    display.print(lines[line]);
  }

  display.display();
}
void appendDecodedCharacter(char character) {
  if (gameInputCaptureActive) {
    handleGameDecodedCharacter(character);
    screenDirty = true;
    return;
  }

  if (writeInputCaptureActive) {
    handleWriteDecodedCharacter(character);
    screenDirty = true;
    return;
  }

  decodedText += character;

  // 5 line 21 characters.
  constexpr size_t MAX_TEXT_LENGTH = 105;
  if (decodedText.length() > MAX_TEXT_LENGTH) {
    decodedText.remove(0, decodedText.length() - MAX_TEXT_LENGTH);
  }

  Serial.print(character);
  screenDirty = true;
}

void registerElement(bool isDah) {
  if (currentMorse.length() < 8) {
    currentMorse += isDah ? '-' : '.';
  } else {
    currentMorse = "#";
  }

  lastElementFinished = millis();
  lastKeyActivity = lastElementFinished;
  characterAlreadyDecoded = false;
  wordSpaceAlreadyAdded = false;
  screenDirty = true;
}

void clearDecoder() {
  currentMorse = "";
  decodedText = "";
  characterAlreadyDecoded = false;
  wordSpaceAlreadyAdded = false;
  screenDirty = true;
}

uint32_t characterTimeoutMs() {
 
  return 3UL * ditTimeMs() + 80UL;
}

uint32_t wordTimeoutMs() {
  return 7UL * ditTimeMs() + 120UL;
}

void serviceDecoder(uint32_t now) {
  bool paddleActive =
    ditInput.pressed() || dahInput.pressed();


  if (!decoderKeyerBusy && !paddleActive && !currentMorse.isEmpty() && !characterAlreadyDecoded && (now - lastElementFinished >= characterTimeoutMs())) {
    appendDecodedCharacter(decodeMorse(currentMorse));
    currentMorse = "";
    characterAlreadyDecoded = true;
    screenDirty = true;
  }

  if (!decoderKeyerBusy && !paddleActive && characterAlreadyDecoded && !wordSpaceAlreadyAdded && (now - lastElementFinished >= wordTimeoutMs())) {
    if (!decodedText.isEmpty() && decodedText.charAt(decodedText.length() - 1) != ' ') {
      appendDecodedCharacter(' ');
    }
    wordSpaceAlreadyAdded = true;
  }
}

void serviceAutoClear(uint32_t now) {
  bool hasVisibleContent =
    !decodedText.isEmpty() || !currentMorse.isEmpty();

  if (hasVisibleContent && lastKeyActivity != 0 && (now - lastKeyActivity >= AUTO_CLEAR_MS)) {
    clearDecoder();
    lastKeyActivity = now;
  }
}

// ============================== KEYER ==================================

enum KeyerPhase : uint8_t {
  KEYER_IDLE,
  KEYER_MARK,
  KEYER_GAP
};

KeyerPhase keyerPhase = KEYER_IDLE;
uint32_t keyerDeadline = 0;
bool activeElementIsDah = false;
bool lastElementWasDit = false;
bool ditMemory = false;
bool dahMemory = false;
bool bothSqueezeActive = false;

// Estado para llave recta/manual.
bool straightKeyActive = false;
uint32_t straightKeyStarted = 0;

bool logicalDitPressed() {
  if (settings.keyMode == KEY_PIN7_DIT_PIN8_DAH) {
    return ditInput.pressed();
  }

  if (settings.keyMode == KEY_PIN7_DAH_PIN8_DIT) {
    return dahInput.pressed();
  }

  return false;
}

bool logicalDahPressed() {
  if (settings.keyMode == KEY_PIN7_DIT_PIN8_DAH) {
    return dahInput.pressed();
  }

  if (settings.keyMode == KEY_PIN7_DAH_PIN8_DIT) {
    return ditInput.pressed();
  }

  return false;
}

bool logicalDitPressedEdge() {
  if (settings.keyMode == KEY_PIN7_DIT_PIN8_DAH) {
    return ditInput.pressedEdge();
  }

  if (settings.keyMode == KEY_PIN7_DAH_PIN8_DIT) {
    return dahInput.pressedEdge();
  }

  return false;
}

bool logicalDahPressedEdge() {
  if (settings.keyMode == KEY_PIN7_DIT_PIN8_DAH) {
    return dahInput.pressedEdge();
  }

  if (settings.keyMode == KEY_PIN7_DAH_PIN8_DIT) {
    return ditInput.pressedEdge();
  }

  return false;
}

bool straightContactPressed() {
  // In straight mode you can use any side
  return ditInput.pressed() || dahInput.pressed();
}


void stopKeyer() {
  setCwOutputs(false);
  keyerPhase = KEYER_IDLE;
  decoderKeyerBusy = false;
  ditMemory = false;
  dahMemory = false;
  bothSqueezeActive = false;
  straightKeyActive = false;
  screenDirty = true;
}

void startElement(bool isDah, uint32_t now) {
  decoderKeyerBusy = true;
  activeElementIsDah = isDah;
  lastElementWasDit = !isDah;

  if (isDah) {
    dahMemory = false;
  } else {
    ditMemory = false;
  }

  setCwOutputs(true);
  keyerPhase = KEYER_MARK;
  keyerDeadline = now + markTimeMs(isDah);
  screenDirty = true;
}

void serviceStraightKey(uint32_t now, bool keyingEnabled) {
  bool pressed = keyingEnabled && straightContactPressed();

  if (pressed && !straightKeyActive) {
    straightKeyActive = true;
    straightKeyStarted = now;
    decoderKeyerBusy = true;
    setCwOutputs(true);
    screenDirty = true;
    return;
  }

  if (!pressed && straightKeyActive) {
    uint32_t heldMs = now - straightKeyStarted;
    uint32_t threshold =
      (ditTimeMs() + markTimeMs(true)) / 2UL;

    setCwOutputs(false);
    straightKeyActive = false;
    decoderKeyerBusy = false;


    registerElement(heldMs >= threshold);
    screenDirty = true;
  }

  if (!keyingEnabled && straightKeyActive) {
    setCwOutputs(false);
    straightKeyActive = false;
    decoderKeyerBusy = false;
  }
}

void serviceKeyer(uint32_t now, bool keyingEnabled) {
  if (settings.keyMode == KEY_STRAIGHT) {
    serviceStraightKey(now, keyingEnabled);
    return;
  }

  bool ditPressed = logicalDitPressed();
  bool dahPressed = logicalDahPressed();

  if (!(ditPressed && dahPressed)) {
    bothSqueezeActive = false;
  }


  if (keyingEnabled && keyerPhase != KEYER_IDLE) {
    if (logicalDitPressedEdge()) {
      ditMemory = true;
    }
    if (logicalDahPressedEdge()) {
      dahMemory = true;
    }
  }

  switch (keyerPhase) {
    case KEYER_MARK:
      if (static_cast<int32_t>(now - keyerDeadline) >= 0) {
        setCwOutputs(false);
        registerElement(activeElementIsDah);
        keyerPhase = KEYER_GAP;
        keyerDeadline = now + elementGapMs();
      }
      break;

    case KEYER_GAP:
      if (static_cast<int32_t>(now - keyerDeadline) >= 0) {
        keyerPhase = KEYER_IDLE;
        decoderKeyerBusy = false;
        screenDirty = true;
      }
      break;

    case KEYER_IDLE:
      if (!keyingEnabled) {
        return;
      }

      {
        bool ditRequested = logicalDitPressed() || ditMemory;
        bool dahRequested = logicalDahPressed() || dahMemory;

        if (!ditRequested && !dahRequested) {
          return;
        }

        bool sendDah;

        if (ditRequested && dahRequested) {
          /*
            Original iambic behaviour:
            alternate from the last element, so a squeeze may produce
            .- or -. depending on the previous state.
          */
          bothSqueezeActive = true;
          sendDah = lastElementWasDit;
        } else {
          sendDah = dahRequested;
        }

        startElement(sendDah, now);
      }
      break;
  }
}

// ============================== MENU ===================================

enum UiMode : uint8_t {
  UI_MAIN,
  UI_HOME_MENU,
  UI_MENU,
  UI_EDIT,
  UI_EASTER,
  UI_GAMES,
  UI_GAME_DIFFICULTY,
  UI_GAME1,
  UI_WRITE_DIFFICULTY,
  UI_GAME_WRITE
};

UiMode uiMode = UI_MAIN;

// Main launcher: Settings / Guide / Games.
uint8_t homeMenuIndex = 0;

// SETTINGS selection.
uint8_t menuIndex = 0;

// GAMES selection: Sound / Write.
uint8_t gamesMenuIndex = 0;

// ============================== Sound Game ================================

enum Game1State : uint8_t {
  GAME1_IDLE,
  GAME1_PLAYBACK,
  GAME1_WAIT_INPUT,
  GAME1_CORRECT_PAUSE,
  GAME1_GAME_OVER
};

enum GamePlaybackPhase : uint8_t {
  GAME_PLAY_MARK,
  GAME_PLAY_ELEMENT_GAP,
  GAME_PLAY_CHARACTER_GAP
};

uint8_t gameDifficultyIndex = 0;  // 0 Easy, 1 Medium, 2 Hard
Game1State game1State = GAME1_IDLE;
GamePlaybackPhase gamePlaybackPhase = GAME_PLAY_MARK;

uint8_t gameLives = 3;
uint8_t gameWpm = 8;
uint8_t gameMaxWpm = 8;
uint8_t gameProgressIndex = 0;

String gameTarget;
String gameAccepted;
char gameWrongChar = '\0';
size_t gameWrongIndex = 0;
uint32_t gameWrongUntil = 0;

bool gameInputsArmed = false;

size_t gamePlaybackCharIndex = 0;
uint8_t gamePlaybackSymbolIndex = 0;
const char* gamePlaybackCode = nullptr;
uint32_t gamePlaybackDeadline = 0;
uint32_t gameCorrectUntil = 0;

/*
  Progression used by Game 1.
  Easy starts at 8, Medium at 20 and Hard at 30.
  The upper part follows the requested 35,37,40,42,45,47,50 pattern.
*/
const uint8_t GAME_WPM_LADDER[] = {
  8, 10, 12, 14, 16, 18, 20, 22,
  25, 27, 30, 32,
  35, 37, 40, 42, 45, 47, 50
};

constexpr uint8_t GAME_WPM_COUNT =
    sizeof(GAME_WPM_LADDER) / sizeof(GAME_WPM_LADDER[0]);

constexpr uint8_t GAME_EASY_START_INDEX = 0;   // 8 WPM
constexpr uint8_t GAME_MEDIUM_START_INDEX = 6; // 20 WPM
constexpr uint8_t GAME_HARD_START_INDEX = 10;  // 30 WPM

const char* GAME_COMMON_ITEMS[] = {
  "CAT", "DOG", "RR", "73", "CQ", "QTH", "QSL",
  "DX", "RST", "WX", "TU", "TNX", "SOS", "ANT", "PWR"
};

constexpr uint8_t GAME_COMMON_ITEM_COUNT =
    sizeof(GAME_COMMON_ITEMS) / sizeof(GAME_COMMON_ITEMS[0]);

constexpr uint32_t GAME_CORRECT_PAUSE_MS = 650;

// ============================== WRITE GAME ==============================

enum WriteGameState : uint8_t {
  WRITE_IDLE,
  WRITE_WAIT_RELEASE,
  WRITE_ACTIVE,
  WRITE_PENALTY,
  WRITE_CORRECT_PAUSE,
  WRITE_GAME_OVER
};

uint8_t writeDifficultyIndex = 0;  // 0 Easy, 1 Medium, 2 Hard
WriteGameState writeGameState = WRITE_IDLE;

String writeTarget;
String writeAccepted;
char writeWrongChar = '\0';
char writeExpectedChar = '\0';

uint8_t writeBaseSeconds = 15;
uint8_t writeReachedSeconds = 15;

uint32_t writeRoundBudgetMs = 0;
uint32_t writeRemainingMs = 0;
uint32_t writeDeadline = 0;
uint32_t writePenaltyUntil = 0;
uint32_t writeCorrectUntil = 0;

constexpr uint32_t WRITE_PENALTY_MS = 3000;
constexpr uint32_t WRITE_CORRECT_PAUSE_MS = 650;

constexpr uint8_t MENU_ITEM_COUNT = 7;
constexpr uint8_t MENU_VISIBLE_ITEMS = 5;

// Forward declarations.
void advanceMenuSelection();
void resetDualPaddleMenuGesture();

uint8_t serviceStraightMenuButton(uint32_t now) {
  bool active =
    settings.keyMode == KEY_STRAIGHT &&
    uiMode != UI_MAIN &&
    uiMode != UI_GAME1 &&
    uiMode != UI_GAME_WRITE;

  bool pressed = straightContactPressed();

  if (!active) {
    straightMenuControlWasActive = false;
    straightMenuInputArmed = false;
    straightMenuButton.reset(pressed, now);
    return BUTTON_NONE;
  }

  // On first entering a menu with STRAIGHT enabled, require a clean
  // release before accepting gestures. This prevents the key press that
  // preceded the menu from becoming an accidental menu command.
  if (!straightMenuControlWasActive) {
    straightMenuControlWasActive = true;
    straightMenuInputArmed = !pressed;
    straightMenuButton.reset(pressed, now);
    return BUTTON_NONE;
  }

  if (!straightMenuInputArmed) {
    straightMenuButton.reset(pressed, now);

    if (!pressed) {
      straightMenuInputArmed = true;
    }
    return BUTTON_NONE;
  }

  return static_cast<uint8_t>(
    straightMenuButton.update(pressed, now));
}


// CW reference guide.
constexpr uint8_t GUIDE_ROWS_PER_PAGE = 6;
uint8_t guidePage = 0;
bool easterInputArmed = false;

// Alternating help line in SETTINGS.
constexpr uint32_t SETTINGS_HELP_INTERVAL_MS = 5000;
uint8_t lastSettingsHelpPage = 255;

const char* menuItemName(uint8_t item) {
  switch (item) {
    case 0: return "Speed";
    case 1: return "Ratio";
    case 2: return "Tone";
    case 3: return "Key";
    case 4: return "Output";
    case 5: return "Invert";
    default: return "Brightness";
  }
}


void setMenuValue(uint8_t item, int value) {
  switch (item) {
    case 0:
      settings.wpm = nearestValidWpm(value);
      break;
    case 1:
      if (value == 20 || value == 25 || value == 30) {
        settings.dahRatio10 = value;
      } else {
        settings.dahRatio10 = 30;
      }
      break;
    case 2:
      settings.toneHz = constrain(value, 300, 1200);
      break;
    case 3:
      settings.keyMode = constrain(value, 0, 2);
      stopKeyer();
      break;
    case 4:
      settings.outputMode = constrain(value, 0, 2);
      break;
    case 5:
      settings.invertDisplay = value ? 1 : 0;
      applyDisplayRotation();
      break;
    default:
      settings.brightness =
          nearestValidBrightness(
              static_cast<uint8_t>(constrain(value, 1, 100)));
      applyDisplayBrightness();
      break;
  }

  screenDirty = true;
}

void incrementMenuValue(uint8_t item) {
  switch (item) {
    case 0:
      settings.wpm += 5;
      if (settings.wpm > 40) {
        settings.wpm = 10;
      }
      break;

    case 1:
      if (settings.dahRatio10 == 20) {
        settings.dahRatio10 = 25;
      } else if (settings.dahRatio10 == 25) {
        settings.dahRatio10 = 30;
      } else {
        settings.dahRatio10 = 20;
      }
      break;

    case 2:
      settings.toneHz += 50;
      if (settings.toneHz > 1200) {
        settings.toneHz = 300;
      }
      break;

    case 3:
      settings.keyMode = (settings.keyMode + 1) % 3;
      stopKeyer();
      break;

    case 4:
      settings.outputMode =
        (settings.outputMode + 1) % 3;
      break;

    case 5:
      settings.invertDisplay = settings.invertDisplay ? 0 : 1;
      applyDisplayRotation();
      break;

    default:
      if (settings.brightness == 1) {
        settings.brightness = 25;
      } else if (settings.brightness == 25) {
        settings.brightness = 50;
      } else if (settings.brightness == 50) {
        settings.brightness = 75;
      } else if (settings.brightness == 75) {
        settings.brightness = 100;
      } else {
        settings.brightness = 1;
      }

      applyDisplayBrightness();
      break;
  }

  // Do not write flash on every Short press.
  // SETTINGS are saved by the existing Twice / Save & Exit action.
  screenDirty = true;
}

uint8_t guideDataPageCount() {
  return static_cast<uint8_t>(
    (MORSE_ENTRY_COUNT + GUIDE_ROWS_PER_PAGE - 1) / GUIDE_ROWS_PER_PAGE);
}

uint8_t guideTotalPageCount() {
  // One additional final page for the thank-you message.
  return guideDataPageCount() + 1;
}

void nextGuidePage() {
  guidePage = (guidePage + 1) % guideTotalPageCount();
  screenDirty = true;
}

void previousGuidePage() {
  if (guidePage == 0) {
    guidePage = guideTotalPageCount() - 1;
  } else {
    guidePage--;
  }

  screenDirty = true;
}

void exitEasterEgg() {
  uiMode = UI_HOME_MENU;
  homeMenuIndex = 1;
  guidePage = 0;
  easterInputArmed = false;
  screenDirty = true;
}

void enterEasterEgg() {
  stopKeyer();
  guidePage = 0;
  easterInputArmed = false;
  uiMode = UI_EASTER;
  screenDirty = true;
}

void enterHomeMenu() {
  stopKeyer();

  // Discard only an unfinished character; keep decoded text intact.
  currentMorse = "";
  characterAlreadyDecoded = false;

  uiMode = UI_HOME_MENU;
  homeMenuIndex = 0;
  screenDirty = true;
}

void enterSettingsMenu() {
  stopKeyer();

  
  currentMorse = "";
  characterAlreadyDecoded = false;

  uiMode = UI_MENU;
  menuIndex = 0;
  screenDirty = true;
}

void enterGamesMenu() {
  stopKeyer();
  gamesMenuIndex = 0;
  uiMode = UI_GAMES;
  screenDirty = true;
}


uint8_t gameDifficultyStartIndex(uint8_t difficulty) {
  switch (difficulty) {
    case 0: return GAME_EASY_START_INDEX;
    case 1: return GAME_MEDIUM_START_INDEX;
    default: return GAME_HARD_START_INDEX;
  }
}

void clearGameDecoderState() {
  currentMorse = "";
  characterAlreadyDecoded = false;
  wordSpaceAlreadyAdded = false;
  decoderKeyerBusy = false;
  ditMemory = false;
  dahMemory = false;
  bothSqueezeActive = false;
}

String generateGameCallsign() {
  // Amateur-style format: two letters, one digit, three letters.
  String value;

  for (uint8_t i = 0; i < 2; ++i) {
    value += static_cast<char>('A' + random(26));
  }

  value += static_cast<char>('0' + random(10));

  for (uint8_t i = 0; i < 3; ++i) {
    value += static_cast<char>('A' + random(26));
  }

  return value;
}

String generateGameThreeDigitNumber() {
  String value;
  value += static_cast<char>('0' + random(10));
  value += static_cast<char>('0' + random(10));
  value += static_cast<char>('0' + random(10));
  return value;
}

String generateGameTarget() {
  long roll = random(100);

  if (roll < 45) {
    // 45%: callsign.
    return generateGameCallsign();
  }

  if (roll < 90) {
    // 45%: three-digit number.
    return generateGameThreeDigitNumber();
  }

  // 10%: short common word / radio abbreviation.
  return String(
      GAME_COMMON_ITEMS[random(GAME_COMMON_ITEM_COUNT)]);
}

uint32_t gameDitTimeMs() {
  return 1200UL / gameWpm;
}

uint32_t gameMarkTimeMs(bool isDah) {
  if (!isDah) {
    return gameDitTimeMs();
  }

  return (gameDitTimeMs() * settings.dahRatio10) / 10UL;
}

void finishGamePlayback(uint32_t now) {
  setCwOutputs(false);
  gamePlaybackCode = nullptr;
  game1State = GAME1_WAIT_INPUT;

  /*
    IMPORTANT:
    Do not accept any key/paddle input yet. A paddle or straight key may
    have been held while the prompt was playing. The game will arm only
    after BOOT, TIP and RING are all physically released.
  */
  gameInputsArmed = false;
  gameInputCaptureActive = false;
  clearGameDecoderState();

  // Discard any BOOT/straight-key gesture accumulated during playback.
  menuButton.reset(now);
  straightMenuButton.reset(straightContactPressed(), now);
  resetDualPaddleMenuGesture();

  screenDirty = true;
}

bool startGamePlaybackCharacter(uint32_t now) {
  while (gamePlaybackCharIndex < gameTarget.length()) {
    char character = gameTarget.charAt(gamePlaybackCharIndex);

    if (character == ' ') {
      gamePlaybackCharIndex++;
      continue;
    }

    gamePlaybackCode = encodeMorse(character);

    if (gamePlaybackCode == nullptr ||
        gamePlaybackCode[0] == '\0') {
      gamePlaybackCharIndex++;
      continue;
    }

    gamePlaybackSymbolIndex = 0;
    bool isDah = gamePlaybackCode[gamePlaybackSymbolIndex] == '-';
    setCwOutputs(true);
    gamePlaybackPhase = GAME_PLAY_MARK;
    gamePlaybackDeadline = now + gameMarkTimeMs(isDah);
    return true;
  }

  finishGamePlayback(now);
  return false;
}

void beginGamePromptPlayback(uint32_t now) {
  stopKeyer();
  setCwOutputs(false);

  clearGameDecoderState();
  gameInputCaptureActive = false;
  gameInputsArmed = false;

  game1State = GAME1_PLAYBACK;
  gamePlaybackCharIndex = 0;
  gamePlaybackSymbolIndex = 0;
  gamePlaybackCode = nullptr;

  startGamePlaybackCharacter(now);
  screenDirty = true;
}

void prepareNextGameRound(uint32_t now) {
  gameTarget = generateGameTarget();
  gameAccepted = "";
  gameWrongChar = '\0';
  gameWrongIndex = 0;
  gameWrongUntil = 0;
  beginGamePromptPlayback(now);
}

void advanceGameWpm() {
  if (gameProgressIndex + 1 < GAME_WPM_COUNT) {
    gameProgressIndex++;
  }

  gameWpm = GAME_WPM_LADDER[gameProgressIndex];
  gameMaxWpm = max(gameMaxWpm, gameWpm);
  cwTimingOverrideWpm = gameWpm;
}

void enterGame1Difficulty() {
  stopKeyer();
  cwTimingOverrideWpm = 0;
  gameInputCaptureActive = false;
  gameDifficultyIndex = 0;
  game1State = GAME1_IDLE;
  clearGameDecoderState();
  uiMode = UI_GAME_DIFFICULTY;
  screenDirty = true;
}

void startGame1(uint32_t now) {
  stopKeyer();

  gameProgressIndex =
      gameDifficultyStartIndex(gameDifficultyIndex);

  gameWpm = GAME_WPM_LADDER[gameProgressIndex];
  gameMaxWpm = gameWpm;
  gameLives = 3;
  gameAccepted = "";
  gameWrongChar = '\0';
  gameWrongIndex = 0;
  gameWrongUntil = 0;
  gameInputsArmed = false;

  // Game speed is independent from the trainer's saved WPM.
  cwTimingOverrideWpm = gameWpm;

  randomSeed(
      static_cast<unsigned long>(micros()) ^
      static_cast<unsigned long>(millis() << 16));

  uiMode = UI_GAME1;
  prepareNextGameRound(now);
  screenDirty = true;
}

void exitGame1ToGames() {
  setCwOutputs(false);
  stopKeyer();

  cwTimingOverrideWpm = 0;
  gameInputCaptureActive = false;
  gameInputsArmed = false;
  game1State = GAME1_IDLE;
  gameTarget = "";
  gameAccepted = "";
  gameWrongChar = '\0';
  gameWrongIndex = 0;
  gameWrongUntil = 0;
  clearGameDecoderState();

  uiMode = UI_GAMES;
  gamesMenuIndex = 0;
  screenDirty = true;
}

bool isGame1TypingEnabled() {
  return uiMode == UI_GAME1 &&
         game1State == GAME1_WAIT_INPUT &&
         gameInputsArmed;
}

void gameLoseLife(char wrongCharacter) {
  gameWrongChar = wrongCharacter;
  gameWrongIndex = gameAccepted.length();
  gameWrongUntil = millis() + 3000UL;

  if (gameLives > 0) {
    gameLives--;
  }

  // Stop and discard any CW state immediately after a wrong character.
  setCwOutputs(false);
  stopKeyer();
  clearGameDecoderState();
  gameInputCaptureActive = false;
  gameInputsArmed = false;

  if (gameLives == 0) {
    game1State = GAME1_GAME_OVER;
  }

  screenDirty = true;
}

void handleGameDecodedCharacter(char character) {
  if (!gameInputCaptureActive ||
      uiMode != UI_GAME1 ||
      game1State != GAME1_WAIT_INPUT) {
    return;
  }

  // Long pauses between letters or characters are intentionally ignored.
  if (character == ' ') {
    return;
  }

  character = static_cast<char>(
      toupper(static_cast<unsigned char>(character)));

  size_t expectedIndex = gameAccepted.length();

  if (expectedIndex >= gameTarget.length()) {
    return;
  }

  char expectedCharacter =
      static_cast<char>(
          toupper(static_cast<unsigned char>(
              gameTarget.charAt(expectedIndex))));

  if (character == expectedCharacter) {
    gameAccepted += character;

    if (gameAccepted.length() >= gameTarget.length()) {
      gameInputCaptureActive = false;
      game1State = GAME1_CORRECT_PAUSE;
      gameCorrectUntil = millis() + GAME_CORRECT_PAUSE_MS;
      setCwOutputs(false);
    }
  } else {
    gameLoseLife(character);
  }

  screenDirty = true;
}

void serviceGamePlayback(uint32_t now) {
  if (uiMode != UI_GAME1 ||
      game1State != GAME1_PLAYBACK) {
    return;
  }

  if (static_cast<int32_t>(now - gamePlaybackDeadline) < 0) {
    return;
  }

  switch (gamePlaybackPhase) {
    case GAME_PLAY_MARK:
      setCwOutputs(false);
      gamePlaybackSymbolIndex++;

      if (gamePlaybackCode != nullptr &&
          gamePlaybackCode[gamePlaybackSymbolIndex] != '\0') {
        gamePlaybackPhase = GAME_PLAY_ELEMENT_GAP;
        gamePlaybackDeadline = now + gameDitTimeMs();
      } else {
        gamePlaybackCharIndex++;

        if (gamePlaybackCharIndex < gameTarget.length()) {
          gamePlaybackPhase = GAME_PLAY_CHARACTER_GAP;
          gamePlaybackDeadline =
              now + 3UL * gameDitTimeMs();
        } else {
          finishGamePlayback(now);
        }
      }
      break;

    case GAME_PLAY_ELEMENT_GAP: {
      bool isDah =
          gamePlaybackCode[gamePlaybackSymbolIndex] == '-';
      setCwOutputs(true);
      gamePlaybackPhase = GAME_PLAY_MARK;
      gamePlaybackDeadline = now + gameMarkTimeMs(isDah);
      break;
    }

    case GAME_PLAY_CHARACTER_GAP:
      startGamePlaybackCharacter(now);
      break;
  }
}

void serviceGame1(uint32_t now) {
  if (uiMode != UI_GAME1) {
    return;
  }

  // Refresh while Wrong letter is active so 3 -> 2 -> 1 is visible.
  if (gameWrongChar != '\0' &&
      gameWrongUntil != 0 &&
      now - lastScreenDraw >= 100) {
    screenDirty = true;
  }

  if (gameWrongChar != '\0' &&
      gameWrongUntil != 0 &&
      static_cast<int32_t>(now - gameWrongUntil) >= 0) {
    gameWrongChar = '\0';
    gameWrongUntil = 0;

    // Still do NOT accept input until the key/paddles are fully released.
    gameInputCaptureActive = false;
    gameInputsArmed = false;
    clearGameDecoderState();
    screenDirty = true;
  }

  if (game1State == GAME1_PLAYBACK) {
    serviceGamePlayback(now);
    return;
  }

  if (game1State == GAME1_CORRECT_PAUSE) {
    if (static_cast<int32_t>(now - gameCorrectUntil) >= 0) {
      advanceGameWpm();
      prepareNextGameRound(now);
    }
    return;
  }

  if (game1State == GAME1_WAIT_INPUT &&
      !gameInputsArmed &&
      gameWrongUntil == 0) {
  
    bool bootReleased =
        digitalRead(MENU_BUTTON_PIN) == HIGH;

    bool paddlePinsReleased =
        digitalRead(DIT_PIN) == HIGH &&
        digitalRead(DAH_PIN) == HIGH;

    bool debouncedPaddlesReleased =
        !ditInput.pressed() &&
        !dahInput.pressed();

    bool allReleased =
        bootReleased &&
        paddlePinsReleased &&
        debouncedPaddlesReleased;

    if (allReleased) {
      // Re-synchronise every gesture state from a known released state.
      menuButton.reset(now);
      straightMenuButton.reset(false, now);
      resetDualPaddleMenuGesture();

      clearGameDecoderState();
      gameInputsArmed = true;
      gameInputCaptureActive = true;
      screenDirty = true;
    }
  }
}


uint8_t writeDifficultyStartSeconds(uint8_t difficulty) {
  switch (difficulty) {
    case 0: return 15;
    case 1: return 10;
    default: return 5;
  }
}

bool writeKeyFullyReleased() {
  bool physicalReleased =
      digitalRead(DIT_PIN) == HIGH &&
      digitalRead(DAH_PIN) == HIGH;

  bool debouncedReleased =
      !ditInput.pressed() &&
      !dahInput.pressed();

  return physicalReleased && debouncedReleased;
}

uint32_t writeNominalCharacterMs(char character) {
  const char* code = encodeMorse(character);
  if (code == nullptr || code[0] == '\0') return 0;

  uint32_t dit = 1200UL / settings.wpm;
  uint32_t total = 0;
  uint8_t symbols = 0;

  for (uint8_t i = 0; code[i] != '\0'; ++i) {
    total += code[i] == '-'
        ? (dit * settings.dahRatio10) / 10UL
        : dit;
    symbols++;
  }

  if (symbols > 1) {
    total += static_cast<uint32_t>(symbols - 1) * dit;
  }

  // Include the trainer's real character-closing delay.
  total += 3UL * dit + 80UL;
  return total;
}

uint32_t writeNominalTargetMs(const String& target) {
  uint32_t total = 0;
  for (size_t i = 0; i < target.length(); ++i) {
    char c = target.charAt(i);
    if (c != ' ') total += writeNominalCharacterMs(c);
  }
  return total;
}

uint32_t writeCurrentRemainingMs(uint32_t now) {
  if (writeGameState == WRITE_ACTIVE) {
    if (static_cast<int32_t>(writeDeadline - now) <= 0) return 0;
    return writeDeadline - now;
  }
  return writeRemainingMs;
}

void clearWriteDecoderState() {
  currentMorse = "";
  characterAlreadyDecoded = false;
  wordSpaceAlreadyAdded = false;
  decoderKeyerBusy = false;
  ditMemory = false;
  dahMemory = false;
  bothSqueezeActive = false;
}

void prepareWriteRound(uint32_t now) {
  stopKeyer();
  setCwOutputs(false);

  // Write uses the normal WPM selected in SETTINGS.
  cwTimingOverrideWpm = 0;
  gameInputCaptureActive = false;
  writeInputCaptureActive = false;

  writeTarget = generateGameTarget();
  writeAccepted = "";
  writeWrongChar = '\0';
  writeExpectedChar = '\0';

  writeRoundBudgetMs =
      static_cast<uint32_t>(writeBaseSeconds) * 1000UL +
      writeNominalTargetMs(writeTarget);
  writeRemainingMs = writeRoundBudgetMs;
  writeDeadline = 0;
  writePenaltyUntil = 0;

  clearWriteDecoderState();
  writeGameState = WRITE_WAIT_RELEASE;
  screenDirty = true;
}

void enterWriteDifficulty() {
  stopKeyer();
  setCwOutputs(false);
  cwTimingOverrideWpm = 0;
  gameInputCaptureActive = false;
  writeInputCaptureActive = false;
  writeDifficultyIndex = 0;
  writeGameState = WRITE_IDLE;
  clearWriteDecoderState();
  uiMode = UI_WRITE_DIFFICULTY;
  screenDirty = true;
}

void startWriteGame(uint32_t now) {
  stopKeyer();
  setCwOutputs(false);
  cwTimingOverrideWpm = 0;
  gameInputCaptureActive = false;
  writeInputCaptureActive = false;

  writeBaseSeconds = writeDifficultyStartSeconds(writeDifficultyIndex);
  writeReachedSeconds = writeBaseSeconds;
  writeGameState = WRITE_IDLE;

  uiMode = UI_GAME_WRITE;
  prepareWriteRound(now);
  screenDirty = true;
}

void exitWriteToGames() {
  stopKeyer();
  setCwOutputs(false);
  cwTimingOverrideWpm = 0;
  gameInputCaptureActive = false;
  writeInputCaptureActive = false;
  writeGameState = WRITE_IDLE;
  writeTarget = "";
  writeAccepted = "";
  writeWrongChar = '\0';
  writeExpectedChar = '\0';
  writeDeadline = 0;
  writePenaltyUntil = 0;
  clearWriteDecoderState();
  uiMode = UI_GAMES;
  gamesMenuIndex = 1;
  screenDirty = true;
}

bool isWriteTypingEnabled() {
  return uiMode == UI_GAME_WRITE &&
         writeGameState == WRITE_ACTIVE &&
         writeInputCaptureActive;
}

void failWriteAttempt(uint32_t now,
                      char wrongCharacter,
                      char expectedCharacter) {
  // Freeze the exact amount of time left.
  writeRemainingMs = writeCurrentRemainingMs(now);

  stopKeyer();
  setCwOutputs(false);
  clearWriteDecoderState();

  writeWrongChar = wrongCharacter;
  writeExpectedChar = expectedCharacter;

  // Restart the same target from the beginning after the penalty.
  writeAccepted = "";
  writeInputCaptureActive = false;

  writePenaltyUntil = now + WRITE_PENALTY_MS;
  writeGameState = WRITE_PENALTY;
  screenDirty = true;
}

void finishWriteGame() {
  stopKeyer();
  setCwOutputs(false);
  writeRemainingMs = 0;
  writeInputCaptureActive = false;
  clearWriteDecoderState();
  writeReachedSeconds = writeBaseSeconds;
  writeGameState = WRITE_GAME_OVER;
  screenDirty = true;
}

void handleWriteDecodedCharacter(char character) {
  if (!writeInputCaptureActive ||
      uiMode != UI_GAME_WRITE ||
      writeGameState != WRITE_ACTIVE) return;

  if (character == ' ') return;

  character = static_cast<char>(
      toupper(static_cast<unsigned char>(character)));

  size_t expectedIndex = writeAccepted.length();
  if (expectedIndex >= writeTarget.length()) return;

  char expectedCharacter = static_cast<char>(
      toupper(static_cast<unsigned char>(
          writeTarget.charAt(expectedIndex))));

  if (character != expectedCharacter) {
    failWriteAttempt(
        millis(),
        character,
        expectedCharacter);
    return;
  }

  writeAccepted += character;

  if (writeAccepted.length() >= writeTarget.length()) {
    uint32_t now = millis();

    // Freeze the displayed timer at the exact value reached on success
    writeRemainingMs = writeCurrentRemainingMs(now);

    stopKeyer();
    setCwOutputs(false);
    clearWriteDecoderState();
    writeInputCaptureActive = false;
    writeGameState = WRITE_CORRECT_PAUSE;
    writeCorrectUntil = now + WRITE_CORRECT_PAUSE_MS;
  }
  screenDirty = true;
}

void serviceWriteGame(uint32_t now) {
  if (uiMode != UI_GAME_WRITE) return;

  if ((writeGameState == WRITE_ACTIVE ||
       writeGameState == WRITE_PENALTY) &&
      now - lastScreenDraw >= 100) {
    screenDirty = true;
  }

  if (writeGameState == WRITE_ACTIVE) {
    if (static_cast<int32_t>(now - writeDeadline) >= 0) {
      finishWriteGame();
    }
    return;
  }

  if (writeGameState == WRITE_PENALTY) {
    if (static_cast<int32_t>(now - writePenaltyUntil) >= 0) {
      writePenaltyUntil = 0;
      clearWriteDecoderState();
      writeGameState = WRITE_WAIT_RELEASE;
      screenDirty = true;
    }
    return;
  }

  if (writeGameState == WRITE_CORRECT_PAUSE) {
    if (static_cast<int32_t>(now - writeCorrectUntil) >= 0) {
      if (writeBaseSeconds > 0) writeBaseSeconds--;
      writeReachedSeconds = writeBaseSeconds;
      prepareWriteRound(now);
    }
    return;
  }

  if (writeGameState == WRITE_WAIT_RELEASE) {
    if (writeKeyFullyReleased()) {
      menuButton.reset(now);
      straightMenuButton.reset(false, now);
      resetDualPaddleMenuGesture();
      clearWriteDecoderState();
      writeInputCaptureActive = true;
      writeDeadline = now + writeRemainingMs;
      writeGameState = WRITE_ACTIVE;
      screenDirty = true;
    }
  }
}

void handleButtonEvent(uint8_t event) {
  if (event == BUTTON_NONE) {
    return;
  }

  switch (uiMode) {
    case UI_MAIN:
      if (event == BUTTON_SHORT || event == BUTTON_LONG) {
        enterHomeMenu();
      } else if (event == BUTTON_DOUBLE) {
       
        clearDecoder();
      }
      break;

    case UI_HOME_MENU:
      if (event == BUTTON_SHORT) {
        homeMenuIndex = (homeMenuIndex + 1) % 3;
        screenDirty = true;
      } else if (event == BUTTON_LONG) {
        switch (homeMenuIndex) {
          case 0:
            enterSettingsMenu();
            break;
          case 1:
            enterEasterEgg();
            break;
          default:
            enterGamesMenu();
            break;
        }
      } else if (event == BUTTON_DOUBLE) {
        uiMode = UI_MAIN;
        screenDirty = true;
      }
      break;

    case UI_MENU:
      if (event == BUTTON_SHORT) {
        advanceMenuSelection();
      } else if (event == BUTTON_LONG) {
        uiMode = UI_EDIT;
        screenDirty = true;
      } else if (event == BUTTON_DOUBLE) {
        saveSettings();
        uiMode = UI_HOME_MENU;
        homeMenuIndex = 0;
        screenDirty = true;
      }
      break;

    case UI_EDIT:
      if (event == BUTTON_SHORT) {
        incrementMenuValue(menuIndex);
      } else if (event == BUTTON_LONG) {
        // Intentionally unused while editing.
      } else if (event == BUTTON_DOUBLE) {
        // Save the edited value and return to SETTINGS.
        saveSettings();
        uiMode = UI_MENU;
        screenDirty = true;
      }
      break;

    case UI_EASTER:
      if (!easterInputArmed) {
        break;
      }

      if (event == BUTTON_SHORT) {
        nextGuidePage();
      } else if (event == BUTTON_LONG) {
        previousGuidePage();
      } else if (event == BUTTON_DOUBLE) {
        exitEasterEgg();
      }
      break;

    case UI_GAMES:
      if (event == BUTTON_SHORT) {
        gamesMenuIndex = (gamesMenuIndex + 1) % 2;
        screenDirty = true;
      } else if (event == BUTTON_LONG) {
        if (gamesMenuIndex == 0) {
          enterGame1Difficulty();
        } else {
          enterWriteDifficulty();
        }
      } else if (event == BUTTON_DOUBLE) {
        uiMode = UI_HOME_MENU;
        homeMenuIndex = 2;
        screenDirty = true;
      }
      break;

    case UI_WRITE_DIFFICULTY:
      if (event == BUTTON_SHORT) {
        writeDifficultyIndex = (writeDifficultyIndex + 1) % 3;
        screenDirty = true;
      } else if (event == BUTTON_LONG) {
        startWriteGame(millis());
      } else if (event == BUTTON_DOUBLE) {
        uiMode = UI_GAMES;
        gamesMenuIndex = 1;
        screenDirty = true;
      }
      break;

    case UI_GAME_WRITE:
      // BOOT double-click exits immediately from every Write state.
      if (event == BUTTON_DOUBLE) {
        exitWriteToGames();
      }
      break;

    case UI_GAME_DIFFICULTY:
      if (event == BUTTON_SHORT) {
        gameDifficultyIndex = (gameDifficultyIndex + 1) % 3;
        screenDirty = true;
      } else if (event == BUTTON_LONG) {
        startGame1(millis());
      } else if (event == BUTTON_DOUBLE) {
        uiMode = UI_GAMES;
        gamesMenuIndex = 0;
        screenDirty = true;
      }
      break;

    case UI_GAME1:
      /*
        During LISTEN, normal presses are ignored, but BOOT double-click
        is an emergency/back gesture: cancel playback and return to GAMES.
      */
      if (game1State == GAME1_PLAYBACK) {
        if (event == BUTTON_DOUBLE) {
          exitGame1ToGames();
        }
        break;
      }

      // Double always goes back, including during the 3-second penalty.
      if (event == BUTTON_DOUBLE) {
        exitGame1ToGames();
        break;
      }

      if (!gameInputsArmed &&
          game1State != GAME1_GAME_OVER) {
        break;
      }

      if (event == BUTTON_SHORT &&
          game1State == GAME1_WAIT_INPUT) {
        // Replay the same target and keep lives/progress already entered.
        beginGamePromptPlayback(millis());
      }
      break;
  }
}

uint8_t serviceDualPaddleMenuGesture(uint32_t now) {
  bool ditPressed = ditInput.pressed();
  bool dahPressed = dahInput.pressed();
  bool bothPressed = ditPressed && dahPressed;
  bool bothReleased = !ditPressed && !dahPressed;

  /*
    After one menu gesture has fired, require a full release before
    accepting another. This prevents a held paddle from becoming a
    second command
  */
  if (paddleMenuGestureLatched) {
    if (bothReleased) {
      paddleMenuGestureLatched = false;
    }
    return BUTTON_NONE;
  }

  if (pendingPaddleMenuAction != PADDLE_MENU_NONE) {
    uint32_t elapsed = now - pendingPaddleMenuStarted;

    if (bothPressed && elapsed <= PADDLE_CHORD_WINDOW_MS) {
      pendingPaddleMenuAction = PADDLE_MENU_NONE;
      paddleMenuGestureLatched = true;
      return BUTTON_DOUBLE;
    }

    if (elapsed >= PADDLE_CHORD_WINDOW_MS) {
      uint8_t event =
          pendingPaddleMenuAction == PADDLE_MENU_DIT
              ? BUTTON_SHORT
              : BUTTON_LONG;

      pendingPaddleMenuAction = PADDLE_MENU_NONE;
      paddleMenuGestureLatched = true;
      return event;
    }

    return BUTTON_NONE;
  }

  /*
    Exact/same-loop squeeze: no delay is needed because both are already
    known to be down.
  */
  if (bothPressed) {
    paddleMenuGestureLatched = true;
    return BUTTON_DOUBLE;
  }

  if (ditInput.pressedEdge()) {
    pendingPaddleMenuAction = PADDLE_MENU_DIT;
    pendingPaddleMenuStarted = now;
    return BUTTON_NONE;
  }

  if (dahInput.pressedEdge()) {
    pendingPaddleMenuAction = PADDLE_MENU_DAH;
    pendingPaddleMenuStarted = now;
    return BUTTON_NONE;
  }

  return BUTTON_NONE;
}

void resetDualPaddleMenuGesture() {
  pendingPaddleMenuAction = PADDLE_MENU_NONE;
  pendingPaddleMenuStarted = 0;
  paddleMenuGestureLatched = false;
}

bool servicePaddleMenuControls(uint32_t now) {
  bool bothPressed = ditInput.pressed() && dahInput.pressed();

 
  if (uiMode == UI_GAME_WRITE &&
      writeGameState != WRITE_GAME_OVER) {
    resetDualPaddleMenuGesture();
    paddleComboLatched = false;
    return false;
  }

  if (uiMode == UI_GAME_WRITE &&
      writeGameState == WRITE_GAME_OVER) {
    // Straight key remains disabled on the final game screen.
    if (settings.keyMode == KEY_STRAIGHT) {
      resetDualPaddleMenuGesture();
      paddleComboLatched = false;
      return true;
    }

    uint8_t paddleEvent =
        serviceDualPaddleMenuGesture(now);

    if (paddleEvent != BUTTON_NONE) {
      handleButtonEvent(paddleEvent);
    }

    return true;
  }

  if (uiMode == UI_GAME1 &&
      game1State != GAME1_GAME_OVER) {
    resetDualPaddleMenuGesture();
    paddleComboLatched = false;
    return false;
  }

  if (uiMode == UI_GAME1 &&
      game1State == GAME1_GAME_OVER) {
    // In STRAIGHT mode the key is intentionally disabled on GAME OVER.
    if (settings.keyMode == KEY_STRAIGHT) {
      resetDualPaddleMenuGesture();
      paddleComboLatched = false;
      return true;
    }

    uint8_t paddleEvent = serviceDualPaddleMenuGesture(now);

    if (paddleEvent != BUTTON_NONE) {
      handleButtonEvent(paddleEvent);
    }

    return true;
  }


  if (settings.keyMode == KEY_STRAIGHT && uiMode != UI_MAIN) {
    paddleComboLatched = false;
    bothPaddlesStarted = 0;
    bothPaddlesMenuTriggered = false;
    resetDualPaddleMenuGesture();

    if (uiMode == UI_EASTER && !easterInputArmed) {
      bool allReleased =
        !menuButton.pressed() && !straightContactPressed();

      if (allReleased) {
        easterInputArmed = true;
      }
    }

    return true;
  }

  /*
    MAIN keeps the original CW behavior completely unchanged.
    Holding both paddles for the existing interval can still open MENU.
  */
  if (uiMode == UI_MAIN) {
    resetDualPaddleMenuGesture();

    if (settings.keyMode == KEY_STRAIGHT) {
      bothPaddlesStarted = 0;
      bothPaddlesMenuTriggered = false;
      return false;
    }

    if (bothPressed) {
      if (bothPaddlesStarted == 0) {
        bothPaddlesStarted = now;
      }

      if (!bothPaddlesMenuTriggered &&
          now - bothPaddlesStarted >= BOTH_PADDLES_MENU_MS) {
        bothPaddlesMenuTriggered = true;
        clearDecoder();
        enterHomeMenu();

        // The same squeeze that opened MENU must not immediately become
        // a Twice/back gesture. Ignore it until both paddles are released.
        pendingPaddleMenuAction = PADDLE_MENU_NONE;
        pendingPaddleMenuStarted = 0;
        paddleMenuGestureLatched = true;

        return true;
      }
    } else {
      bothPaddlesStarted = 0;
      bothPaddlesMenuTriggered = false;
    }

    return false;
  }

  /*
    GUIDE waits for a clean release after entering, just like before.
  */
  if (uiMode == UI_EASTER && !easterInputArmed) {
    bool allReleased =
      !menuButton.pressed() &&
      !ditInput.pressed() &&
      !dahInput.pressed();

    resetDualPaddleMenuGesture();

    if (allReleased) {
      easterInputArmed = true;
    }
    return true;
  }

  /*
    Every non-main dual-paddle screen now uses the same guarded gesture:
      dit only  -> BUTTON_SHORT
      dah only  -> BUTTON_LONG
      both      -> BUTTON_DOUBLE / Twice

    The 70 ms chord window prevents a slightly uneven squeeze from
    triggering one individual action before the second paddle arrives.
  */
  uint8_t paddleEvent = serviceDualPaddleMenuGesture(now);

  if (paddleEvent != BUTTON_NONE) {
    handleButtonEvent(paddleEvent);
  }

  return true;
}

// ============================ PANTALLA ================================

void printWrappedTail(const String& text,
                      int16_t startY,
                      uint8_t rows,
                      uint8_t columns) {
  size_t capacity = static_cast<size_t>(rows) * columns;
  size_t start = 0;

  if (text.length() > capacity) {
    start = text.length() - capacity;
  }

  uint8_t column = 0;
  uint8_t row = 0;

  display.setCursor(0, startY);

  for (size_t i = start; i < text.length() && row < rows; ++i) {
    char c = text.charAt(i);

    if (c == '\n') {
      row++;
      column = 0;
      display.setCursor(0, startY + row * 8);
      continue;
    }

    display.write(c);
    column++;

    if (column >= columns) {
      row++;
      column = 0;
      display.setCursor(0, startY + row * 8);
    }
  }
}

void drawGearIcon(int16_t cx, int16_t cy) {
  display.drawCircle(cx, cy, 4, SSD1306_WHITE);
  display.drawCircle(cx, cy, 1, SSD1306_WHITE);

  display.drawFastHLine(cx - 7, cy, 3, SSD1306_WHITE);
  display.drawFastHLine(cx + 5, cy, 3, SSD1306_WHITE);
  display.drawFastVLine(cx, cy - 7, 3, SSD1306_WHITE);
  display.drawFastVLine(cx, cy + 5, 3, SSD1306_WHITE);

  display.drawLine(cx - 5, cy - 5, cx - 3, cy - 3, SSD1306_WHITE);
  display.drawLine(cx + 3, cy + 3, cx + 5, cy + 5, SSD1306_WHITE);
  display.drawLine(cx + 3, cy - 3, cx + 5, cy - 5, SSD1306_WHITE);
  display.drawLine(cx - 5, cy + 5, cx - 3, cy + 3, SSD1306_WHITE);
}

void drawPaperIcon(int16_t x, int16_t y) {
  display.drawRect(x, y, 11, 14, SSD1306_WHITE);
  display.drawLine(x + 7, y, x + 10, y + 3, SSD1306_WHITE);
  display.drawLine(x + 7, y, x + 7, y + 3, SSD1306_WHITE);
  display.drawLine(x + 7, y + 3, x + 10, y + 3, SSD1306_WHITE);
  display.drawFastHLine(x + 2, y + 6, 6, SSD1306_WHITE);
  display.drawFastHLine(x + 2, y + 9, 6, SSD1306_WHITE);
}

void drawGamepadIcon(int16_t x, int16_t y) {
  display.drawRoundRect(x, y + 3, 14, 9, 3, SSD1306_WHITE);
  display.drawFastHLine(x + 3, y + 7, 5, SSD1306_WHITE);
  display.drawFastVLine(x + 5, y + 5, 5, SSD1306_WHITE);
  display.drawPixel(x + 10, y + 6, SSD1306_WHITE);
  display.drawPixel(x + 12, y + 8, SSD1306_WHITE);
}

void drawMainMenuHelp() {
  uint8_t helpPage =
      static_cast<uint8_t>(
          (millis() / SETTINGS_HELP_INTERVAL_MS) % 2UL);

  display.setCursor(0, 56);

  if (helpPage == 0) {
    display.print("Short:next Long:OK");
  } else {
    display.print("Twice:back Use paddle");
  }
}

void drawHomeMenuScreen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* title = "MENU";
  display.setCursor((SCREEN_WIDTH - strlen(title) * 6) / 2, 0);
  display.print(title);

  const char* labels[3] = { "Settings", "Guide", "Games" };

  for (uint8_t item = 0; item < 3; ++item) {
    int16_t y = 11 + item * 15;

    if (item == homeMenuIndex) {
      display.drawRoundRect(0, y - 1, SCREEN_WIDTH, 14, 2, SSD1306_WHITE);
    }

    if (item == 0) {
      drawGearIcon(11, y + 6);
    } else if (item == 1) {
      drawPaperIcon(6, y);
    } else {
      drawGamepadIcon(5, y);
    }

    display.setCursor(25, y + 2);
    display.print(labels[item]);
  }

 drawMainMenuHelp();
}

void drawGamesMenuScreen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* title = "GAMES:";
  display.setCursor(
      (SCREEN_WIDTH - static_cast<int16_t>(strlen(title) * 6)) / 2,
      0);
  display.print(title);

  display.setCursor(8, 19);
  display.print(gamesMenuIndex == 0 ? "> Sound" : "  Sound");

  display.setCursor(8, 34);
  display.print(gamesMenuIndex == 1 ? "> Write" : "  Write");

  // One-line alternating help, like MENU and SETTINGS.
  drawMainMenuHelp();
}

void drawGameDifficultyScreen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* title = "DIFFICULTY";
  display.setCursor(
      (SCREEN_WIDTH - static_cast<int16_t>(strlen(title) * 6)) / 2,
      0);
  display.print(title);
  display.drawFastHLine(0, 8, SCREEN_WIDTH, SSD1306_WHITE);

  const char* labels[3] = {"Easy", "Medium", "Hard"};

  for (uint8_t item = 0; item < 3; ++item) {
    int16_t y = 12 + item * 14;

    if (item == gameDifficultyIndex) {
      display.drawRoundRect(
          14, y - 2, 100, 13, 2, SSD1306_WHITE);
    }

    int16_t textWidth =
        static_cast<int16_t>(strlen(labels[item]) * 6);
    display.setCursor((SCREEN_WIDTH - textWidth) / 2, y);
    display.print(labels[item]);
  }

  drawMainMenuHelp();
}

void drawGameTypedAnswer() {
  // Correctly copied characters.
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);

  int16_t baseX = 4;
  int16_t y = 34;

  for (size_t i = 0; i < gameAccepted.length(); ++i) {
    display.setCursor(baseX + static_cast<int16_t>(i * 12), y);
    display.write(gameAccepted.charAt(i));
  }

  /*
    Wrong character: normal-size text and underlined for three seconds.
    Keep it at the position where the error was made.
  */
  if (gameWrongChar != '\0' &&
      gameWrongUntil != 0) {
    int16_t wrongX =
        baseX + static_cast<int16_t>(gameWrongIndex * 12);

    if (wrongX <= SCREEN_WIDTH - 6) {
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(wrongX, y + 5);
      display.write(gameWrongChar);
      display.drawFastHLine(
          wrongX,
          y + 14,
          6,
          SSD1306_WHITE);
    }
  }
}

void drawGame1Screen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* title = "SOUND";
  display.setCursor(
      (SCREEN_WIDTH - static_cast<int16_t>(strlen(title) * 6)) / 2,
      0);
  display.print(title);

  display.drawFastHLine(0, 8, SCREEN_WIDTH, SSD1306_WHITE);

  display.setCursor(0, 11);
  display.print("WPM:");
  display.print(gameWpm);

  display.setCursor(70, 11);
  display.print("Lives:");
  display.print(gameLives);

  if (game1State == GAME1_PLAYBACK) {
    const char* listenText = "LISTEN...";
    display.setCursor(
        (SCREEN_WIDTH -
         static_cast<int16_t>(strlen(listenText) * 6)) / 2,
        30);
    display.print(listenText);

    // During LISTEN only double-click is valid.
    display.setTextSize(1);
    const char* backText = "Twice:go back";
    display.setCursor(
        (SCREEN_WIDTH -
         static_cast<int16_t>(strlen(backText) * 6)) / 2,
        56);
    display.print(backText);
    return;
  }

  if (game1State == GAME1_CORRECT_PAUSE) {
    const char* correctText = "CORRECT!";
    display.setTextSize(2);
    display.setCursor(
        (SCREEN_WIDTH -
         static_cast<int16_t>(strlen(correctText) * 12)) / 2,
        28);
    display.print(correctText);
    return;
  }

  if (game1State == GAME1_GAME_OVER) {
    const char* overText = "GAME OVER";
    display.setTextSize(2);
    display.setCursor(
        (SCREEN_WIDTH -
         static_cast<int16_t>(strlen(overText) * 12)) / 2,
        22);
    display.print(overText);

    display.setTextSize(1);
    String reached =
        String("Reached: ") + String(gameMaxWpm) + " WPM";
    display.setCursor(
        (SCREEN_WIDTH -
         static_cast<int16_t>(reached.length() * 6)) / 2,
        43);
    display.print(reached);

    display.setCursor(20, 56);
    display.print("Twice:go back");
    return;
  }

  // Waiting for the player to key the answer.
  display.setTextSize(1);

  if (gameWrongChar != '\0' &&
      gameWrongUntil != 0) {
    uint32_t remainingMs =
        static_cast<int32_t>(gameWrongUntil - millis()) > 0
            ? gameWrongUntil - millis()
            : 1UL;

    uint8_t countdown =
        static_cast<uint8_t>((remainingMs + 999UL) / 1000UL);
    countdown = constrain(countdown, 1, 3);

    display.setCursor(0, 23);
    display.print("Wrong letter ");
    display.print(countdown);
  }

  drawGameTypedAnswer();

  uint8_t helpPage =
      static_cast<uint8_t>(
          (millis() / SETTINGS_HELP_INTERVAL_MS) % 2UL);

  display.setTextSize(1);
  display.setCursor(0, 56);
  if (helpPage == 0) {
    display.print("Short:listen again");
  } else {
    display.print("Twice:go back");
  }
}

void drawWriteDifficultyScreen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* title = "WRITE DIFFICULTY";
  int16_t titleWidth = static_cast<int16_t>(strlen(title) * 6);
  display.setCursor(max<int16_t>(0, (SCREEN_WIDTH - titleWidth) / 2), 0);
  display.print(title);

  const char* labels[3] = {"Easy", "Medium", "Hard"};
  for (uint8_t item = 0; item < 3; ++item) {
    int16_t y = 13 + item * 14;
    if (item == writeDifficultyIndex) {
      display.drawRoundRect(12, y - 2, 104, 13, 2, SSD1306_WHITE);
    }
    int16_t width = static_cast<int16_t>(strlen(labels[item]) * 6);
    display.setCursor((SCREEN_WIDTH - width) / 2, y);
    display.print(labels[item]);
  }
  drawMainMenuHelp();
}

void drawWriteGameScreen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* title = "WRITE";
  display.setCursor((SCREEN_WIDTH - static_cast<int16_t>(strlen(title) * 6)) / 2, 0);
  display.print(title);

  if (writeGameState == WRITE_GAME_OVER) {
    const char* overText = "TIME OUT";
    display.setTextSize(2);
    display.setCursor((SCREEN_WIDTH - static_cast<int16_t>(strlen(overText) * 12)) / 2, 19);
    display.print(overText);

    display.setTextSize(1);
    String reached = String("Reached: ") + String(writeReachedSeconds) + "s";
    display.setCursor((SCREEN_WIDTH - static_cast<int16_t>(reached.length() * 6)) / 2, 42);
    display.print(reached);

    const char* backText = "Twice:go back";
    display.setCursor((SCREEN_WIDTH - static_cast<int16_t>(strlen(backText) * 6)) / 2, 56);
    display.print(backText);
    return;
  }

  uint32_t now = millis();
  uint32_t remaining = writeCurrentRemainingMs(now);

  display.setTextSize(1);
  display.setCursor(0, 10);
  display.print("Time:");
  display.print(remaining / 1000UL);
  display.print(".");
  display.print((remaining % 1000UL) / 100UL);
  display.print("s");

  display.setCursor(84, 10);
  display.print(settings.wpm);
  display.print("W");

  display.setTextSize(2);
  int16_t targetWidth = static_cast<int16_t>(writeTarget.length() * 12);
  int16_t targetX = max<int16_t>(0, (SCREEN_WIDTH - targetWidth) / 2);
  display.setCursor(targetX, 22);
  display.print(writeTarget);

  display.setTextSize(1);
  if (writeGameState == WRITE_PENALTY) {
    uint32_t penaltyRemaining =
        static_cast<int32_t>(writePenaltyUntil - now) > 0
            ? writePenaltyUntil - now : 1UL;
    uint8_t countdown =
        static_cast<uint8_t>((penaltyRemaining + 999UL) / 1000UL);
    countdown = constrain(countdown, 1, 3);

    // Example: "Wrong C Need A 3"
    String wrongText =
        String("Wrong ") +
        String(writeWrongChar) +
        " Need " +
        String(writeExpectedChar) +
        " " +
        String(countdown);

    display.setCursor(
        max<int16_t>(
            0,
            (SCREEN_WIDTH -
             static_cast<int16_t>(wrongText.length() * 6)) / 2),
        44);
    display.print(wrongText);
  } else if (writeGameState == WRITE_WAIT_RELEASE) {
    if (!writeKeyFullyReleased()) {
      const char* releaseText = "Release the key";
      display.setCursor((SCREEN_WIDTH - static_cast<int16_t>(strlen(releaseText) * 6)) / 2, 44);
      display.print(releaseText);
    }
  } else if (writeGameState == WRITE_CORRECT_PAUSE) {
    const char* correctText = "CORRECT!";
    display.setCursor((SCREEN_WIDTH - static_cast<int16_t>(strlen(correctText) * 6)) / 2, 44);
    display.print(correctText);
  } else {
    String typed = String("> ") + writeAccepted;
    display.setCursor(0, 44);
    display.print(typed);
  }

  const char* backText = "Twice:go back";
  display.setCursor((SCREEN_WIDTH - static_cast<int16_t>(strlen(backText) * 6)) / 2, 56);
  display.print(backText);
}

void drawMainScreen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(settings.wpm);
  display.print("W ");
  display.print(ratioText(settings.dahRatio10));
  display.print(":1 ");
  display.print(settings.toneHz);
  display.print("Hz ");
  display.print(keyModeTopLabel(settings.keyMode));

  // Separador fino bajo los ajustes actuales.
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  display.setCursor(0, 16);
  display.print("CW: ");
  if (currentMorse.isEmpty()) {
    display.print("-");
  } else {
    display.print(currentMorse);
  }

  printWrappedTail(decodedText, 24, 5, 21);
}

void drawSettingsHelpAt(int16_t xOffset) {
  uint8_t helpPage =
    static_cast<uint8_t>(
      (millis() / SETTINGS_HELP_INTERVAL_MS) % 2UL);

  display.setCursor(xOffset, 56);

  if (helpPage == 0) {
    display.print("Short:next Long:OK");
  } else {
    display.print("Twice:back Use paddle");
  }
}

String menuValueText(uint8_t item) {
  switch (item) {
    case 0:
      return String(settings.wpm) + " WPM";
    case 1:
      return ratioText(settings.dahRatio10) + ":1";
    case 2:
      return String(settings.toneHz) + "Hz";
    case 3:
      return String(keyModeName(settings.keyMode));
    case 4:
      return String(outputModeName(settings.outputMode));
    case 5:
      return settings.invertDisplay ? "ON" : "OFF";
    default:
      return String(settings.brightness) + "%";
  }
}

void drawMenuPage(uint8_t firstItem,
                  uint8_t selectedItem,
                  int16_t xOffset) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  const char* title = "SETTINGS:";
  int16_t titleX =
      xOffset +
      (SCREEN_WIDTH - static_cast<int16_t>(strlen(title) * 6)) / 2;
  display.setCursor(titleX, 0);
  display.print(title);

  for (uint8_t row = 0; row < MENU_VISIBLE_ITEMS; ++row) {
    uint8_t item = firstItem + row;
    if (item >= MENU_ITEM_COUNT) {
      break;
    }

    int16_t y = 8 + row * 9;
    String value = menuValueText(item);

    display.setCursor(xOffset, y);
    display.print(menuItemName(item));

    int16_t valueX =
      xOffset + SCREEN_WIDTH - static_cast<int16_t>(value.length() * 6);

    display.setCursor(valueX, y);
    display.print(value);

    if (item == selectedItem) {
      display.drawFastHLine(
        valueX,
        y + 8,
        value.length() * 6,
        SSD1306_WHITE);
    }
  }

  drawSettingsHelpAt(xOffset);
}

uint8_t currentMenuFirstItem() {
 
  //Keep five rows visible
  
  if (menuIndex < MENU_VISIBLE_ITEMS) {
    return 0;
  }

  return menuIndex - (MENU_VISIBLE_ITEMS - 1);
}

void drawMenuScreen() {
  drawMenuPage(currentMenuFirstItem(), menuIndex, 0);
}

void advanceMenuSelection() {
  menuIndex = (menuIndex + 1) % MENU_ITEM_COUNT;
  screenDirty = true;
}

void drawEditScreen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("EDIT ");
  display.print(menuItemName(menuIndex));

  int16_t underlineWidth = 92;

  if (menuIndex == 3) {
    // Key-mode names are long, so use the small font.
    String keyText = keyModeFullName(settings.keyMode);
    int16_t valueX = max<int16_t>(
      0,
      (SCREEN_WIDTH - static_cast<int16_t>(keyText.length() * 6)) / 2);

    display.setTextSize(1);
    display.setCursor(valueX, 22);
    display.print(keyText);
    underlineWidth = min<int16_t>(
      SCREEN_WIDTH,
      static_cast<int16_t>(keyText.length() * 6));
    display.drawFastHLine(valueX, 32, underlineWidth, SSD1306_WHITE);
  } else {
    display.setTextSize(2);
    display.setCursor(0, 18);

    switch (menuIndex) {
      case 0:
        display.print(settings.wpm);
        display.print(" WPM");
        break;
      case 1:
        display.print(ratioText(settings.dahRatio10));
        display.print(":1");
        break;
      case 2:
        display.print(settings.toneHz);
        display.print(" Hz");
        break;
      case 4:
        display.print(outputModeName(settings.outputMode));
        break;
      case 5:
        display.print(settings.invertDisplay ? "ON" : "OFF");
        break;
      case 6:
        display.print(settings.brightness);
        display.print("%");
        break;
      default:
        break;
    }

    display.drawFastHLine(0, 39, underlineWidth, SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setCursor(0, 48);
  display.print("Short: change");

  display.setCursor(0, 56);
  display.print("Twice: Save & Exit");
}

void drawEasterEggScreen() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  uint8_t dataPages = guideDataPageCount();

  // Final thank-you page.
  if (guidePage >= dataPages) {
    const char* line1 = "Thanks for";
    const char* line2 = "using me :3";

    display.setCursor(
        (SCREEN_WIDTH - static_cast<int16_t>(strlen(line1) * 6)) / 2,
        12);
    display.print(line1);

    display.setCursor(
        (SCREEN_WIDTH - static_cast<int16_t>(strlen(line2) * 6)) / 2,
        25);
    display.print(line2);

    // Heart.
    display.fillCircle(60, 40, 3, SSD1306_WHITE);
    display.fillCircle(67, 40, 3, SSD1306_WHITE);
    display.fillTriangle(57, 41, 70, 41, 63, 50, SSD1306_WHITE);

    // Back help.
    const char* backText = "Twice: go back";
    display.setCursor(
        (SCREEN_WIDTH - static_cast<int16_t>(strlen(backText) * 6)) / 2,
        56);
    display.print(backText);

    return;
  }

  // Normal CW GUIDE pages.
  String guideTitle =
      String("CW GUIDE ") +
      String(guidePage + 1) +
      "/" +
      String(dataPages);

  int16_t guideTitleX =
      (SCREEN_WIDTH -
       static_cast<int16_t>(guideTitle.length() * 6)) / 2;

  display.setCursor(guideTitleX, 0);
  display.print(guideTitle);

  size_t firstEntry =
      static_cast<size_t>(guidePage) * GUIDE_ROWS_PER_PAGE;

  for (uint8_t row = 0; row < GUIDE_ROWS_PER_PAGE; ++row) {
    size_t entryIndex = firstEntry + row;

    if (entryIndex >= MORSE_ENTRY_COUNT) {
      break;
    }

    const MorseEntry& entry = MORSE_TABLE[entryIndex];
    int16_t y = 9 + row * 9;

    display.setCursor(4, y);
    display.print(entry.character);

    display.setCursor(22, y);
    display.print(entry.code);
  }
}

void serviceDisplay(uint32_t now) {
 if (uiMode == UI_MENU ||
    uiMode == UI_HOME_MENU ||
    uiMode == UI_GAMES ||
    uiMode == UI_GAME_DIFFICULTY ||
    uiMode == UI_GAME1 ||
    uiMode == UI_WRITE_DIFFICULTY ||
    uiMode == UI_GAME_WRITE) {
    uint8_t currentHelpPage =
      static_cast<uint8_t>(
        (now / SETTINGS_HELP_INTERVAL_MS) % 2UL);

    if (currentHelpPage != lastSettingsHelpPage) {
      lastSettingsHelpPage = currentHelpPage;
      screenDirty = true;
    }
  } else {
    lastSettingsHelpPage = 255;
  }

  if (!oledAvailable || !screenDirty) {
    return;
  }

  if (now - lastScreenDraw < 30) {
    return;
  }

  display.clearDisplay();

  switch (uiMode) {
    case UI_MAIN:
      drawMainScreen();
      break;
    case UI_HOME_MENU:
      drawHomeMenuScreen();
      break;
    case UI_MENU:
      drawMenuScreen();
      break;
    case UI_EDIT:
      drawEditScreen();
      break;
    case UI_EASTER:
      drawEasterEggScreen();
      break;
    case UI_GAMES:
      drawGamesMenuScreen();
      break;
    case UI_GAME_DIFFICULTY:
      drawGameDifficultyScreen();
      break;
    case UI_GAME1:
      drawGame1Screen();
      break;
    case UI_WRITE_DIFFICULTY:
      drawWriteDifficultyScreen();
      break;
    case UI_GAME_WRITE:
      drawWriteGameScreen();
      break;
  }

  display.display();
  lastScreenDraw = now;
  screenDirty = false;
}

// =========================== SETUP and LOOP ===============================

void setup() {
  // Lower CPU frequency to reduce power consumption.
  setCpuFrequencyMhz(80);

  Serial.begin(115200);

  loadSettings();

  ditInput.begin(DIT_PIN);
  dahInput.begin(DAH_PIN);
  menuButton.begin(MENU_BUTTON_PIN);

  initializeCwLed();
  initializeBuzzer();
  setCwOutputs(false);

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  oledAvailable =
    display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);

  if (!oledAvailable) {
    Serial.println("SSD1306 OLED not found.");
  } else {
    applyDisplayRotation();
    applyDisplayBrightness();

    // Show the startup identification as Morse text only.
    // It does not produce sound or light.
    drawStartupMorseText("CW POCKET TRAINER BY EA1JVS");
    delay(2200);

    display.clearDisplay();
    display.display();
    screenDirty = true;
  }

  Serial.println();
  Serial.println("CW Pocket Trainer started.");
  Serial.print("CPU frequency: ");
  Serial.print(getCpuFrequencyMhz());
  Serial.println(" MHz");
  Serial.println("Startup ID shown as Morse text only.");
  Serial.println("Jack: TIP=GPIO0, RING=GPIO1, SLEEVE=GND.");
  Serial.println("OLED: SDA=GPIO10, SCL=GPIO20.");
  Serial.println("Buzzer: GPIO21.");
  Serial.println("Speeds: 10/15/20/25/30/35/40 WPM.");
  Serial.println("Character gap: 3 units + 80 ms.");
  Serial.println("Key modes: TIP=./RING=-, TIP=-/RING=., STRAIGHT.");
  Serial.print("CW LEDs: GPIO8, 1 kHz PWM, duty=");
  Serial.print(CW_LED_PWM_DUTY);
  Serial.print("/255 (~");
  Serial.print((static_cast<uint16_t>(CW_LED_PWM_DUTY) * 100U) / 255U);
  Serial.println("%).");
  Serial.println("BOOT opens MENU from CW; Short=next, Long=select, Double=back.");
  Serial.println("MENU: Settings / Guide / Games.");
  Serial.println("Games: Sound / Write.");
  Serial.println("Write difficulty: Easy / Medium / Hard.");
  Serial.println("Both paddles in menus: same action as Twice.");
  Serial.println("Display invert: OFF/ON, stored in NVS.");
  Serial.print("OLED brightness: ");
  Serial.print(settings.brightness);
  Serial.println("%, stored in NVS.");
  Serial.println("STRAIGHT key controls menus like BOOT once inside a menu.");
}

void loop() {
  uint32_t now = millis();

  ditInput.update(now);
  dahInput.update(now);

  if (ditInput.pressedEdge() || dahInput.pressedEdge()) {
    lastKeyActivity = now;
  }

  uint8_t buttonEvent =
    static_cast<uint8_t>(menuButton.update(now));

  handleButtonEvent(buttonEvent);

  /*
    In STRAIGHT mode the key remains a real CW key on UI_MAIN.
    Once inside a menu or GUIDE it behaves like the BOOT button:
    short, long and double.
  */
  uint8_t straightButtonEvent =
    serviceStraightMenuButton(now);

  if (buttonEvent == BUTTON_NONE && straightButtonEvent != BUTTON_NONE) {
    handleButtonEvent(straightButtonEvent);
  }

  bool paddleUsedForMenu =
    servicePaddleMenuControls(now);

  // Progress game state machines.
  serviceGame1(now);
  serviceWriteGame(now);

  bool gameTypingEnabled = isGame1TypingEnabled();
  bool writeTypingEnabled = isWriteTypingEnabled();

  bool keyingEnabled =
      ((uiMode == UI_MAIN) || gameTypingEnabled || writeTypingEnabled) &&
      !paddleUsedForMenu;

  serviceKeyer(now, keyingEnabled);

  if (uiMode == UI_MAIN && keyingEnabled) {
    serviceDecoder(now);
    serviceAutoClear(now);
  } else if (gameTypingEnabled && keyingEnabled) {
    serviceDecoder(now);
  } else if (writeTypingEnabled && keyingEnabled) {
    serviceDecoder(now);
  }

  serviceDisplay(now);
}