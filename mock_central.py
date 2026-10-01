"""
MERAZ BATTLESHIP - MOCK CENTRAL (test tool, not a deliverable file)
=====================================================================
Pretends to be the Central ESP32 so you can fully exercise dashboard.py
- live updates, buttons, match-log rollover, reconnect behavior - with
zero hardware. It speaks the exact same protocol central_node.ino does:
newline-delimited JSON ({"type":"state",...} / {"type":"event",...})
out, and plain-text commands (GET_STATE/START/FORCE_START/RESET/
SKIP_TURN) in.

WHAT IT DOES ON ITS OWN, so you can just watch the dashboard:
  1. Registers all 4 teams, a couple seconds apart.
  2. Waits for you to click START on the dashboard - or auto-starts
     after 15s if you don't, so you're never stuck waiting.
  3. Plays out random (but rule-following) attacks every 1.5-3
     seconds until one team is left, producing a realistic mix of
     hits, misses, and an elimination.
  4. Resets and starts another round automatically, so you can watch
     the match-log rollover happen too.
You can also drive it manually at any point - clicking RESET, START,
FORCE_START, or SKIP_TURN on the dashboard talks to this script exactly
like it would talk to the real Central.

=====================================================================
SETUP - you need a VIRTUAL SERIAL PORT PAIR (two linked fake serial
ports, so this script can write to one end and dashboard.py reads the
other). Real hardware isn't involved at all.

Linux / Mac (using socat - `brew install socat` or `apt install socat`):
    socat -d -d pty,raw,echo=0 pty,raw,echo=0
  This prints two paths, e.g.:
    2026/01/01 12:00:00 socat[1234] N PTY is /dev/pts/3
    2026/01/01 12:00:00 socat[1234] N PTY is /dev/pts/4
  Leave that command running. Set MOCK_PORT below to one path (e.g.
  /dev/pts/3), and dashboard.py's SERIAL_PORT to the other (/dev/pts/4).

Windows (using com0com, free - search "com0com" to download):
  Install it and create a port pair, e.g. COM8 <-> COM9. Set MOCK_PORT
  to COM8 and dashboard.py's SERIAL_PORT to COM9.

Then just run:
    python mock_central.py
  (or `python mock_central.py <port>` to override MOCK_PORT for one run)
and separately run dashboard.py as usual, pointed at the other port.
=====================================================================
"""

import json
import random
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:
    print("Missing dependency 'pyserial'. Install it with: pip install pyserial")
    raise SystemExit(1)

# =====================================================================
# CONFIGURATION
# =====================================================================

MOCK_PORT = "/dev/pts/3"  # TODO: change to your half of the virtual port pair
BAUD_RATE = 115200

MAX_TEAMS = 4
GRID_SIZE = 5

CELL_WATER, CELL_SHIP, CELL_MISS, CELL_HIT = 0, 1, 2, 3
STATE_SETUP, STATE_READY, STATE_RUNNING, STATE_GAMEOVER = 0, 1, 2, 3

AUTO_START_AFTER_SECONDS = 15   # if nobody clicks START, start anyway
ATTACK_DELAY_RANGE = (1.5, 3.0)  # seconds between automatic attacks
NEXT_ROUND_DELAY_SECONDS = 8

# =====================================================================
# STATE
# =====================================================================

state_lock = threading.RLock()  # reentrant - action functions call send_state()/send_event() while holding it
reset_event = threading.Event()  # lets an incoming RESET interrupt the autoplay loop immediately
skip_next_auto_reset = False     # avoids a redundant second reset right after an externally-triggered one


def blank_team(team_id):
    return {
        "id": team_id,
        "name": "",
        "registered": False,
        "eliminated": False,
        "remaining": 0,
        "grid": [[CELL_WATER] * GRID_SIZE for _ in range(GRID_SIZE)],
    }


state = {
    "round_state": STATE_SETUP,
    "current_turn": 0,
    "teams": [blank_team(i) for i in range(1, MAX_TEAMS + 1)],
}


