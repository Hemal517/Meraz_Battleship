# Meraz Battleship — Project Guide

A 4-team Battleship game played on ESP32 boards. Each team shoots at the
other three; a **Central** ESP32 is the referee; a laptop **dashboard**
shows the whole war on a projector. Everything runs offline over ESP-NOW
(no router, no internet, no cloud).

---

## 0. File map — what each file is and who uses it

```
   TEAM BOARDS                    REFEREE                      LAPTOP
 participant_node.ino  --ESP-NOW--> central_node.ino --USB--> dashboard.py --> projector
   (+ VirtualTFT.h)                  (+ game_logic.h)
        |  USB
        v
 virtual_display.py  (fake touch screen in a browser, for use before the real TFTs arrive)
```

### Organizer side (you)
| File | What it is | Runs on |
|---|---|---|
| `central_node.ino` | The referee. Owns all grids, turns, eliminations. Saves state to flash so a power blip doesn't lose the match. | Central ESP32 |
| `game_logic.h` | **All the game rules** in one header (ship validation incl. diagonals, registration, attacks, turn order). Must sit in the same folder as `central_node.ino`. | (included by Central and the test) |
| `participant_node.ino` | The finished team-board firmware with the touch UI. Same file for all 4 boards; only the CONFIGURATION block changes. | 4 team ESP32s |
| `VirtualTFT.h` | Stand-in for the display driver. Only used when `USE_VIRTUAL_DISPLAY` is switched on. Keep next to `participant_node.ino`. | team ESP32 |
| `dashboard.py` | Projector dashboard + organizer buttons (START, FORCE START, SKIP TURN, RESET) + per-round match logs. | laptop |
| `virtual_display.py` | A fake 240×320 touch screen in your browser, fed by a team board over USB. | laptop |
| `mock_central.py` | A fake Central so you can test `dashboard.py` with no ESP32. | laptop |
| `central_logic_test.cpp` | Desktop test of the rules (77 checks, no hardware). | laptop |

### Participant side (what you hand out)
| File | What it is |
|---|---|
| `participant_starter.ino` | Skeleton: message formats + a numbered TODO list. **No ESP-NOW code** — learning that is part of the challenge. |
| `PARTICIPANT_GUIDE.md` | Everything Central expects: grid and ship rules, byte-level message formats, game flow, example bytes, troubleshooting. |

> **Do not hand out** `participant_node.ino` — it contains the full radio
> setup that participants are meant to work out themselves.

---

## 1. Hardware

- 5× ESP32 dev boards (generic ESP32-WROOM-32; Arduino board "ESP32 Dev Module"): 1 Central + 4 team boards
- 5× data-capable USB cables, a laptop with Arduino IDE + Python 3
- USB power banks (or a multi-port charger) for the 4 team boards during the event
- 4× 2.4" SPI touch TFT (ILI9341, 240×320, resistive touch)
- Optional: projector

**TFT wiring** (VSPI; verify against the labels printed on *your* module):

| TFT pin | ESP32 GPIO |
|---|---|
| VCC / GND | 3.3V / GND |
| CS / RESET / DC | 5 / 4 / 2 |
| SDI (MOSI) / SCK / SDO (MISO) | 23 / 18 / 19 |
| LED (backlight) | 3.3V |
| T_CS / T_IRQ | 15 / 27 |
| T_CLK / T_DIN / T_DO | shared with SCK / MOSI / MISO |

**When the real displays arrive** (not needed before then):
1. Install the TFT_eSPI library and paste the pin block from the top of
   `participant_node.ino` into TFT_eSPI's own `User_Setup.h` (once per laptop).
2. Run TFT_eSPI's `Touch_calibrate` example on **each** board and paste its
   5 numbers into `touchCalData[]` (every touch panel is slightly different).
3. Comment out `#define USE_VIRTUAL_DISPLAY` in `participant_node.ino`.

---

## 2. Testing ladder — start here, no display needed

Work through these in order. Each level needs a bit more hardware than the last.

### Level 1 — no ESP32 at all
**a) Test the game rules.** In the folder with `game_logic.h`:
```
g++ -std=c++11 -Wall -Wextra central_logic_test.cpp -o central_logic_test
./central_logic_test
```
Expect `77/77 checks passed`. This runs the *same* `game_logic.h` the Central uses.

