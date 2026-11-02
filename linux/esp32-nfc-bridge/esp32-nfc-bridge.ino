// AnyShare NFC bridge: ESP32 + HW-147C (Si512) reader.
//
// The Si512 is a register-level reader chip (reports version 0x82 like NXP's PN512), not a
// PN532, so libnfc cannot drive it. This sketch drives it over SPI, reads the
// AnyShare HCE payload from a phone in receive mode (ISO 14443-4 / ISO-DEP,
// SELECT AID F0414E59534852) and prints one line per event on USB serial:
//
//   READER OK version=0xNN          chip answered over SPI
//   READER ERROR ...                no SPI answer: wiring / DIP switches
//   TAG uid=... sak=0xNN            something entered the field
//   PAYLOAD {...json...}            handover read from an AnyShare phone
//   CARD ...                        a plain card/tag (not a phone) was read
//   ERROR ...                       tap failed (reason given)
//
// Wiring (the team's board). HW-147C: 8-pin header on the left edge,
// SW1 switch 1 = OFF, switch 2 = ON -> SPI. The 4-pin bottom header stays unused.
//   HW-147C SCK  -> GPIO18     HW-147C MISO -> GPIO19    HW-147C MOSI -> GPIO23
//   HW-147C SS   -> GPIO5      HW-147C RST  -> GPIO4     HW-147C IRQ  -> not connected
//   HW-147C VCC  -> 3V3        HW-147C GND  -> GND
//   OLED SDA     -> GPIO21     OLED SCL     -> GPIO22    (SSD1306, I2C 0x3C; optional)
//   Buzzer +/SIG -> GPIO25     Buzzer -     -> GND       (optional)
//   RGB LED, 5 mm (optional): longest leg -> 3V3 for common anode (our LED; LED_COMMON_ANODE = true)
//                                         or -> GND for common cathode (set LED_COMMON_ANODE = false),
//     red   -> 220 ohm -> GPIO32
//     green -> 100 ohm -> GPIO33
//     blue  -> 100 ohm -> GPIO26
//
// RGB LED meaning (it follows the OLED):
//   solid green        ready / idle
//   blinking red       receiving (phone or PC -> this laptop)
//   blinking purple    sending (this laptop -> phone or PC)
//   fast green         transfer finished OK (5 s, then solid green)
//   solid cyan         a card or phone was tapped (4 s)
//   fast amber         error: transfer failed, tap failed, laptop stopped reporting (5 s)
//   solid amber        no reader found (wiring)
// At boot it shows red, green, blue for 0.3 s each: a wiring check. "LEDTEST" repeats it slowly.
//
// The laptop app drives the OLED over the same serial line (see linux/include/Esp32Bridge.hpp):
//   HELLO                                   -> "INFO AnyShare NFC bridge ..." (used to find the port)
//   XFER|SEND or RECV|percent|file|type|i/n -> progress screen
//   DONE|OK or FAIL|summary                 -> result for 5 s, then the idle screen
//   IDLE                                    -> idle screen ("tap an NFC card or phone")
//   CARD|{json}                             -> CARD MODE: the module behaves like an NFC card that
//                                              serves this laptop's handover (phones tap it to send here)
//   CARD|OFF                                -> back to reader mode
//   WRITE|name|hex                          -> write a small file onto the next MIFARE Classic 1K card tapped
//                                              (default key A FFFFFFFFFFFF); answers WPROG n, WRITTEN n / WRITEFAIL why
// Tapping a card that holds such a file prints CARDDATA name|hex (card -> laptop).
// Card mode reports "SERVED" each time a phone read the handover. EXPERIMENTAL: it relies on
// the PN512 card-operation commands (Config, AutoColl); run SELFTEST / CONFIGTEST to check the chip.
// Serial: 115200 baud. Send "INFO" to print the reader status again.
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static const uint8_t PIN_SS = 5;
static const uint8_t PIN_RST = 4;      // HIGH = running; the library pulses it for a hard reset
static const uint8_t PIN_SDA = 21;
static const uint8_t PIN_SCL = 22;
static const uint8_t PIN_BUZZER = 25;
static const uint8_t PIN_LED_R = 32;
static const uint8_t PIN_LED_G = 33;
static const uint8_t PIN_LED_B = 26;
// The team's LED is sold as common cathode but is common ANODE (longest leg on 3V3,
// verified 2026-10-14). Set to false for a real common-cathode LED (longest leg on GND).
static const bool LED_COMMON_ANODE = true;
static const uint8_t AID[] = {0xF0, 0x41, 0x4E, 0x59, 0x53, 0x48, 0x52};   // protocol::kHceAidHex

MFRC522 rfid(PIN_SS, PIN_RST);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
bool oledOk = false;
bool readerOk = false;
uint8_t blockNumber = 0;
uint32_t cooldownUntil = 0;
void readerPollOnce();
String pendingWriteName;   // WRITE| request waiting for a card
String pendingWriteHex;
bool writePending = false;

// What the OLED shows. A message expires back to the idle screen; a transfer
// screen stays until DONE/IDLE, or until the laptop goes quiet for 15 s.
enum class Screen { Idle, Message, Transfer };
Screen screen = Screen::Idle;
uint32_t messageUntil = 0;
uint32_t lastTransferUpdate = 0;
bool lastTransferWasSend = false;
static const uint32_t TRANSFER_WATCHDOG_MS = 15000;

// ---------------------------------------------------------------------------
// RGB status LED (optional). PWM levels balance the colours: at 3.3 V red is
// much brighter than green and blue.
// ---------------------------------------------------------------------------
struct Rgb {
  uint8_t r, g, b;
  const char* name;
};
const Rgb LED_GREEN = {0, 150, 0, "green"};
const Rgb LED_RED = {255, 0, 0, "red"};
const Rgb LED_PURPLE = {110, 0, 255, "purple"};
const Rgb LED_AMBER = {255, 60, 0, "amber"};
const Rgb LED_CYAN = {0, 170, 255, "cyan"};
const Rgb LED_BLUE = {0, 0, 255, "blue"};
enum class Blink { Solid, Slow, Fast };

Rgb ledColor = LED_GREEN;
Blink ledBlink = Blink::Solid;
bool ledLit = false;

// Common anode: a colour lights when its pin is LOW, so the duty is inverted.
void writeLed(bool on) {
  auto level = [](uint8_t v) -> uint32_t { return LED_COMMON_ANODE ? 255 - v : v; };
  ledcWrite(PIN_LED_R, level(on ? ledColor.r : 0));
  ledcWrite(PIN_LED_G, level(on ? ledColor.g : 0));
  ledcWrite(PIN_LED_B, level(on ? ledColor.b : 0));
}

