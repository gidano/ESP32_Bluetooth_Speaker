/*
  ESP32 Bluetooth A2DP Speaker + ST7735 160x128 Display
  Styled UI version with a nicer header inspired by the user's reference.

  Hardware:
  - Classic ESP32 DevKit
  - 8-pin ST7735 160x128 SPI display
  - WeAct I2S Speaker Module PCM5100A 2x3W
    or a simple PCM5102A / DAC5102 I2S DAC

  Libraries:
  - ESP32-A2DP by Phil Schatzmann
  - Adafruit GFX Library
  - Adafruit ST7735 and ST7789 Library

  Recommended:
  - ESP32 Arduino Core 2.0.17
*/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include "BluetoothA2DPSink.h"
#include "esp_avrc_api.h"

// =======================================================
// ST7735 8-pin display wiring
// =======================================================
#define TFT_SCK   18
#define TFT_MOSI  23
#define TFT_CS    17
#define TFT_DC    16
#define TFT_RST   4
#define TFT_BL    21

// =======================================================
// I2S audio wiring
// =======================================================
#define I2S_BCLK  26
#define I2S_LRCK  25
#define I2S_DATA  22

// =======================================================
// Color helpers
// =======================================================
#define UI_RED        0xE8A2
#define UI_DARKGREY   0x4208
#define UI_MIDGREY    0x6B4D
#define UI_BLUE       0x041F
#define UI_LIGHTBLUE  0x4D7F
#define UI_CYAN2      0x2E9F
#define UI_YELLOW2    0xFE40
#define UI_GREEN2     0x07E0
#define UI_ORANGE2    0xFD20

Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);
BluetoothA2DPSink a2dp_sink;

// =======================================================
// UI state
// =======================================================
String btName = "ESP32 BT Speaker";
String artistText = "-";
String titleText  = "-";
String albumText  = "-";
String playState  = "PAIR";

bool connected = false;
int volumePercent = 0;

int btRssiDelta = 0;
int btSignalBars = 0;
bool btRssiValid = false;
String btSignalText = "WAIT";
unsigned long lastRssiRead = 0;

bool needRedraw = true;
bool needSignalRedraw = false;
unsigned long lastDraw = 0;
unsigned long lastSignalDraw = 0;

// =======================================================
// Helpers
// =======================================================
String fitText(String s, int maxChars) {
  if (s.length() <= maxChars) return s;
  if (maxChars <= 3) return s.substring(0, maxChars);
  return s.substring(0, maxChars - 3) + "...";
}

void requestRedraw() {
  needRedraw = true;
}

void requestSignalRedraw() {
  needSignalRedraw = true;
}

uint16_t stateColor() {
  if (playState == "PLAY") return UI_GREEN2;
  if (playState == "PAUSE") return UI_YELLOW2;
  if (playState == "STOP") return ST77XX_RED;
  return UI_ORANGE2;
}

uint16_t signalColor() {
  if (!connected || !btRssiValid) return UI_YELLOW2;
  if (btSignalBars >= 2) return UI_GREEN2;
  if (btSignalBars == 1) return UI_YELLOW2;
  return ST77XX_RED;
}

// =======================================================
// Drawing functions
// =======================================================
void drawTopBadge() {
  // Header group is horizontally centered on the 160px display:
  // ESP badge 48px + gap 6px + BT icon 16px + gap 6px + SPEAKER 42px = 118px
  // (160 - 118) / 2 = 21px
  int x = 21;
  int y = 4;
  int h = 18;
  int leftW = 26;
  int rightW = 22;

  tft.drawRoundRect(x, y, leftW + rightW, h, 3, UI_MIDGREY);
  tft.fillRoundRect(x, y, leftW + rightW, h, 3, UI_DARKGREY);
  tft.fillRoundRect(x, y, leftW, h, 3, UI_RED);
  tft.fillRect(x + leftW - 3, y, 3, h, UI_RED);

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(x + 4, y + 5);
  tft.print("ESP");

  tft.setCursor(x + leftW + 4, y + 5);
  tft.print("32");
}

void drawBtIcon(int x, int y) {
  tft.drawRoundRect(x, y, 16, 16, 3, UI_LIGHTBLUE);
  tft.fillRoundRect(x, y, 16, 16, 3, UI_BLUE);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(x + 3, y + 5);
  tft.print("BT");
}

