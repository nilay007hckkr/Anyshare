# How to Run and Test AnyShare

All commands to build and test every part of AnyShare on **Ubuntu** and **Arch Linux**.
Run every command from the **repository root** (the folder that contains `core-engine/`, `linux/` and `android/`) unless a step says otherwise.

> Verified on Ubuntu 22.04 (WSL2). Arch commands use the official Arch package names but were not run by us. If a package name differs on your system, search with `pacman -Ss <name>`.

| Step | Needs hardware? |
| :--- | :--- |
| 1 Install packages | no |
| 2 Core engine tests | no |
| 3 Linux CLI + desktop app + end-to-end tests | no |
| 4 Manual Linux -> Linux transfer (one or two PCs) | no |
| 5 JNI bridge test | no |
| 6 Bluetooth tests | Bluetooth adapter |
| 7 NFC reader tests (PN532, or HW-147C Si512 + ESP32) | NFC reader + Android phone |
| 8 Android app | Android SDK, phones |
| 9 Performance test | no |

---

## 0. Get the code

```bash
git clone https://github.com/nilay007hckkr/Anyshare.git
cd Anyshare
git checkout <branch-to-test>          # e.g. main, core-asp1-protocol, linux-cli, android-jni-integration
```

If the parts arrived as zips, unzip all three into the repo root:

```bash
unzip -o AnyShare-CoreEngine.zip -d .
unzip -o AnyShare-Linux.zip -d .
unzip -o AnyShare-Android.zip -d .
chmod +x tests/run_linux_tests.sh tests/jni/run_jni_desktop_test.sh android/gradlew
```

---

## 1. Install packages

### Ubuntu (22.04 / 24.04)

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config python3 unzip \
                    nlohmann-json3-dev \
                    libbluetooth-dev bluez \
                    libnfc-dev libnfc-bin \
                    qt6-base-dev libgl-dev \
                    openjdk-17-jdk
```

### Arch Linux

```bash
sudo pacman -Syu --needed base-devel cmake git pkgconf python unzip \
                          nlohmann-json \
                          bluez bluez-libs bluez-utils \
                          libnfc \
                          qt6-base \
                          jdk17-openjdk
