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
// PWM háttérvilágítás
// =======================================================
#define BL_PWM_CHANNEL  0
#define BL_PWM_FREQ     5000
#define BL_PWM_RES      8         // 8 bit = 0..255
#define BL_BRIGHT_FULL  255
#define BL_BRIGHT_DIM   51        // ~20%
#define SCREEN_DIM_MS   60000UL   // 1 perc

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

// =======================================================
// Scroll konstansok
// =======================================================
#define SCROLL_AREA_X     4
#define SCROLL_AREA_W   152
#define CHAR_W            6    // textSize(1) = 6x8px
#define SCROLL_SPEED_MS   30   // ms / pixel lépés
#define SCROLL_IDLE_MS 10000   // pihenő az elejénél (10 mp)

// =======================================================
// Scroll struct — KÖTELEZŐ a függvények előtt szerepelnie
// =======================================================
enum ScrollPhase { SCROLL_IDLE, SCROLL_RUN };

struct ScrollRow {
  String text;        // görgetéshez: loopUnit + eredeti szöveg
  String origText;    // csak az eredeti szöveg (rövid szöveg kiírásához)
  int    pixelOffset;
  int    origPixelW;  // eredeti szöveg szélessége (scroll döntéshez)
  int    totalPixelW; // egy ciklus szélessége (loopUnit)
  ScrollPhase phase;
  unsigned long phaseStart;
};

// =======================================================
// Globális objektumok
// =======================================================
Adafruit_ST7735 tft = Adafruit_ST7735(&SPI, TFT_CS, TFT_DC, TFT_RST);
BluetoothA2DPSink a2dp_sink;

// =======================================================
// UI state
// =======================================================
String btName     = "ESP32 BT Speaker";
String artistText = "-";
String titleText  = "-";
String albumText  = "-";
String playState  = "PAIR";

bool connected    = false;
int  volumePercent = 0;

int    btRssiDelta   = 0;
int    btSignalBars  = 0;
bool   btRssiValid   = false;
String btSignalText  = "WAIT";
unsigned long lastRssiRead = 0;

bool needRedraw      = true;
bool needSignalRedraw = false;
unsigned long lastDraw       = 0;
unsigned long lastSignalDraw = 0;

// PC/Windows AVRC polling
unsigned long connectedSince       = 0;
bool          initialMetaRequested = false;

// Háttérvilágítás
bool          screenDimmed   = false;
unsigned long lastActivityMs = 0;

// Scroll state
ScrollRow     scrollArtist;
ScrollRow     scrollTitle;
int           scrollActiveRow = 0;
unsigned long lastScrollStep  = 0;

// =======================================================
// Háttérvilágítás
// =======================================================
void wakeScreen() {
  lastActivityMs = millis();
  if (screenDimmed) {
    screenDimmed = false;
    ledcWrite(BL_PWM_CHANNEL, BL_BRIGHT_FULL);
  }
}

void dimScreen() {
  if (!screenDimmed) {
    screenDimmed = true;
    ledcWrite(BL_PWM_CHANNEL, BL_BRIGHT_DIM);
  }
}

// =======================================================
// Scroll init
// =======================================================
void initScrollRow(ScrollRow &row, const String &text) {
  row.origText    = text;
  row.origPixelW  = text.length() * CHAR_W;
  if (row.origPixelW > SCROLL_AREA_W) {
    // Hosszú szöveg: körkörös scroll, loopUnit = "szöveg   *   "
    String loopUnit = text + "   *   ";
    row.totalPixelW = loopUnit.length() * CHAR_W;
    row.text        = loopUnit + text;  // két példány a folyamatos rajzoláshoz
  } else {
    // Rövid szöveg: nem scrolloz, text és totalPixelW nem számít
    row.text        = text;
    row.totalPixelW = row.origPixelW;
  }
  row.pixelOffset = 0;
  row.phase       = SCROLL_IDLE;
  row.phaseStart  = millis();
}

void resetScroll() {
  initScrollRow(scrollArtist, artistText);
  initScrollRow(scrollTitle,  titleText);
  scrollActiveRow = 0;
  lastScrollStep  = millis();
}

// =======================================================
// Helpers
// =======================================================
String fitText(String s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  if (maxChars <= 3) return s.substring(0, maxChars);
  return s.substring(0, maxChars - 3) + "...";
}