void drawHeader() {
  tft.fillRect(0, 0, 160, 28, ST77XX_BLACK);

  // Centered header layout:
  // ESP badge x=21, BT icon x=75, SPEAKER x=97
  drawTopBadge();
  drawBtIcon(75, 5);

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(UI_YELLOW2);
  tft.setCursor(97, 9);
  tft.print("SPEAKER");
}

void drawSectionLabels() {
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(4, 32);
  tft.print("STATUS:");
  tft.drawFastHLine(4, 42, 64, UI_LIGHTBLUE);

  tft.setCursor(84, 32);
  tft.print("AUDIO:");
  tft.drawFastHLine(84, 42, 72, UI_LIGHTBLUE);

  tft.setCursor(4, 73);
  tft.print("SONG:");
  tft.drawFastHLine(4, 83, 152, UI_LIGHTBLUE);
}

void drawPlayStateValue() {
  tft.fillRect(4, 46, 72, 16, ST77XX_BLACK);

  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(stateColor());
  tft.setCursor(4, 47);

  if (!connected) {
    tft.print("PAIR");
  } else {
    tft.print(playState);
  }
}

void drawAudioPanelValue() {
  tft.fillRect(84, 45, 72, 23, ST77XX_BLACK);

  tft.setTextWrap(false);
  tft.setTextSize(1);

  // BT bars / quality line
  tft.setTextColor(signalColor());
  tft.setCursor(84, 46);
  if (!connected) {
    tft.print("BT: ---");
  } else if (!btRssiValid) {
    tft.print("BT: ?/3");
  } else {
    tft.print("BT:");
    tft.print(btSignalBars);
    tft.print("/3");
  }

  // signal text line
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(84, 58);
  if (!connected) {
    tft.print("No Link");
  } else if (!btRssiValid) {
    tft.print("WAIT");
  } else {
    tft.print(btSignalText);
    tft.print(" ");
    tft.print(btRssiDelta);
  }
}

void drawSongInfo() {
  tft.fillRect(4, 87, 152, 28, ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(1);

  bool haveTrackInfo = connected && ((artistText != "-") || (titleText != "-"));

  if (!haveTrackInfo) {
    tft.setTextColor(UI_CYAN2);
    tft.setCursor(4, 88);
    tft.print("Artist: ");
    tft.setTextColor(ST77XX_WHITE);
    tft.print(artistText);

    tft.setTextColor(UI_CYAN2);
    tft.setCursor(4, 101);
    tft.print("Title : ");
    tft.setTextColor(ST77XX_WHITE);
    tft.print(titleText);
  } else {
    tft.setTextColor(UI_CYAN2);
    tft.setCursor(4, 89);
    tft.print(fitText(artistText, 24));

    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(4, 102);
    tft.print(fitText(titleText, 24));
  }
}

void drawVolumeBar() {
  int barX = 4;
  int barY = 121;
  int barW = 120;
  int barH = 6;

  int fillW = map(volumePercent, 0, 100, 0, barW - 2);
  fillW = constrain(fillW, 0, barW - 2);

  tft.fillRect(barX, barY, 156, 7, ST77XX_BLACK);
  tft.drawRect(barX, barY, barW, barH, UI_LIGHTBLUE);
  tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, ST77XX_BLACK);
  if (fillW > 0) {
    tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, UI_LIGHTBLUE);
  }

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(128, 120);
  tft.print(volumePercent);
  tft.print("%");
}

void drawFrameDecor() {
  tft.drawFastHLine(0, 27, 160, UI_DARKGREY);
}

void drawSignalPartial() {
  drawAudioPanelValue();
}

void drawScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextWrap(false);

  drawHeader();
  drawFrameDecor();
  drawSectionLabels();
  drawPlayStateValue();
  drawAudioPanelValue();
  drawSongInfo();
  drawVolumeBar();
}