```

What each package is for:

* `nlohmann-json` is optional; a copy is bundled in `core-engine/third_party/`.
* `bluez` / `libbluetooth` are needed only for Bluetooth.
* `libnfc` is needed only for the PN532 NFC reader.
* Qt (`qt6-base-dev` / `qt6-base`) is needed only for the desktop app `anyshare-gui`. Qt 5.12+ (`qtbase5-dev`) also works.
* The JDK is needed only for the JNI test and the Android build.

Check the tools:

```bash
g++ --version && cmake --version && java -version && python3 --version
```

---

## 2. Core engine (C++): build + automatic tests

```bash
cmake -S core-engine -B build-core -DCMAKE_BUILD_TYPE=Release
cmake --build build-core -j"$(nproc)"
./build-core/core_tests
```

Expected last line: **`112 checks, 0 failures`**. The test prints a few `Connect attempt ... failed` lines on purpose; that part tests connection failures.

A bigger stress file (default 256 MiB):

```bash
ANYSHARE_LARGE_MB=2048 ./build-core/core_tests
```

### Original demo programs (old API on the new protocol)

```bash
mkdir -p /tmp/demo && cd /tmp/demo
printf 'Hello from the AnyShare core engine. This is test file one.\n' > test1.txt
"$OLDPWD/build-core/receiver" 8080 /tmp/demo/inbox &      # ASP/1 receiver on 127.0.0.1:8080
sleep 0.5
"$OLDPWD/build-core/demo"                                 # sends test1.txt
wait
cmp test1.txt inbox/test1.txt && echo "DEMO OK"
cd "$OLDPWD"
```

`demo` prints `ERROR: warning: test1.txt does not match the checksum in its metadata`. That is expected: the hard-coded value is for a different `test1.txt`. The transfer itself is verified by the receiver ("Verified OK").

---

## 3. Linux CLI: build + full end-to-end test

### Build

```bash
cmake -S linux -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux -j"$(nproc)"
./build-linux/anyshare            # usage; the last line shows "Built with: TCP, Bluetooth RFCOMM (BlueZ), NFC (libnfc)"
```

If Bluetooth or NFC says *disabled* during `cmake`, the dev packages from step 1 are missing. To make the build fail instead of silently disabling them:

```bash
cmake -S linux -B build-linux -DANYSHARE_WITH_BLUEZ=ON -DANYSHARE_WITH_LIBNFC=ON
```

### Automatic end-to-end tests (Linux <-> Linux over loopback)

```bash
bash tests/run_linux_tests.sh                   # builds into /tmp/anyshare-build
# or choose the build folder:
bash tests/run_linux_tests.sh "$PWD/build-e2e"
```

Expected: **`Result: 33 passed, 0 failed`** (29 without Qt installed; the desktop-app checks are then skipped). Port 8080 must be free: stop any running AnyShare receiver first. It tests:

* simulated tap with token auto-accept;
* duplicate names (no overwrite);
* declining an offer without a token;
* NFC hardware missing -> fallback;
* the Android payload format;
* a missing file, a refused connection and a malformed handover;
* the old `demo` program;
* finding receivers by name (UDP discovery, `anyshare discover`, `send --to NAME`);
* the ESP32 OLED bridge protocol (a pseudo-terminal plays the ESP32: handshake, messages, unplug, real transfer);
* the desktop app, headless: the hidden menu needs three quick right-clicks, no NFC text without a reader, real send / demo-tap send / accept and decline, CLI -> GUI transfer, clean exit on Ctrl-C.

### Desktop app (Qt window)

```bash
bash linux/run_gui.sh               # builds anyshare-gui if needed, then opens the window
bash linux/run_gui.sh --receive     # open and start receiving at once
```

The window has two tabs, like LocalSend / Quick Share:

* **Send:** drop files anywhere on the window (or *Add files*; double-click a file to remove it). *Nearby devices* fills itself every few seconds with phones and PCs in receive mode; click one to send. *Send to an address...* is the manual fallback.
* **Receive:** *Start receiving* makes this computer visible by name (pulsing circle). Received files are listed on the right; double-click shows text and images in a preview inside the app, other files open in the default app (on WSL: the Windows app, via `explorer.exe`).
* **Transfer animation** (left of the transfer panel): this computer and the peer (laptop or phone icon, with its name); packets fly from the sender to the receiver while data flows, purple when sending and red when receiving, then a check (done) or a cross (failed). The Android app shows the same animation on both tabs.
* **Transfer panel** (always visible): a state pill in the ESP32 LED colours (Sending purple, Receiving red, Done green, Failed amber), progress, speed, time left, current file and the real transport/handover. The activity log is under *Show details*.

The terminal you started it from keeps the engine log; close the window or press Ctrl+C to quit.

* **Hidden mode menu:** right-click **three times quickly** in the empty **top-right corner** of the window. It offers Hardware NFC (only when a reader was found), Bluetooth fallback / Manual address, NFC-style demo, the demo partner address and the NFC reader settings.
* **Linux can send:** add files, press **Send**, type the receiver's address (phone: AnyShare -> Receive shows it, e.g. `192.168.1.5:8080`).
* **Demo mode:** a SIMULATED tap (clearly labelled) followed by a real Wi-Fi / Bluetooth transfer to the saved partner.
* Without an NFC reader the window does not mention NFC at all.
* On Ubuntu with both Qt 5 and Qt 6 installed, CMake may pick Qt 5 (works fine). To force Qt 6 in a new build folder:
  `EXTRA_CMAKE_ARGS="-DQT_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt6" bash linux/run_gui.sh`
* WSL on Windows 11 shows the window through WSLg; no extra setup is needed.

Headless GUI tests only: `QT_QPA_PLATFORM=offscreen ./build-linux/anyshare-gui-tests` -> `Totals: 18 passed, 0 failed`. With `ANYSHARE_SCREENSHOT_DIR=DIR` they also save screenshots of each screen.

### Pick the receiver by name (no IP typing)

A receiver in receive mode answers a small UDP broadcast (port **48080**) with its name. Senders list it by name:

```bash
./build-linux/anyshare receive                       # prints: Discoverable as 'NILAY' (UDP 48080)
./build-linux/anyshare discover                      # NILAY   192.168.1.9:8080  (linux)
./build-linux/anyshare send --to nilay photo.jpg     # exact name or a unique prefix, any case
```

* **Android:** *Send* opens a list; laptops and phones in receive mode appear at the top by name while it is open (e.g. `NILAY (Linux · 192.168.1.9)`). Phones in receive mode answer too, with the phone's device name.
* **Desktop app:** the *Send to* dialog lists them the same way; you can still type an address. Rename this computer from the hidden menu (*This computer's name...*).
* Found devices do not carry the NFC token, so the receiver still asks **Accept**. Turn it off on a receiver with `--no-discovery`.
* Firewalls must allow **UDP 48080** in addition to the TCP port (`sudo ufw allow 48080/udp`).
* **WSL:** in the default NAT mode UDP broadcasts never reach WSL, so phones cannot find it by name. Use WSL *mirrored* networking (Windows 11 22H2+): put `[wsl2]` / `networkingMode=mirrored` in `%UserProfile%\.wslconfig`, run `wsl --shutdown`, remove the old `netsh portproxy` for 8080, and allow TCP 8080 + UDP 48080 inbound.

---

## 4. Manual Linux -> Linux transfers

### 4a. One PC, two terminals

**Terminal 1 (receiver):**
```bash
./build-linux/anyshare receive --dir /tmp/inbox --handover-out /tmp/tap.json
```
It prints `Listening: TCP 0.0.0.0:8080` and a handover payload.

**Terminal 2 (sender):**
```bash
head -c 50000000 /dev/urandom > /tmp/test.bin