void requestRedraw()       { needRedraw = true; }
void requestSignalRedraw() { needSignalRedraw = true; }

uint16_t stateColor() {
  if (playState == "PLAY")  return UI_GREEN2;
  if (playState == "PAUSE") return UI_YELLOW2;
  if (playState == "STOP")  return ST77XX_RED;
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
  int x = 21, y = 4, h = 18, leftW = 26, rightW = 22;
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
  tft.setCursor(4, 32);  tft.print("STATUS:");
  tft.drawFastHLine(4, 42, 64, UI_LIGHTBLUE);
  tft.setCursor(84, 32); tft.print("AUDIO:");
  tft.drawFastHLine(84, 42, 72, UI_LIGHTBLUE);
  tft.setCursor(4, 73);  tft.print("SONG:");
  tft.drawFastHLine(4, 83, 152, UI_LIGHTBLUE);
}

void drawPlayStateValue() {
  tft.fillRect(4, 46, 72, 16, ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(stateColor());
  tft.setCursor(4, 47);
  if (!connected) tft.print("PAIR");
  else            tft.print(playState);
}

void drawAudioPanelValue() {
  tft.fillRect(84, 45, 72, 23, ST77XX_BLACK);
  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(signalColor());
  tft.setCursor(84, 46);
  if (!connected)        tft.print("BT: ---");
  else if (!btRssiValid) tft.print("BT: ?/3");
  else { tft.print("BT:"); tft.print(btSignalBars); tft.print("/3"); }
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(84, 58);
  if (!connected)        tft.print("No Link");
  else if (!btRssiValid) tft.print("WAIT");
  else { tft.print(btSignalText); tft.print(" "); tft.print(btRssiDelta); }
}

void drawScrollLine(int y, uint16_t color, const ScrollRow &row) {
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setTextColor(color, ST77XX_BLACK);  // háttér=fekete, GFX felülírja az előző pixeleket

  if (row.origPixelW <= SCROLL_AREA_W) {
    // Rövid szöveg: töröljük a sort, kiírjuk
    tft.fillRect(SCROLL_AREA_X, y, SCROLL_AREA_W, 8, ST77XX_BLACK);
    tft.setCursor(SCROLL_AREA_X, y);
    tft.print(row.origText);
  } else {
    // Scroll: NEM töröljük előre a sort — a GFX fekete háttérrel írja felül,
    // így nincs villogás. Csak a két szélső maszkot frissítjük.
    tft.setCursor(SCROLL_AREA_X - row.pixelOffset, y);
    tft.print(row.text);
    tft.fillRect(0,                              y, SCROLL_AREA_X,                              8, ST77XX_BLACK);
    tft.fillRect(SCROLL_AREA_X + SCROLL_AREA_W, y, 160 - SCROLL_AREA_X - SCROLL_AREA_W,        8, ST77XX_BLACK);
  }
}

void drawSongInfo() {
  tft.setTextWrap(false);
  tft.setTextSize(1);
  bool haveTrackInfo = connected && ((artistText != "-") || (titleText != "-"));
  if (!haveTrackInfo) {
    tft.fillRect(4, 87, 152, 28, ST77XX_BLACK);
    tft.setTextColor(UI_CYAN2);
    tft.setCursor(4, 88);  tft.print("Artist: ");
    tft.setTextColor(ST77XX_WHITE); tft.print("-");
    tft.setTextColor(UI_CYAN2);
    tft.setCursor(4, 101); tft.print("Title : ");
    tft.setTextColor(ST77XX_WHITE); tft.print("-");
  } else {
    drawScrollLine(88,  UI_CYAN2,       scrollArtist);
    drawScrollLine(101, ST77XX_WHITE,   scrollTitle);
  }
}

void drawVolumeBar() {
  int barX = 4, barY = 121, barW = 120, barH = 6;
  int fillW = map(volumePercent, 0, 100, 0, barW - 2);
  fillW = constrain(fillW, 0, barW - 2);
  tft.fillRect(barX, barY, 156, 7, ST77XX_BLACK);
  tft.drawRect(barX, barY, barW, barH, UI_LIGHTBLUE);
  tft.fillRect(barX + 1, barY + 1, barW - 2, barH - 2, ST77XX_BLACK);
  if (fillW > 0) tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, UI_LIGHTBLUE);
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
// Scroll tick
// =======================================================
// Logika (soronként, egymás után):
//   SCROLL_IDLE : 10 mp áll az induló pozícióban
//   SCROLL_RUN  : görget balra; a szöveg + egy résznyi üres hely után
//                 visszaér az elejére (= totalPixelW + SCROLL_AREA_W pixel),
//                 ekkor visszaugrik offset=0-ra és IDLE-be vált.
//                 Így a szöveg végig látszik: balra csúszik, eltűnik,
//                 majd a jobb oldalon újra megjelenik és megáll az elejénél.
//   Egyik sor fut, utána a másik — felváltva.
void updateScroll() {
  bool haveTrackInfo = connected && ((artistText != "-") || (titleText != "-"));
  if (!haveTrackInfo) return;

  unsigned long now = millis();

  ScrollRow &active  = (scrollActiveRow == 0) ? scrollArtist : scrollTitle;
  ScrollRow &passive = (scrollActiveRow == 0) ? scrollTitle  : scrollArtist;
  int activeY        = (scrollActiveRow == 0) ? 88  : 101;
  int passiveY       = (scrollActiveRow == 0) ? 101 : 88;
  uint16_t activeColor  = (scrollActiveRow == 0) ? (uint16_t)UI_CYAN2    : (uint16_t)ST77XX_WHITE;
  uint16_t passiveColor = (scrollActiveRow == 0) ? (uint16_t)ST77XX_WHITE : (uint16_t)UI_CYAN2;

  bool needsScroll = (active.origPixelW > SCROLL_AREA_W);

  switch (active.phase) {

    case SCROLL_IDLE:
      if (now - active.phaseStart >= (unsigned long)SCROLL_IDLE_MS) {
        if (!needsScroll) {
          // Rövid sor: nincs mit görgetni, váltunk a másikra
          active.pixelOffset = 0;
          active.phase       = SCROLL_IDLE;
          active.phaseStart  = now;
          scrollActiveRow    = 1 - scrollActiveRow;
          ScrollRow &next    = (scrollActiveRow == 0) ? scrollArtist : scrollTitle;
          next.pixelOffset   = 0;
          next.phase         = SCROLL_IDLE;
          next.phaseStart    = now;
        } else {
          active.phase      = SCROLL_RUN;
          active.phaseStart = now;
        }
      }
      break;

    case SCROLL_RUN:
      if (now - lastScrollStep < (unsigned long)SCROLL_SPEED_MS) break;
      lastScrollStep = now;

      active.pixelOffset++;

      // Egy teljes ciklus után megáll (az első karakter visszaért a bal szélre)
      if (active.pixelOffset >= active.totalPixelW) {
        // Visszaért az elejére — megáll
        active.pixelOffset = 0;
        active.phase       = SCROLL_IDLE;
        active.phaseStart  = now;

        // Visszarajzoljuk az elejéről (offset=0)
        drawScrollLine(activeY, activeColor, active);

        // Váltunk a másik sorra
        scrollActiveRow  = 1 - scrollActiveRow;
        ScrollRow &next  = (scrollActiveRow == 0) ? scrollArtist : scrollTitle;
        next.pixelOffset = 0;
        next.phase       = SCROLL_IDLE;
        next.phaseStart  = now;
        // A most-passzív sor marad ahol van (offset=0, látszik)
        (void)passive; (void)passiveY; (void)passiveColor;
      } else {
        drawScrollLine(activeY, activeColor, active);
      }
      break;
  }
}

// =======================================================
// Bluetooth callbacks
// =======================================================
void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
  if (!text) return;
  String value = String((const char *)text);
  switch (id) {
    case ESP_AVRC_MD_ATTR_TITLE:  titleText  = value; break;
    case ESP_AVRC_MD_ATTR_ARTIST: artistText = value; break;
    case ESP_AVRC_MD_ATTR_ALBUM:  albumText  = value; break;
    default: break;
  }
  resetScroll();
  requestRedraw();
}

