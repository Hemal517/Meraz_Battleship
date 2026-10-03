"""
MERAZ BATTLESHIP - VIRTUAL TFT DISPLAY & HARDWARE BRIDGE
=========================================================
Run this on your laptop to simulate the 2.4" ILI9341 touch display:
    python virtual_display.py
Or with a specific port:
    python virtual_display.py --port COM4 --team 1

What this script does:
  1. Mirrors the 240x320 touch UI of participant_node.ino (same pixel
     coordinates, buttons, colours and timing) in a desktop window.
  2. Connects over USB Serial to an ESP32 flashed with participant_node.ino.
     ZERO code changes are needed on participant_node.ino!
  3. BEFORE the board talks to Central, you choose your LOADOUT here
     (2 attack powers + 1 defence power) and press LOCK IN. The window then
     sends "LOADOUT ..." to the board, which registers with Central.
  4. On your turn you Single-Strike (pick a team, a cell, ATTACK) or press
     POWERS and pick a power: Sonar, Salvo, Mine, Double Attack, Smoke
     Screen, Repair - or the Shield, which every team has. The window sends the
     matching "t x y" or "POWER ..." command to the board.
  5. The ESP32 sends it over ESP-NOW to Central, receives the reply and
     prints it on Serial. This script parses the reply and shows the same
     result banner the real display would (HIT, MISS, ELIMINATED, MINE,
     BLOCKED, NO REPORT, or the power's result).
  6. Without any hardware the quick-test buttons still let you look at
     every screen and click through the flows ("Simulation Mode").

Serial lines this window understands (all printed by participant_node.ino):
  [LOADOUT] ...            the board is waiting for a loadout
  Loadout locked: ...      (byte 0xNN) the loadout is set
  >>> YOUR TURN <<<       Waiting - it is TEAM n's turn.       GAME OVER
  RESULT: TEAM t (x,y) -> HIT | MISS | MINE | BLOCKED | UNKNOWN | INVALID ...
  POWER_RESULT: power=P status=S target=T count=N r=a,b,c
  [UI] message            ship n: len=L start=(x,y) HORIZONTAL   (from CONFIG)
"""

import sys
import time
import threading
import argparse
import re
import tkinter as tk
from tkinter import ttk, messagebox

try:
    import serial
    import serial.tools.list_ports
    SERIAL_AVAILABLE = True
except ImportError:
    SERIAL_AVAILABLE = False


# =====================================================================
# CONSTANTS - the same numbers as participant_node.ino
# =====================================================================
BASE_WIDTH = 240
BASE_HEIGHT = 320
SCALE = 1.5  # 1.5x zoom -> 360x480 window for comfortable desktop viewing

UI_RESULT_DISPLAY_MS = 1500
UI_POWER_RESULT_MS = 3500

UI_TARGET_BTN_Y = 36
UI_TARGET_BTN_H = 30
UI_TARGET_BTN_W = 70
UI_TARGET_GAP   = 10

UI_GRID_X       = 22
UI_GRID_Y       = 74
UI_CELL_SIZE    = 28
GRID_SIZE       = 7

UI_ALT_BTN_X    = 10
UI_ALT_BTN_W    = 84
UI_ATTACK_BTN_X = 104
UI_ATTACK_BTN_Y = 280
UI_ATTACK_BTN_W = 126
UI_ATTACK_BTN_H = 28

UI_MENU_X, UI_MENU_W, UI_MENU_Y0, UI_MENU_H, UI_MENU_GAP = 10, 220, 36, 50, 6

LD_BTN_W, LD_BTN_H = 108, 40
LD_COL1_X, LD_COL2_X = 8, 124
LD_ATK_Y1, LD_ATK_Y2, LD_DEF_Y = 56, 100, 158
LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H = 40, 264, 160, 40

# power ids (same as meraz_loadout.h)
SONAR, SALVO, MINE, REPAIR, SMOKE, SHIELD, DOUBLE = 1, 2, 3, 4, 5, 6, 7
POWER_USES = 2
LD_ATTACK_IDS = [SONAR, SALVO, MINE, DOUBLE]
LD_DEFENCE_IDS = [SMOKE, REPAIR]

# id -> (name, hint, needs a target team, cells are on MY board, how many cells, action button label)
SPECS = {
    SONAR:  ("SONAR",  "scan a 3x3 area",  True,  False, 1, "PING"),
    SALVO:  ("SALVO",  "3 in a line",      True,  False, 3, "FIRE x3"),
    MINE:   ("MINE",   "hidden trap",      False, True,  1, "PLACE MINE"),
    REPAIR: ("REPAIR", "fix a hit cell",   False, True,  1, "REPAIR"),
    SMOKE:  ("SMOKE",  "hide attacks",     False, False, 0, "DEPLOY"),
    SHIELD: ("SHIELD", "block a 3x3 area", False, True,  1, "RAISE"),
    DOUBLE: ("DOUBLE", "2 shots at once",  True,  False, 2, "FIRE x2"),
}
MENU_ORDER = [SONAR, SALVO, MINE, DOUBLE, SMOKE, REPAIR]

# Colours (TFT_eSPI names, desktop-friendly shades)
BLACK, WHITE, NAVY, YELLOW, GREEN = "#000000", "#FFFFFF", "#000080", "#FFFF00", "#00FF00"
BLUE, ORANGE, RED, CYAN = "#0000FF", "#FFA500", "#FF0000", "#00FFFF"
DARKGREY, MIDGREY, MAROON, PURPLE = "#404040", "#7B7D7B", "#7B0000", "#7B007B"
DARKCYAN, DARKGREEN = "#007B7B", "#007B00"