# (a) simulated tap: uses the token -> accepted automatically
./build-linux/anyshare send --handover-file /tmp/tap.json /tmp/test.bin

# (b) manual address: terminal 1 asks "Accept? [y/N]" -> type y
./build-linux/anyshare send --tcp 127.0.0.1:8080 /tmp/test.bin
```

Check the result:
```bash
sha256sum /tmp/test.bin /tmp/inbox/test.bin        # both hashes must be equal
ls /tmp/inbox                                      # second copy is "test (1).bin" (never overwritten)
```

Stop the receiver with **Ctrl+C**.

### 4b. Ubuntu PC <-> Arch PC (same Wi-Fi / LAN)

Find each PC's IP address with `ip -4 addr` (look for `192.168.x.x` or `10.x.x.x`).

**Receiver (e.g. Arch):**
```bash
./build-linux/anyshare receive --handover-out ~/tap.json
```

Open port 8080 if a firewall is running:
```bash
# Ubuntu (ufw):
sudo ufw allow 8080/tcp
# Arch (only if you enabled ufw / firewalld):
sudo ufw allow 8080/tcp        # or: sudo firewall-cmd --add-port=8080/tcp
```

**Sender (e.g. Ubuntu):**
```bash
# Option 1: manual address -> answer "y" on the receiver
./build-linux/anyshare send --tcp <RECEIVER_IP>:8080 ~/Pictures/photo.jpg ~/Documents/notes.pdf

# Option 2: simulated tap -> copy the payload over, no question asked
scp <user>@<RECEIVER_IP>:~/tap.json /tmp/tap.json
./build-linux/anyshare send --handover-file /tmp/tap.json ~/Pictures/photo.jpg
```

Then swap roles (Ubuntu receives, Arch sends) and repeat.

### 4c. Failure tests to try by hand

```bash
./build-linux/anyshare send --tcp 127.0.0.1:8080 /no/such/file             # "file not found", nothing sent
./build-linux/anyshare send --tcp 127.0.0.1:9 --connect-timeout 5 /tmp/test.bin   # gives up after 5 s
# Start a large transfer, then press Ctrl+C on the sender OR the receiver:
#   -> "Cancelled" / "connection lost", and no partial file in the inbox.
# Answer "n" on the receiver -> the sender prints "receiver declined".
```

---

## 5. JNI bridge test (Android's C++ <-> Java code, on a PC)

Runs the real Android JNI code (`anyshare_jni.cpp` + `NativeBridge.java`) on the desktop JVM with `-Xcheck:jni`:

```bash
# Ubuntu
JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64 bash tests/jni/run_jni_desktop_test.sh
# Arch
JAVA_HOME=/usr/lib/jvm/java-17-openjdk bash tests/jni/run_jni_desktop_test.sh
```

Expected: **`33 checks, 0 failures`**. The build output goes to `tests/jni/build/` (git-ignored).

---

## 6. Bluetooth (needs a Bluetooth adapter)

### Turn Bluetooth on (both distros)

```bash
sudo systemctl enable --now bluetooth
bluetoothctl power on
./build-linux/anyshare bt-info               # prints your adapter address + AnyShare UUID
```

### Enable the SDP compatibility mode (needed for receiving from Android)

Android finds the AnyShare service through SDP, which needs `bluetoothd --compat`:

```bash
systemctl cat bluetooth | grep ExecStart     # note the bluetoothd path, e.g. /usr/lib/bluetooth/bluetoothd
sudo systemctl edit bluetooth
```

In the editor, add these lines (use the path you saw above), then save:

```ini
[Service]
ExecStart=
ExecStart=/usr/lib/bluetooth/bluetoothd --compat
```

```bash
sudo systemctl daemon-reload && sudo systemctl restart bluetooth
```

### Pair two devices (once)

```bash
bluetoothctl
  scan on                 # wait for the other device to appear
  pair XX:XX:XX:XX:XX:XX
  trust XX:XX:XX:XX:XX:XX
  scan off
  quit
