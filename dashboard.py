"""
MERAZ BATTLESHIP - ORGANIZER DASHBOARD
=======================================
Run this on the organizer's laptop:  python dashboard.py
Then open http://localhost:5000 in a browser and project that tab.

What this file does (and does NOT do):
  - Reads the Central ESP32 over USB Serial, one JSON line at a time.
  - Serves a single-page web dashboard (HTML/CSS/JS embedded below,
    so this one file is everything you need to run).
  - Pushes live updates to the browser as they arrive from Central.
  - Sends START / FORCE_START / RESET / SKIP_TURN back to Central when
    the organizer clicks the matching button. FORCE_START starts a
    round with fewer than all 4 teams registered (e.g. one team didn't
    show up) - the button confirms with the organizer first, since
    it's a less common, deliberate action.
  - Writes a plain-text match log to disk (match_logs/round_N_*.txt) -
    a new file per round, starting fresh every time the game is RESET.
    This is separate from the live in-memory log the browser shows,
    so a full match history survives a RESET, a dashboard restart, or
    closing the browser.
  - Contains ZERO game logic. It never decides a hit, a turn, or an
    elimination - it only displays whatever Central says and forwards
    organizer button clicks as plain Serial commands. If this script
    crashes or the laptop reboots, the match keeps running on Central;
    restarting this script and reloading the browser fetches a full
    state snapshot and picks the display back up.

Two things worth knowing about how it stays in sync with Central:
  1. Every action on Central (attack, registration, elimination, etc.)
     immediately streams a one-line JSON "event" message - that's what
     keeps the log panel live.
  2. The full grids only travel over Serial when Central answers a
     GET_STATE command, so this script polls GET_STATE every couple of
     seconds in the background (on top of requesting it once on
     startup/reconnect). This keeps the 5x5 grids in sync without
     needing any changes to central_node.ino, at the cost of the grid
     view lagging an event by at most ~2 seconds - fine for a
     turn-based, human-paced game.
"""

import json
import os
import threading
import queue
import time
from datetime import datetime

try:
    import serial  # pyserial
except ImportError:
    print("Missing dependency 'pyserial'. Install it with: pip install pyserial")
    raise SystemExit(1)

try:
    from flask import Flask, Response, request, jsonify
except ImportError:
    print("Missing dependency 'flask'. Install it with: pip install flask")
    raise SystemExit(1)

# =====================================================================
# CONFIGURATION - CHANGE THESE FOR YOUR LAPTOP
# =====================================================================

# TODO: change this to the Central ESP32's port.
# Windows looks like "COM5". Mac/Linux looks like "/dev/ttyUSB0" or
# "/dev/cu.usbserial-XXXX". See the setup instructions for how to find it.
SERIAL_PORT = "COM5"
BAUD_RATE = 115200

WEB_PORT = 5000  # dashboard will be at http://localhost:5000

STATE_POLL_INTERVAL_SECONDS = 2.0  # how often to re-request the full grid snapshot

# =====================================================================
# SHARED STATE (read/written from both the Flask thread and the
# Serial-reading thread, so everything here is guarded by locks)
# =====================================================================

state_lock = threading.Lock()
latest_state = {
    "type": "state",
    "round_state": 0,
    "current_turn": 0,
    "teams": [],
    "log": [],
    "central_connected": False,
}

central_connected = False

subscribers = []  # one queue.Queue() per open browser tab (Server-Sent Events)
subscribers_lock = threading.Lock()

serial_conn = None
serial_lock = threading.Lock()

# --- persistent match log (a plain file per round, not a database) ---
LOG_DIR = "match_logs"
round_counter = 0
current_log_file = None
log_file_lock = threading.Lock()

# =====================================================================
# SERIAL <-> DASHBOARD PLUMBING
# =====================================================================


def write_to_log(line):
    """Append one timestamped line to the current round's log file."""
    with log_file_lock:
        if current_log_file is None:
            return
        stamp = datetime.now().strftime("%H:%M:%S")
        try:
            with open(current_log_file, "a", encoding="utf-8") as f:
                f.write(f"[{stamp}] {line}\n")
        except OSError as e:
            print(f"Could not write to match log: {e}")