def sx(v):
    return int(v * SCALE)


def sy(v):
    return int(v * SCALE)


def cells_form_line(cells):
    """Same rule as Central: n consecutive cells in one row or one column."""
    n = len(cells)
    rows = len({y for _, y in cells}) == 1
    cols = len({x for x, _ in cells}) == 1
    if rows == cols:
        return False
    vals = sorted(x if rows else y for x, y in cells)
    return vals[-1] - vals[0] == n - 1 and len(set(vals)) == n


class VirtualTFTDisplay(tk.Tk):
    def __init__(self, default_port=None, default_team=1):
        super().__init__()

        self.title(f"Meraz Battleship - Virtual TFT (Team {default_team})")
        self.resizable(False, False)
        self.configure(bg="#121620")

        self.my_team_id = default_team
        self.opponent_ids = [t for t in range(1, 5) if t != self.my_team_id]
        self.own_ship = set()
        self.set_ships([(1, 2, 4, 0), (3, 0, 0, 0), (5, 4, 0, 1)])   # default layout until CONFIG is read

        # which screen is showing
        self.screen = "LOADOUT"            # LOADOUT, MESSAGE, WAITING, ACTION, MENU, BANNER
        self.message_text = "Waiting for start..."
        self.waiting_team = 0
        self.banner = None                 # dict(bg, lines=[(text,size,color,y)])
        self.banner_expiry = 0
        self.banner_active = False
        self.pending_screen = None
        self.running = False               # a round is in progress
        self.my_turn = False

        # loadout
        self.ld_attack = set()
        self.ld_defence = None
        self.ld_armed_until = 0
        self.loadout_locked = False
        self.loadout = set()               # chosen optional powers
        self.uses_left = {p: 0 for p in SPECS}
        self.last_move_was_shield = False
        self.my_mine = None                # my last mine cell (x, y)

        # action screen state
        self.mode_power = 0                # 0 = Single Strike
        self.sel_target = 0
        self.sel_cells = []
        self.send_lock_until = 0
        self.last_send = None              # (power, target, cells) for the result banner

        # Serial connection
        self.serial_conn = None
        self.serial_thread = None
        self.running_app = True
        self.default_port = default_port

        self._create_widgets()
        self._refresh_ports()
        self.redraw()

        if default_port and SERIAL_AVAILABLE:
            self.port_combo.set(default_port)
            self.connect_serial()

        self.after(50, self.update_timer)

    # ------------------------------------------------------------------
    # window furniture
    # ------------------------------------------------------------------
    def _create_widgets(self):
        top_bar = tk.Frame(self, bg="#1a202c", pady=6, padx=8)
        top_bar.pack(fill=tk.X)

        tk.Label(top_bar, text="COM Port:", bg="#1a202c", fg="#e2e8f0", font=("Arial", 9, "bold")).pack(side=tk.LEFT, padx=3)
        self.port_combo = ttk.Combobox(top_bar, width=9, values=[])
        self.port_combo.pack(side=tk.LEFT, padx=3)

        tk.Button(top_bar, text="⟳", bg="#2d3748", fg="#e2e8f0", relief=tk.FLAT,
                  command=self._refresh_ports, font=("Arial", 9, "bold"), padx=4).pack(side=tk.LEFT, padx=2)

        self.btn_connect = tk.Button(top_bar, text="Connect", bg="#3182ce", fg="#ffffff", relief=tk.FLAT,
                                     command=self.toggle_connection, font=("Arial", 9, "bold"), padx=6)
        self.btn_connect.pack(side=tk.LEFT, padx=4)

        self.lbl_status = tk.Label(top_bar, text="Disconnected", bg="#1a202c", fg="#e53e3e", font=("Arial", 9))
        self.lbl_status.pack(side=tk.LEFT, padx=6)

        tk.Label(top_bar, text="Team:", bg="#1a202c", fg="#e2e8f0", font=("Arial", 9, "bold")).pack(side=tk.RIGHT, padx=2)
        self.team_spin = ttk.Spinbox(top_bar, from_=1, to=4, width=3, command=self._on_team_change)
        self.team_spin.set(self.my_team_id)
        self.team_spin.pack(side=tk.RIGHT, padx=4)

        bezel = tk.Frame(self, bg="#2a2e39", padx=10, pady=10)
        bezel.pack(padx=12, pady=8)
        tk.Label(bezel, text="2.4\" TFT TOUCH (ILI9341 240x320)", bg="#2a2e39", fg="#718096",
                 font=("Consolas", 8)).pack(anchor=tk.W, pady=(0, 4))

        self.canvas = tk.Canvas(bezel, width=sx(BASE_WIDTH), height=sy(BASE_HEIGHT),
                                bg=BLACK, highlightthickness=1, highlightbackground="#4a5568")
        self.canvas.pack()
        self.canvas.bind("<Button-1>", self.on_canvas_click)

        ctrl_bar = tk.Frame(self, bg="#1a202c", pady=6, padx=8)
        ctrl_bar.pack(fill=tk.X)
        tk.Label(ctrl_bar, text="Quick Test:", bg="#1a202c", fg="#a0aec0", font=("Arial", 8, "bold")).pack(side=tk.LEFT, padx=4)

        def qbtn(text, bg, cmd):
            tk.Button(ctrl_bar, text=text, bg=bg, fg="#fff", relief=tk.FLAT, font=("Arial", 8),
                      command=cmd).pack(side=tk.LEFT, padx=2)

        qbtn("Loadout", "#805ad5", self.demo_loadout)
        qbtn("Your Turn", "#2b6cb0", lambda: self.handle_line(">>> YOUR TURN <<<"))
        qbtn("Waiting", "#4a5568", lambda: self.handle_line("Waiting - it is TEAM 2's turn."))
        qbtn("Hit", "#dd6b20", lambda: self.handle_line("RESULT: TEAM 2 (1,2) -> HIT"))
        qbtn("Miss", "#3182ce", lambda: self.handle_line("RESULT: TEAM 2 (0,0) -> MISS"))
        qbtn("Mine", "#9b2c2c", lambda: self.handle_line("RESULT: TEAM 2 (4,4) -> MINE - it was a mine!"))

        self.log_visible = tk.BooleanVar(value=False)
        tk.Checkbutton(ctrl_bar, text="Console", variable=self.log_visible, bg="#1a202c", fg="#a0aec0",
                       selectcolor="#2d3748", command=self._toggle_log, font=("Arial", 8)).pack(side=tk.RIGHT, padx=4)

        self.log_frame = tk.Frame(self, bg="#0d1117", padx=4, pady=4)
        self.log_text = tk.Text(self.log_frame, height=5, width=45, bg="#0d1117", fg="#58a6ff",
                                font=("Consolas", 8), insertbackground="white")
        self.log_text.pack(fill=tk.BOTH, expand=True)

    def _toggle_log(self):
        if self.log_visible.get():
            self.log_frame.pack(fill=tk.BOTH, padx=8, pady=(0, 6), expand=True)
        else:
            self.log_frame.pack_forget()

    def _on_team_change(self):
        try:
            tid = int(self.team_spin.get())
        except ValueError:
            return
        if 1 <= tid <= 4:
            self.my_team_id = tid
            self.opponent_ids = [t for t in range(1, 5) if t != tid]
            self.title(f"Meraz Battleship - Virtual TFT (Team {tid})")
            self.redraw()

    # ------------------------------------------------------------------
    # serial
    # ------------------------------------------------------------------
    def _refresh_ports(self):
        if not SERIAL_AVAILABLE:
            self.port_combo['values'] = ["No pyserial"]
            return
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo['values'] = ports
        if ports and not self.port_combo.get():
            self.port_combo.set(ports[0])

    def toggle_connection(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.disconnect_serial()
        else:
            self.connect_serial()

    def connect_serial(self):
        if not SERIAL_AVAILABLE:
            messagebox.showerror("Error", "pyserial is not installed. Install with: pip install pyserial")
            return
        port = self.port_combo.get()
        if not port:
            messagebox.showwarning("Warning", "Select a valid COM port first.")
            return
        try:
            self.serial_conn = serial.Serial(port, 115200, timeout=0.1)
            self.lbl_status.config(text=f"Connected ({port})", fg="#38a169")
            self.btn_connect.config(text="Disconnect", bg="#e53e3e")
            self.append_log(f"--- Connected to {port} at 115200 baud ---")
            self.serial_thread = threading.Thread(target=self._read_serial_loop, daemon=True)
            self.serial_thread.start()
            self.after(500, lambda: self.send_serial_line("CONFIG"))
            self.after(900, lambda: self.send_serial_line("STATUS"))
        except Exception as e:
            messagebox.showerror("Serial Connection Failed", str(e))
            self.lbl_status.config(text="Error", fg="#e53e3e")

    def disconnect_serial(self):
        if self.serial_conn:
            try:
                self.serial_conn.close()
            except Exception:
                pass
            self.serial_conn = None
        self.lbl_status.config(text="Disconnected", fg="#e53e3e")
        self.btn_connect.config(text="Connect", bg="#3182ce")
        self.append_log("--- Disconnected ---")

    def send_serial_line(self, line):
        self.append_log(f">> {line}")
        if self.serial_conn and self.serial_conn.is_open:
            try:
                self.serial_conn.write((line.strip() + "\n").encode("utf-8"))
            except Exception as e:
                self.append_log(f"[Write Error] {e}")

    def append_log(self, text):
        def _add():
            self.log_text.insert(tk.END, text + "\n")
            self.log_text.see(tk.END)
        self.after(0, _add)

    def _read_serial_loop(self):
        buf = ""
        while self.running_app and self.serial_conn and self.serial_conn.is_open:
            try:
                data = self.serial_conn.read(256).decode("utf-8", errors="ignore")
                if data:
                    buf += data
                    while "\n" in buf:
                        line, buf = buf.split("\n", 1)
                        line = line.strip()
                        if line and not line.startswith("@TFT"):
                            self.append_log(line)
                            self.after(0, self.handle_line, line)
            except Exception as e:
                self.append_log(f"[Serial Read Error] {e}")
                break
            time.sleep(0.01)

    # ------------------------------------------------------------------
    # PARSE SERIAL MESSAGES FROM participant_node.ino
    # ------------------------------------------------------------------
    def set_ships(self, ships):
        """ships = [(len, x, y, orientation)] -> the set of my ship cells."""
        self.own_ship = set()
        for ln, x, y, o in ships:
            dx, dy = {0: (1, 0), 1: (0, 1), 2: (1, 1), 3: (1, -1)}.get(o, (1, 0))
            for i in range(ln):
                cx, cy = x + dx * i, y + dy * i
                if 0 <= cx < GRID_SIZE and 0 <= cy < GRID_SIZE:
                    self.own_ship.add((cx, cy))
        self._ships_read = []

    def handle_line(self, line):
        line = line.strip()

        # --- loadout ---
        if line.startswith("[LOADOUT]"):
            if not self.loadout_locked and self.screen != "LOADOUT":
                self.show_loadout()
            return
        m = re.search(r"Loadout locked:.*\(byte 0x([0-9A-Fa-f]+)\)", line)
        if m:
            self.apply_loadout_byte(int(m.group(1), 16))
            return

        # --- config (ships, team id) ---
        m = re.search(r"MY_TEAM_ID:\s*(\d+)", line)
        if m:
            self.my_team_id = int(m.group(1))
            self.opponent_ids = [t for t in range(1, 5) if t != self.my_team_id]
            self.team_spin.set(self.my_team_id)
            self.title(f"Meraz Battleship - Virtual TFT (Team {self.my_team_id})")
            self._ships_read = []
            self.redraw()
            return
        m = re.search(r"ship\s+\d+:\s*len=(\d+)\s+start=\((\d+),(\d+)\)\s+(\w+)(?:\s+\((\w+)-?(\w*)\))?", line)
        if m:
            ln, x, y = int(m.group(1)), int(m.group(2)), int(m.group(3))
            word = m.group(4)
            o = {"HORIZONTAL": 0, "VERTICAL": 1}.get(word, 2)
            if word == "DIAGONAL":
                o = 3 if "up" in line else 2
            self._ships_read.append((ln, x, y, o))
            if len(self._ships_read) == 3:
                ships = self._ships_read
                self.set_ships(ships)
                self.redraw()
            return
        m = re.search(r"Loadout:.*\(byte 0x([0-9A-Fa-f]+)\)", line)
        if m and not self.loadout_locked:
            self.apply_loadout_byte(int(m.group(1), 16))
            return

        # --- turn updates ---
        if ">>> YOUR TURN <<<" in line:
            self.running, self.my_turn = True, True
            self.show_your_turn()
            return
        m = re.search(r"Waiting - it is TEAM (\d+)'s turn", line, re.IGNORECASE)
        if m:
            self.running, self.my_turn = True, False
            self.show_waiting(int(m.group(1)))
            return
        if "Waiting for the organizer to start" in line:
            self.running, self.my_turn = False, False
            self.show_message("Waiting for start...")
            return
        if "GAME OVER" in line and "RESULT" not in line:
            self.running, self.my_turn = False, False
            self.show_message("GAME OVER")
            return

        # --- Single Strike result ---
        m = re.search(r"RESULT:\s*TEAM\s*(\d+)\s*\((\d+),(\d+)\)\s*->\s*(.*)", line, re.IGNORECASE)
        if m:
            target, x, y = int(m.group(1)), int(m.group(2)), int(m.group(3))
            raw = m.group(4).upper()
            if "ELIMINATED" in raw:
                kind = "ELIMINATED!"
            elif raw.startswith("MINE"):
                kind = "MINE"
            elif raw.startswith("BLOCKED"):
                kind = "BLOCKED"
            elif raw.startswith("UNKNOWN"):
                kind = "UNKNOWN"
            elif raw.startswith("HIT"):
                kind = "HIT"
            elif raw.startswith("MISS"):
                kind = "MISS"
            else:
                kind = "INVALID"
            if kind != "INVALID":
                self.last_move_was_shield = False
            self.show_result_banner(target, x, y, kind)
            return

        # --- power-up result ---
        m = re.search(r"POWER_RESULT:\s*power=(\d+)\s+status=(\d+)\s+target=(\d+)\s+count=(\d+)\s+r=(\d+),(\d+),(\d+)", line)
        if m:
            power, status, target, count = (int(m.group(i)) for i in range(1, 5))
            res = [int(m.group(5)), int(m.group(6)), int(m.group(7))]
            self.show_power_banner(power, status, target, count, res)
            return

        if line.startswith("[UI]"):
            self.show_message(line[4:].strip())

    def apply_loadout_byte(self, byte):
        self.loadout = {p for p in LD_ATTACK_IDS + LD_DEFENCE_IDS if byte & (1 << (p - 1))}
        self.uses_left = {p: (POWER_USES if p in self.loadout else 0) for p in SPECS}
        self.loadout_locked = True
        if self.screen == "LOADOUT":
            self.show_message("Connecting...")

    # ------------------------------------------------------------------
    # SCREEN STATE LOGIC
    # ------------------------------------------------------------------
    def _banner_busy(self):
        return time.time() * 1000 < self.banner_expiry

    def show_loadout(self):
        self.screen = "LOADOUT"
        self.redraw()

    def demo_loadout(self):
        self.loadout_locked = False
        self.ld_attack, self.ld_defence = set(), None
        self.show_loadout()

    def show_message(self, msg):
        if self._banner_busy():
            self.pending_screen = ("MESSAGE", msg)
            return
        self.screen, self.message_text = "MESSAGE", msg
        self.redraw()

    def show_waiting(self, team):
        if self._banner_busy():
            self.pending_screen = ("WAITING", team)
            return
        self.screen, self.waiting_team = "WAITING", team
        self.redraw()

    def show_your_turn(self):
        if self._banner_busy():
            self.pending_screen = ("ACTION", None)
            return
        if self.screen in ("ACTION", "MENU") and self.my_turn and not self.banner_active:
            return                       # a repeated turn update must not wipe a half-made selection
        self.start_strike()

    def start_strike(self):
        self.mode_power, self.sel_target, self.sel_cells = 0, 0, []
        self.screen = "ACTION"
        self.redraw()

    def start_power(self, power):
        self.mode_power, self.sel_target, self.sel_cells = power, 0, []
        self.screen = "ACTION"
        self.redraw()

    def _start_banner(self, banner, ms):
        self.banner = banner
        self.banner_expiry = time.time() * 1000 + ms
        self.banner_active = True
        self.screen = "BANNER"
        self.send_lock_until = 0
        self.redraw()

    def show_result_banner(self, target, x, y, kind):
        look = {"MISS": (BLUE, "MISS", ""), "HIT": (ORANGE, "HIT", ""), "ELIMINATED!": (RED, "ELIMINATED!", ""),
                "MINE": (MAROON, "MINE!", "YOU LOSE NEXT TURN"), "BLOCKED": (PURPLE, "BLOCKED", "SHIELD ABSORBED IT"),
                "UNKNOWN": (MIDGREY, "NO REPORT", "SMOKE SCREEN")}
        bg, label, extra = look.get(kind, (MIDGREY, "INVALID", ""))
        lines = [(label, 3, WHITE, 110), (f"TEAM {target} ({x},{y})", 2, WHITE, 150)]
        if extra:
            lines.append((extra, 2, WHITE, 182))
        self._start_banner({"bg": bg, "lines": lines}, UI_RESULT_DISPLAY_MS)

    def show_power_banner(self, power, status, target, count, res):
        name = SPECS[power][0] if power in SPECS else "POWER"
        info = self.last_send if self.last_send and self.last_send[0] == power else (power, target, [])
        cells = info[2]
        lines, bg = [], "#007B00"
        if status == 1:                                     # rejected
            bg = MIDGREY
            hint = {SONAR: "Cell already shot? Target out?", SALVO: "Cell already shot? Target out?",
                    DOUBLE: "Cell already shot? Target out?", MINE: "Mine still active? Cell shot?",
                    REPAIR: "That cell is not damaged", SHIELD: "No Shield two turns in a row",
                    SMOKE: "Smoke can't be used now"}.get(power, "Central refused it")
            lines = [("REJECTED", 3, WHITE, 70), (name, 2, WHITE, 110), ("Turn NOT used.", 2, WHITE, 150), (hint, 1, WHITE, 186)]
            self._start_banner({"bg": bg, "lines": lines}, UI_RESULT_DISPLAY_MS + 300)
            return
        # the move worked: update what the screens show
        if power != SHIELD and power in self.uses_left and self.uses_left[power] > 0:
            self.uses_left[power] -= 1
        self.last_move_was_shield = (power == SHIELD)
        if power == MINE and cells:
            self.my_mine = cells[0]
        c0 = cells[0] if cells else (0, 0)
        long_read = power in (SONAR, SALVO, DOUBLE)
        if power == SONAR:
            if status == 2:
                bg = MIDGREY
                lines = [("SONAR", 3, WHITE, 70), ("JAMMED", 3, WHITE, 120), ("Smoke blocked the ping", 1, WHITE, 170)]
            else:
                bg = DARKCYAN
                lines = [("SONAR", 3, WHITE, 70), (f"TEAM {target} around ({c0[0]},{c0[1]})", 1, WHITE, 114),
                         (f"{count} SHIP CELL" + ("" if count == 1 else "S"), 3, YELLOW, 140)]
        elif power in (SALVO, DOUBLE):
            names = {0: "MISS", 1: "HIT", 3: "SUNK", 4: "MINE", 5: "BLOCKED", 6: "UNKNOWN"}
            n = 3 if power == SALVO else 2
            bg = BLUE
            for r in res[:n]:
                if r == 3:
                    bg = RED
                elif r == 1 and bg != RED:
                    bg = ORANGE
                elif r == 4 and bg not in (RED, ORANGE):
                    bg = MAROON
                elif r in (5, 6) and bg == BLUE:
                    bg = MIDGREY
            lines = [(name, 3, WHITE, 40), (f"at TEAM {target}", 1, WHITE, 80)]
            for i in range(min(n, len(cells))):
                lines.append((f"({cells[i][0]},{cells[i][1]}) {names.get(res[i], 'NOT FIRED')}", 2, WHITE, 110 + i * 30))
        elif power == MINE:
            lines = [("MINE SET", 3, WHITE, 80), (f"at ({c0[0]},{c0[1]})", 2, YELLOW, 130), ("Only you know where it is.", 1, WHITE, 170)]
        elif power == REPAIR:
            lines = [("REPAIRED", 3, WHITE, 80), (f"({c0[0]},{c0[1]}) is intact", 2, YELLOW, 130)]
        elif power == SMOKE:
            lines = [("SMOKE UP", 3, WHITE, 80), ("Attacks on you are hidden", 1, WHITE, 130), ("until your next turn.", 1, WHITE, 144)]
        elif power == SHIELD:
            lines = [("SHIELD UP", 3, WHITE, 80), (f"3x3 around ({c0[0]},{c0[1]})", 2, YELLOW, 130), ("until your next turn.", 1, WHITE, 170)]
        else:
            lines = [("DONE", 3, WHITE, 120)]
        self._start_banner({"bg": bg, "lines": lines}, UI_POWER_RESULT_MS if long_read else UI_RESULT_DISPLAY_MS + 300)

    def update_timer(self):
        now = time.time() * 1000
        if self.loadout_locked is False and self.ld_armed_until and now > self.ld_armed_until:
            self.ld_armed_until = 0
            if self.screen == "LOADOUT":
                self.redraw()
        if self.banner_active and now >= self.banner_expiry:
            self.banner_active = False
            pend, self.pending_screen = self.pending_screen, None
            if pend:
                kind, data = pend
                if kind == "ACTION":
                    self.start_strike()
                elif kind == "WAITING":
                    self.show_waiting(data)
                else:
                    self.show_message(data)
            elif self.running and self.my_turn:
                self.start_strike()                       # a refused move: it is still my turn
            elif self.running:
                self.show_waiting(self.waiting_team)
            else:
                self.show_message(self.message_text)
        self.after(50, self.update_timer)

    # ------------------------------------------------------------------
    # DRAWING (a mirror of participant_node.ino's UI layer)
    # ------------------------------------------------------------------
    def text(self, x, y, s, size=1, color=WHITE, anchor="nw"):
        """x, y = top-left of the text in 240x320 screen pixels; size = TFT text size (6x8 px per char)."""
        self.canvas.create_text(sx(x), sy(y), text=s, fill=color, anchor=anchor,
                                font=("Courier", -int(10 * size * SCALE), "bold"))

    def ctext(self, s, y, size, color):
        self.canvas.create_text(sx(120), sy(y + 4 * size), text=s, fill=color, anchor="center",
                                font=("Courier", -int(10 * size * SCALE), "bold"))

    def rect(self, x, y, w, h, fill=None, outline=WHITE):
        self.canvas.create_rectangle(sx(x), sy(y), sx(x + w), sy(y + h), fill=fill or "", outline=outline or "", width=1)

    def button(self, x, y, w, h, fill, label, size=2, color=WHITE):
        self.rect(x, y, w, h, fill=fill)
        self.canvas.create_text(sx(x + w / 2), sy(y + h / 2), text=label, fill=color, anchor="center",
                                font=("Courier", -int(10 * size * SCALE), "bold"))

    def redraw(self):
        self.canvas.delete("all")
        self.canvas.create_rectangle(0, 0, sx(BASE_WIDTH), sy(BASE_HEIGHT), fill=BLACK, outline="")
        if self.screen == "BANNER" and self.banner:
            self.canvas.create_rectangle(0, 0, sx(BASE_WIDTH), sy(BASE_HEIGHT), fill=self.banner["bg"], outline="")
            for text, size, color, y in self.banner["lines"]:
                self.ctext(text, y, size, color)
        elif self.screen == "LOADOUT":
            self._draw_loadout()
        elif self.screen == "ACTION":
            self._draw_action()
        elif self.screen == "MENU":
            self._draw_menu()
        elif self.screen == "WAITING":
            self.text(10, 130, "Waiting for", 2)
            self.text(10, 160, f"TEAM {self.waiting_team}" if self.waiting_team else "NEXT TURN", 2)
        else:
            self.text(10, 140, self.message_text, 2)

    # ---- loadout ----
    def _ld_valid(self):
        return len(self.ld_attack) == 2 and self.ld_defence is not None

    def _draw_loadout(self):
        self.text(10, 8, "CHOOSE POWERS", 2, GREEN)
        self.text(8, 30, "Always yours: STRIKE + SHIELD", 1)
        self.text(8, 44, "ATTACK - pick 2 (2 uses each)", 1, CYAN)
        for i, pid in enumerate(LD_ATTACK_IDS):
            x = LD_COL2_X if i % 2 else LD_COL1_X
            y = LD_ATK_Y2 if i // 2 else LD_ATK_Y1
            self._ld_button(x, y, pid, pid in self.ld_attack)
        self.text(8, 146, "DEFENCE - pick 1 (2 uses each)", 1, CYAN)
        for j, pid in enumerate(LD_DEFENCE_IDS):
            self._ld_button(LD_COL2_X if j else LD_COL1_X, LD_DEF_Y, pid, self.ld_defence == pid)
        self.text(8, 214, f"Picked: {len(self.ld_attack)}/2 attack, {1 if self.ld_defence else 0}/1 defence", 1)
        self.text(8, 232, "Locked in = no changes this match.", 1)
        armed = self.ld_armed_until and time.time() * 1000 < self.ld_armed_until
        if not self._ld_valid():
            fill, label = DARKGREY, "PICK 2 + 1"
        elif armed:
            fill, label = RED, "TAP AGAIN"
        else:
            fill, label = DARKGREEN, "LOCK IN"
        self.button(LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H, fill, label)

    def _ld_button(self, x, y, pid, selected):
        name, hint = SPECS[pid][0], SPECS[pid][1]
        fill, fg = (YELLOW, BLACK) if selected else (NAVY, WHITE)
        self.rect(x, y, LD_BTN_W, LD_BTN_H, fill=fill)
        self.text(x + 8, y + 6, name, 2, fg)
        self.text(x + 8, y + 26, hint, 1, fg)

    # ---- action screen (strike / power) ----
    def _spec(self):
        return SPECS.get(self.mode_power)

    def _needs_target(self):
        return True if self.mode_power == 0 else self._spec()[2]

    def _own_grid(self):
        return False if self.mode_power == 0 else self._spec()[3]

    def _n_cells(self):
        return 1 if self.mode_power == 0 else self._spec()[4]

    def _action_ready(self):
        if self._needs_target() and not self.sel_target:
            return False
        if len(self.sel_cells) != self._n_cells():
            return False
        if self.mode_power == SALVO and not cells_form_line(self.sel_cells):
            return False
        return True

    def _cell_allowed(self, c, r):
        if self.mode_power == MINE:
            return (c, r) not in self.own_ship
        if self.mode_power == REPAIR:
            return (c, r) in self.own_ship
        if self.mode_power == SHIELD:
            return 1 <= c <= GRID_SIZE - 2 and 1 <= r <= GRID_SIZE - 2
        return True

    def _toggle_cell(self, c, r):
        n = self._n_cells()
        if n == 1:
            self.sel_cells = [(c, r)]
        elif (c, r) in self.sel_cells:
            self.sel_cells.remove((c, r))
        else:
            if len(self.sel_cells) == n:
                self.sel_cells.pop(0)
            self.sel_cells.append((c, r))

    def _cell_color(self, c, r):
        sel = False
        if self.mode_power in (SHIELD, SONAR) and len(self.sel_cells) == 1:
            sel = abs(c - self.sel_cells[0][0]) <= 1 and abs(r - self.sel_cells[0][1]) <= 1
        else:
            sel = (c, r) in self.sel_cells
        if sel:
            return YELLOW
        if self._own_grid():
            if self.my_mine == (c, r):
                return MAROON
            if (c, r) in self.own_ship:
                return DARKCYAN
        return DARKGREY

    def _draw_action(self):
        strike = self.mode_power == 0
        spec = None if strike else self._spec()
        self.text(10, 8, "YOUR TURN" if strike else spec[0], 2, GREEN if strike else YELLOW)
        if self._needs_target():
            self.text(10, UI_TARGET_BTN_Y - 10, "Select target:", 1)
            for i in range(3):
                tid = self.opponent_ids[i]
                x = 5 + i * (UI_TARGET_BTN_W + UI_TARGET_GAP)
                sel = self.sel_target == tid
                self.button(x, UI_TARGET_BTN_Y, UI_TARGET_BTN_W, UI_TARGET_BTN_H, YELLOW if sel else NAVY,
                            f"T{tid}", 2, BLACK if sel else WHITE)
        else:
            l1, l2 = {MINE: ("Tap an EMPTY cell of YOUR board.", "Hidden until someone shoots it."),
                      REPAIR: ("Tap one of YOUR damaged cells.", "(teal = your ships)"),
                      SHIELD: ("Tap the CENTRE of a 3x3 block on", "YOUR board (inner cells only)."),
                      SMOKE: ("No cells to pick.", "")}.get(self.mode_power, ("", ""))
            self.text(10, 30, l1, 1)
            self.text(10, 42, l2, 1)

        if self._n_cells() == 0:
            self.text(10, 96, "SMOKE SCREEN", 2)
            for k, s in enumerate(["Until your next turn, every attack", "on you is hidden from the attacker",
                                   "AND from the projector."]):
                self.text(10, 130 + 14 * k, s, 1)
            self.text(10, 180, "Sonar pings at you are jammed.", 1)
            self.text(10, 194, "Using it takes your turn.", 1)
        else:
            for r in range(GRID_SIZE):
                for c in range(GRID_SIZE):
                    x, y = UI_GRID_X + c * UI_CELL_SIZE, UI_GRID_Y + r * UI_CELL_SIZE
                    color = self._cell_color(c, r)
                    self.rect(x + 1, y + 1, UI_CELL_SIZE - 2, UI_CELL_SIZE - 2, fill=color, outline=None)
                    self.rect(x, y, UI_CELL_SIZE, UI_CELL_SIZE)
                    if self._own_grid() and color == MAROON:
                        self.text(x + 8, y + 6, "M", 2)

        self.button(UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H, NAVY, "POWERS" if strike else "BACK")
        label = "ATTACK" if strike else spec[5]
        self.button(UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H,
                    RED if self._action_ready() else DARKGREY, label)
        if time.time() * 1000 < self.send_lock_until:
            self.text(125, 12, "Sent - waiting...", 1)

    # ---- power menu ----
    def _menu_items(self):
        items = [p for p in MENU_ORDER if p in self.loadout][:3]
        return items + [SHIELD]

    def _usable(self, pid):
        return pid == SHIELD or self.uses_left.get(pid, 0) > 0

    def _draw_menu(self):
        self.text(10, 8, "POWERS", 2, GREEN)
        self.text(10, 26, "Using a power takes your turn.", 1)
        for i, pid in enumerate(self._menu_items()):
            y = UI_MENU_Y0 + i * (UI_MENU_H + UI_MENU_GAP)
            usable = self._usable(pid)
            self.rect(UI_MENU_X, y, UI_MENU_W, UI_MENU_H, fill=NAVY if usable else DARKGREY)
            self.text(UI_MENU_X + 10, y + 8, SPECS[pid][0], 2)
            self.text(UI_MENU_X + 10, y + 32, SPECS[pid][1], 1)
            if pid == SHIELD:
                self.text(UI_MENU_X + 150, y + 20, "COOLDOWN" if self.last_move_was_shield else "ALWAYS", 1)
            else:
                self.text(UI_MENU_X + 192, y + 8, str(self.uses_left.get(pid, 0)), 3)
                self.text(UI_MENU_X + 184, y + 36, "left", 1)
        self.button(UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H, NAVY, "BACK")

    # ------------------------------------------------------------------
    # TOUCH / CLICK HANDLING
    # ------------------------------------------------------------------
    @staticmethod
    def _in(px, py, x, y, w, h):
        return x <= px < x + w and y <= py < y + h

    def on_canvas_click(self, event):
        px, py = event.x / SCALE, event.y / SCALE
        if self.screen == "LOADOUT":
            return self._click_loadout(px, py)
        if self.screen == "MENU":
            return self._click_menu(px, py)
        if self.screen != "ACTION" or not (self.running and self.my_turn):
            return
        if time.time() * 1000 < self.send_lock_until:
            return
        if self._needs_target():
            for i in range(3):
                if self._in(px, py, 5 + i * (UI_TARGET_BTN_W + UI_TARGET_GAP), UI_TARGET_BTN_Y, UI_TARGET_BTN_W, UI_TARGET_BTN_H):
                    self.sel_target = self.opponent_ids[i]
                    return self.redraw()
        if self._n_cells() > 0 and self._in(px, py, UI_GRID_X, UI_GRID_Y, GRID_SIZE * UI_CELL_SIZE, GRID_SIZE * UI_CELL_SIZE):
            c, r = int((px - UI_GRID_X) // UI_CELL_SIZE), int((py - UI_GRID_Y) // UI_CELL_SIZE)
            if self._cell_allowed(c, r):
                self._toggle_cell(c, r)
                self.redraw()
            return
        if self._in(px, py, UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H):
            self.screen = "MENU"          # POWERS opens the menu; BACK on a power screen returns to it
            return self.redraw()
        if self._in(px, py, UI_ATTACK_BTN_X, UI_ATTACK_BTN_Y, UI_ATTACK_BTN_W, UI_ATTACK_BTN_H) and self._action_ready():
            self._send_selection()

    def _send_selection(self):
        if self.mode_power == 0:
            x, y = self.sel_cells[0]
            self.send_serial_line(f"{self.sel_target} {x} {y}")
        else:
            name = SPECS[self.mode_power][0]
            parts = ["POWER", name]
            if SPECS[self.mode_power][2]:
                parts.append(str(self.sel_target))
            for x, y in self.sel_cells:
                parts += [str(x), str(y)]
            self.last_send = (self.mode_power, self.sel_target, list(self.sel_cells))
            self.send_serial_line(" ".join(parts))
        self.send_lock_until = time.time() * 1000 + 2000
        self.redraw()

    def _click_menu(self, px, py):
        if self._in(px, py, UI_ALT_BTN_X, UI_ATTACK_BTN_Y, UI_ALT_BTN_W, UI_ATTACK_BTN_H):
            return self.start_strike()
        for i, pid in enumerate(self._menu_items()):
            y = UI_MENU_Y0 + i * (UI_MENU_H + UI_MENU_GAP)
            if self._in(px, py, UI_MENU_X, y, UI_MENU_W, UI_MENU_H):
                if self._usable(pid):
                    self.start_power(pid)
                return

    def _click_loadout(self, px, py):
        if self.loadout_locked:
            return
        for i, pid in enumerate(LD_ATTACK_IDS):
            x = LD_COL2_X if i % 2 else LD_COL1_X
            y = LD_ATK_Y2 if i // 2 else LD_ATK_Y1
            if self._in(px, py, x, y, LD_BTN_W, LD_BTN_H):
                if pid in self.ld_attack:
                    self.ld_attack.discard(pid)
                elif len(self.ld_attack) < 2:
                    self.ld_attack.add(pid)
                self.ld_armed_until = 0
                return self.redraw()
        for j, pid in enumerate(LD_DEFENCE_IDS):
            if self._in(px, py, LD_COL2_X if j else LD_COL1_X, LD_DEF_Y, LD_BTN_W, LD_BTN_H):
                self.ld_defence = None if self.ld_defence == pid else pid
                self.ld_armed_until = 0
                return self.redraw()
        if self._in(px, py, LD_OK_X, LD_OK_Y, LD_OK_W, LD_OK_H) and self._ld_valid():
            now = time.time() * 1000
            if not (self.ld_armed_until and now < self.ld_armed_until):
                self.ld_armed_until = now + 3000          # first tap arms it, the second one locks in
                return self.redraw()
            ids = sorted(self.ld_attack) + [self.ld_defence]
            self.ld_armed_until = 0
            self.send_serial_line("LOADOUT " + " ".join(str(i) for i in ids))
            # the board answers "Loadout locked ... (byte 0x..)", which switches the window over;
            # with no board connected, lock it here so the rest of the screens can be tried
            if not (self.serial_conn and self.serial_conn.is_open):
                byte = 0x20
                for i in ids:
                    byte |= 1 << (i - 1)
                self.apply_loadout_byte(byte)
                self.handle_line("Waiting for the organizer to start the game...")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Meraz Battleship - Virtual TFT Display")
    parser.add_argument("--port", type=str, default=None, help="Serial port of participant ESP32 (e.g. COM4)")
    parser.add_argument("--team", type=int, default=1, help="Team ID (1-4)")
    args = parser.parse_args()

    app = VirtualTFTDisplay(default_port=args.port, default_team=args.team)
    try:
        app.mainloop()
    finally:
        app.running_app = False
        if app.serial_conn:
            try:
                app.serial_conn.close()
            except Exception:
                pass