bluetoothctl devices Paired
```

### Without a shared Wi-Fi: NFC tap + Bluetooth

The NFC tap only exchanges the receiver's address and a one-time token; the file then goes over Wi-Fi when both devices reach each other, otherwise over Bluetooth. Bluetooth Classic is slow: we measured about 200 KiB/s (a 1 MB photo takes about 5 s), which is normal for RFCOMM. For large files use a shared Wi-Fi, or the phone's hotspot.

1. **Pair the phone with the laptop once** (phone Bluetooth settings, or `bluetoothctl` -> `scan on` -> `pair <MAC>` -> `trust <MAC>`). Android phones cannot share their own Bluetooth address, only their name; the laptop finds a *paired* phone by name instantly (`bluetoothctl devices Paired`).
2. **Laptop -> phone:** phone in **Receive**, laptop has files in the Send tab, hold the phone on the ESP32 reader. If the phone is not on the laptop's Wi-Fi, the laptop connects over Bluetooth to the paired phone.
3. **Phone -> laptop by tap:** phone in **Send by NFC tap**, hold it on the ESP32 reader. The laptop fetches the files (section 7b, "the laptop fetches"): Wi-Fi if both share one, otherwise Bluetooth to the paired phone.
4. **Phone -> laptop without NFC:** start **Receive** on the laptop with *Wi-Fi + Bluetooth* (the default when the laptop has Bluetooth); the phone picks the laptop in **Bluetooth fallback** mode. For phones to find the laptop's Bluetooth service, bluetoothd needs `--compat` (see above).
5. **Verified on hardware (2026-10-14, WSL with the laptop's Realtek adapter, Samsung Galaxy M34 with Wi-Fi off):** pairing; **laptop -> phone by NFC tap over Bluetooth** (`bt_test.txt`, checksum verified, connected in under 1 s via the paired name). Phone -> laptop over Bluetooth failed in the old APK with `SecurityException: need BLUETOOTH_SCAN` (`cancelDiscovery()` on Android 12+); with the fixed APK, **phone -> laptop in Bluetooth fallback mode** (503.7 KiB photo, verified) and **phone -> laptop by NFC tap** (section 7b) both work.

### Linux <-> Linux over Bluetooth

```bash
# Receiver PC
./build-linux/anyshare receive --transport rfcomm --dir /tmp/bt-inbox
#   prints e.g. "Listening: Bluetooth RFCOMM AA:BB:CC:DD:EE:FF channel 1"

# Sender PC
./build-linux/anyshare send --bt AA:BB:CC:DD:EE:FF /tmp/test.bin      # finds the channel via SDP
./build-linux/anyshare send --bt AA:BB:CC:DD:EE:FF@1 /tmp/test.bin    # or give the channel directly
```

### Linux -> Android over Bluetooth

On the phone, open AnyShare and tap **Receive**. Then:

```bash
./build-linux/anyshare send --bt <PHONE_BT_MAC> photo.jpg
```

Android apps cannot show their own MAC address; get it from `bluetoothctl devices Paired`.

---

## 7. NFC readers (needs a reader + Android phone)

Which module do you have? Read the chip name on the board:

| Board | Chip | How it connects to AnyShare |
| :--- | :--- | :--- |
| PN532 V3 (red/blue) | NXP PN532 | Directly via libnfc (7a) |
| **HW-147C** | **Si512** (MFRC522/PN512 register family) | **Not libnfc-compatible.** Use an ESP32 with `linux/esp32-nfc-bridge` (7b) |

## 7a. PN532 reader

### Connect the PN532

* **USB-UART adapter:** set the PN532 DIP switches to **HSU**, plug it in, then run `ls /dev/ttyUSB*`.
* Give your user access to the serial port, then **log out and back in**:

```bash
# Ubuntu
sudo usermod -aG dialout $USER
# Arch
sudo usermod -aG uucp $USER
```

### Check the reader

```bash
./build-linux/anyshare nfc-probe pn532_uart:/dev/ttyUSB0
# Must say "Opened: ...". If it says "NFC hardware NOT available", check wiring, DIP switches and group.
nfc-list            # optional libnfc tool (Ubuntu package libnfc-bin)
```

Other connections: `pn532_i2c:/dev/i2c-1`, `pn532_spi:/dev/spidev0.0`.

### Tap test: Linux sends to a phone

1. On the phone: AnyShare -> **Receive** (NFC must be on).
2. On Linux:

```bash
./build-linux/anyshare send --nfc --nfc-device pn532_uart:/dev/ttyUSB0 photo.jpg
#   "[NFC] ... ready. Hold the receiving phone ... on the reader"
```

3. Hold the phone on the reader. Expect `Handover: physical NFC tap (libnfc ...)`, then the transfer (Bluetooth, or Wi-Fi with `--prefer-tcp`).

With automatic fallback if the reader is missing:

```bash
./build-linux/anyshare send --nfc --nfc-device pn532_uart:/dev/ttyUSB0 \
                            --fallback-tcp <PHONE_IP>:8080 photo.jpg