def start_new_round_log():
    """
    Starts a fresh log file for a new round. Called once at dashboard
    startup (covers whatever round is already in progress on Central)
    and again every time Central reports a RESET, so each round gets
    its own file instead of one growing file for the whole event.
    """
    global round_counter, current_log_file
    round_counter += 1
    os.makedirs(LOG_DIR, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    filename = os.path.join(LOG_DIR, f"round_{round_counter}_{stamp}.txt")
    with log_file_lock:
        current_log_file = filename
    print(f"Match log for this round: {filename}")
    write_to_log(f"=== Round {round_counter} log started ===")


def broadcast(message):
    """Push a message to every currently-open browser tab."""
    with subscribers_lock:
        for q in subscribers:
            q.put(message)


def set_central_connected(connected):
    global central_connected
    if central_connected != connected:
        central_connected = connected
        with state_lock:
            latest_state["central_connected"] = connected
        broadcast({"type": "conn", "connected": connected})
        write_to_log("Central connected" if connected else "Central disconnected")


def apply_state_message(msg):
    """Called when a full {"type":"state",...} line arrives from Central."""
    global latest_state
    msg = dict(msg)
    msg["central_connected"] = central_connected
    with state_lock:
        latest_state = msg
    broadcast(msg)


def apply_event_message(msg):
    """Called when a {"type":"event","text":...} line arrives from Central."""
    text = msg.get("text", "")

    # Central's resetGame() always logs exactly this line - treat it as
    # the boundary between one round's log file and the next.
    if text == "Game reset - waiting for teams to register":
        start_new_round_log()

    write_to_log(text)

    with state_lock:
        latest_state.setdefault("log", [])
        latest_state["log"].append(text)
        latest_state["log"] = latest_state["log"][-20:]  # keep it bounded
    broadcast(msg)


def send_command(cmd):
    """Send a plain-text command line to Central (STATUS/START/RESET/etc.)."""
    with serial_lock:
        if serial_conn is None:
            print(f"Could not send '{cmd}' - Central is not connected.")
            return
        try:
            serial_conn.write((cmd + "\n").encode("utf-8"))
            if cmd in ("START", "FORCE_START", "RESET", "SKIP_TURN"):
                write_to_log(f"Organizer sent {cmd} from the dashboard")
        except serial.SerialException:
            print(f"Could not send '{cmd}' - Serial write failed.")


def request_state():
    send_command("GET_STATE")


def serial_reader_thread():
    """
    Runs forever in the background. Opens (and re-opens, if it drops)
    the Serial connection to Central, reads one JSON line at a time,
    and periodically asks for a full state snapshot to keep the grids
    in sync. Kept deliberately simple - a dropped connection just
    means we retry every few seconds until it comes back.
    """
    global serial_conn
    last_poll = 0.0

    while True:
        try:
            if serial_conn is None:
                serial_conn = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
                print(f"Connected to Central on {SERIAL_PORT}")
                set_central_connected(True)
                time.sleep(2)  # let the ESP32 finish rebooting-on-connect
                request_state()
                last_poll = time.time()

            line = serial_conn.readline().decode("utf-8", errors="ignore").strip()

            if line:
                # Lines that aren't valid JSON are meant for a human reading
                # the Arduino Serial Monitor (STATUS/GRID/HELP output) -
                # just ignore them here.
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    msg = None

                if msg is not None:
                    msg_type = msg.get("type")
                    if msg_type == "state":
                        apply_state_message(msg)
                    elif msg_type == "event":
                        apply_event_message(msg)

            if time.time() - last_poll > STATE_POLL_INTERVAL_SECONDS:
                request_state()
                last_poll = time.time()

        except serial.SerialException as e:
            print(f"Serial error: {e}. Retrying in 3s...")
            set_central_connected(False)
            try:
                if serial_conn is not None:
                    serial_conn.close()
            except Exception:
                pass
            serial_conn = None
            time.sleep(3)
        except Exception as e:
            print(f"Unexpected error in serial thread: {e}")
            time.sleep(1)


# =====================================================================
# FLASK APP
# =====================================================================

app = Flask(__name__)


@app.route("/")
def index():
    return DASHBOARD_HTML


@app.route("/api/state")
def api_state():
    with state_lock:
        return jsonify(latest_state)


@app.route("/api/command", methods=["POST"])
def api_command():
    data = request.get_json(silent=True) or {}
    cmd = str(data.get("cmd", "")).upper()
    if cmd not in ("START", "FORCE_START", "RESET", "SKIP_TURN", "GET_STATE"):
        return jsonify({"ok": False, "error": "unknown command"}), 400
    send_command(cmd)
    return jsonify({"ok": True})


@app.route("/events")
def events():
    """Server-Sent Events stream - keeps the browser updated live."""

    def stream():
        q = queue.Queue()
        with subscribers_lock:
            subscribers.append(q)
        try:
            # Send what we already know immediately, so the tab isn't
            # blank while waiting for the next Central update.
            with state_lock:
                yield f"data: {json.dumps(latest_state)}\n\n"
            while True:
                msg = q.get()
                yield f"data: {json.dumps(msg)}\n\n"
        finally:
            with subscribers_lock:
                if q in subscribers:
                    subscribers.remove(q)

    return Response(stream(), mimetype="text/event-stream")


# =====================================================================
# DASHBOARD PAGE (HTML/CSS/JS embedded so this is the only file to run)
# =====================================================================

DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Meraz Battleship</title>
<style>
  :root {
    --bg-deep: #060B12;
    --panel: #0E1B2B;
    --grid-line: #16283D;
    --accent: #2DE8A0;
    --hit: #FF4B3E;
    --miss: #4A6484;
    --text-primary: #E8F1F7;
    --text-secondary: #7C93AC;
    --font-sans: -apple-system, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    --font-mono: "Cascadia Code", Consolas, "SF Mono", "Courier New", monospace;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: var(--bg-deep);
    color: var(--text-primary);
    font-family: var(--font-sans);
    min-height: 100vh;
    padding: 24px;
  }
  header {
    display: flex;
    flex-wrap: wrap;
    justify-content: space-between;
    align-items: center;
    gap: 12px;
    margin-bottom: 8px;
  }
  h1 { font-size: 20px; letter-spacing: 1px; margin: 0; font-weight: 600; }
  .status-row { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
  .status-pill {
    font-family: var(--font-mono);
    font-size: 13px;
    padding: 5px 12px;
    border-radius: 4px;
    border: 1px solid var(--grid-line);
    color: var(--text-secondary);
    display: flex;
    align-items: center;
  }
  .conn-dot { display:inline-block; width:8px; height:8px; border-radius:50%; margin-right:8px; background: var(--miss); }
  .conn-dot.on { background: var(--accent); }
  .conn-dot.off { background: var(--hit); }

  .controls { display: flex; gap: 8px; }
  button {
    font-family: var(--font-sans);
    background: transparent;
    color: var(--text-primary);
    border: 1px solid var(--grid-line);
    padding: 6px 14px;
    border-radius: 4px;
    cursor: pointer;
    font-size: 13px;
  }
  button:hover { border-color: var(--accent); color: var(--accent); }

  .turn-banner {
    text-align: center;
    font-size: 26px;
    font-weight: 700;
    letter-spacing: 2px;
    color: var(--accent);
    margin: 20px 0 28px;
    font-family: var(--font-mono);
  }
  .turn-banner.gameover { color: var(--hit); }
  .turn-banner.waiting { color: var(--text-secondary); font-size: 17px; letter-spacing: 1px; }

  .grid-of-teams {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 16px;
    max-width: 900px;
    margin: 0 auto;
  }
  @media (max-width: 640px) {
    .grid-of-teams { grid-template-columns: 1fr; }
  }
  .team-panel {
    background: var(--panel);
    border: 1px solid var(--grid-line);
    border-radius: 6px;
    padding: 16px;
    transition: border-color 0.2s, opacity 0.2s;
  }
  .team-panel.active-turn { border-color: var(--accent); }
  .team-panel.eliminated { opacity: 0.4; }
  .team-header {
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    margin-bottom: 10px;
    gap: 8px;
  }
  .team-name { font-weight: 600; font-size: 15px; }
  .team-badge { font-family: var(--font-mono); font-size: 12px; color: var(--text-secondary); white-space: nowrap; }
  .team-badge.alive { color: var(--accent); }
  .team-badge.eliminated { color: var(--hit); }

  .cell-grid {
    display: grid;
    grid-template-columns: repeat(5, 1fr);
    gap: 4px;
    max-width: 220px;
    margin: 0 auto;
  }
  .cell {
    aspect-ratio: 1;
    background: var(--grid-line);
    border-radius: 3px;
    display: flex;
    align-items: center;
    justify-content: center;
  }
  .cell.ship::after { content: ""; width: 7px; height: 7px; border-radius: 50%; background: var(--text-secondary); opacity: 0.5; }
  .cell.miss::after { content: ""; width: 5px; height: 5px; border-radius: 50%; background: var(--miss); }
  .cell.hit { background: rgba(255,75,62,0.18); }
  .cell.hit::after { content: "\\2715"; color: var(--hit); font-weight: 700; font-size: 14px; }

  .log-panel {
    max-width: 900px;
    margin: 24px auto 0;
    background: var(--panel);
    border: 1px solid var(--grid-line);
    border-radius: 6px;
    padding: 12px 16px;
    font-family: var(--font-mono);
    font-size: 12px;
    color: var(--text-secondary);
    max-height: 200px;
    overflow-y: auto;
  }
  .log-panel div { padding: 2px 0; }
  .log-panel .empty { color: var(--text-secondary); opacity: 0.6; }
</style>
</head>
<body>
  <header>
    <h1>MERAZ BATTLESHIP</h1>
    <div class="status-row">
      <span class="status-pill"><span id="conn-dot" class="conn-dot off"></span><span id="conn-text">connecting...</span></span>
      <span class="status-pill" id="round-pill">SETUP</span>
      <div class="controls">
        <button onclick="sendCommand('START')">START</button>
        <button onclick="forceStart()">FORCE START</button>
        <button onclick="sendCommand('SKIP_TURN')">SKIP TURN</button>
        <button onclick="sendCommand('RESET')">RESET</button>
      </div>
    </div>
  </header>

  <div id="turn-banner" class="turn-banner waiting">WAITING FOR TEAMS TO REGISTER</div>

  <div class="grid-of-teams" id="team-grid"></div>

  <div class="log-panel" id="log-panel"><div class="empty">no events yet</div></div>

<script>
  const ROUND_STATE_NAMES = {0: "SETUP", 1: "READY", 2: "RUNNING", 3: "GAME OVER"};
  let state = {round_state: 0, current_turn: 0, teams: [], log: []};

  function cellClass(v) {
    if (v === 1) return "cell ship";
    if (v === 2) return "cell miss";
    if (v === 3) return "cell hit";
    return "cell";
  }

  function emptyGrid() {
    return Array.from({length: 5}, () => Array(5).fill(0));
  }

  function render() {
    document.getElementById("round-pill").textContent = ROUND_STATE_NAMES[state.round_state] ?? "?";

    const banner = document.getElementById("turn-banner");
    if (state.round_state === 2) {
      banner.className = "turn-banner";
      banner.textContent = "TEAM " + state.current_turn + "'S TURN";
    } else if (state.round_state === 3) {
      banner.className = "turn-banner gameover";
      banner.textContent = "GAME OVER";
    } else if (state.round_state === 1) {
      banner.className = "turn-banner waiting";
      banner.textContent = "READY - WAITING FOR ORGANIZER TO START";
    } else {
      banner.className = "turn-banner waiting";
      banner.textContent = "WAITING FOR TEAMS TO REGISTER";
    }

    const gridEl = document.getElementById("team-grid");
    gridEl.innerHTML = "";
    const teams = (state.teams && state.teams.length)
      ? state.teams
      : [1, 2, 3, 4].map(id => ({id, name: "", registered: false, eliminated: false, remaining: 0, grid: null}));

    teams.forEach(team => {
      const panel = document.createElement("div");
      panel.className = "team-panel";
      if (state.round_state === 2 && team.id === state.current_turn && !team.eliminated) {
        panel.classList.add("active-turn");
      }
      if (team.eliminated) panel.classList.add("eliminated");

      const header = document.createElement("div");
      header.className = "team-header";

      const name = document.createElement("span");
      name.className = "team-name";
      name.textContent = team.registered ? ("TEAM " + team.id + " - " + team.name) : ("TEAM " + team.id);

      const badge = document.createElement("span");
      badge.className = "team-badge " + (team.eliminated ? "eliminated" : (team.registered ? "alive" : ""));
      badge.textContent = !team.registered
        ? (state.round_state >= 2 ? "not in this round" : "waiting to register...")
        : (team.eliminated ? "eliminated" : team.remaining + " ships left");

      header.appendChild(name);
      header.appendChild(badge);
      panel.appendChild(header);

      const cg = document.createElement("div");
      cg.className = "cell-grid";
      const grid = team.grid || emptyGrid();
      for (let r = 0; r < 5; r++) {
        for (let c = 0; c < 5; c++) {
          const cell = document.createElement("div");
          cell.className = cellClass(grid[r][c]);
          cg.appendChild(cell);
        }
      }
      panel.appendChild(cg);

      gridEl.appendChild(panel);
    });

    const logEl = document.getElementById("log-panel");
    const log = state.log || [];
    if (log.length === 0) {
      logEl.innerHTML = '<div class="empty">no events yet</div>';
    } else {
      logEl.innerHTML = "";
      for (let i = log.length - 1; i >= 0; i--) {
        const line = document.createElement("div");
        line.textContent = "> " + log[i];
        logEl.appendChild(line);
      }
    }
  }

  function setConnection(connected) {
    const dot = document.getElementById("conn-dot");
    const text = document.getElementById("conn-text");
    dot.className = "conn-dot " + (connected ? "on" : "off");
    text.textContent = connected ? "central connected" : "central disconnected";
  }

  function sendCommand(cmd) {
    fetch("/api/command", {
      method: "POST",
      headers: {"Content-Type": "application/json"},
      body: JSON.stringify({cmd: cmd})
    });
  }

  function forceStart() {
    const unregistered = (state.teams || []).filter(t => !t.registered).length;
    const msg = unregistered > 0
      ? `Start now with ${4 - unregistered} of 4 teams registered? The other ${unregistered} won't be in this round.`
      : "Start now?";
    if (confirm(msg)) sendCommand("FORCE_START");
  }

  // Initial load - fills the page in immediately, before the live stream
  // has sent anything of its own.
  fetch("/api/state")
    .then(r => r.json())
    .then(data => {
      state = data;
      setConnection(!!data.central_connected);
      render();
    })
    .catch(() => { /* dashboard server itself isn't reachable yet - ignore */ });

  // Live updates via Server-Sent Events. The browser reconnects this
  // automatically if it drops.
  const source = new EventSource("/events");
  source.onmessage = function (e) {
    const msg = JSON.parse(e.data);
    if (msg.type === "state") {
      state = msg;
      if (typeof msg.central_connected !== "undefined") setConnection(msg.central_connected);
      render();
    } else if (msg.type === "event") {
      state.log = state.log || [];
      state.log.push(msg.text);
      state.log = state.log.slice(-20);
      render();
    } else if (msg.type === "conn") {
      setConnection(msg.connected);
    }
  };
</script>
</body>
</html>
"""

# =====================================================================
# ENTRY POINT
# =====================================================================

if __name__ == "__main__":
    start_new_round_log()  # covers whatever round is already in progress on Central

    reader = threading.Thread(target=serial_reader_thread, daemon=True)
    reader.start()

    print("=== MERAZ BATTLESHIP DASHBOARD ===")
    print(f"Serial port: {SERIAL_PORT} @ {BAUD_RATE} baud (change SERIAL_PORT above if wrong)")
    print(f"Open http://localhost:{WEB_PORT} in a browser and project that tab.")
    print(f"Match logs are saved under ./{LOG_DIR}/")

    app.run(host="0.0.0.0", port=WEB_PORT, threaded=True)