void avrc_rn_playstatus_callback(esp_avrc_playback_stat_t playback) {
  switch (playback) {
    case ESP_AVRC_PLAYBACK_PLAYING: playState = "PLAY";  break;
    case ESP_AVRC_PLAYBACK_PAUSED:  playState = "PAUSE"; break;
    case ESP_AVRC_PLAYBACK_STOPPED: playState = "STOP";  break;
    default: playState = connected ? "READY" : "PAIR";   break;
  }
  requestRedraw();
}

void connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
  connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
  if (connected) {
    playState            = "READY";
    btRssiValid          = false;
    btSignalText         = "WAIT";
    btSignalBars         = 0;
    lastRssiRead         = 0;
    connectedSince       = millis();
    initialMetaRequested = false;
    wakeScreen();
  } else {
    playState    = "PAIR";
    artistText   = "-";
    titleText    = "-";
    albumText    = "-";
    btRssiValid  = false;
    btSignalText = "---";
    btSignalBars = 0;
    btRssiDelta  = 0;
    resetScroll();
  }
  requestRedraw();
}

void volume_changed(int volume) {
  volumePercent = map(volume, 0, 127, 0, 100);
  volumePercent = constrain(volumePercent, 0, 100);
  wakeScreen();
  requestRedraw();
}