```

> A phone tapping a **Linux receiver** is not implemented yet (the PN532 works only as a reader).

## 7b. HW-147C (Si512) + ESP32: module check

1. DIP switches on the module: **1 = OFF, 2 = ON** (SPI), as in the module's datasheet.
2. Wiring: use the 8-pin header on the module's left edge (RST, IRQ, GND, VCC, SS, MOSI, MISO, SCK). Power it from **3.3 V** so its signals match the ESP32. The 4-pin bottom header (GND/VCC/SCL/SDA) is for I2C/UART and stays unused.

   | Part / pin | ESP32 | Note |
   | :--- | :--- | :--- |
   | HW-147C SCK | GPIO18 | VSPI clock |
   | HW-147C MISO | GPIO19 | VSPI MISO |
   | HW-147C MOSI | GPIO23 | VSPI MOSI |
   | HW-147C SS | GPIO5 | chip select |
   | HW-147C RST | GPIO4 | reset (kept HIGH while running) |
   | HW-147C IRQ | - | not used |
   | HW-147C VCC / GND | 3V3 / GND | |
   | OLED SDA / SCL | GPIO21 / GPIO22 | optional SSD1306 128x64, I2C 0x3C |
   | Buzzer + / SIG | GPIO25 | optional; beeps once per tag, twice per phone |
   | RGB LED red leg | GPIO32 via **220 Ω** | optional status light (5 mm RGB) |
   | RGB LED green leg | GPIO33 via **100 Ω** | |
   | RGB LED blue leg | GPIO26 via **100 Ω** | |
   | RGB LED **longest leg** | **3V3** (common anode) or GND (common cathode) | our LEDs were sold as common cathode but are **common anode**: longest leg on 3V3, `LED_COMMON_ANODE = true` in the sketch. For a true common-cathode LED put it on GND and set it to `false` |

   Never connect an LED leg to a GPIO without its resistor. 220 Ω on all three also works (green and blue are then dimmer). The usual leg order is red, GND (longest), green, blue; at boot the LED shows **red, green, blue** for 0.3 s each. If the colours come out in another order, swap those two wires (or send `LEDTEST` for a slow repeat).

3. Flash `linux/esp32-nfc-bridge/esp32-nfc-bridge.ino` (Arduino IDE or arduino-cli, board "ESP32 Dev Module", libraries **MFRC522** and **Adafruit SSD1306**):

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install MFRC522 "Adafruit SSD1306"
arduino-cli compile --fqbn esp32:esp32:esp32 linux/esp32-nfc-bridge
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 linux/esp32-nfc-bridge
arduino-cli monitor -p /dev/ttyUSB0 -c baudrate=115200
```

4. Read the serial output:

| Line | Meaning |
| :--- | :--- |
| `READER OK version=0x..` | The ESP32 talks to the Si512: wiring is correct |
| `READER ERROR no answer over SPI` | Wiring, power or DIP switches are wrong |
| `TAG uid=... sak=0x08` + `CARD plain NFC card` | A card was read: the antenna works (verified with a MIFARE Classic card, UID 7672F906) |
| `TAG ... sak=0x20` + `PAYLOAD {...}` | An AnyShare phone in Receive mode was read: the full NFC handover works |
| `ERROR phone answered SW 6A82` | The phone is not in Receive mode |

Diagnostics: send `DUMP` (all registers), `SCAN` (25 card-search attempts with status), `REG aa vv` (write a register live), `BEEP` or `LEDTEST` over the serial port.

> Why the firmware sets `ControlReg = 0x10`: the Si512 reports version `0x82` like NXP's PN512, which starts as an NFC *target*. Without the Initiator bit it never transmits, so no card is found (seen on our board: `SCAN ... Timeout`, then `TAG` immediately after setting the bit). The MFRC522 library does not set this bit.

### OLED status screen during transfers

With the ESP32 connected to the laptop, the app shows every transfer on the OLED:

| OLED shows | When |
| :--- | :--- |
| `Ready` / `Tap an NFC card or an NFC phone` | idle (also 5 s after every result) |
| `Card read` / `ID 7672F906` / `MIFARE 1KB` | a card was tapped (4 s) |
| `Receiving 42%` + bar + file name + type (`Image`, `Video`, `PDF`, ...) + `1/3` | a file is being received |
| `Sending 42%` ... | a file is being sent |
| `Received!` / `Sent!` / `Failed` + summary | the transfer ended (5 s, then `Ready`) |
| `No update` | the laptop app stopped reporting for 15 s |