def demo_grid():
    """The same fixed layout game_logic.h's validShips() uses - good
    enough for exercising the dashboard's rendering, doesn't need to
    be a different layout per team."""
    grid = [[CELL_WATER] * GRID_SIZE for _ in range(GRID_SIZE)]
    grid[4][2] = CELL_SHIP                      # size 1 at (x=2, y=4)
    for x in range(0, 3):
        grid[0][x] = CELL_SHIP                  # size 3 horizontal at (x=0-2, y=0)
    for y in range(0, 5):
        grid[y][4] = CELL_SHIP                  # size 5 vertical at (x=4, y=0-4)
    return grid


# =====================================================================
# SENDING (the wire protocol - matches central_node.ino exactly)
# =====================================================================

def send_state(ser):
    with state_lock:
        snapshot = {
            "type": "state",
            "round_state": state["round_state"],
            "current_turn": state["current_turn"],
            "teams": [dict(t) for t in state["teams"]],
        }
    ser.write((json.dumps(snapshot) + "\n").encode("utf-8"))


def send_event(ser, text):
    print(f"[mock central] {text}")
    ser.write((json.dumps({"type": "event", "text": text}) + "\n").encode("utf-8"))


# =====================================================================
# ACTIONS (mirror central_node.ino's behavior closely enough to be a
# useful stand-in - this is a test double, not the authoritative rules,
# so it doesn't re-implement every validation check game_logic.h has)
# =====================================================================

def do_reset(ser):
    with state_lock:
        for i in range(MAX_TEAMS):
            state["teams"][i] = blank_team(i + 1)
        state["round_state"] = STATE_SETUP
        state["current_turn"] = 0
    send_event(ser, "Game reset - waiting for teams to register")
    send_state(ser)


def register_team(ser, team_id):
    with state_lock:
        team = state["teams"][team_id - 1]
        team["registered"] = True
        team["name"] = f"TEAM {team_id}"
        team["eliminated"] = False
        team["remaining"] = 9
        team["grid"] = demo_grid()
        if all(t["registered"] for t in state["teams"]):
            state["round_state"] = STATE_READY
    send_event(ser, f"TEAM {team_id} (TEAM {team_id}) registered")
    send_state(ser)


def start_game(ser):
    with state_lock:
        if state["round_state"] != STATE_READY:
            return
        state["round_state"] = STATE_RUNNING
        state["current_turn"] = 1
    send_event(ser, "GAME STARTED")
    send_state(ser)


def force_start_game(ser):
    with state_lock:
        if state["round_state"] in (STATE_RUNNING, STATE_GAMEOVER):
            return
        registered_ids = [t["id"] for t in state["teams"] if t["registered"]]
        if len(registered_ids) < 2:
            return
        state["round_state"] = STATE_RUNNING
        state["current_turn"] = registered_ids[0]
        count = len(registered_ids)
    send_event(ser, f"GAME FORCE-STARTED by organizer with {count} team(s) registered")
    send_state(ser)


def advance_turn(ser):
    """Caller must hold state_lock."""
    alive = [t["id"] for t in state["teams"] if t["registered"] and not t["eliminated"]]
    if len(alive) <= 1:
        state["round_state"] = STATE_GAMEOVER
        winner_text = f"GAME OVER - TEAM {alive[0]} WINS" if alive else "GAME OVER - no teams remain"
        send_event(ser, winner_text)
        return
    idx = state["current_turn"] - 1
    for _ in range(MAX_TEAMS):
        idx = (idx + 1) % MAX_TEAMS
        team = state["teams"][idx]
        if team["registered"] and not team["eliminated"]:
            state["current_turn"] = team["id"]
            return


def skip_turn(ser):
    with state_lock:
        if state["round_state"] != STATE_RUNNING:
            return
        send_event(ser, f"Organizer manually skipped TEAM {state['current_turn']}'s turn")
        advance_turn(ser)
    send_state(ser)


