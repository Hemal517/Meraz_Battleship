# Meraz Battleship — Setup, Testing & Reference Guide

Companion doc to `central_node.ino`, `participant_node.ino`, and
`dashboard.py`. Covers hardware, wiring, testing without physical displays,
test plan, troubleshooting, and extras.

---

## 1. Hardware Requirements

**Needed:**
- 5x ESP32 dev boards (any generic ESP32-WROOM-32 based board works —
  the code targets the generic `esp32:esp32:esp32` board in Arduino IDE)
- 5x USB cables (data-capable, not charge-only) for flashing and testing
- 1x organizer laptop with a free USB port, Arduino IDE, and Python 3
- USB power banks or a multi-port USB charger for the 4 participant
  boards during the event (they don't need to stay connected to a PC)
- Projector with an HDMI/VGA input for the laptop (displays `dashboard.py`)

**TFT Display (2.4" SPI Touch TFT — ILI9341 + XPT2046):**
`participant_node.ino` is fully implemented for a 2.4" 240x320 touch display
using the `TFT_eSPI` library.
- **Before the physical displays arrive:** You can test everything using the included
  `virtual_display.py` desktop tool or Serial Monitor. **NO code changes are needed
  on `participant_node.ino`.**
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
  2. Run `File > Examples > TFT_eSPI > Generic > Touch_calibrate` on each board and paste
     the 5 numbers into `touchCalData[5]` in `participant_node.ino`.

---

## 2. Testing Without Physical Displays (Step-by-Step)

You do **NOT** need to wait for physical displays to arrive. The project includes three
dedicated tools to test every aspect of the system:

### Method A: Interactive Virtual TFT Display (`virtual_display.py`)
This desktop tool acts as a 1:1 graphical substitute for the physical ILI9341 touch display.
It renders the exact same target buttons, 5x5 grid, attack button, and color-coded result banners.

```bash
# 1. Connect a participant ESP32 running participant_node.ino via USB (e.g. COM4)
# 2. Run the virtual display:
python virtual_display.py --port COM4 --team 1
```

- **Zero changes to `participant_node.ino`:** It communicates over USB Serial.
- **Interactive:** When it's your turn, click the opponent button (`T2`), click a cell (`B3`),
  and click **ATTACK**. The tool sends the command to the ESP32, which fires the ESP-NOW packet!
- **Feedback:** Displays the exact 1500ms orange "HIT", blue "MISS", or red "ELIMINATED" banner.
- **Offline Demo:** You can even run `python virtual_display.py` without any hardware plugged in
  and use the built-in test buttons ("Your Turn", "Hit", "Miss", "Sunk") to verify the UI.

### Method B: Serial Monitor Manual Testing
`participant_node.ino` has a built-in Serial testing mode at 115200 baud:
- Type `CONFIG` — shows board configuration, team ID, and Central MAC.
- Type `STATUS` — displays current round state and active turn.
- Type `<target> <x> <y>` — e.g. `2 3 4` fires an attack at Team 2, column 3, row 4.

### Method C: Mock Central Testing (`mock_central.py`)
To test the organizer dashboard without plugging in the Central ESP32:
```bash
python mock_central.py --port COM3
```
In another terminal:
```bash
python dashboard.py
```
Open `http://localhost:5000` to watch the simulated registrations, hits, misses, and eliminations live.

---

## 3. Setup Instructions (Real Hardware)

1. **Install Arduino IDE** (2.x recommended), then add ESP32 board support:
   File → Preferences → "Additional Boards Manager URLs" → add
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`,
   then Tools → Board → Boards Manager → search "esp32" → Install.
   Select "ESP32 Dev Module" as the board.
2. **Install Python dependencies:**
   ```bash
   pip install flask pyserial
   ```
3. **Find each board's MAC address:** Both sketches print their own MAC on boot (115200 baud).
   Flash `central_node.ino` to the Central board and note its MAC. Then flash
   `participant_node.ino` to each participant board one by one, noting each MAC.
4. **Enter the 4 participant MACs** into `central_node.ino`'s `participantMacs[]` array in Team 1→4 order.
5. **Enter Central's MAC** into every participant board's `centralMac[]`.
6. **Set `MY_TEAM_ID` (1–4) and `MY_TEAM_NAME`** on each participant board.
7. **Set `myShips[3]`** on each participant board (or keep the default valid example).
8. **Check `ESPNOW_CHANNEL`** matches across all 5 boards (default `1`).
9. **Flash Central** with the final MAC table.
10. **Flash all 4 participant boards** with their respective team configs.
11. **Connect Central to laptop via USB**, set `SERIAL_PORT` in `dashboard.py` (e.g. `COM3`),
    close the Arduino Serial Monitor, and run `python dashboard.py`. Open `http://localhost:5000`.

---

## 4. Organizer Commands & `FORCE_START`

From the dashboard or via Central's Serial interface:
- **`START`** — Starts the round once all 4 teams have registered.
- **`FORCE_START`** — Starts the round when only 2 or 3 teams have registered (e.g. a team fails
  to show up). Unregistered teams are automatically skipped in the turn order.
- **`SKIP_TURN`** — Manually advances turn to the next alive team if a team's node disconnects or stalls.
- **`RESET`** — Resets all grids and team state back to SETUP for a fresh round. Creates a new match log.

---

## 5. Testing Plan

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

**Resilience**
- [ ] Powering off one participant mid-game doesn't affect other teams
- [ ] Powering that board back on RECONNECTS with state intact
- [ ] Dashboard restart / browser refresh rebuilds full state from Central
- [ ] Central power-loss recovery: rebooting Central restores full game state from NVS Flash

---

## 6. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Participant never registers | MAC typo somewhere | Re-check `participantMacs[]` on Central and `centralMac[]` on participant |
| Packets silently drop | ESP-NOW channel mismatch | Confirm `ESPNOW_CHANNEL` is identical on all 5 boards |
| `dashboard.py` cannot open port | Port locked by Arduino Serial Monitor or wrong port | Close Arduino Serial Monitor; verify COM port in Device Manager |
| No physical display yet | Hardware still in transit | Use `python virtual_display.py --port COMx` to test with full graphics |
| Touch offset on physical TFT | Uncalibrated touch controller | Run `Touch_calibrate` example and update `touchCalData[]` |

---

## 7. Features Added Beyond Original Spec

- **`virtual_display.py`** — Interactive 2.4" ILI9341 display simulator & hardware bridge.
- **`mock_central.py`** — Fake Central ESP32 for testing without hardware.
- **`FORCE_START`** — Start rounds with 2 or 3 teams when attendance is partial.
- **Shared `game_logic.h`** — Pure C++ rule engine used by Central and test tools.
- **Fog of War Protection** — Dashboard hides intact ship coordinates so projected screen does not leak positions to players.
- **Persistent match logs** — Each round writes to `match_logs/round_N_*.txt`.
- **Power-loss protection** — Central saves state to ESP32 NVS Preferences on every move.
- **Neon College-Fest Dashboard** — Modern widescreen layout with live HP bars, 75/25 split, and animated tactical mesh.