// Called every loop: drives the blinking without blocking.
void updateLed(bool force = false) {
  uint32_t period = ledBlink == Blink::Slow ? 800 : 200;
  bool on = ledBlink == Blink::Solid || (millis() % period) < period / 2;
  if (on != ledLit || force) {
    ledLit = on;
    writeLed(on);
  }
}

void setLed(const Rgb& color, Blink blink) {
  if (color.name == ledColor.name && blink == ledBlink) return;
  ledColor = color;
  ledBlink = blink;
  Serial.printf("LED %s %s\n", color.name, blink == Blink::Solid ? "solid" : blink == Blink::Slow ? "blink" : "fast-blink");
  updateLed(true);
}

void ledSelfTest(uint32_t stepMs) {
  const Rgb* steps[] = {&LED_RED, &LED_GREEN, &LED_BLUE};
  for (const Rgb* c : steps) {
    Serial.printf("LEDTEST %s\n", c->name);
    ledColor = *c;
    writeLed(true);
    delay(stepMs);
  }
  writeLed(false);
}

// ---------------------------------------------------------------------------
// OLED + buzzer (both optional: the bridge works without them)
// ---------------------------------------------------------------------------
// Every screen change is also echoed as "SCREEN ..." on serial, so the
// laptop (and tests) can see what the OLED shows. The app ignores these lines.
void show(const String& title, const String& detail) {
  String flat = detail;
  flat.replace("\n", " / ");
  Serial.printf("SCREEN %s | %s\n", title.c_str(), flat.c_str());
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.println("AnyShare NFC");
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(0, 14);
  oled.println(title);
  oled.setTextSize(1);
  oled.setCursor(0, 34);
  oled.println(detail.substring(0, 84));   // 4 lines of 21 characters
  oled.display();
}

bool cardModeOn();

void showIdle() {
  screen = Screen::Idle;
  if (cardModeOn()) show("Receive", "Tap your phone here\nto send to this laptop");
  else if (readerOk) show("Ready", "Tap an NFC card\nor an NFC phone");
  else show("NO READER", "No SPI answer. Check wiring + SW1 (1=OFF 2=ON)");
  setLed(readerOk ? LED_GREEN : LED_AMBER, Blink::Solid);
}

void showMessage(const String& title, const String& detail, uint32_t ms, const Rgb& color, Blink blink) {
  show(title, detail);
  screen = Screen::Message;
  messageUntil = millis() + ms;
  setLed(color, blink);
}

// "Receiving  1/3" / big "42%" / progress bar / file name / file type
void showTransfer(bool sending, int percent, const String& file, const String& type, const String& position) {
  screen = Screen::Transfer;
  lastTransferUpdate = millis();
  lastTransferWasSend = sending;
  setLed(sending ? LED_PURPLE : LED_RED, Blink::Slow);
  percent = constrain(percent, 0, 100);
  Serial.printf("SCREEN %s %d%% | %s | %s | %s\n", sending ? "Sending" : "Receiving", percent, file.c_str(),
                type.c_str(), position.c_str());
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(sending ? "Sending" : "Receiving");
  oled.setCursor(128 - 6 * position.length(), 0);
  oled.print(position);
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);
  oled.setTextSize(2);
  oled.setCursor(0, 14);
  oled.print(String(percent) + "%");
  oled.drawRect(0, 32, 128, 8, SSD1306_WHITE);
  oled.fillRect(2, 34, (124 * percent) / 100, 4, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 44);
  oled.print(file.substring(0, 21));
  oled.setCursor(0, 55);
  oled.print(type);
  oled.display();
}

void updateScreen() {
  if (screen == Screen::Message && (int32_t)(millis() - messageUntil) >= 0) showIdle();
  if (screen == Screen::Transfer && millis() - lastTransferUpdate > TRANSFER_WATCHDOG_MS) {
    showMessage("No update", "The laptop stopped reporting this transfer", 5000, LED_AMBER, Blink::Fast);
  }
}

// n-th '|' separated field of a host message.
String field(const String& line, int n) {
  int start = 0;
  for (int i = 0; i < n; ++i) {
    start = line.indexOf('|', start);
    if (start < 0) return "";
    ++start;
  }
  int end = line.indexOf('|', start);
  return end < 0 ? line.substring(start) : line.substring(start, end);
}

void beep(int count) {
  Serial.flush();   // finish the serial line first: the buzzer's current spike garbles it otherwise
  for (int i = 0; i < count; ++i) {
    tone(PIN_BUZZER, 2700, 70);
    delay(120);
  }
}

// ---------------------------------------------------------------------------
// Reader status
// ---------------------------------------------------------------------------
bool checkReader() {
  rfid.PCD_Init();
  uint8_t version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.printf("READER ERROR no answer over SPI (version=0x%02X). Check wiring and that the DIP switches select SPI (1=OFF, 2=ON).\n", version);
    readerOk = false;
    showIdle();
    return false;
  }
  // PN512-family chips (the Si512 reports 0x82 like NXP's PN512) start as an NFC
  // *target*: ControlReg bit 4 "Initiator" must be set or Transceive waits to
  // receive and never transmits REQA. Plain MFRC522 chips ignore this bit.
  rfid.PCD_WriteRegister(MFRC522::ControlReg, 0x10);
  rfid.PCD_SetAntennaGain(MFRC522::RxGain_max);
  Serial.printf("READER OK version=0x%02X antenna=on\n", version);
  readerOk = true;
  showMessage("Reader OK", "Chip version 0x" + String(version, HEX), 1500, LED_GREEN, Blink::Solid);
  return true;
}

