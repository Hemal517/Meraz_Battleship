"""
MERAZ BATTLESHIP - VIRTUAL TFT DISPLAY & HARDWARE BRIDGE
=========================================================
Run this on your laptop to simulate the 2.4" ILI9341 touch display:
    python virtual_display.py
Or with a specific port:
    python virtual_display.py --port COM4 --team 1

What this script does:
  1. Emulates the physical 240x320 ILI9341 TFT display down to the exact
     pixel coordinates, button dimensions, colors, and timing defined in
     participant_node.ino.
  2. Connects over USB Serial to an ESP32 flashed with participant_node.ino.
     ZERO code changes are needed on participant_node.ino!
  3. When it is your turn, you click the target team button (T2, T3, T4)
     and click a grid cell (A-E, 1-5).
  4. Clicking the red ATTACK button transmits the attack string
     (e.g., "2 3 4\n") over Serial to the participant ESP32.
  5. The ESP32 sends the attack over ESP-NOW to Central, receives the feedback,
     and logs the result back over Serial.
  6. This script parses the result and displays the exact 1500ms full-screen
     result banner (HIT = Orange, MISS = Blue, ELIMINATED = Red).
  7. Includes a built-in "Simulation Mode" so you can test all screens and
     clicks even without plugging in any hardware!
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
# EXACT CONSTANTS FROM participant_node.ino
# =====================================================================
BASE_WIDTH = 240
BASE_HEIGHT = 320
SCALE = 1.5  # 1.5x zoom -> 360x480 window for comfortable desktop viewing

UI_RESULT_DISPLAY_MS = 1500

UI_TARGET_BTN_Y = 40
UI_TARGET_BTN_H = 36
UI_TARGET_BTN_W = 70
UI_TARGET_GAP   = 10

UI_GRID_X       = 20
UI_GRID_Y       = 90
UI_CELL_SIZE    = 40
GRID_SIZE       = 5

UI_ATTACK_BTN_X = 70
UI_ATTACK_BTN_Y = 292
UI_ATTACK_BTN_W = 100
UI_ATTACK_BTN_H = 26

# Colors matching TFT_eSPI
COLOR_BLACK     = "#000000"
COLOR_WHITE     = "#FFFFFF"
COLOR_NAVY      = "#000080"
COLOR_YELLOW    = "#FFFF00"
COLOR_GREEN     = "#00FF00"
COLOR_BLUE      = "#0000FF"
COLOR_ORANGE    = "#FFA500"
COLOR_RED       = "#FF0000"
COLOR_DARKGREY  = "#404040"
COLOR_CYAN      = "#00FFFF"


def sx(val):
    return int(val * SCALE)

def sy(val):
    return int(val * SCALE)


class VirtualTFTDisplay(tk.Tk):
    def __init__(self, default_port=None, default_team=1):
        super().__init__()

        self.title(f"Meraz Battleship - Virtual TFT (Team {default_team})")
        self.resizable(False, False)
        self.configure(bg="#121620")

        self.my_team_id = default_team
        self.my_team_name = f"TEAM {default_team}"
        self.opponent_ids = [t for t in range(1, 5) if t != self.my_team_id]

        # UI state
        self.current_screen = "WAITING"   # WAITING, YOUR_TURN, BANNER, MESSAGE, GAMEOVER
        self.pending_screen = None
        self.waiting_team = 0
        self.message_text = "Waiting for start..."
        self.banner_info = None           # (target, x, y, result_str)
        self.banner_expiry = 0

        # Selection state
        self.sel_target = 0
        self.sel_x = -1
        self.sel_y = -1
        self.attack_in_flight = False

        # Serial connection
        self.serial_conn = None
        self.serial_thread = None
        self.running = True
        self.default_port = default_port

        self._create_widgets()
        self._refresh_ports()

        if default_port and SERIAL_AVAILABLE:
            self.port_combo.set(default_port)
            self.connect_serial()

        # Start render & timer loop
        self.after(50, self.update_timer)

    def _create_widgets(self):
        # --- Top Toolbar: COM port controls ---
        top_bar = tk.Frame(self, bg="#1a202c", pady=6, padx=8)
        top_bar.pack(fill=tk.X)

        tk.Label(top_bar, text="COM Port:", bg="#1a202c", fg="#e2e8f0", font=("Arial", 9, "bold")).pack(side=tk.LEFT, padx=3)
        self.port_combo = ttk.Combobox(top_bar, width=9, values=[])
        self.port_combo.pack(side=tk.LEFT, padx=3)

        btn_refresh = tk.Button(top_bar, text="⟳", bg="#2d3748", fg="#e2e8f0", relief=tk.FLAT,
                                command=self._refresh_ports, font=("Arial", 9, "bold"), padx=4)
        btn_refresh.pack(side=tk.LEFT, padx=2)

        self.btn_connect = tk.Button(top_bar, text="Connect", bg="#3182ce", fg="#ffffff", relief=tk.FLAT,
                                     command=self.toggle_connection, font=("Arial", 9, "bold"), padx=6)
        self.btn_connect.pack(side=tk.LEFT, padx=4)

        self.lbl_status = tk.Label(top_bar, text="Disconnected", bg="#1a202c", fg="#e53e3e", font=("Arial", 9))
        self.lbl_status.pack(side=tk.LEFT, padx=6)

        # Team selector
        tk.Label(top_bar, text="Team:", bg="#1a202c", fg="#e2e8f0", font=("Arial", 9, "bold")).pack(side=tk.RIGHT, padx=2)
        self.team_spin = ttk.Spinbox(top_bar, from_=1, to=4, width=3, command=self._on_team_change)
        self.team_spin.set(self.my_team_id)
        self.team_spin.pack(side=tk.RIGHT, padx=4)

        # --- Middle: TFT Canvas Frame (with realistic bezel) ---
        bezel = tk.Frame(self, bg="#2a2e39", padx=10, pady=10)
        bezel.pack(padx=12, pady=8)

        # Sub-header label for physical display bezel
        bezel_header = tk.Label(bezel, text="2.4\" TFT TOUCH (ILI9341 240x320)", bg="#2a2e39", fg="#718096", font=("Consolas", 8))
        bezel_header.pack(anchor=tk.W, pady=(0, 4))

        self.canvas = tk.Canvas(bezel, width=sx(BASE_WIDTH), height=sy(BASE_HEIGHT),
                                bg=COLOR_BLACK, highlightthickness=1, highlightbackground="#4a5568")
        self.canvas.pack()
        self.canvas.bind("<Button-1>", self.on_canvas_click)

        # --- Bottom Toolbar: Simulation & Log Drawer ---
        ctrl_bar = tk.Frame(self, bg="#1a202c", pady=6, padx=8)
        ctrl_bar.pack(fill=tk.X)

        tk.Label(ctrl_bar, text="Quick Test:", bg="#1a202c", fg="#a0aec0", font=("Arial", 8, "bold")).pack(side=tk.LEFT, padx=4)

        btn_sim_turn = tk.Button(ctrl_bar, text="Your Turn", bg="#2b6cb0", fg="#fff", relief=tk.FLAT, font=("Arial", 8),
                                 command=lambda: self.handle_line(">>> YOUR TURN <<<"))
        btn_sim_turn.pack(side=tk.LEFT, padx=2)

        btn_sim_wait = tk.Button(ctrl_bar, text="Waiting", bg="#4a5568", fg="#fff", relief=tk.FLAT, font=("Arial", 8),
                                 command=lambda: self.handle_line("Waiting - it is TEAM 2's turn."))
        btn_sim_wait.pack(side=tk.LEFT, padx=2)

        btn_sim_hit = tk.Button(ctrl_bar, text="Hit", bg="#dd6b20", fg="#fff", relief=tk.FLAT, font=("Arial", 8),
                                command=lambda: self.handle_line("RESULT: TEAM 2 (1,2) -> HIT"))
        btn_sim_hit.pack(side=tk.LEFT, padx=2)

        btn_sim_miss = tk.Button(ctrl_bar, text="Miss", bg="#3182ce", fg="#fff", relief=tk.FLAT, font=("Arial", 8),
                                 command=lambda: self.handle_line("RESULT: TEAM 2 (0,0) -> MISS"))
        btn_sim_miss.pack(side=tk.LEFT, padx=2)

        btn_sim_sunk = tk.Button(ctrl_bar, text="Sunk", bg="#e53e3e", fg="#fff", relief=tk.FLAT, font=("Arial", 8),
                                 command=lambda: self.handle_line("RESULT: TEAM 2 (4,4) -> HIT - TEAM 2 ELIMINATED!"))
        btn_sim_sunk.pack(side=tk.LEFT, padx=2)

        # Serial monitor expander
        self.log_visible = tk.BooleanVar(value=False)
        btn_log_toggle = tk.Checkbutton(ctrl_bar, text="Console", variable=self.log_visible,
                                        bg="#1a202c", fg="#a0aec0", selectcolor="#2d3748",
                                        command=self._toggle_log, font=("Arial", 8))
        btn_log_toggle.pack(side=tk.RIGHT, padx=4)

        # Log Text Box
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
            if 1 <= tid <= 4:
                self.my_team_id = tid
                self.my_team_name = f"TEAM {tid}"
                self.opponent_ids = [t for t in range(1, 5) if t != self.my_team_id]
                self.title(f"Meraz Battleship - Virtual TFT (Team {tid})")
                self.redraw()
        except ValueError:
            pass

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

            # Start background reader thread
            self.serial_thread = threading.Thread(target=self._read_serial_loop, daemon=True)
            self.serial_thread.start()

            # Request config from board
            self.after(500, lambda: self.send_serial_line("CONFIG"))
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
        while self.running and self.serial_conn and self.serial_conn.is_open:
            try:
                data = self.serial_conn.read(256).decode("utf-8", errors="ignore")
                if data:
                    buf += data
                    while "\n" in buf:
                        line, buf = buf.split("\n", 1)
                        line = line.strip()
                        if line:
                            self.append_log(line)
                            self.after(0, self.handle_line, line)
            except Exception as e:
                self.append_log(f"[Serial Read Error] {e}")
                break
            time.sleep(0.01)

    # =====================================================================
    # PARSE SERIAL MESSAGES FROM participant_node.ino
    # =====================================================================
    def handle_line(self, line):
        line_clean = line.strip()

        # Turn update: YOUR TURN
        if ">>> YOUR TURN <<<" in line_clean:
            self.show_your_turn()
            return

        # Turn update: Waiting for another team
        m_wait = re.search(r"Waiting - it is TEAM (\d+)'s turn", line_clean, re.IGNORECASE)
        if m_wait:
            team_num = int(m_wait.group(1))
            self.show_waiting(team_num)
            return

        # Turn update: Setup/Ready
        if "Waiting for the organizer to start" in line_clean:
            self.show_message("Waiting for start...")
            return

        # Game over
        if "GAME OVER" in line_clean:
            self.show_message("GAME OVER")
            return

        # Result Banner:
        # e.g.: RESULT: TEAM 2 (3,4) -> HIT
        # e.g.: RESULT: TEAM 2 (3,4) -> MISS
        # e.g.: RESULT: TEAM 2 (3,4) -> HIT - TEAM 2 ELIMINATED!
        # e.g.: RESULT: TEAM 2 (3,4) -> INVALID (rejected by Central)
        m_res = re.search(r"RESULT:\s*TEAM\s*(\d+)\s*\((\d+),(\d+)\)\s*->\s*(.*)", line_clean, re.IGNORECASE)
        if m_res:
            target_id = int(m_res.group(1))
            x = int(m_res.group(2))
            y = int(m_res.group(3))
            raw_res = m_res.group(4).upper()

            if "ELIMINATED" in raw_res:
                result_code = "ELIMINATED!"
            elif "HIT" in raw_res:
                result_code = "HIT"
            elif "MISS" in raw_res:
                result_code = "MISS"
            else:
                result_code = "INVALID"

            self.show_result_banner(target_id, x, y, result_code)
            return

        # [UI] prefixes from participant firmware
        if line_clean.startswith("[UI]"):
            msg = line_clean[4:].strip()
            self.show_message(msg)
            return

        # Config detection: auto-adjust team ID
        m_cfg = re.search(r"MY_TEAM_ID:\s*(\d+)", line_clean)
        if m_cfg:
            self.my_team_id = int(m_cfg.group(1))
            self.my_team_name = f"TEAM {self.my_team_id}"
            self.opponent_ids = [t for t in range(1, 5) if t != self.my_team_id]
            self.team_spin.set(self.my_team_id)
            self.title(f"Meraz Battleship - Virtual TFT (Team {self.my_team_id})")
            self.redraw()

    # =====================================================================
    # SCREEN STATE LOGIC
    # =====================================================================
    def show_message(self, msg):
        now = time.time() * 1000
        if now < self.banner_expiry:
            self.pending_screen = ("MESSAGE", msg)
            return
        self.current_screen = "MESSAGE"
        self.message_text = msg
        self.redraw()

    def show_waiting(self, current_team):
        now = time.time() * 1000
        if now < self.banner_expiry:
            self.pending_screen = ("WAITING", current_team)
            return
        self.current_screen = "WAITING"
        self.waiting_team = current_team
        self.redraw()

    def show_your_turn(self):
        now = time.time() * 1000
        if now < self.banner_expiry:
            self.pending_screen = ("YOUR_TURN", None)
            return
        self.current_screen = "YOUR_TURN"
        self.sel_target = 0
        self.sel_x = -1
        self.sel_y = -1
        self.attack_in_flight = False
        self.redraw()

    def show_result_banner(self, target, x, y, result_str):
        self.current_screen = "BANNER"
        self.banner_info = (target, x, y, result_str)
        self.banner_expiry = (time.time() * 1000) + UI_RESULT_DISPLAY_MS
        self.redraw()

    def update_timer(self):
        now = time.time() * 1000
        if self.current_screen == "BANNER" and now >= self.banner_expiry:
            if self.pending_screen:
                stype, sdata = self.pending_screen
                self.pending_screen = None
                if stype == "YOUR_TURN":
                    self.show_your_turn()
                elif stype == "WAITING":
                    self.show_waiting(sdata)
                elif stype == "MESSAGE":
                    self.show_message(sdata)
            else:
                self.show_waiting(0)

        self.after(50, self.update_timer)

    # =====================================================================
    # DRAWING FUNCTIONS (exact replica of participant_node.ino UI layer)
    # =====================================================================
    def redraw(self):
        self.canvas.delete("all")

        if self.current_screen == "BANNER" and self.banner_info:
            target, x, y, res = self.banner_info
            self._draw_result_banner(target, x, y, res)
        elif self.current_screen == "YOUR_TURN":
            self._draw_your_turn()
        elif self.current_screen == "WAITING":
            self._draw_waiting()
        else:
            self._draw_message()

    def _draw_message(self):
        # Black background with centered message
        self.canvas.create_rectangle(0, 0, sx(BASE_WIDTH), sy(BASE_HEIGHT), fill=COLOR_BLACK, outline="")
        self.canvas.create_text(sx(120), sy(150), text=self.message_text, fill=COLOR_WHITE,
                                font=("Arial", int(14 * SCALE), "bold"), justify=tk.CENTER)

    def _draw_waiting(self):
        self.canvas.create_rectangle(0, 0, sx(BASE_WIDTH), sy(BASE_HEIGHT), fill=COLOR_BLACK, outline="")
        self.canvas.create_text(sx(120), sy(135), text="Waiting for", fill=COLOR_WHITE,
                                font=("Arial", int(14 * SCALE), "bold"))
        tname = f"TEAM {self.waiting_team}" if self.waiting_team else "NEXT TURN"
        self.canvas.create_text(sx(120), sy(165), text=tname, fill=COLOR_CYAN,
                                font=("Arial", int(18 * SCALE), "bold"))

    def _draw_result_banner(self, target, x, y, result_str):
        # Color coding from uiDrawResultBanner()
        if result_str == "MISS":
            bg = COLOR_BLUE
        elif result_str == "HIT":
            bg = COLOR_ORANGE
        elif result_str == "ELIMINATED!":
            bg = COLOR_RED
        else:
            bg = COLOR_DARKGREY

        self.canvas.create_rectangle(0, 0, sx(BASE_WIDTH), sy(BASE_HEIGHT), fill=bg, outline="")
        self.canvas.create_text(sx(120), sy(120), text=result_str, fill=COLOR_WHITE,
                                font=("Arial", int(22 * SCALE), "bold"))
        self.canvas.create_text(sx(120), sy(165), text=f"TEAM {target} ({x},{y})", fill=COLOR_WHITE,
                                font=("Arial", int(13 * SCALE), "bold"))

    def _draw_your_turn(self):
        self.canvas.create_rectangle(0, 0, sx(BASE_WIDTH), sy(BASE_HEIGHT), fill=COLOR_BLACK, outline="")

        # Title: "YOUR TURN"
        self.canvas.create_text(sx(10), sy(12), text="YOUR TURN", fill=COLOR_GREEN,
                                font=("Arial", int(12 * SCALE), "bold"), anchor=tk.W)

        # "Select target:"
        self.canvas.create_text(sx(10), sy(28), text="Select target:", fill=COLOR_WHITE,
                                font=("Arial", int(8 * SCALE)), anchor=tk.W)

        # 3 Target Buttons
        for i in range(3):
            tid = self.opponent_ids[i]
            bx = 5 + i * (UI_TARGET_BTN_W + UI_TARGET_GAP)
            by = UI_TARGET_BTN_Y
            selected = (self.sel_target == tid)

            fill_c = COLOR_YELLOW if selected else COLOR_NAVY
            text_c = COLOR_BLACK if selected else COLOR_WHITE

            self.canvas.create_rectangle(sx(bx), sy(by), sx(bx + UI_TARGET_BTN_W), sy(by + UI_TARGET_BTN_H),
                                        fill=fill_c, outline=COLOR_WHITE, width=1)
            self.canvas.create_text(sx(bx + UI_TARGET_BTN_W // 2), sy(by + UI_TARGET_BTN_H // 2),
                                    text=f"T{tid}", fill=text_c, font=("Arial", int(12 * SCALE), "bold"))

        # 5x5 Grid
        for r in range(GRID_SIZE):
            for c in range(GRID_SIZE):
                gx = UI_GRID_X + c * UI_CELL_SIZE
                gy = UI_GRID_Y + r * UI_CELL_SIZE
                is_sel = (self.sel_x == c and self.sel_y == r)

                fill_c = COLOR_YELLOW if is_sel else COLOR_DARKGREY
                self.canvas.create_rectangle(sx(gx + 1), sy(gy + 1), sx(gx + UI_CELL_SIZE - 1), sy(gy + UI_CELL_SIZE - 1),
                                            fill=fill_c, outline="")
                self.canvas.create_rectangle(sx(gx), sy(gy), sx(gx + UI_CELL_SIZE), sy(gy + UI_CELL_SIZE),
                                            outline=COLOR_WHITE, width=1)

        # Coordinate helper labels (A-E, 1-5)
        for c, col_letter in enumerate(["A", "B", "C", "D", "E"]):
            gx = UI_GRID_X + c * UI_CELL_SIZE + (UI_CELL_SIZE // 2)
            self.canvas.create_text(sx(gx), sy(UI_GRID_Y - 8), text=col_letter, fill="#a0aec0", font=("Arial", int(7 * SCALE)))
        for r in range(GRID_SIZE):
            gy = UI_GRID_Y + r * UI_CELL_SIZE + (UI_CELL_SIZE // 2)
            self.canvas.create_text(sx(UI_GRID_X - 10), sy(gy), text=str(r + 1), fill="#a0aec0", font=("Arial", int(7 * SCALE)))

        # ATTACK button
        ready = (self.sel_target != 0 and self.sel_x >= 0 and self.sel_y >= 0 and not self.attack_in_flight)
        btn_color = COLOR_RED if ready else COLOR_DARKGREY

        self.canvas.create_rectangle(sx(UI_ATTACK_BTN_X), sy(UI_ATTACK_BTN_Y),
                                    sx(UI_ATTACK_BTN_X + UI_ATTACK_BTN_W), sy(UI_ATTACK_BTN_Y + UI_ATTACK_BTN_H),
                                    fill=btn_color, outline=COLOR_WHITE, width=1)
        self.canvas.create_text(sx(UI_ATTACK_BTN_X + UI_ATTACK_BTN_W // 2), sy(UI_ATTACK_BTN_Y + UI_ATTACK_BTN_H // 2),
                                text="ATTACK", fill=COLOR_WHITE, font=("Arial", int(11 * SCALE), "bold"))

        # In-flight message
        if self.attack_in_flight:
            self.canvas.create_text(sx(120), sy(UI_ATTACK_BTN_Y + UI_ATTACK_BTN_H + 12),
                                    text="Sent - waiting for result...", fill=COLOR_WHITE, font=("Arial", int(8 * SCALE)))

    # =====================================================================
    # TOUCH / CLICK HANDLING
    # =====================================================================
    def on_canvas_click(self, event):
        if self.current_screen != "YOUR_TURN":
            return

        # Convert back from scaled desktop coords to real 240x320 physical coords
        px = event.x / SCALE
        py = event.y / SCALE

        # 1. Target buttons
        for i in range(3):
            bx = 5 + i * (UI_TARGET_BTN_W + UI_TARGET_GAP)
            by = UI_TARGET_BTN_Y
            if bx <= px <= bx + UI_TARGET_BTN_W and by <= py <= by + UI_TARGET_BTN_H:
                self.sel_target = self.opponent_ids[i]
                self.redraw()
                return

        # 2. Grid cells
        if UI_GRID_X <= px < UI_GRID_X + GRID_SIZE * UI_CELL_SIZE and UI_GRID_Y <= py < UI_GRID_Y + GRID_SIZE * UI_CELL_SIZE:
            self.sel_x = int((px - UI_GRID_X) // UI_CELL_SIZE)
            self.sel_y = int((py - UI_GRID_Y) // UI_CELL_SIZE)
            self.redraw()
            return

        # 3. ATTACK button
        if UI_ATTACK_BTN_X <= px <= UI_ATTACK_BTN_X + UI_ATTACK_BTN_W and UI_ATTACK_BTN_Y <= py <= UI_ATTACK_BTN_Y + UI_ATTACK_BTN_H:
            if self.sel_target != 0 and self.sel_x >= 0 and self.sel_y >= 0 and not self.attack_in_flight:
                self.attack_in_flight = True
                self.redraw()
                # Transmit over Serial to participant ESP32
                cmd = f"{self.sel_target} {self.sel_x} {self.sel_y}"
                self.send_serial_line(cmd)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Meraz Battleship - Virtual TFT Display")
    parser.add_argument("--port", type=str, default=None, help="Serial port of participant ESP32 (e.g. COM4)")
    parser.add_argument("--team", type=int, default=1, help="Team ID (1-4)")
    args = parser.parse_args()

    app = VirtualTFTDisplay(default_port=args.port, default_team=args.team)
    try:
        app.mainloop()
    finally:
        app.running = False
        if app.serial_conn:
            try:
                app.serial_conn.close()
            except Exception:
                pass