**b) Test the dashboard with a fake Central.** `pip install flask pyserial`, then
create a virtual serial pair (Linux/Mac: `socat -d -d pty,raw,echo=0 pty,raw,echo=0`;
Windows: com0com). Set `MOCK_PORT` in `mock_central.py` to one end and
`SERIAL_PORT` in `dashboard.py` to the other. Run `python mock_central.py`, then
`python dashboard.py`, and open `http://localhost:5000`. The mock registers four
teams, plays a random game, and resets — click START / FORCE START / SKIP TURN /
RESET to check every button.

### Level 2 — one ESP32 (the display is not needed)
Test the **touch UI** on a virtual screen:
1. In `participant_node.ino` uncomment `#define USE_VIRTUAL_DISPLAY`. Keep
   `VirtualTFT.h` in the same folder. Upload to one board.
2. Close the Arduino Serial Monitor (it locks the USB port).
3. `python virtual_display.py COM6` (your port), open `http://localhost:5001`.
4. Click the screen to tap. With no Central connected, walk through the screens
   with the command box: `DEMO TURN`, `DEMO WAIT`, `DEMO HIT`, `DEMO MISS`,
   `DEMO SUNK`, `DEMO INVALID`, `DEMO MSG`. On the "your turn" screen: tap a
   team, tap a cell, press ATTACK — the log shows the attack the board sent.

The virtual screen shows the real layout, button positions and touch logic. It
does **not** test wiring, SPI speed, backlight or touch calibration.

### Level 3 — two ESP32s (Central + one team board)
Flash `central_node.ino` (+`game_logic.h`) to one board and `participant_node.ino`
to the other. Fill in the two MAC addresses (see section 3, steps 3–6). Open the
Central's Serial Monitor and type `STATUS`: the team should show **REGISTERED**.
Try `GRID`, then `FORCE_START` — it must refuse (needs at least 2 registered teams).

### Level 4 — three ESP32s (Central + two team boards)
Register two different teams (`MY_TEAM_ID` 1 and 2), type `FORCE_START` (or press
the dashboard button), and play a real game. Run `virtual_display.py` for each
team board (one copy per board, different ports and web ports, e.g.
`python virtual_display.py COM6 5001` and `python virtual_display.py COM7 5002`).

### Level 5 — real displays
Follow the four steps under "When the real displays arrive" above.

---

## 3. Event setup (all boards)