def auto_attack(ser):
    with state_lock:
        if state["round_state"] != STATE_RUNNING:
            return
        attacker_id = state["current_turn"]
        targets = [t for t in state["teams"] if t["registered"] and not t["eliminated"] and t["id"] != attacker_id]
        if not targets:
            return
        target = random.choice(targets)

        candidates = [(x, y) for y in range(GRID_SIZE) for x in range(GRID_SIZE)
                      if target["grid"][y][x] in (CELL_WATER, CELL_SHIP)]
        if not candidates:
            return
        x, y = random.choice(candidates)
        cell = target["grid"][y][x]

        if cell == CELL_WATER:
            target["grid"][y][x] = CELL_MISS
            send_event(ser, f"TEAM {attacker_id} attacked TEAM {target['id']} at ({x},{y}) - MISS")
        else:
            target["grid"][y][x] = CELL_HIT
            target["remaining"] -= 1
            if target["remaining"] <= 0:
                target["eliminated"] = True
                send_event(ser, f"TEAM {attacker_id} attacked TEAM {target['id']} at ({x},{y}) "
                                 f"- HIT, TEAM {target['id']} ELIMINATED")
            else:
                send_event(ser, f"TEAM {attacker_id} attacked TEAM {target['id']} at ({x},{y}) - HIT")

        advance_turn(ser)
    send_state(ser)


# =====================================================================
# READING COMMANDS FROM THE DASHBOARD
# =====================================================================

def reader_thread(ser):
    while True:
        try:
            line = ser.readline().decode("utf-8", errors="ignore").strip()
        except (serial.SerialException, OSError):
            time.sleep(1)
            continue

        if not line:
            continue

        cmd = line.upper()
        if cmd == "GET_STATE":
            send_state(ser)
        elif cmd == "START":
            start_game(ser)
        elif cmd == "FORCE_START":
            force_start_game(ser)
        elif cmd == "RESET":
            global skip_next_auto_reset
            reset_event.set()
            do_reset(ser)
            skip_next_auto_reset = True
        elif cmd == "SKIP_TURN":
            skip_turn(ser)
        # STATUS/GRID/HELP are for a human at the real Serial Monitor -
        # dashboard.py never sends them, so there's nothing to do here.


# =====================================================================
# THE AUTOPLAY DEMO LOOP
# =====================================================================

def sleep_interruptible(seconds):
    """Returns early if a RESET comes in while sleeping."""
    reset_event.wait(timeout=seconds)


def run_one_demo_round(ser):
    global skip_next_auto_reset
    reset_event.clear()
    if skip_next_auto_reset:
        skip_next_auto_reset = False
    else:
        do_reset(ser)

    for team_id in range(1, MAX_TEAMS + 1):
        if reset_event.is_set():
            return
        sleep_interruptible(random.uniform(1.0, 2.5))
        if reset_event.is_set():
            return
        register_team(ser, team_id)

    if reset_event.is_set():
        return
    send_event(ser, f"All 4 teams registered - click START on the dashboard "
                     f"(auto-starting in {AUTO_START_AFTER_SECONDS}s otherwise)")

    deadline = time.time() + AUTO_START_AFTER_SECONDS
    while True:
        if reset_event.is_set():
            return
        with state_lock:
            if state["round_state"] == STATE_RUNNING:
                break
        if time.time() > deadline:
            start_game(ser)
            break
        time.sleep(0.3)

    while True:
        if reset_event.is_set():
            return
        with state_lock:
            still_running = state["round_state"] == STATE_RUNNING
        if not still_running:
            break
        sleep_interruptible(random.uniform(*ATTACK_DELAY_RANGE))
        if reset_event.is_set():
            return
        auto_attack(ser)

    send_event(ser, f"Demo round finished - starting a fresh round in "
                     f"{NEXT_ROUND_DELAY_SECONDS}s (or click RESET now)")
    sleep_interruptible(NEXT_ROUND_DELAY_SECONDS)


# =====================================================================
# ENTRY POINT
# =====================================================================

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else MOCK_PORT

    print("=== MERAZ BATTLESHIP - MOCK CENTRAL ===")
    print(f"Opening {port} @ {BAUD_RATE} baud...")
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"Could not open {port}: {e}")
        print("Check the virtual-port setup instructions at the top of this file.")
        raise SystemExit(1)

    print("Connected. Point dashboard.py's SERIAL_PORT at the OTHER half of the pair.")
    print("Running an automatic demo round now - also watching for dashboard button clicks.\n")

    threading.Thread(target=reader_thread, args=(ser,), daemon=True).start()

    while True:
        run_one_demo_round(ser)


if __name__ == "__main__":
    main()