// ---------------------------------------------------------------------------
// ISO 14443-4 on top of the chip's Transceive command
// ---------------------------------------------------------------------------
// Sends `len` bytes plus CRC_A and waits up to timeoutMs for one frame.
// Returns the frame length without CRC, or a negative error code.
int transceive(const uint8_t* data, uint8_t len, uint8_t* out, uint8_t outMax, uint32_t timeoutMs) {
  uint8_t frame[64];
  if (len + 2 > sizeof(frame)) return -10;
  memcpy(frame, data, len);
  if (rfid.PCD_CalculateCRC(frame, len, frame + len) != MFRC522::STATUS_OK) return -11;

  rfid.PCD_WriteRegister(MFRC522::CommandReg, MFRC522::PCD_Idle);
  rfid.PCD_WriteRegister(MFRC522::ComIrqReg, 0x7F);
  rfid.PCD_WriteRegister(MFRC522::FIFOLevelReg, 0x80);
  rfid.PCD_WriteRegister(MFRC522::FIFODataReg, len + 2, frame);
  rfid.PCD_WriteRegister(MFRC522::BitFramingReg, 0x00);
  rfid.PCD_WriteRegister(MFRC522::CommandReg, MFRC522::PCD_Transceive);
  rfid.PCD_SetRegisterBitMask(MFRC522::BitFramingReg, 0x80);   // StartSend

  uint32_t start = millis();
  for (;;) {
    uint8_t irq = rfid.PCD_ReadRegister(MFRC522::ComIrqReg);
    if (irq & 0x30) break;                              // RxIRq | IdleIRq
    if (millis() - start > timeoutMs) return -1;        // phone did not answer
    delayMicroseconds(200);
  }
  uint8_t error = rfid.PCD_ReadRegister(MFRC522::ErrorReg);
  if (error & 0x13) return -2;                          // buffer overflow / parity / protocol
  uint8_t n = rfid.PCD_ReadRegister(MFRC522::FIFOLevelReg);
  if (n < 3 || n > sizeof(frame)) return -3;
  rfid.PCD_ReadRegister(MFRC522::FIFODataReg, n, frame, 0);
  uint8_t crc[2];
  if (rfid.PCD_CalculateCRC(frame, n - 2, crc) != MFRC522::STATUS_OK) return -11;
  if (crc[0] != frame[n - 2] || crc[1] != frame[n - 1]) return -4;
  n -= 2;
  if (n > outMax) return -5;
  memcpy(out, frame, n);
  return n;
}

// Sends one APDU in an I-block; follows WTX requests and chained responses.
int exchangeApdu(const uint8_t* apdu, uint8_t apduLen, uint8_t* response, int responseMax) {
  uint8_t tx[40], rx[64];
  tx[0] = 0x02 | blockNumber;
  memcpy(tx + 1, apdu, apduLen);
  int n = transceive(tx, apduLen + 1, rx, sizeof(rx), 600);
  int total = 0;
  for (int frames = 0; frames < 40; ++frames) {
    if (n < 1) return n == 0 ? -6 : n;
    uint8_t pcb = rx[0];
    if ((pcb & 0xF7) == 0xF2 && n >= 2) {               // S(WTX): phone needs more time
      uint8_t wtx[2] = {0xF2, (uint8_t)(rx[1] & 0x3F)};
      n = transceive(wtx, 2, rx, sizeof(rx), 2000);
      continue;
    }
    if ((pcb & 0xE2) != 0x02) return -7;                // not an I-block
    blockNumber ^= 1;
    if (total + n - 1 > responseMax) return -8;
    memcpy(response + total, rx + 1, n - 1);
    total += n - 1;
    if ((pcb & 0x10) == 0) return total;                // last block of the chain
    uint8_t ack = 0xA2 | blockNumber;                   // R(ACK): send the next part
    n = transceive(&ack, 1, rx, sizeof(rx), 600);
  }
  return -9;
}

const char* explain(int code) {
  switch (code) {
    case -1: return "no answer from the phone (moved away too early?)";
    case -2: return "radio error (hold the phone still, centred on the antenna)";
    case -4: return "CRC error (hold the phone still)";
    case -7: return "unexpected ISO-DEP block";
    case -8: return "response too long";
    default: return "communication error";
  }
}

// Tap results never cover a running transfer's progress screen.
void tapMessage(const String& title, const String& detail, bool ok) {
  if (screen != Screen::Transfer) showMessage(title, detail, 4000, ok ? LED_CYAN : LED_AMBER, ok ? Blink::Solid : Blink::Fast);
}

void readPhone() {
  uint8_t ats[32];
  uint8_t rats[2] = {0xE0, 0x40};   // RATS: FSD = 48 bytes (fits the 64-byte FIFO), CID 0
  int n = transceive(rats, 2, ats, sizeof(ats), 100);
  if (n < 1) {
    Serial.printf("ERROR RATS failed (%d): %s\n", n, explain(n));
    tapMessage("Tap failed", explain(n), false);
    return;
  }
  blockNumber = 0;

  uint8_t select[6 + sizeof(AID)] = {0x00, 0xA4, 0x04, 0x00, sizeof(AID)};
  memcpy(select + 5, AID, sizeof(AID));
  select[sizeof(select) - 1] = 0x00;   // Le
  static uint8_t response[300];
  int len = exchangeApdu(select, sizeof(select), response, sizeof(response) - 1);
  if (len < 2) {
    Serial.printf("ERROR SELECT failed (%d): %s\n", len, explain(len));
    tapMessage("Tap failed", explain(len), false);
    return;
  }
  uint16_t sw = (response[len - 2] << 8) | response[len - 1];
  if (sw != 0x9000) {
    Serial.printf("ERROR phone answered SW %04X: in AnyShare tap Receive, or Send by NFC tap, first\n", sw);
    tapMessage("Not ready", "Open AnyShare: Receive, or Send by NFC tap", false);
    return;
  }
  response[len - 2] = 0;
  Serial.printf("PAYLOAD %s\n", (const char*)response);
  String json((const char*)response);
  if (json.indexOf("\"role\":\"send\"") >= 0) {   // the phone has files: the laptop connects and fetches them
    tapMessage("Phone read", "Fetching its files...", true);
  } else {
    int ip = json.indexOf("\"ip\":\"");
    String where = ip < 0 ? json : json.substring(ip + 6, json.indexOf('"', ip + 6));
    tapMessage("Phone read", "Receiver " + where, true);
  }
  beep(2);

  uint8_t deselect = 0xC2, ignored[4];
  transceive(&deselect, 1, ignored, sizeof(ignored), 50);
}


// ---------------------------------------------------------------------------
// Card mode (EXPERIMENTAL): the module answers like an ISO 14443-4 card with
// the AnyShare AID, so a phone in reader mode can tap it and read where to
// send. Uses the PN512 card-operation commands from the NXP PN512 datasheet
// (Rev. 5.3): Config (0001b) stores ATQA/UID/SAK, AutoColl (1101b) handles
// REQA/anticollision/SELECT, then the chip switches to Transceive and the
// first command after SELECT (RATS) is in the FIFO. ISO-DEP is done here.
// ---------------------------------------------------------------------------
namespace reg {   // raw PN512 register addresses
const uint8_t Command = 0x01, ComIrq = 0x04, Error = 0x06, Status1 = 0x07, Status2 = 0x08, FifoData = 0x09,
              FifoLevel = 0x0A, Control = 0x0C, BitFraming = 0x0D, Mode = 0x11, TxMode = 0x12, RxMode = 0x13,
              TxControl = 0x14, TxAuto = 0x15, TxSel = 0x16, FelNFC2 = 0x1B, MifNFC = 0x1C, GsNOff = 0x23,
              RFCfg = 0x26, AutoTest = 0x36;
const uint8_t CmdIdle = 0x00, CmdConfig = 0x01, CmdCalcCRC = 0x03, CmdAutoColl = 0x0D, CmdSoftReset = 0x0F;
}  // namespace reg

