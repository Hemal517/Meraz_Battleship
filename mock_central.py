"""
MERAZ BATTLESHIP - MOCK CENTRAL ESP32 (HARDWARE-LESS TEST TOOL)
================================================================
Simulates the Central ESP32 over a Serial COM port so you can test
the organizer dashboard and participant nodes without any real hardware.

Usage:
    python mock_central.py --port COM2
Or loopback / virtual port pair:
    e.g. COM2 <-> COM3 (using com0com or socat)

What it does:
  1. Emulates the Central node state machine:
     SETUP -> READY -> RUNNING -> GAMEOVER
  2. Simulates incoming participant registrations with pacing.
  3. Responds to GET_STATE with full 5x5 grid state JSON snapshots.
  4. Responds to organizer commands:
     - START: Starts game if 4 teams are registered.
     - FORCE_START: Starts game immediately if >= 2 teams registered.
     - SKIP_TURN: Advances turn to the next alive team.
     - RESET: Clears all grids and resets to SETUP.
  5. Plays simulated attack turns (hits, misses, eliminations) so you
     can watch the live dashboard in action!
"""

import sys
import time
import json
import random
import argparse
import threading

try:
    import serial
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False


class MockCentral:
    def __init__(self, port, baud=115200, auto_play=True):
        self.port = port
        self.baud = baud
        self.auto_play = auto_play

        self.round_state = 0  # 0=SETUP, 1=READY, 2=RUNNING, 3=GAMEOVER
        self.current_turn = 1
        self.teams = [
            {"id": 1, "name": "AlphaSquad", "registered": False, "eliminated": False, "remaining": 9, "grid": self._make_grid()},
            {"id": 2, "name": "BetaCrew",   "registered": False, "eliminated": False, "remaining": 9, "grid": self._make_grid()},
            {"id": 3, "name": "GammaFleet", "registered": False, "eliminated": False, "remaining": 9, "grid": self._make_grid()},
            {"id": 4, "name": "DeltaForce", "registered": False, "eliminated": False, "remaining": 9, "grid": self._make_grid()},
        ]
        self.log = []
        self.running = True
        self.lock = threading.Lock()

    def _make_grid(self):
        # 5x5 grid: 0=water, 1=ship, 2=miss, 3=hit
        g = [[0 for _ in range(5)] for _ in range(5)]
        # Place 3 ships: size 1 at (0,0), size 3 at (0,1)-(2,1), size 5 at (4,0)-(4,4)
        g[0][0] = 1
        g[1][0] = 1
        g[1][1] = 1
        g[1][2] = 1
        for y in range(5):
            g[y][4] = 1
        return g

    def add_log(self, text):
        with self.lock:
            self.log.append(text)
            self.log = self.log[-20:]
        print(f"[Central Log] {text}")
        return json.dumps({"type": "event", "text": text}) + "\n"

    def get_state_json(self):
        with self.lock:
            state = {
                "type": "state",
                "round_state": self.round_state,
                "current_turn": self.current_turn if self.round_state == 2 else 0,
                "teams": self.teams,
                "log": self.log
            }
        return json.dumps(state) + "\n"

    def reset_game(self):
        with self.lock:
            self.round_state = 0
            self.current_turn = 1
            for t in self.teams:
                t["registered"] = False
                t["eliminated"] = False
                t["remaining"] = 9
                t["grid"] = self._make_grid()
        return self.add_log("Game reset - waiting for teams to register")

    def register_team(self, team_id):
        with self.lock:
            idx = team_id - 1
            if 0 <= idx < 4 and not self.teams[idx]["registered"]:
                self.teams[idx]["registered"] = True
                name = self.teams[idx]["name"]
                all_reg = all(t["registered"] for t in self.teams)
                if all_reg:
                    self.round_state = 1
                return self.add_log(f"Team {team_id} ({name}) registered. Ships verified OK.")
        return ""

    def start_game(self):
        with self.lock:
            if self.round_state == 1:
                self.round_state = 2
                self.current_turn = 1
                return self.add_log("Round started! Team 1 goes first.")
            else:
                return self.add_log("Cannot start: not all 4 teams are registered.")

    def force_start(self):
        with self.lock:
            reg_count = sum(1 for t in self.teams if t["registered"])
            if reg_count >= 2 and self.round_state in (0, 1):
                self.round_state = 2
                # first registered team
                first = next(t["id"] for t in self.teams if t["registered"])
                self.current_turn = first
                return self.add_log(f"GAME FORCE-STARTED by organizer with {reg_count} teams! Team {first} goes first.")
            else:
                return self.add_log("FORCE_START rejected: need at least 2 teams.")

    def skip_turn(self):
        with self.lock:
            if self.round_state != 2:
                return self.add_log("Cannot skip turn - game is not running.")
            return self._advance_turn(reason="Turn skipped by organizer")

    def _advance_turn(self, reason=""):
        alive_teams = [t["id"] for t in self.teams if t["registered"] and not t["eliminated"]]
        if len(alive_teams) <= 1:
            self.round_state = 3
            winner = alive_teams[0] if alive_teams else None
            win_msg = f"GAME OVER! Winner is Team {winner}!" if winner else "GAME OVER! No teams remain."
            return self.add_log(win_msg)

        # Advance clockwise
        cur = self.current_turn
        for _ in range(4):
            cur = (cur % 4) + 1
            if cur in alive_teams:
                self.current_turn = cur
                break

        msg = f"Team {self.current_turn}'s turn."
        if reason:
            msg = f"{reason}. {msg}"
        return self.add_log(msg)

    def execute_random_attack(self):
        with self.lock:
            # 1. Validate game is running
            if self.round_state != 2:
                return ""
            attacker = self.current_turn
            
            # 2. Find teams that are alive to target
            alive_opponents = [t for t in self.teams if t["registered"] and not t["eliminated"] and t["id"] != attacker]
            if not alive_opponents:
                return ""
            target_team = random.choice(alive_opponents)
            tid = target_team["id"]

            # 3. Pick a random grid cell that hasn't been shot yet
            unshot = []
            for r in range(5):
                for c in range(5):
                    if target_team["grid"][r][c] in (0, 1):
                        unshot.append((r, c))
            if not unshot:
                return ""

            r, c = random.choice(unshot)
            col_letter = chr(ord('A') + c)
            coord_str = f"{col_letter}{r + 1}"

            # 4. Check hit or miss and update the target's grid
            if target_team["grid"][r][c] == 1:
                target_team["grid"][r][c] = 3  # Hit
                target_team["remaining"] -= 1
                
                # Check if this hit eliminated them entirely
                if target_team["remaining"] <= 0:
                    target_team["eliminated"] = True
                    log_text = f"HIT! Team {attacker} sank Team {tid}'s final ship at {coord_str}! TEAM {tid} ELIMINATED!"
                else:
                    log_text = f"HIT! Team {attacker} struck Team {tid} at {coord_str}! ({target_team['remaining']} cells remain)"
            else:
                target_team["grid"][r][c] = 2  # Miss
                log_text = f"MISS! Team {attacker} fired at Team {tid} at {coord_str} (splash)."

        # 5. Log the result and automatically advance the turn
        out1 = self.add_log(log_text)
        out2 = self._advance_turn()
        return out1 + out2

    def run(self):
        print(f"=== MOCK CENTRAL ESP32 RUNNING on {self.port} ===")
        print("Ready for connections from dashboard.py or participant nodes.")

        ser = None
        if SERIAL_AVAILABLE:
            try:
                ser = serial.Serial(self.port, self.baud, timeout=0.1)
                print(f"Serial port {self.port} opened successfully.")
            except Exception as e:
                print(f"Notice: Serial port {self.port} could not be opened ({e}).")
                print("Running in simulation log mode.")

        # Registration simulation timer
        reg_step = 1
        last_action = time.time()

        while self.running:
            now = time.time()

            # Read incoming commands from dashboard
            if ser and ser.is_open:
                try:
                    line = ser.readline().decode("utf-8", errors="ignore").strip()
                    if line:
                        cmd = line.upper()
                        print(f"[Received Cmd] {cmd}")
                        if cmd == "GET_STATE":
                            ser.write(self.get_state_json().encode("utf-8"))
                        elif cmd == "START":
                            res = self.start_game()
                            if res: ser.write(res.encode("utf-8"))
                        elif cmd == "FORCE_START":
                            res = self.force_start()
                            if res: ser.write(res.encode("utf-8"))
                        elif cmd == "RESET":
                            res = self.reset_game()
                            if res: ser.write(res.encode("utf-8"))
                            reg_step = 1
                        elif cmd == "SKIP_TURN":
                            res = self.skip_turn()
                            if res: ser.write(res.encode("utf-8"))
                except Exception as e:
                    print(f"Serial error: {e}")

            # Auto-play game logic pacing
            if self.auto_play and (now - last_action > 3.0):
                last_action = now
                if self.round_state == 0 and reg_step <= 4:
                    ev = self.register_team(reg_step)
                    reg_step += 1
                    if ser and ser.is_open and ev:
                        ser.write(ev.encode("utf-8"))
                elif self.round_state == 2:
                    ev = self.execute_random_attack()
                    if ser and ser.is_open and ev:
                        ser.write(ev.encode("utf-8"))

            time.sleep(0.05)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Mock Central ESP32")
    parser.add_argument("--port", type=str, default="COM2", help="COM port to listen on")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("--no-autoplay", action="store_true", help="Disable automatic simulated registrations/attacks")
    args = parser.parse_args()

    mock = MockCentral(args.port, args.baud, auto_play=not args.no_autoplay)
    try:
        mock.run()
    except KeyboardInterrupt:
        print("\nShutting down Mock Central.")