// =======================================================
// Bluetooth callbacks
// =======================================================
void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  if (!text) return;

  String value = String((const char *)text);

  switch (id) {
    case ESP_AVRC_MD_ATTR_TITLE:
      titleText = value;
      break;
    case ESP_AVRC_MD_ATTR_ARTIST:
      artistText = value;
      break;
    case ESP_AVRC_MD_ATTR_ALBUM:
      albumText = value;
      break;
    default:
      break;
  }

  requestRedraw();
}

void avrc_rn_playstatus_callback(esp_avrc_playback_stat_t playback) {
  switch (playback) {
    case ESP_AVRC_PLAYBACK_PLAYING:
      playState = "PLAY";
      break;
    case ESP_AVRC_PLAYBACK_PAUSED:
      playState = "PAUSE";
      break;
    case ESP_AVRC_PLAYBACK_STOPPED:
      playState = "STOP";
      break;
    default:
      playState = connected ? "READY" : "PAIR";
      break;
  }

  requestRedraw();
}

void connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
  connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);

  if (connected) {
    playState = "READY";
    btRssiValid = false;
    btSignalText = "WAIT";
    btSignalBars = 0;
    lastRssiRead = 0;
  } else {
    playState = "PAIR";
    artistText = "-";
    titleText = "-";
    albumText = "-";
    btRssiValid = false;
    btSignalText = "---";
    btSignalBars = 0;
    btRssiDelta = 0;
  }

  requestRedraw();
}

void volume_changed(int volume) {
  volumePercent = map(volume, 0, 127, 0, 100);
  volumePercent = constrain(volumePercent, 0, 100);
  requestRedraw();
}

void rssi_callback(esp_bt_gap_cb_param_t::read_rssi_delta_param &rssi) {
  int oldDelta = btRssiDelta;
  int oldBars = btSignalBars;
  bool oldValid = btRssiValid;
  String oldText = btSignalText;

  if (rssi.stat != ESP_BT_STATUS_SUCCESS) {
    btRssiValid = false;
    btSignalText = "ERR";
    btSignalBars = 0;
    Serial.println("BT RSSI update failed");

    if (oldValid != btRssiValid || oldText != btSignalText || oldBars != btSignalBars) {
      requestSignalRedraw();
    }
    return;
  }

  btRssiDelta = rssi.rssi_delta;
  btRssiValid = true;

  if (btRssiDelta >= -3) {
    btSignalBars = 3;
    btSignalText = "GOOD";
  } else if (btRssiDelta >= -10) {
    btSignalBars = 2;
    btSignalText = "FAIR";
  } else if (btRssiDelta >= -18) {
    btSignalBars = 1;
    btSignalText = "WEAK";
  } else {
    btSignalBars = 0;
    btSignalText = "BAD";
  }

  Serial.print("BT RSSI delta: ");
  Serial.println(btRssiDelta);

  if (oldDelta != btRssiDelta || oldBars != btSignalBars || oldValid != btRssiValid || oldText != btSignalText) {
    requestSignalRedraw();
  }
}

// =======================================================
// Setup
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("ESP32 Bluetooth A2DP Speaker starting...");

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  drawScreen();

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRCK,
    .data_out_num = I2S_DATA,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  a2dp_sink.set_pin_config(pin_config);
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_avrc_rn_playstatus_callback(avrc_rn_playstatus_callback);
  a2dp_sink.set_on_connection_state_changed(connection_state_changed);
  a2dp_sink.set_on_volumechange(volume_changed);
  a2dp_sink.set_rssi_callback(rssi_callback);
  a2dp_sink.set_rssi_active(true);
  a2dp_sink.start(btName.c_str());

  Serial.print("Bluetooth device name: ");
  Serial.println(btName);

  playState = "PAIR";
  requestRedraw();
}

// =======================================================
// Loop
// =======================================================
void loop() {
  if (connected && millis() - lastRssiRead > 3000) {
    lastRssiRead = millis();
    bool requested = a2dp_sink.update_rssi();
    if (!requested) {
      Serial.println("BT RSSI request not accepted yet");
    }
  }

  if (needRedraw && millis() - lastDraw > 150) {
    needRedraw = false;
    needSignalRedraw = false;
    lastDraw = millis();
    drawScreen();
  } else if (needSignalRedraw && millis() - lastSignalDraw > 100) {
    needSignalRedraw = false;
    lastSignalDraw = millis();
    drawSignalPartial();
  }
}