uint8_t rd(uint8_t a) { return rfid.PCD_ReadRegister((MFRC522::PCD_Register)(a << 1)); }
void wr(uint8_t a, uint8_t v) { rfid.PCD_WriteRegister((MFRC522::PCD_Register)(a << 1), v); }
void fifoWrite(const uint8_t* d, uint8_t n) { rfid.PCD_WriteRegister(MFRC522::FIFODataReg, n, (byte*)d); }
uint8_t fifoRead(uint8_t* d, uint8_t max) {
  uint8_t n = rd(reg::FifoLevel) & 0x7F;
  if (n > max) n = max;
  if (n) rfid.PCD_ReadRegister(MFRC522::FIFODataReg, n, d, 0);
  return n;
}
bool waitIdle(uint32_t ms) {
  uint32_t start = millis();
  while ((rd(reg::Command) & 0x0F) != reg::CmdIdle) {
    if (millis() - start > ms) return false;
  }
  return true;
}

// ISO 14443A CRC_A (preset 6363h), to recognise frames that still carry their CRC.
void crcA(const uint8_t* d, uint8_t n, uint8_t* out) {
  uint16_t crc = 0x6363;
  for (uint8_t i = 0; i < n; ++i) {
    uint8_t b = d[i] ^ (uint8_t)(crc & 0xFF);
    b ^= b << 4;
    crc = (crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3) ^ (b >> 4);
  }
  out[0] = crc & 0xFF;
  out[1] = crc >> 8;
}

enum class CardState { Off, Armed, Active };
CardState cardState = CardState::Off;
String cardPayload;                 // laptop handover JSON
uint8_t cardResponse[300];          // payload + 90 00 (or a status word)
uint16_t cardResponseLen = 0, cardResponseSent = 0;
uint8_t cardFrameMax = 13;          // data bytes per frame (from the reader's FSD and our 64-byte FIFO)
uint8_t cardLastFrame[64];
uint8_t cardLastLen = 0;
uint32_t cardDeadline = 0;
uint8_t cardGsNOff = 0x6F;          // load modulation strength (tune with REG 23 xx)
bool cardDebug = true;

void cardLog(const char* what, const uint8_t* d, uint8_t n) {
  if (!cardDebug) return;
  Serial.printf("CARDDBG %s", what);
  for (uint8_t i = 0; i < n && i < 24; ++i) Serial.printf(" %02X", d[i]);
  Serial.println(n > 24 ? " ..." : "");
}

void cardSend(const uint8_t* frame, uint8_t n) {
  memcpy(cardLastFrame, frame, n);
  cardLastLen = n;
  wr(reg::FifoLevel, 0x80);                    // flush
  fifoWrite(frame, n);
  wr(reg::ComIrq, 0x7F);
  wr(reg::BitFraming, 0x80);                   // StartSend: transmit, then receive the next frame
  cardLog("TX", frame, n);
}

void cardSendChunk(uint8_t blockNumber) {
  uint16_t left = cardResponseLen - cardResponseSent;
  uint8_t n = left > cardFrameMax ? cardFrameMax : left;
  uint8_t frame[64];
  frame[0] = 0x02 | (blockNumber & 1) | (left > n ? 0x10 : 0x00);   // I-block, chaining if more follows
  memcpy(frame + 1, cardResponse + cardResponseSent, n);
  cardResponseSent += n;
  cardSend(frame, n + 1);
  if (cardResponseSent >= cardResponseLen && cardResponseLen > 2 &&
      cardResponse[cardResponseLen - 2] == 0x90) {
    Serial.println("SERVED handover read by a phone");
    tapMessage("Phone tapped", "Sending to this laptop...", true);
  }
}

void cardArm() {
  wr(reg::Command, reg::CmdIdle);
  wr(reg::FifoLevel, 0x80);
  wr(reg::ComIrq, 0x7F);
  wr(reg::Command, reg::CmdAutoColl);
  cardState = CardState::Armed;
}

// Config: SENS_RES (2) + NFCID1 (3, first byte fixed 08h) + SEL_RES (1) + FeliCa polling response (18) + NFCID3 (1)
bool cardConfigure() {
  // One UID per boot: this runs again after every reader slice, and a UID that changed between
  // the phone's anticollision and its next poll looks to Android like a different (lost) tag.
  static uint8_t uid[3] = {(uint8_t)random(256), (uint8_t)random(256), (uint8_t)random(256)};
  uint8_t cfg[25] = {0};
  cfg[0] = 0x04; cfg[1] = 0x00;                       // ATQA: single-size UID
  cfg[2] = uid[0]; cfg[3] = uid[1]; cfg[4] = uid[2];
  cfg[5] = 0x20;                                      // SAK: ISO/IEC 14443-4 compliant
  cfg[6] = 0x01; cfg[7] = 0xFE;                       // FeliCa polling response header (unused here)
  wr(reg::Command, reg::CmdIdle);
  wr(reg::FifoLevel, 0x80);
  fifoWrite(cfg, sizeof(cfg));
  wr(reg::Command, reg::CmdConfig);
  return waitIdle(50);
}

void cardStart(const String& payload, bool quiet = false) {
  cardPayload = payload;
  uint16_t n = payload.length();
  if (n > sizeof(cardResponse) - 2) n = sizeof(cardResponse) - 2;
  memcpy(cardResponse, payload.c_str(), n);
  wr(reg::Command, reg::CmdIdle);
  wr(reg::Control, 0x00);          // target (card) mode
  wr(reg::TxControl, 0x80);        // own RF field off: answers by load modulation
  wr(reg::TxAuto, 0x00);
  wr(reg::TxSel, 0x10);            // drivers modulated by the internal coder
  wr(reg::GsNOff, cardGsNOff);     // conductance while "off" = load modulation depth
  wr(reg::Mode, 0x3D);             // ModeDetOff: ISO 14443A 106 kbit only, CRC preset 6363
  wr(reg::TxMode, 0x80);           // CRC added by the chip after AutoColl
  wr(reg::RxMode, 0x80);
  wr(reg::FelNFC2, 0x80);          // WaitForSelected: AutoColl ends only after SELECT + next command
  wr(reg::MifNFC, 0x62);
  wr(reg::RFCfg, 0x48);
  bool ok = cardConfigure();
  cardArm();
  if (quiet) return;
  Serial.printf("CARDMODE on (%u-byte handover, Config %s)\n", n, ok ? "ok" : "TIMEOUT");
  showIdle();
}