The RGB LED follows the same states:

| LED | Meaning |
| :--- | :--- |
| solid green | ready / idle |
| blinking red | receiving (phone or PC -> this laptop) |
| blinking purple | sending (this laptop -> phone or PC) |
| fast green, 5 s | transfer finished OK |
| solid cyan, 4 s | a card or phone was tapped |
| fast amber, 5 s | error: transfer failed, tap failed, laptop stopped reporting |
| solid amber | no reader found (check the reader wiring) |

```bash
bash linux/run_gui.sh                                   # the desktop app finds the ESP32 automatically
./build-linux/anyshare receive --esp32 auto             # CLI receiver with the OLED
./build-linux/anyshare send --esp32 auto --tcp IP:PORT file.jpg
```

Only USB serial chips used on ESP32 boards (CP210x, CH34x, FTDI, Espressif) are probed, and only a device that answers `HELLO` with the AnyShare greeting is used. Turn it off with `--esp32 off`, or in the desktop app's hidden menu (*ESP32 display port...*). A missing or unplugged ESP32 never affects a transfer.

**WSL:** the ESP32 must be attached to WSL first (Windows PowerShell; `bind` once as Administrator):

```powershell
usbipd list                          # find the CP2102 / CH340 BUSID, e.g. 1-4
usbipd bind --busid 1-4              # once, as Administrator
usbipd attach --wsl --busid 1-4      # after every replug or WSL restart
```

### NFC taps in the desktop app

The desktop app also **reads** the ESP32 reader:

* **Card / tag:** the status line shows `NFC card tapped · ID 7672F906 · MIFARE Classic 1K`; the NFC line keeps the last tap. Nothing is sent.
* **Phone in Receive mode (`PAYLOAD`):** this is a real NFC handover. With files selected the transfer starts at once, labelled `physical NFC tap (ESP32 + Si512 reader)`, and the phone accepts automatically (the tap carries its one-time token). Without files the app asks you to add files and tap again.
* **Phone in Send by NFC tap (`PAYLOAD` with `"role":"send"`):** the laptop connects to the phone and fetches its files (see "the laptop fetches" below).
* **Failed tap:** `NFC tap failed: ...` (for example the phone is on neither screen: answers 6A82).

> Verified with a real phone (Samsung Galaxy M34, 2026-10-14): the module reads the phone's payload (Receive) and its 6A82 when AnyShare is not ready. Laptop -> phone then needs a shared Wi-Fi or a paired phone.

### Demo without an NFC phone: files on a MIFARE Classic 1K card

A blank MIFARE Classic 1K card (default key `FFFFFFFFFFFF`) can stand in for a phone. The data really travels over NFC; the UI calls it an NFC card, not a phone.

* **Laptop -> card:** Send tab, select one small file (or none, to type a note), click **NFC card (tap on the reader)** in *Nearby devices*, tap the card. The OLED shows *Writing card* then *Written!*; the app shows *Written to NFC card ...*.
* **Card -> laptop:** in Receive mode, tap a card that holds a file. It is saved to the inbox (never overwriting: `note (1).txt`) and listed under *Received files*. Tapping the same card again does not save another copy of identical content.
* Capacity: about 700 bytes (45 blocks of sectors 1-15; sector 0 and the key blocks are not touched). Larger files are refused before writing. Cards with other keys (metro cards and similar) report *card is locked*.
* Firmware commands: `WRITE|name|hex`, answered by `WPROG n`, `WRITTEN n bytes name` or `WRITEFAIL reason`; a tapped card with a file prints `CARDDATA name|hex`. The file carries a CRC-16, checked on every read.
* Verified on our hardware (2026-10-14): a 78-byte `hello.txt` and a 19-byte typed note were written to card 7672F906 from the app and read back into the inbox.

### Phone -> laptop by NFC tap: the laptop fetches ("pull")

The ESP32 module reads phones reliably, but phones cannot reliably read the module (see card mode below). So for phone -> laptop the roles of the tap are swapped, and only the direction that works is used:

