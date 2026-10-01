# Meraz Battleship — Setup, Testing & Reference Guide

Companion doc to `central_node.ino`, `participant_node.ino`, and
`dashboard.py`. Covers hardware, wiring up all 5 boards, a full test
plan, troubleshooting, and what's left for V2.

---

## 1. Hardware Requirements

**Needed now:**
- 5x ESP32 dev boards (any generic ESP32-WROOM-32 based board works —
  the code targets the generic `esp32:esp32:esp32` board in Arduino IDE)
- 5x USB cables (data-capable, not charge-only) for flashing
- 1x organizer laptop with a free USB port, Arduino IDE, and Python 3
- USB power banks or a multi-port USB charger for the 4 participant
  boards during the event (they don't need to stay connected to a PC)
- Optional: projector with an HDMI/VGA input for the laptop

**Display: 2.4" SPI touch TFT (ILI9341, 240x320, resistive touch).**
Each participant board drives one, through the TFT_eSPI library. The
touch screen shows the target-team buttons, the 5x5 coordinate grid,
and the ATTACK button. Until the display is wired up, the code still
runs fine without it (SPI writes to a missing display just do
nothing) and the Serial testing mode (`CONFIG` / `STATUS` / manual
`target x y` attacks) stands in for the touchscreen.

Default wiring (VSPI; check against the silkscreen on your module):

| TFT pin | ESP32 GPIO |
|---|---|
| VCC / GND | 3.3V / GND |
| CS / RESET / DC | 5 / 4 / 2 |
| SDI (MOSI) / SCK / SDO (MISO) | 23 / 18 / 19 |
| LED (backlight) | 3.3V |
| T_CS / T_IRQ | 15 / 27 |
| T_CLK / T_DIN / T_DO | shared with SCK / MOSI / MISO |

One-time library setup: paste the pin block from the top of
`participant_node.ino` into TFT_eSPI's own `User_Setup.h`. Then, **per
board**, run the TFT_eSPI `Touch_calibrate` example and paste its 5
numbers into `touchCalData[]` (touch panels vary unit to unit).

---

## 2. Setup Instructions

1. **Install Arduino IDE** (2.x recommended), then add ESP32 board
   support: File → Preferences → "Additional Boards Manager URLs" →
   add `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`,
   then Tools → Board → Boards Manager → search "esp32" → Install.
   Select a generic "ESP32 Dev Module" as the board.
2. **Install Python 3**, then `pip install flask pyserial` (add
   `--break-system-packages` if pip complains about an
   "externally-managed environment").
3. **Find each board's MAC address.** Both sketches print their own
   MAC address on boot (115200 baud). Flash `central_node.ino` to the
   board that will be your Central and note its MAC — you'll need it
   for every participant's `centralMac[]`. Then flash
   `participant_node.ino` to each of the 4 participant boards **one at
   a time**, noting each one's MAC before moving to the next.
4. **Enter the 4 participant MACs** into `central_node.ino`'s
   `participantMacs[]` array, in Team 1→4 order.
5. **Enter the Central's MAC** into every participant board's
   `centralMac[]`.
6. **Set `MY_TEAM_ID` and `MY_TEAM_NAME`** on each participant board
   (1–4, matching the order you used in step 4).
7. **Set each board's `myShips[3]`** — either your real layout for
   that team, or leave the built-in example as-is for a quick first
   test (it's already valid). Ships can be horizontal, vertical, or
   **diagonal** (`ORIENT_DIAG_DOWN` goes down-right from the start cell,
   `ORIENT_DIAG_UP` goes up-right). `start_x`/`start_y` is always the
   ship's first cell. On the 5x5 grid a size-5 diagonal only fits
   corner to corner.
8. **Check `ESPNOW_CHANNEL`** matches across all 5 boards (defaults to
   `1` in both files — only change it if you have a reason to).
9. **Re-upload `central_node.ino`** to the Central board (after step 4).
   Keep **`game_logic.h` in the same sketch folder** as
   `central_node.ino` — it holds the game rules and the sketch won't
   compile without it (Arduino IDE shows it as a second tab).
10. **Re-upload `participant_node.ino`** to each participant board
    (after steps 5–7, with that board's own config).
11. **Connect Central to the laptop** via USB and leave it connected
    for the whole event.
12. **Set `SERIAL_PORT`** at the top of `dashboard.py` to Central's
    port — Device Manager on Windows (e.g. `COM5`), or
    `ls /dev/tty.*` (Mac) / `ls /dev/ttyUSB*` (Linux).
13. **Close the Arduino Serial Monitor** if it's open — it locks the
    port and `dashboard.py` won't be able to connect while it's open.
14. **Run `python dashboard.py`**, open `http://localhost:5000`, and
    project that tab.

---

## 3. Testing Plan

**Registration**
- [ ] A correctly-configured board registers and shows REGISTERED
- [ ] A board with a mismatched `MY_TEAM_ID` vs. its MAC gets rejected
- [ ] A board with an invalid ship layout (wrong sizes, overlapping
      ships, or a ship running off the 5x5 grid) gets rejected
- [ ] Diagonal ships (down-right and up-right) register fine, and a
      diagonal running off any edge or crossing another ship is rejected
- [ ] All 4 registered → Central moves from SETUP to READY

**Attacks**
- [ ] A miss updates the target's grid to `M` and passes the turn
- [ ] A hit updates the target's grid to `H` and passes the turn
- [ ] Attacking an already-attacked cell → INVALID, turn is **not**
      consumed
- [ ] Bad coordinates (outside 0–4) → INVALID
- [ ] Attacking yourself → INVALID
- [ ] Attacking out of turn → INVALID
- [ ] The 9th hit on a team eliminates it (attacker gets SUNK, not HIT)

**Turns & rounds**
- [ ] Eliminating a team correctly skips them in the turn order
      (eliminate Team 2, confirm turns go 1→3→4→1, not through 2)
- [ ] Game ends automatically when only one team is left
- [ ] `SKIP_TURN` manually advances the turn when a board is stuck
- [ ] `RESET` clears everything and returns to SETUP
- [ ] A second round can register and play through cleanly after RESET

**Resilience**
- [ ] Powering off one participant mid-game doesn't affect the other
      3 teams' play
- [ ] Powering that board back on gets it RECONNECTED with its
      existing hits/misses intact (not reset)
- [ ] A **mismatched** reconnect attempt (different ship config sent
      from the same MAC) gets REJECTED and doesn't touch stored state
- [ ] Killing and restarting `dashboard.py` mid-game re-fetches state
      via `GET_STATE` and redraws correctly
- [ ] Refreshing the browser tab repopulates immediately from
      `/api/state`, without waiting for the next live event
- [ ] **Central power-loss recovery:** mid-round, physically power-cycle
      the Central board (not just unplug from the laptop — an actual
      power loss). On reboot it should restore exactly where it left
      off (check via `STATUS`/`GRID` or the dashboard).

**Match log**
- [ ] `match_logs/round_1_*.txt` is created on startup and contains
      timestamped lines for each event
- [ ] Triggering `RESET` closes that file and opens `round_2_*.txt`
- [ ] Organizer button clicks (START/RESET/SKIP_TURN) show up in the
      log, not just Central-originated events

---

## 4. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Participant never registers | MAC typo somewhere | Re-check `participantMacs[]` on Central and `centralMac[]` on the participant byte-for-byte against what each board printed on boot |
| Registration/attacks silently never arrive, no errors | ESP-NOW channel mismatch | Confirm `ESPNOW_CHANNEL` is identical on **all 5** boards |
| `FATAL: esp_now_init() failed` | Core/board issue | Re-flash; double check "ESP32 Dev Module" (or your exact board) is selected in Tools → Board |
| `WARNING: could not add ESP-NOW peer` | Malformed MAC array | Check for typos/missing commas in the MAC byte arrays |
| `dashboard.py` can't open the serial port | Wrong `SERIAL_PORT`, board not plugged in, or Arduino Serial Monitor has it open | Fix the port string; close the Serial Monitor before running the dashboard |
| `pip install` fails with "externally-managed-environment" | Newer Python/OS package policy | Add `--break-system-packages` to the pip command |
| Browser can't reach `http://localhost:5000` | Dashboard didn't start, or a firewall is blocking it | Check the terminal for errors; try `http://127.0.0.1:5000` |
| `WARNING: ignored a packet of unexpected size` | `central_node.ino` and `participant_node.ino` have drifted out of sync | Make sure both files' packet structs are byte-for-byte identical — re-copy from the same version of each file |
| Won't compile: `esp_now_recv_info_t does not name a type` | You're on an older ESP32 core than expected | Shouldn't happen anymore — both files now auto-detect core 2.x vs 3.x via `ESP_IDF_VERSION_MAJOR`. If it still does, update the ESP32 board package in Boards Manager |
| Blank / white TFT | `User_Setup.h` pins not set, or wiring differs | Paste the pin block into TFT_eSPI's `User_Setup.h` and re-check wiring against your module's silkscreen |
| Touches land in the wrong place | Default `touchCalData[]` still in use | Run TFT_eSPI's `Touch_calibrate` on that board and paste its 5 numbers in |
| Won't compile: `game_logic.h: No such file` | Not in the same folder as `central_node.ino` | Put both files in one sketch folder |

---

## 5. Future Improvements (V2 ideas)

- Finalized TFT model + real touch UI (the one piece intentionally
  left open in V1)
- Drag-to-place ship UI instead of typing coordinates into `myShips[]`
- LEDs / buzzer / sound effects for hit, miss, and elimination
- A visible per-turn countdown timer
- A score system / leaderboard across multiple rounds
- Separate public vs. organizer views (hide ship positions until hit)
- Automatic tournament bracket management across rounds
- A proper ESP-NOW ACK/retry protocol (today a failed send just logs
  a warning — fine for a turn-based, human-paced game, but a real
  retry loop would be more robust)
- Physical START/RESET buttons wired directly to Central, as a backup
  to the dashboard/Serial commands
- A desktop test harness / mock-Central script for automated testing
  without hardware (discussed, not yet built — happy to build this if
  useful before the boards arrive)

---

## 6. Already Added Beyond the Original Plan

A few things came up during development and are already done, not
just planned:

- **Persistent match logs** — `dashboard.py` writes a timestamped,
  plain-text log per round to `match_logs/`, surviving RESETs and
  dashboard restarts.
- **Power-loss protection** — `central_node.ino` saves game state to
  flash after every action and restores it on boot, so a Central power
  blip mid-round no longer loses the match.
- **ESP32 core 2.x/3.x compatibility** — both `.ino` files now detect
  the installed core version at compile time and use the matching
  ESP-NOW callback signature, so it doesn't matter which core version
  ends up on your lab machines.
- **FORCE_START** — organizer button/command to start with fewer than
  4 teams (minimum 2) if a team doesn't show up. Turn order skips any
  team that never registered.
- **Diagonal ships** — ships may be horizontal, vertical, or diagonal
  (both directions).
- **`game_logic.h`** — all game rules in one shared header, used by
  both `central_node.ino` and the desktop test harness.
- **Test tools (no hardware needed)** — `central_logic_test.cpp`
  (`g++ -std=c++11 central_logic_test.cpp -o t && ./t`) runs the rules
  directly; `mock_central.py` plus a virtual serial pair (socat on
  Linux/Mac, com0com on Windows) lets you exercise `dashboard.py`
  end to end.
- **Real TFT touch UI** in `participant_node.ino` (see section 1).