// While in card mode the module still checks for cards/phones now and then:
// ~700 ms as a card (phones in reader mode poll often enough to catch it),
// then one short reader poll (~10-30 ms), then back to card mode.
// The reader slices matter more than card mode: reading a phone works reliably, being read by one does
// not on the Si512 (see HOW_TO_RUN). So they keep running while a phone's field is present.
// SLICE|0 turns them off completely (card only, for card-mode experiments).
uint32_t cardSliceStart = 0;
static const uint32_t CARD_SLICE_MS = 700;
uint32_t lastFieldMs = 0, lastFieldLogMs = 0;
bool readerSlices = true;

uint32_t lastCardFrameMs = 0, lastRearmMs = 0, rearmCount = 0;

bool externalField() {
  if (!(rd(reg::Status1) & 0x04)) return false;   // Status1.RFOn: an external RF field is present
  lastFieldMs = millis();
  if (cardDebug && millis() - lastFieldLogMs > 2000) {
    lastFieldLogMs = millis();
    // Command (AutoColl = 0Dh, Transceive = 0Ch), Status2 low 3 bits = modem state, Error, FIFO level
    Serial.printf("CARDDBG field present: Command=%02X Status2=%02X Error=%02X ComIrq=%02X Fifo=%u rearms=%u\n",
                  rd(reg::Command), rd(reg::Status2), rd(reg::Error), rd(reg::ComIrq), rd(reg::FifoLevel) & 0x7F,
                  (unsigned)rearmCount);
  }
  return true;
}

void enterReaderRegisters() {
  wr(reg::Command, reg::CmdIdle);
  wr(reg::Control, 0x10);          // initiator (reader)
  wr(reg::TxAuto, 0x40);           // 100 % ASK
  wr(reg::Mode, 0x3D);
  wr(reg::RFCfg, 0x70);            // max receiver gain, as in reader mode
  wr(reg::TxControl, 0x83);        // antenna on
  delay(5);                        // let the field settle before REQA
}


void cardStop() {
  if (cardState == CardState::Off) return;
  wr(reg::Command, reg::CmdIdle);
  cardState = CardState::Off;
  Serial.println("CARDMODE off");
  readerOk = checkReader();        // back to reader mode
}

void cardHandle(uint8_t* d, uint8_t n, bool first) {
  if (n >= 3) {   // drop a CRC that is still attached (the first frame after AutoColl keeps it)
    uint8_t c[2];
    crcA(d, n - 2, c);
    if (c[0] == d[n - 2] && c[1] == d[n - 1]) n -= 2;
  }
  cardLog("RX", d, n);
  lastCardFrameMs = millis();
  cardDeadline = millis() + 3000;
  if (first || d[0] == 0xE0) {     // RATS -> ATS
    if (d[0] != 0xE0 || n < 2) { cardArm(); return; }
    static const uint16_t fsdTable[] = {16, 24, 32, 40, 48, 64, 96, 128, 256};
    uint8_t fsdi = d[1] >> 4;
    uint16_t fsd = fsdTable[fsdi > 8 ? 8 : fsdi];
    cardFrameMax = (fsd > 64 ? 64 : fsd) - 4;  // PCB + CRC, inside our 64-byte FIFO
    const uint8_t ats[] = {0x05, 0x75, 0x80, 0x70, 0x00};   // FSCI 5 (64 B), 106 kbit only, FWI 7 (~38 ms)
    cardSend(ats, sizeof(ats));
    cardState = CardState::Active;
    return;
  }
  uint8_t pcb = d[0];
  if ((pcb & 0xE2) == 0x02) {                  // I-block: an APDU
    const uint8_t* apdu = d + 1;
    uint8_t len = n - 1;
    bool select = len >= 5 + sizeof(AID) && apdu[0] == 0x00 && apdu[1] == 0xA4 && apdu[2] == 0x04 &&
                  apdu[4] == sizeof(AID) && memcmp(apdu + 5, AID, sizeof(AID)) == 0;
    uint16_t body = select ? cardPayload.length() : 0;
    if (body > sizeof(cardResponse) - 2) body = sizeof(cardResponse) - 2;
    if (select) memcpy(cardResponse, cardPayload.c_str(), body);
    cardResponse[body] = select ? 0x90 : 0x6A;
    cardResponse[body + 1] = select ? 0x00 : 0x82;
    cardResponseLen = body + 2;
    cardResponseSent = 0;
    cardSendChunk(pcb & 1);
  } else if ((pcb & 0xF6) == 0xA2) {           // R(ACK): next part of a chained response
    if (cardResponseSent < cardResponseLen) cardSendChunk(pcb & 1);
  } else if ((pcb & 0xF6) == 0xB2) {           // R(NAK): repeat
    cardSend(cardLastFrame, cardLastLen);
  } else if ((pcb & 0xF7) == 0xC2) {           // S(DESELECT)
    const uint8_t deselect = 0xC2;
    cardSend(&deselect, 1);
    delay(5);
    cardArm();
  } else {
    cardArm();
  }
}

void cardPoll() {
  uint8_t irq = rd(reg::ComIrq);
  if (irq & 0x20) {                            // RxIRq: a frame arrived
    uint8_t err = rd(reg::Error);
    uint8_t d[64];
    uint8_t n = fifoRead(d, sizeof(d));
    wr(reg::ComIrq, 0x20);
    if (err & 0x13) {                          // parity / protocol / buffer overflow
      if (cardDebug) Serial.printf("CARDDBG error %02X\n", err);
      if (cardState == CardState::Armed) cardArm();
      return;
    }
    if (n) cardHandle(d, n, cardState == CardState::Armed);
    return;
  }
  if (cardState == CardState::Active && (int32_t)(millis() - cardDeadline) > 0) cardArm();   // phone gone
}

bool cardModeOn() { return cardState != CardState::Off; }