void rssi_callback(esp_bt_gap_cb_param_t::read_rssi_delta_param &rssi) {
  int    oldDelta = btRssiDelta;
  int    oldBars  = btSignalBars;
  bool   oldValid = btRssiValid;
  String oldText  = btSignalText;

  if (rssi.stat != ESP_BT_STATUS_SUCCESS) {
    btRssiValid  = false;
    btSignalText = "ERR";
    btSignalBars = 0;
    Serial.println("BT RSSI update failed");
    if (oldValid != btRssiValid || oldText != btSignalText || oldBars != btSignalBars)
      requestSignalRedraw();
    return;
  }

  btRssiDelta = rssi.rssi_delta;
  btRssiValid = true;

  if      (btRssiDelta >= -3)  { btSignalBars = 3; btSignalText = "GOOD"; }
  else if (btRssiDelta >= -10) { btSignalBars = 2; btSignalText = "FAIR"; }
  else if (btRssiDelta >= -18) { btSignalBars = 1; btSignalText = "WEAK"; }
  else                         { btSignalBars = 0; btSignalText = "BAD";  }

  Serial.print("BT RSSI delta: "); Serial.println(btRssiDelta);

  if (oldDelta != btRssiDelta || oldBars != btSignalBars || oldValid != btRssiValid || oldText != btSignalText)
    requestSignalRedraw();
}

// =======================================================
// Setup
// =======================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("ESP32 Bluetooth A2DP Speaker starting...");

  // PWM háttérvilágítás init
  ledcSetup(BL_PWM_CHANNEL, BL_PWM_FREQ, BL_PWM_RES);
  ledcAttachPin(TFT_BL, BL_PWM_CHANNEL);
  ledcWrite(BL_PWM_CHANNEL, BL_BRIGHT_FULL);
  lastActivityMs = millis();

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);

  resetScroll();
  drawScreen();

  i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_BCLK,
    .ws_io_num    = I2S_LRCK,
    .data_out_num = I2S_DATA,
    .data_in_num  = I2S_PIN_NO_CHANGE
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
  unsigned long now = millis();

  // Háttérvilágítás dimmer
  if (!screenDimmed && now - lastActivityMs > SCREEN_DIM_MS) {
    dimScreen();
  }

  // RSSI lekérés
  if (connected && now - lastRssiRead > 3000) {
    lastRssiRead = now;
    if (!a2dp_sink.update_rssi())
      Serial.println("BT RSSI request not accepted yet");
  }

  // PC/Windows: AVRC metadata kérés csatlakozás után
  if (connected && !initialMetaRequested && now - connectedSince > 2000) {
    initialMetaRequested = true;
    // Az ESP32-A2DP könyvtár automatikusan regisztrálja a notifikációkat;
    // itt csak jelezzük hogy az initial polling megtörtént
    Serial.println("AVRC ready (PC mode)");
  }

  // Képernyő újrarajzolás
  if (needRedraw && now - lastDraw > 150) {
    needRedraw      = false;
    needSignalRedraw = false;
    lastDraw        = now;
    drawScreen();
    resetScroll();
  } else if (needSignalRedraw && now - lastSignalDraw > 100) {
    needSignalRedraw = false;
    lastSignalDraw   = now;
    drawSignalPartial();
  }

  // Scroll animáció
  updateScroll();
}