1. Phone: AnyShare, **Hardware NFC** mode, add files, **Send by NFC tap**. The phone opens a Wi-Fi (TCP) and a Bluetooth (RFCOMM) server and its HCE service serves `{"v":1,"role":"send","transport":..,"name":..,"ip":..,"port":..,"token":..}`. It also keeps looking for a phone in Receive, so phone -> phone taps still work.
2. Hold the phone on the ESP32 module. The module reads the payload (`PAYLOAD ...`, OLED: *Phone read / Fetching its files...*).
3. The laptop app (Receive mode not needed) connects to the phone: Wi-Fi first (3 s when Bluetooth is also offered), then Bluetooth to the **paired** phone by name. It sends `PULL` with the token (`"ASPP" u8 version u16 len {"token","receiver"}`, see `Protocol.hpp`); the phone checks the token and sends exactly as usual, the laptop accepts without asking because the same token is in the offer.
4. Someone else on the Wi-Fi who connects to the phone's port without the token gets nothing (not even file names); the phone keeps waiting. The ESP32 reads a phone several times while it is held: the laptop fetches once per tap.

Tests: `core_tests` (pull with a stranger, a garbage peer and the right token), `gui_tests` `phoneTapWithFilesIsFetched` (fake ESP32 + fake phone), `JniBridgeTest` pull (the Android JNI code, PULL frame written by hand in Java).

**Verified on hardware (2026-10-14, Samsung Galaxy M34, phone Wi-Fi off, laptop Bluetooth in WSL):** two taps, 2 files (981.6 KiB) and 1 file (268.2 KiB), fetched over Bluetooth about 1 s after the tap, all checksums verified, one fetch per tap, no Accept question. Bluetooth speed in WSL (adapter via usbipd) was about 200 KiB/s.

### Card mode (EXPERIMENTAL, not reliable on our module)

While the laptop is in **Receive** mode the app also sends its handover to the ESP32 (`CARD|{json}`), and the module tries to behave like an NFC card with the AnyShare AID, so a phone that *reads* could get the laptop's address. *Stop receiving* sends `CARD|OFF`.

* The module alternates: about 700 ms as a card, then a short reader poll, so cards and phones are still read while receiving (verified with two MIFARE cards and a phone).
* It uses the PN512 card-operation commands (`Config`, `AutoColl`). On our board `AUTOCOLLTEST` says AutoColl is present; `SELFTEST` shows the Si512 is **not** a bit-exact PN512 (1/64 bytes).
* Result with a Samsung Galaxy M34 (2026-10-14): the module senses the phone's field (`CARDDBG field present`), and once the phone's Android tag check got as far as RATS/ATS and an NDEF SELECT (answered 6A82). With the phone in AnyShare reader mode, 4 captures of 20+ s produced **no** frame at all. Fixes tried: one card UID per boot (it changed every 0.7 s), re-arming AutoColl when a field is present but no frame arrives. Hence the pull method above.
* Debug: `CARDDEBUG` toggles `CARDDBG RX/TX/field` lines; `SLICE|0` disables the reader polls (card only); load modulation can be tuned with `REG 23 xx` (default `6F`).

---

## 8. Android app

### Install Android Studio + SDK

```bash
# Ubuntu
sudo snap install android-studio --classic
# Arch (AUR, e.g. with yay)
yay -S android-studio
```

Open Android Studio, then go to **More Actions -> SDK Manager**:

* *SDK Platforms*: **Android API 37**
* *SDK Tools*: **NDK (Side by side)**, **CMake 3.22.1**, **Android SDK Platform-Tools**

Then open the `android/` folder as a project.

### Build, unit-test, install (command line)

```bash
cd android
echo "sdk.dir=$HOME/Android/Sdk" > local.properties     # path to your SDK (never commit this file)
./gradlew assembleDebug                                 # builds Java + libanyshare_jni.so
./gradlew test                                          # JUnit tests (ApduProtocolTest)
```

Connect the phone over USB with *Developer options -> USB debugging* turned on, then:

```bash
adb devices                                             # the phone must be listed
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -s AnyShare AnyShare.Transfer AnyShare.NFC AnyShare.HCE AnyShare.BT
cd ..
```

`adb` comes with the SDK in `~/Android/Sdk/platform-tools/`. It is also available as a package: Ubuntu `sudo apt install adb`, Arch `sudo pacman -S android-tools`.

> The Android code has not been compiled yet (no SDK on the dev machine). If `assembleDebug` fails, fix the reported errors first.

### Phone test checklist