// Does the chip implement AutoColl (1101b)? The PN512 datasheet: "During the AutoColl command, bit
// ProtocolErr is set to logic 1, if the bit Initiator in register ControlReg is set to logic 1."
// A chip without card mode has no reason to flag that. No phone needed.
void autocollTest() {
  wr(reg::Command, reg::CmdIdle);
  wr(reg::TxControl, 0x80);                    // RF off
  wr(reg::Control, 0x10);                      // Initiator = 1 (wrong for AutoColl on purpose)
  wr(reg::ComIrq, 0x7F);
  wr(reg::Command, reg::CmdAutoColl);
  delay(20);
  uint8_t errAsInitiator = rd(reg::Error), cmdAsInitiator = rd(reg::Command), irqAsInitiator = rd(reg::ComIrq);
  wr(reg::Command, reg::CmdIdle);
  wr(reg::Control, 0x00);                      // Initiator = 0 (card): AutoColl should just wait for a reader
  wr(reg::ComIrq, 0x7F);
  wr(reg::Command, reg::CmdAutoColl);
  delay(20);
  uint8_t errAsTarget = rd(reg::Error), cmdAsTarget = rd(reg::Command), st2 = rd(reg::Status2), st1 = rd(reg::Status1);
  wr(reg::Command, reg::CmdIdle);
  // Seen on our Si512: no ProtocolErr, but AutoColl ended and switched the chip to Transceive (0Ch), which is
  // what the datasheet describes when AutoColl terminates. A chip without AutoColl would not do that.
  bool protocolErrWhenInitiator = (errAsInitiator & 0x01) || (cmdAsInitiator & 0x0F) == 0x0C;
  bool cleanWhenTarget = !(errAsTarget & 0x01) && (cmdAsTarget & 0x0F) == reg::CmdAutoColl;
  Serial.printf("AUTOCOLLTEST initiator: Error=%02X Command=%02X ComIrq=%02X | target: Error=%02X Command=%02X "
                "Status2=%02X Status1=%02X (RFOn=%d)\n", errAsInitiator, cmdAsInitiator, irqAsInitiator, errAsTarget,
                cmdAsTarget, st2, st1, (st1 >> 2) & 1);
  Serial.printf("AUTOCOLLTEST %s\n", protocolErrWhenInitiator && cleanWhenTarget
                                       ? "LIKELY: AutoColl is implemented (confirm with a phone tap in card mode)"
                                   : cleanWhenTarget ? "UNCLEAR: AutoColl runs as target, but no ProtocolErr as initiator"
                                                     : "FAIL: AutoColl not implemented (no card mode on this chip)");
  readerOk = checkReader();
}

// Datasheet self-test (section 19.1): a real PN512 with VersionReg 82h always answers these 64 bytes.
void selfTest() {
  static const uint8_t expected[64] = {
      0x00, 0xEB, 0x66, 0xBA, 0x57, 0xBF, 0x23, 0x95, 0xD0, 0xE3, 0x0D, 0x3D, 0x27, 0x89, 0x5C, 0xDE,
      0x9D, 0x3B, 0xA7, 0x00, 0x21, 0x5B, 0x89, 0x82, 0x51, 0x3A, 0xEB, 0x02, 0x0C, 0xA5, 0x00, 0x49,
      0x7C, 0x84, 0x4D, 0xB3, 0xCC, 0xD2, 0x1B, 0x81, 0x5D, 0x48, 0x76, 0xD5, 0x71, 0x61, 0x21, 0xA9,
      0x86, 0x96, 0x83, 0x38, 0xCF, 0x9D, 0x5B, 0x6D, 0xDC, 0x15, 0xBA, 0x3E, 0x7D, 0x95, 0x3B, 0x2F};
  wr(reg::Command, reg::CmdSoftReset);
  delay(50);
  uint8_t zeros[25] = {0};
  wr(reg::FifoLevel, 0x80);
  fifoWrite(zeros, sizeof(zeros));
  wr(reg::Command, reg::CmdConfig);
  waitIdle(50);
  wr(reg::AutoTest, 0x09);
  wr(reg::FifoLevel, 0x80);
  uint8_t zero = 0;
  fifoWrite(&zero, 1);
  wr(reg::Command, reg::CmdCalcCRC);
  delay(50);
  uint8_t got[64] = {0};
  uint8_t n = fifoRead(got, 64);
  wr(reg::AutoTest, 0x00);
  int same = 0;
  for (int i = 0; i < 64; ++i) same += got[i] == expected[i];
  Serial.printf("SELFTEST %u bytes, %d/64 match the PN512 (VersionReg 82h) answer%s\n", n, same,
                same == 64 ? " -> genuine PN512 core" : " -> not a bit-exact PN512 (own core, compatible registers)");
  for (int i = 0; i < n; ++i) Serial.printf("%02X%s", got[i], (i % 16 == 15) ? "\n" : " ");
  Serial.println();
  readerOk = checkReader();
}

