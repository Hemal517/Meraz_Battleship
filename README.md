# Meraz Battleship — Setup, Testing & Reference Guide

4-team Battleship system for Meraz college fest (Electronics Club).
Built with 5 ESP32 dev boards (1 Central game server + 4 Participant nodes with 2.4" SPI touch displays) and a projector Command Deck dashboard.

---

## 📁 Repository File Structure

```text
├── central_node/
│   ├── central_node.ino          # Authoritative game server firmware (USB Serial + ESP-NOW)
│   └── game_logic.h              # Pure C++ battleship rules (validation, turns, eliminations)
├── participant_node/
│   └── participant_node.ino      # Participant node firmware for all 4 teams (TFT + Serial)
├── match_logs/
│   └── .gitkeep                  # Auto-generated plain-text match logs saved per round
├── dashboard.py                  # Projector Command Deck (Flask + Server-Sent Events)
├── simulate_dashboard.py         # Offline rehearsal server (run dashboard with ZERO hardware)
├── virtual_display.py            # Virtual 2.4" ILI9341 touch display & USB hardware bridge
├── mock_central.py               # Mock Central ESP32 over Serial for end-to-end rehearsal
├── central_logic_test.cpp        # Desktop C++ test suite (verifies all 52 game rules)
├── .gitignore                    # Ignores __pycache__, .pyc, and generated match logs
└── README.md                     # This manual
```

---

## 1. Hardware Requirements

**Event Setup:**
- **5x ESP32 dev boards** (generic ESP32-WROOM-32 based boards — targeted as `esp32:esp32:esp32` in Arduino IDE).
- **5x USB data cables** (for flashing and connecting Central to organizer laptop).
- **1x organizer laptop** with Python 3 and free USB ports.
- **USB power banks / 4-port wall charger** for the 4 participant boards (they only need power; all game communication is wireless over ESP-NOW).
- **Projector** connected via HDMI to the organizer laptop displaying `dashboard.py`.

**TFT Display (2.4" SPI Touch TFT — ILI9341 + XPT2046):**
`participant_node.ino` is fully implemented for a 2.4" 240x320 touch display using the `TFT_eSPI` library.
- **Before physical displays arrive:** You can test everything using `virtual_display.py` or Serial Monitor. **NO code changes are needed on `participant_node.ino`.**
- **Once physical displays arrive:**
  1. Paste the pin configuration into TFT_eSPI's library-level `User_Setup.h` once:
     ```cpp
     #define ILI9341_DRIVER
     #define TFT_CS   5
     #define TFT_DC   2
     #define TFT_RST  4
     #define TFT_MOSI 23
     #define TFT_SCLK 18
     #define TFT_MISO 19
     #define TOUCH_CS 15
     #define SPI_FREQUENCY        40000000
     #define SPI_TOUCH_FREQUENCY  2500000
     ```
  2. Run `File > Examples > TFT_eSPI > Generic > Touch_calibrate` on each board and paste the 5 numbers into `touchCalData[5]` in `participant_node.ino`.

---

## 2. Testing Without Physical Displays (Step-by-Step)

You do **NOT** need to wait for physical displays to arrive. The repository includes four dedicated test tools:

### Method A: Interactive Virtual TFT Display (`virtual_display.py`)
A 1:1 desktop simulation of the physical 240x320 ILI9341 touch display.
It renders the exact same target buttons, 5x5 coordinate grid, attack button, and color-coded result banners.

```bash
# 1. Connect a participant ESP32 running participant_node.ino via USB (e.g. COM4)
# 2. Run the virtual display:
python virtual_display.py --port COM4 --team 1
```

- **Zero changes to `participant_node.ino`:** Communicates over USB Serial fallback.
- **Interactive:** When it's your turn, click the opponent button (`T2`), click a grid cell (`B3`), and click **ATTACK**. The tool sends the command to the ESP32, which fires the ESP-NOW packet to Central!
- **Feedback:** Displays the 1500ms orange "HIT", blue "MISS", or red "ELIMINATED" banner.
- **Offline Demo Mode:** Run `python virtual_display.py` without any hardware connected and use the bottom test buttons ("Your Turn", "Hit", "Miss", "Sunk") to verify all screens.

### Method B: Offline Dashboard Rehearsal (`simulate_dashboard.py`)
Rehearse the projector dashboard with **ZERO hardware and ZERO serial ports**:
```bash
python simulate_dashboard.py
```
Open `http://localhost:5000` in your browser. It automatically registers simulated teams, plays turns with randomized hits/misses, and lets you test `START`, `FORCE_START`, `SKIP_TURN`, and `RESET` buttons directly.

### Method C: Mock Central Testing (`mock_central.py`)
To test the organizer dashboard against a fake Central ESP32 over a virtual/loopback COM port:
```bash
python mock_central.py --port COM3
```
In another terminal:
```bash
python dashboard.py
```

### Method D: Desktop C++ Logic Tests (`central_logic_test.cpp`)
Verifies all pure game rules in `game_logic.h` (ship validation, force start, turn skipping, repeat attacks, 9-cell sinkings):
```bash
g++ -std=c++17 -Wall -Wextra -I central_node central_logic_test.cpp -o central_logic_test
./central_logic_test
```

### Method E: Serial Monitor Manual Testing
`participant_node.ino` has a built-in Serial testing mode at 115200 baud:
- Type `CONFIG` — shows board configuration, team ID, and Central MAC.
- Type `STATUS` — displays current round state and active turn.
- Type `<target> <x> <y>` — e.g. `2 3 4` fires an attack at Team 2, column 3, row 4.

---

## 3. Setup Instructions (Real Hardware)

1. **Install Arduino IDE** (2.x recommended), then add ESP32 board support:
   File → Preferences → "Additional Boards Manager URLs" → add:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   Then Tools → Board → Boards Manager → search "esp32" → Install.
   Select "ESP32 Dev Module" as the board.
2. **Install Python dependencies:**
   ```bash
   pip install flask pyserial
   ```
3. **Find each board's MAC address:** Both sketches print their own MAC on boot (115200 baud).
   Flash `central_node.ino` to the Central board and note its MAC. Then flash `participant_node.ino` to each participant board one by one, noting each MAC.
4. **Enter the 4 participant MACs** into `central_node.ino`'s `participantMacs[]` array in Team 1→4 order.
5. **Enter Central's MAC** into every participant board's `centralMac[]`.
6. **Set `MY_TEAM_ID` (1–4) and `MY_TEAM_NAME`** on each participant board.
7. **Set `myShips[3]`** on each participant board (or keep the default valid example).
8. **Check `ESPNOW_CHANNEL`** matches across all 5 boards (default `1`).
9. **Flash Central** with the final MAC table.
10. **Flash all 4 participant boards** with their respective team configs.
11. **Connect Central to laptop via USB**, set `SERIAL_PORT` in `dashboard.py` (e.g. `COM3`), close the Arduino Serial Monitor, and run `python dashboard.py`. Open `http://localhost:5000` and project the tab.

---

## 4. Organizer Controls & `FORCE_START`

From the projector dashboard or via Central's Serial interface:
- **`START`** — Starts the round once all 4 teams have registered.
- **`FORCE_START`** — Starts the round when only 2 or 3 teams have registered (e.g. a team fails to show up). Unregistered teams are automatically skipped in the turn order.
- **`SKIP_TURN`** — Manually advances turn to the next alive team if a team's node disconnects or stalls.
- **`RESET`** — Resets all grids and team state back to SETUP for a fresh round. Automatically creates a new match log.

---

## 5. Testing Plan Checklist

**Registration**
- [ ] A correctly-configured board registers and shows REGISTERED
- [ ] A board with a mismatched `MY_TEAM_ID` vs. its MAC gets rejected
- [ ] A board with an invalid ship layout (wrong sizes, overlapping ships) gets rejected
- [ ] All 4 registered → Central moves from SETUP to READY
- [ ] 2 or 3 registered → `FORCE_START` initiates the match cleanly

**Attacks**
- [ ] A miss updates target's grid to `M` and passes turn
- [ ] A hit updates target's grid to `H` and passes turn
- [ ] Attacking an already-attacked cell → INVALID, turn **not** consumed
- [ ] Bad coordinates (outside 0–4) → INVALID
- [ ] Attacking yourself → INVALID
- [ ] Attacking out of turn → INVALID
- [ ] The 9th hit on a team eliminates it (attacker gets SUNK / ELIMINATED)

**Turns & rounds**
- [ ] Eliminating a team correctly skips them in turn order (1→3→4→1)
- [ ] Game ends automatically when only one team remains alive
- [ ] `SKIP_TURN` advances turn past disconnected/stalled players
- [ ] `RESET` clears everything and starts a fresh round

**Resilience & Persistence**
- [ ] Powering off one participant mid-game doesn't affect other teams
- [ ] Powering that board back on RECONNECTS with state intact
- [ ] Dashboard restart / browser refresh rebuilds full state from Central snapshot
- [ ] Central power-loss recovery: rebooting Central restores full game state from NVS Flash

---

## 6. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Participant never registers | MAC typo somewhere | Re-check `participantMacs[]` on Central and `centralMac[]` on participant |
| Packets silently drop | ESP-NOW channel mismatch | Confirm `ESPNOW_CHANNEL` is identical on all 5 boards |
| `dashboard.py` cannot open port | Port locked by Arduino Serial Monitor or wrong port | Close Arduino Serial Monitor; verify COM port in Device Manager |
| No physical display yet | Hardware still in transit | Use `python virtual_display.py --port COMx` or `simulate_dashboard.py` |
| Touch offset on physical TFT | Uncalibrated touch controller | Run `Touch_calibrate` example and update `touchCalData[]` |

---

## 7. Features Added Beyond Original Spec

- **`dashboard.py` Command Deck (v2)** — 75/25 widescreen tactical layout, live HP bars, offline-safe typography (no CDN fonts), and animated tactical mesh.
- **Fog of War Protection** — Dashboard hides intact ship coordinates so projected screen does not leak positions to players.
- **`virtual_display.py`** — Interactive 2.4" ILI9341 display simulator & hardware bridge.
- **`simulate_dashboard.py`** — Offline rehearsal server for testing dashboard with zero hardware.
- **`mock_central.py`** — Fake Central ESP32 for testing over Serial without hardware.
- **`central_logic_test.cpp`** — Desktop automated test harness for game rules.
- **`FORCE_START`** — Start rounds with 2 or 3 teams when attendance is partial.
- **Shared `game_logic.h`** — Pure C++ rule engine used by Central and test tools.
- **Persistent match logs** — Each round writes to `match_logs/round_N_*.txt`.
- **Power-loss protection** — Central saves state to ESP32 NVS Preferences on every move.