1. **Arduino IDE 2.x** + ESP32 board support (File → Preferences → Additional
   Boards Manager URLs → add
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`,
   then Boards Manager → "esp32"). Board: "ESP32 Dev Module".
2. **Python 3**, then `pip install flask pyserial` (add `--break-system-packages`
   if pip complains about an "externally-managed environment").
3. **Find each board's MAC address.** Every sketch prints its own MAC on boot
   (115200 baud). Flash the Central first and note its MAC; flash each team board
   one at a time and note theirs.
4. **Put the 4 team MACs** into `participantMacs[]` in `central_node.ino`
   (Team 1 → 4 order).
5. **Put the Central's MAC** into `centralMac[]` in every team board's sketch.
6. **Set `MY_TEAM_ID` (1–4) and `MY_TEAM_NAME`** on each team board.
7. **Set each board's `myShips[3]`** (or keep the built-in example, which is valid).
   Ships may be horizontal, vertical or **diagonal**: `ORIENT_DIAG_DOWN` goes
   down-right from the start cell, `ORIENT_DIAG_UP` goes up-right. `start_x`/
   `start_y` is always the ship's first cell. The grid is 7×7 (x and y run 0–6), so a size-5
   diagonal can start anywhere a 5×5 block of free cells fits.
8. **`ESPNOW_CHANNEL`** must be identical on all boards (default `1`).
9. **Re-upload** `central_node.ino` (with `game_logic.h` in the same folder) and
   each team board with its own config.
10. **Connect Central to the laptop** by USB for the whole event.
11. **Set `SERIAL_PORT`** at the top of `dashboard.py` to Central's port
    (e.g. `COM5`; Mac `ls /dev/tty.*`; Linux `ls /dev/ttyUSB*`). Close the Arduino
    Serial Monitor.
12. **Run `python dashboard.py`**, open `http://localhost:5000`, project that tab.

**Central serial commands:** `STATUS`, `GRID`, `START`, `FORCE_START`, `RESET`,
`SKIP_TURN`, `GET_STATE`, `HELP`.

### Participant boards need to be registered by MAC
Central only accepts boards whose MAC is in `participantMacs[]`. With 20 teams
playing in groups of 4, either supply the 4 boards yourself and have each group
flash onto them (MACs stay fixed), or update that table and re-flash Central
between matches. The first option is far easier.

---

## 4. Game test checklist

**Registration**
- [ ] A correctly configured board registers and shows REGISTERED
- [ ] A board whose `MY_TEAM_ID` doesn't match its MAC is rejected
- [ ] An invalid layout (wrong sizes, overlap, off the grid) is rejected
- [ ] Diagonal ships register fine; a diagonal off any edge or crossing another ship is rejected
- [ ] All 4 registered → Central moves SETUP → READY

**Attacks**
- [ ] Miss → grid shows `M`, turn passes; hit → `H`, turn passes
- [ ] Re-attacking a cell, bad coordinates, self-attack, out-of-turn → INVALID, and the turn is **not** used up
- [ ] The final hit on a team eliminates it (attacker gets SUNK)

**Turns & rounds**
- [ ] Eliminated teams are skipped in the turn order
- [ ] The game ends automatically when one team is left
- [ ] `SKIP_TURN` advances a stuck turn; `RESET` returns to SETUP; a second round plays cleanly
- [ ] `FORCE_START` works with 2–3 teams and skips the missing ones

**Resilience**
- [ ] Powering off one team board doesn't disturb the others; powering it back on RECONNECTS with hits/misses intact
- [ ] A reconnect with a *different* layout from the same MAC is rejected
- [ ] Restarting `dashboard.py`, or refreshing the browser, restores the full state
- [ ] **Central power-loss recovery:** power-cycle Central mid-round; it restores exactly where it was

**Match log**
- [ ] `match_logs/round_1_*.txt` is created and filled; `RESET` opens `round_2_*.txt`

---

## 5. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Team board never registers | MAC typo | Re-check `participantMacs[]` and `centralMac[]` byte for byte against what each board printed |
| Nothing arrives, no errors | ESP-NOW channel mismatch | `ESPNOW_CHANNEL` must match on **all** boards |
| `FATAL: esp_now_init() failed` | Board/core issue | Re-flash; check the board type in Tools → Board |
| `WARNING: could not add ESP-NOW peer` | Bad MAC array | Look for typos/missing commas |
| `WARNING: ignored a packet of unexpected size` | Packet structs have drifted | `central_node.ino`/`game_logic.h` and the team sketch must define identical structs |
| `dashboard.py` / `virtual_display.py` can't open the port | Wrong port name, or the Arduino Serial Monitor has it open | Fix the port; close the Serial Monitor |
| Virtual display page says "not connected" | Same as above, or the board isn't flashed with `USE_VIRTUAL_DISPLAY` | Check both |
| Virtual screen stays black | Board was already running before the page opened | Click **Repaint screen** (or press the board's reset button) |
| `pip install` "externally-managed-environment" | Newer Python/OS policy | Add `--break-system-packages` |
| Browser can't reach `localhost:5000` | Dashboard not started / firewall | Check the terminal; try `127.0.0.1:5000` |
| Won't compile: `game_logic.h: No such file` | Not in the same folder as `central_node.ino` | Put them in one sketch folder |
| Won't compile: `VirtualTFT.h: No such file` | `USE_VIRTUAL_DISPLAY` is on but the header isn't next to the sketch | Put `VirtualTFT.h` in the same folder |
| Won't compile: `esp_now_recv_info_t does not name a type` | Old ESP32 core | Both sketches auto-detect core 2.x/3.x; update the core if it still happens |
| Blank/white real TFT | `User_Setup.h` pins not set, or wiring differs | Paste the pin block into TFT_eSPI's `User_Setup.h`; re-check wiring |
| Touches land in the wrong place | Default `touchCalData[]` | Run `Touch_calibrate` on that board and paste the 5 numbers |

---

## 6. Ideas for later (V2)

- Drag-to-place ship placement on the touch screen (instead of editing `myShips[]`)
- LEDs / buzzer for hit, miss and elimination; a per-turn countdown timer
- Score system across rounds; automatic tournament bracket
- Separate public vs. organizer dashboard views
- ESP-NOW ACK/retry protocol (today a failed send just logs a warning)
- Physical START/RESET buttons wired to Central

## 7. Already built beyond the original plan

- Persistent per-round match logs; Central power-loss protection (state saved to flash)
- ESP32 core 2.x/3.x compatibility in every sketch
- `FORCE_START` (play with 2–3 teams) and diagonal ships
- All rules in one shared header, with a hardware-free test (77 checks)
- Mock Central, virtual touch display, and `DEMO` commands for hardware-free testing
- Participant kit (skeleton + guide) so participants build their own radio side