// Config round trip: store 25 bytes, read them back with an empty FIFO. A plain MFRC522 has no Config command.
void configTest() {
  uint8_t pattern[25];
  for (int i = 0; i < 25; ++i) pattern[i] = 0x30 + i;
  wr(reg::Command, reg::CmdIdle);
  wr(reg::FifoLevel, 0x80);
  fifoWrite(pattern, 25);
  wr(reg::Command, reg::CmdConfig);
  bool stored = waitIdle(50);
  wr(reg::FifoLevel, 0x80);
  wr(reg::Command, reg::CmdConfig);            // empty FIFO: copies the 25 bytes back
  bool read = waitIdle(50);
  uint8_t back[64] = {0};
  uint8_t n = fifoRead(back, 64);
  int same = 0;
  for (int i = 0; i < 25 && i < n; ++i) same += back[i] == pattern[i];
  // Note: command 0001b is also the MFRC522 "Mem" command, so a pass alone does not prove card mode.
  Serial.printf("CONFIGTEST store %s, read %s, %u bytes back, %d/25 match%s\n", stored ? "ok" : "TIMEOUT",
                read ? "ok" : "TIMEOUT", n, same, same >= 24 ? " -> 25-byte Config/Mem buffer works" : " -> buffer NOT working");
  readerOk = checkReader();
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.setRxBufferSize(4096);   // WRITE| lines carry up to ~1.5 KB of hex
  Serial.begin(115200);
  delay(300);
  Serial.println("INFO AnyShare NFC bridge (ESP32 + Si512) v1");
  bool ledOk = ledcAttach(PIN_LED_R, 5000, 8) & ledcAttach(PIN_LED_G, 5000, 8) & ledcAttach(PIN_LED_B, 5000, 8);
  Serial.printf("INFO RGB LED PWM on GPIO%d/%d/%d: %s\n", PIN_LED_R, PIN_LED_G, PIN_LED_B, ledOk ? "ready" : "FAILED");
  ledSelfTest(300);   // red, green, blue: shows at a glance whether the LED is wired right
  Wire.begin(PIN_SDA, PIN_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Serial.printf("INFO OLED %s\n", oledOk ? "found at 0x3C" : "not found (optional)");
  SPI.begin();   // VSPI defaults: SCK 18, MISO 19, MOSI 23
  readerOk = checkReader();
}

// Diagnostics over serial:
//   DUMP        print all chip registers
//   SCAN        try REQA/WUPA every 200 ms for 5 s and print each result
//   REG aa vv   write hex value vv to register aa (live tuning, not saved)
//   BEEP        test the buzzer
//   LEDTEST     red, green, blue for 1 s each (swap wires if the colours come out in another order)
//   LEDON       all three legs fully on (white) for 10 s: time to check the wiring by hand
//   SELFTEST    PN512 datasheet self-test (compares 64 bytes with a genuine PN512)
//   CONFIGTEST  stores and reads back the 25-byte Config buffer (card mode needs it)
//   AUTOCOLLTEST checks that the chip implements AutoColl (needed for card mode); no phone needed
//   CARDDEBUG   toggles CARDDBG frame logging in card mode
//   SLICE|0/1   card mode without / with the short reader polls in between
void dumpRegisters() {
  for (uint8_t r = 0x01; r < 0x40; ++r) {
    Serial.printf("REG %02X=%02X%s", r, rfid.PCD_ReadRegister((MFRC522::PCD_Register)(r << 1)), (r % 8 == 7) ? "\n" : "  ");
  }
  Serial.println();
}

void scanVerbose() {
  for (int i = 0; i < 25; ++i) {
    byte atqa[2];
    byte size = sizeof(atqa);
    MFRC522::StatusCode status = rfid.PICC_WakeupA(atqa, &size);
    Serial.printf("SCAN %2d %s atqa=%02X%02X err=%02X irq=%02X status2=%02X\n", i,
                  (const char*)MFRC522::GetStatusCodeName(status), atqa[0], atqa[1],
                  rfid.PCD_ReadRegister(MFRC522::ErrorReg), rfid.PCD_ReadRegister(MFRC522::ComIrqReg),
                  rfid.PCD_ReadRegister(MFRC522::Status2Reg));
    delay(200);
  }
}

void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.startsWith("XFER|")) {
    showTransfer(field(cmd, 1) == "SEND", field(cmd, 2).toInt(), field(cmd, 3), field(cmd, 4), field(cmd, 5));
    return;
  }
  if (cmd.startsWith("DONE|")) {
    bool ok = field(cmd, 1) == "OK";
    showMessage(ok ? (lastTransferWasSend ? "Sent!" : "Received!") : "Failed", field(cmd, 2), 5000,
                ok ? LED_GREEN : LED_AMBER, Blink::Fast);
    if (ok) beep(1);
    return;
  }
  if (cmd.startsWith("WRITE|")) {
    pendingWriteName = field(cmd, 1);
    pendingWriteHex = field(cmd, 2);
    writePending = pendingWriteHex.length() % 2 == 0;
    Serial.printf("WRITEWAIT %u bytes %s: tap a MIFARE Classic 1K card\n", pendingWriteHex.length() / 2,
                  pendingWriteName.c_str());
    show("Tap card", "Hold a card here to write\n" + pendingWriteName);
    setLed(LED_PURPLE, Blink::Slow);
    screen = Screen::Message;
    messageUntil = millis() + 30000;
    return;
  }
  if (cmd == "WRITECANCEL") {
    writePending = false;
    showIdle();
    return;
  }
  if (cmd.startsWith("CARD|")) {
    String payload = cmd.substring(5);
    if (payload == "OFF") cardStop();
    else cardStart(payload);
    return;
  }
  cmd.toUpperCase();
  if (cmd == "HELLO") Serial.println("INFO AnyShare NFC bridge (ESP32 + Si512) v1");
  else if (cmd == "IDLE") showIdle();
  else if (cmd == "INFO") readerOk = checkReader();
  else if (cmd == "DUMP") dumpRegisters();
  else if (cmd == "SCAN") scanVerbose();
  else if (cmd == "BEEP") beep(1);
  else if (cmd == "SELFTEST") selfTest();
  else if (cmd == "CONFIGTEST") configTest();
  else if (cmd == "AUTOCOLLTEST") autocollTest();
  else if (cmd == "CARDDEBUG") cardDebug = !cardDebug;
  else if (cmd.startsWith("SLICE|")) {
    readerSlices = cmd.substring(6) != "0";
    Serial.printf("SLICE reader slices in card mode %s\n", readerSlices ? "on" : "off");
  }
  else if (cmd == "LEDON") {
    Serial.println("LEDTEST white (GPIO32, 33, 26 high) for 10 s");
    ledColor = {255, 255, 255, "white"};
    writeLed(true);
    delay(10000);
    ledColor = LED_GREEN;
    updateLed(true);
  } else if (cmd == "LEDTEST") {
    ledSelfTest(1000);
    updateLed(true);
  }
  else if (cmd.startsWith("REG ")) {
    uint8_t reg = strtol(cmd.substring(4, 6).c_str(), nullptr, 16);
    uint8_t val = strtol(cmd.substring(7, 9).c_str(), nullptr, 16);
    rfid.PCD_WriteRegister((MFRC522::PCD_Register)(reg << 1), val);
    Serial.printf("REG %02X <- %02X (now %02X)\n", reg, val, rfid.PCD_ReadRegister((MFRC522::PCD_Register)(reg << 1)));
  }
}

void loop() {
  if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));
  updateScreen();
  updateLed();
  if (!readerOk) {   // keep retrying so wiring can be fixed live
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 3000) {
      lastTry = millis();
      readerOk = checkReader();
    }
    return;
  }
  if (cardState != CardState::Off) {   // the laptop is receiving: mostly a card, briefly a reader
    cardPoll();
    // A phone field but no frame from it: AutoColl may be stuck after a broken anticollision. Re-arm it
    // (registers + Config, own field stays off) so the phone's next poll meets a fresh card.
    if (externalField() && cardState == CardState::Armed && millis() - lastCardFrameMs > 600 &&
        millis() - lastRearmMs > 600) {
      lastRearmMs = millis();
      ++rearmCount;
      cardStart(cardPayload, true);
    }
    if (cardState == CardState::Armed && millis() - cardSliceStart > CARD_SLICE_MS) {
      if (readerSlices && millis() >= cooldownUntil) {
        enterReaderRegisters();
        readerPollOnce();
        wr(reg::TxControl, 0x80);      // antenna off again
        cardStart(cardPayload, true);
      }
      cardSliceStart = millis();
    }
    return;
  }
  readerPollOnce();
}


// ---------------------------------------------------------------------------
// Small files on MIFARE Classic 1K cards (demo without an NFC phone).
// Layout: sectors 1..15, data blocks 0..2 of each sector (45 x 16 = 720 bytes).
// Stream = "ASH1" | length (2, big endian, name+data) | CRC-16/CCITT (2) | name length (1) | name | data
// ---------------------------------------------------------------------------
static const uint16_t CARD_CAPACITY = 45 * 16;
static const uint8_t CARD_HEADER = 9;