| # | Test | Steps | Expected |
| :-- | :--- | :--- | :--- |
| 1 | Android -> Android, NFC | Pair the phones. B: **Receive**. A: choose files -> **Send** -> hold the phones back to back | No Accept dialog on B; "checksums verified"; files in `Downloads/AnyShare` |
| 1b | Android -> Linux, NFC tap on the ESP32 | Laptop app open, ESP32 connected. Phone: Hardware NFC -> add files -> **Send by NFC tap** -> hold on the module | OLED *Fetching its files...*; laptop: "Received N file(s)", transport "fetched after an NFC tap"; phone: "Sent" |
| 2 | Android -> Android, Bluetooth fallback | Long-press the top-right of the header -> **Bluetooth fallback**. A: Send -> pick B | Accept dialog on B; mode badge says Bluetooth fallback |
| 3 | Demo mode | Mode -> **NFC-style demo**. A: Send -> pick demo partner (paired phone, or "Wi-Fi: enter IP address" for a laptop receiver) -> tap the animation card | Screen says "SIMULATED tap"; a real Bluetooth or Wi-Fi transfer; Accept dialog on the receiver |
| 4 | Back to normal | Mode -> **Hardware NFC** | Badge shows Hardware NFC |
| 5 | Android -> Linux (Wi-Fi) | PC: `./build-linux/anyshare receive`. Phone: Send -> "Wi-Fi: enter IP address" -> `<PC_IP>:8080` | PC asks Accept; files in `~/Downloads/AnyShare` |
| 6 | Linux -> Android (Wi-Fi) | Phone: **Receive** (it shows its IP:port). PC: `./build-linux/anyshare send --tcp <PHONE_IP>:8080 file` | Accept dialog on phone; file in Downloads/AnyShare |
| 7 | Linux -> Android (Bluetooth) | See section 6 | Same as test 6 |
| 8 | No NFC | Turn NFC off -> Send in Hardware NFC mode | "NFC unavailable" dialog offering Bluetooth fallback |
| 9 | Failures | Deny Bluetooth permission / turn Bluetooth off / walk away / press Cancel | Error or "Cancelled"; never a success message; no half file |

Compare file hashes after a phone transfer:

```bash
adb shell sha256sum /sdcard/Download/AnyShare/<file>
sha256sum <original file>
```

---

## 9. Performance test (2 GiB)

```bash
# Arch only: the GNU time tool is a separate package
sudo pacman -S --needed time

head -c 2147483648 /dev/urandom > /tmp/big.bin
/usr/bin/time -v ./build-linux/anyshare receive --bind 127.0.0.1 --once --dir /tmp/perf-in \
      --handover-out /tmp/perf.json 2> /tmp/rx-time.txt &
sleep 1
/usr/bin/time -v ./build-linux/anyshare send --handover-file /tmp/perf.json /tmp/big.bin 2> /tmp/tx-time.txt
wait
grep -E "Elapsed|Maximum resident" /tmp/tx-time.txt /tmp/rx-time.txt
cmp /tmp/big.bin /tmp/perf-in/big.bin && echo "IDENTICAL"
rm -rf /tmp/big.bin /tmp/perf-in
```

Our result (WSL2, loopback): about **2.3 s** and about **4.9 MB** peak RAM on each side. Over real Wi-Fi or Bluetooth it will be slower. Please write down your numbers in README section 6.

---

## 10. Troubleshooting

| Problem | Fix |
| :--- | :--- |
| `cmake` says Bluetooth/NFC *disabled* | Install the packages from step 1 (Ubuntu: `libbluetooth-dev libnfc-dev`; Arch: `bluez-libs libnfc`), delete the build folder, run cmake again |
| `Address already in use` on port 8080 | Another receiver is running: `ss -ltnp \| grep 8080`, or use `--port 9090` |
| Sender: `receiver declined: ... no token` | The receiver has no terminal to answer. Run it in a terminal, use `--handover-file`, or add `--yes` (tests only) |
| `Connection refused` / timeout between two PCs | Wrong IP, different networks, or a firewall: `sudo ufw allow 8080/tcp` |
| `AnyShare service not found` (Bluetooth) | Receiver not running, or SDP missing: enable `--compat` (section 6) or use `--bt ADDR@CHANNEL` |
| `no Bluetooth adapter found` | `sudo systemctl start bluetooth`, `bluetoothctl power on`, `rfkill unblock bluetooth` |
| `no NFC reader could be opened` | DIP switches to HSU, correct `/dev/ttyUSB*`, user in `dialout` (Ubuntu) / `uucp` (Arch), re-login |
| `Unable to find USB busses` messages from libnfc | Harmless when using a UART/I2C/SPI reader |
| Android: `Native transfer engine failed to load` | NDK/CMake not installed, or the build failed for your phone's ABI |
| `Permission denied` running a script | `chmod +x tests/*.sh tests/jni/*.sh android/gradlew` |

### Clean up

```bash
rm -rf build-core build-linux build-e2e /tmp/anyshare-build tests/jni/build android/app/build android/.gradle /tmp/inbox /tmp/tap.json /tmp/test.bin
```