uint16_t crc16(const uint8_t* d, uint16_t n) {
  uint16_t crc = 0xFFFF;
  for (uint16_t i = 0; i < n; ++i) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

uint8_t dataBlock(uint16_t index) { return (1 + index / 3) * 4 + index % 3; }   // skips sector 0 and trailers

bool authSector(uint8_t block) {
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; ++i) key.keyByte[i] = 0xFF;
  uint8_t trailer = (block / 4) * 4 + 3;
  return rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, trailer, &key, &rfid.uid) == MFRC522::STATUS_OK;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Writes the pending file onto the card that was just selected.
void writeCard() {
  static uint8_t stream[CARD_CAPACITY];
  uint16_t dataLen = pendingWriteHex.length() / 2;
  uint8_t nameLen = pendingWriteName.length() > 40 ? 40 : pendingWriteName.length();
  uint16_t total = CARD_HEADER + nameLen + dataLen;
  if (total > CARD_CAPACITY) {
    Serial.printf("WRITEFAIL file too large for the card (%u of %u bytes)\n", total, CARD_CAPACITY);
    tapMessage("Too large", "This card holds about 700 bytes", false);
    return;
  }
  memset(stream, 0, sizeof(stream));
  memcpy(stream, "ASH1", 4);
  uint16_t body = nameLen + dataLen;
  stream[4] = body >> 8;
  stream[5] = body & 0xFF;
  stream[8] = nameLen;
  memcpy(stream + CARD_HEADER, pendingWriteName.c_str(), nameLen);
  for (uint16_t i = 0; i < dataLen; ++i) {
    int hi = hexVal(pendingWriteHex[2 * i]), lo = hexVal(pendingWriteHex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      Serial.println("WRITEFAIL bad data from the laptop");
      return;
    }
    stream[CARD_HEADER + nameLen + i] = (hi << 4) | lo;
  }
  uint16_t crc = crc16(stream + CARD_HEADER, body);
  stream[6] = crc >> 8;
  stream[7] = crc & 0xFF;

  uint16_t blocks = (total + 15) / 16;
  show("Writing card", pendingWriteName);
  setLed(LED_PURPLE, Blink::Fast);
  for (uint16_t i = 0; i < blocks; ++i) {
    uint8_t block = dataBlock(i);
    if ((i % 3 == 0 && !authSector(block))) {
      Serial.printf("WRITEFAIL card is locked (sector %u does not use the default key)\n", block / 4);
      tapMessage("Card locked", "Sector key is not the default FFFFFFFFFFFF", false);
      return;
    }
    if (rfid.MIFARE_Write(block, stream + i * 16, 16) != MFRC522::STATUS_OK) {
      Serial.printf("WRITEFAIL write error at block %u (keep the card still)\n", block);
      tapMessage("Write failed", "Keep the card still and try again", false);
      return;
    }
    Serial.printf("WPROG %u\n", (unsigned)((i + 1) * 100 / blocks));
  }
  writePending = false;
  Serial.printf("WRITTEN %u bytes %s\n", dataLen, pendingWriteName.c_str());
  showMessage("Written!", pendingWriteName + "\n" + String(dataLen) + " bytes on the card", 5000, LED_GREEN, Blink::Fast);
  beep(2);
}

// If the card holds an AnyShare file, prints CARDDATA name|hex. Returns true if it did.
bool readCardFile() {
  uint8_t buf[18];
  byte size = sizeof(buf);
  if (!authSector(dataBlock(0))) return false;   // locked card: nothing of ours on it
  if (rfid.MIFARE_Read(dataBlock(0), buf, &size) != MFRC522::STATUS_OK || memcmp(buf, "ASH1", 4) != 0) return false;
  uint16_t body = (buf[4] << 8) | buf[5];
  uint16_t crc = (buf[6] << 8) | buf[7];
  uint8_t nameLen = buf[8];
  if (body > CARD_CAPACITY - CARD_HEADER || nameLen > body) return false;
  static uint8_t stream[CARD_CAPACITY];
  memcpy(stream, buf, 16);
  uint16_t blocks = (CARD_HEADER + body + 15) / 16;
  for (uint16_t i = 1; i < blocks; ++i) {
    uint8_t block = dataBlock(i);
    if (i % 3 == 0 && !authSector(block)) return false;
    size = sizeof(buf);
    if (rfid.MIFARE_Read(block, buf, &size) != MFRC522::STATUS_OK) {
      Serial.println("ERROR card moved away while reading its file");
      return false;
    }
    memcpy(stream + i * 16, buf, 16);
  }
  if (crc16(stream + CARD_HEADER, body) != crc) {
    Serial.println("ERROR the file on this card is damaged (CRC mismatch)");
    return false;
  }
  String name;
  for (uint8_t i = 0; i < nameLen; ++i) name += (char)stream[CARD_HEADER + i];
  Serial.printf("CARDDATA %s|", name.c_str());
  for (uint16_t i = CARD_HEADER + nameLen; i < CARD_HEADER + body; ++i) Serial.printf("%02X", stream[i]);
  Serial.println();
  tapMessage("Card file", name + "\n" + String(body - nameLen) + " bytes read", true);
  return true;
}

// One reader-mode look for a card or phone (reader registers must be set).
void readerPollOnce() {
  if (millis() < cooldownUntil) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  Serial.print("TAG uid=");
  for (byte i = 0; i < rfid.uid.size; ++i) Serial.printf("%02X", rfid.uid.uidByte[i]);
  Serial.printf(" sak=0x%02X\n", rfid.uid.sak);
  String uid;
  for (byte i = 0; i < rfid.uid.size; ++i) uid += (rfid.uid.uidByte[i] < 0x10 ? "0" : "") + String(rfid.uid.uidByte[i], HEX);
  beep(1);

  if (rfid.uid.sak & 0x20) {
    rfid.PCD_WriteRegister(MFRC522::TModeReg, 0x00);    // ISO-DEP waits use millis(), not the 25 ms chip timer
    readPhone();
    rfid.PCD_WriteRegister(MFRC522::TModeReg, 0x80);
  } else {
    Serial.println("CARD plain NFC card/tag, not a phone (the reader works)");
    uid.toUpperCase();
    String type = String(MFRC522::PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak)));
    bool classic = rfid.PICC_GetType(rfid.uid.sak) == MFRC522::PICC_TYPE_MIFARE_1K;
    if (writePending && classic) writeCard();
    else if (!(classic && readCardFile())) tapMessage("Card read", "ID " + uid + "\n" + type, true);
    rfid.PCD_StopCrypto1();
  }
  rfid.PICC_HaltA();
  cooldownUntil = millis() + 1500;
}
