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
     startup/reconnect). This keeps the 7x7 grids in sync without
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
SERIAL_PORT = "COM3"
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
#
# FRONTEND v2 - "Command Deck". Only this string changed; every route,
# thread, lock and Serial behaviour above is exactly as before. It talks
# to the same three endpoints (/api/state, /events, /api/command) and
# understands the same message shapes, so central_node.ino needs no
# changes either. Nothing here is loaded from the internet - no Google
# Fonts, no CDN - so it renders identically on an offline venue laptop.
# =====================================================================

DASHBOARD_HTML = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>BATTLESHIP - COMMAND DECK</title>
<style>
/* =====================================================================
   MERAZ BATTLESHIP - COMMAND DECK  (frontend v2)
   Projector-first redesign. No external fonts / CDNs / images:
   everything is inline so it renders identically on an offline laptop.
   ===================================================================== */

:root{
  --bg:#04050c;
  --panel:rgba(8,11,26,.80);
  --panel-2:rgba(9,12,26,.88);
  --line:rgba(255,255,255,.13);
  --line-strong:rgba(255,255,255,.16);

  --t1:#27e6ff;
  --t2:#ff3d84;
  --t3:#b06bff;
  --t4:#3dff88;

  --hit:#ff4747;
  --hit-soft:rgba(255,71,71,.30);
  --miss:#8aa2cc;
  --gold:#ffd24a;
  --warn:#ffa72b;
  --ok:#3dff88;

  --txt:#f0f5ff;
  --dim:#93a9cf;      /* brighter than v1: readable from the back row */
  --dimmer:#61759b;

  /* offline-safe type stacks (Bahnschrift/DIN on Win, Avenir/HelveticaNeue on Mac) */
  --f-display:"Bahnschrift","DIN Alternate","Avenir Next Condensed","Segoe UI Semibold",system-ui,sans-serif;
  --f-body:"Inter","Segoe UI",system-ui,-apple-system,Roboto,sans-serif;
  --f-mono:ui-monospace,"Cascadia Mono","JetBrains Mono",Consolas,"DejaVu Sans Mono",monospace;

  --r:16px;
  --gap:14px;
}

*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%}
body{
  background:var(--bg);
  color:var(--txt);
  font-family:var(--f-body);
  overflow:hidden;
  -webkit-font-smoothing:antialiased;
  text-rendering:optimizeLegibility;
}

/* ---------- ambient background ----------
   Four cheap layers, all transform-animated so the GPU does the work:
   drifting aurora -> panning tactical grid -> sonar range rings -> radar sweep.
   Everything is switched off by performance mode.                         */
body::before{                       /* drifting aurora (colour, not flat black) */
  content:'';position:fixed;inset:-25%;z-index:0;pointer-events:none;
  background:
    radial-gradient(ellipse 42% 38% at 16% 14%,rgba(39,230,255,.42),transparent 62%),
    radial-gradient(ellipse 38% 34% at 86% 84%,rgba(255,61,132,.38),transparent 62%),
    radial-gradient(ellipse 46% 40% at 76% 10%,rgba(176,107,255,.32),transparent 64%),
    radial-gradient(ellipse 40% 36% at 22% 92%,rgba(61,255,136,.20),transparent 62%),
    radial-gradient(ellipse 62% 52% at 50% 52%,rgba(22,44,120,.45),transparent 72%);
  animation:aurora 34s ease-in-out infinite alternate;
  will-change:transform;
}
@keyframes aurora{
  0%  {transform:translate3d(-1.5%,-1%,0) scale(1.00) rotate(0deg)}
  50% {transform:translate3d(2%,1.5%,0)   scale(1.08) rotate(1.5deg)}
  100%{transform:translate3d(-1%,2%,0)    scale(1.03) rotate(-1deg)}
}

.bg-grid{                           /* tactical grid, slowly panning one tile */
  position:fixed;inset:-90px;z-index:0;pointer-events:none;
  background:
    repeating-linear-gradient(0deg,transparent 0 63px,rgba(39,230,255,.13) 63px 64px),
    repeating-linear-gradient(90deg,transparent 0 63px,rgba(39,230,255,.13) 63px 64px),
    repeating-linear-gradient(0deg,transparent 0 319px,rgba(39,230,255,.20) 319px 320px),
    repeating-linear-gradient(90deg,transparent 0 319px,rgba(39,230,255,.20) 319px 320px);
  animation:gridpan 26s linear infinite;
  will-change:transform;
}
@keyframes gridpan{to{transform:translate3d(64px,64px,0)}}

.bg-beams{                          /* slow shafts of light through deep water */
  position:fixed;inset:-30%;z-index:0;pointer-events:none;opacity:.85;
  background:
    linear-gradient(104deg,transparent 14%,rgba(39,230,255,.16) 24%,transparent 33%),
    linear-gradient(104deg,transparent 42%,rgba(176,107,255,.14) 52%,transparent 61%),
    linear-gradient(104deg,transparent 68%,rgba(255,61,132,.13) 77%,transparent 86%);
  filter:blur(22px);
  animation:beams 26s ease-in-out infinite alternate;
  will-change:transform;
}
@keyframes beams{
  from{transform:translate3d(-6%,0,0) scaleY(1)}
  to  {transform:translate3d(6%,0,0)  scaleY(1.08)}
}

.bg-noise{                          /* faint film grain so nothing looks like flat fill */
  position:fixed;inset:0;z-index:0;pointer-events:none;opacity:.05;mix-blend-mode:overlay;
  background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='180' height='180'%3E%3Cfilter id='n'%3E%3CfeTurbulence type='fractalNoise' baseFrequency='0.9' numOctaves='3' stitchTiles='stitch'/%3E%3C/filter%3E%3Crect width='180' height='180' filter='url(%23n)'/%3E%3C/svg%3E");
}


body::after{ /* vignette keeps the projector edges from washing out */
  content:'';position:fixed;inset:0;z-index:0;pointer-events:none;
  background:radial-gradient(ellipse 92% 82% at 50% 45%,transparent 62%,rgba(0,0,0,.42) 100%);
}
#fx{position:fixed;inset:0;z-index:1;pointer-events:none}
body.perf #fx,body.perf .bg-beams{display:none}
body.perf::before,body.perf .bg-grid{animation:none}
body.perf .blur{backdrop-filter:none!important}

/* ---------- shell ---------- */
.deck{
  position:relative;z-index:2;height:100vh;
  display:grid;grid-template-rows:auto auto 1fr;
  gap:10px;padding:14px 18px 10px;
}

/* ---------- header ---------- */
.top{display:flex;align-items:center;gap:16px;flex-wrap:wrap}
.brand{display:flex;align-items:baseline;gap:10px;flex-shrink:0}
.brand b{
  font-family:var(--f-display);font-weight:700;letter-spacing:.15em;
  font-size:clamp(26px,3.7vmin,46px);white-space:nowrap;
  background:linear-gradient(100deg,#6ff2ff,#ff5c9c 42%,#cb92ff 72%,#6ff2ff);
  background-size:250% 100%;-webkit-background-clip:text;background-clip:text;
  -webkit-text-fill-color:transparent;color:transparent;
  animation:shift 9s linear infinite;
  filter:drop-shadow(0 0 26px rgba(39,230,255,.32));
}
.brand i{
  font-style:normal;font-family:var(--f-mono);font-size:clamp(8px,1vmin,11px);
  letter-spacing:.3em;color:var(--dimmer);
}
@keyframes shift{to{background-position:250% 0}}

.chips{display:flex;align-items:center;gap:7px;flex-wrap:wrap;margin-left:auto}
.chip{
  display:inline-flex;align-items:center;gap:7px;
  font-family:var(--f-mono);font-size:clamp(10px,1.35vmin,14px);letter-spacing:.1em;
  padding:7px 11px;border-radius:999px;border:1px solid var(--line);
  background:rgba(255,255,255,.035);color:var(--dim);white-space:nowrap;
}
.chip.live{border-color:rgba(61,255,136,.35);color:var(--ok);background:rgba(61,255,136,.07)}
.chip.dead{border-color:rgba(255,71,71,.40);color:var(--hit);background:rgba(255,71,71,.07)}
.chip.reveal{border-color:rgba(255,167,43,.5);color:var(--warn);background:rgba(255,167,43,.1)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--dimmer);flex-shrink:0}
.dot.on{background:var(--ok);box-shadow:0 0 10px var(--ok)}
.dot.off{background:var(--hit);box-shadow:0 0 10px var(--hit);animation:blink 1.4s ease-in-out infinite}
@keyframes blink{50%{opacity:.25}}

.phase{font-family:var(--f-display);letter-spacing:.2em;font-weight:700}
.phase[data-p="0"]{color:var(--dim)}
.phase[data-p="1"]{color:var(--warn);border-color:rgba(255,167,43,.4);background:rgba(255,167,43,.08)}
.phase[data-p="2"]{color:var(--ok);border-color:rgba(61,255,136,.4);background:rgba(61,255,136,.08)}
.phase[data-p="3"]{color:var(--gold);border-color:rgba(255,210,74,.45);background:rgba(255,210,74,.1)}

.ctl{display:flex;gap:7px;align-items:center}
.btn{
  font-family:var(--f-mono);font-size:clamp(12px,1.75vmin,18px);letter-spacing:.11em;font-weight:700;
  text-transform:uppercase;padding:10px 20px;border-radius:10px;cursor:pointer;
  border:none;color:#05080f;background:var(--btn,#7c8aa5);
  box-shadow:inset 0 1px 0 rgba(255,255,255,.28),inset 0 0 0 1px rgba(0,0,0,.18),
             0 4px 14px -8px var(--btn),0 2px 8px -4px rgba(0,0,0,.7);
  transition:transform .15s,box-shadow .22s,filter .2s;
  display:inline-flex;align-items:center;gap:8px;
}
.btn kbd{
  font-family:var(--f-mono);font-size:.74em;font-weight:700;color:inherit;opacity:.7;
  border:1px solid rgba(0,0,0,.32);background:rgba(0,0,0,.17);
  border-radius:5px;padding:1px 6px;line-height:1.3;
}
.btn:hover{
  transform:translateY(-2px);filter:brightness(1.1) saturate(1.08);
  box-shadow:inset 0 1px 0 rgba(255,255,255,.45),0 0 0 3px color-mix(in srgb,var(--btn) 28%,transparent),
             0 0 30px 2px var(--btn),0 0 60px -6px var(--btn),0 12px 22px -12px #000;
}
.btn:active{transform:translateY(0) scale(.97)}
.b-start{--btn:#3dff88;background:linear-gradient(168deg,#78ffb2,#11cf67)}
.b-force{--btn:#ffa72b;background:linear-gradient(168deg,#ffcb72,#f0850a)}
.b-skip {--btn:#27e6ff;background:linear-gradient(168deg,#86f3ff,#02bada)}
.b-reset{--btn:#ff3d84;background:linear-gradient(168deg,#ff88b6,#e5135e)}
.b-ico{
  --btn:#8ea8d6;color:var(--txt);background:rgba(255,255,255,.07);
  border:1px solid var(--line);padding:9px 13px;font-size:clamp(14px,1.9vmin,19px);letter-spacing:0;font-weight:400;
  box-shadow:none;
}
.b-ico:hover{background:rgba(255,255,255,.14);box-shadow:0 0 18px -6px #cfe3ff}
.b-ico.active{color:#05080f;background:linear-gradient(168deg,#86f3ff,#02bada);border-color:transparent}

/* ---------- turn bar ---------- */
.turnbar{
  display:grid;grid-template-columns:auto 1fr auto;align-items:center;gap:16px;
  border:1px solid var(--line);border-radius:var(--r);
  background:var(--panel);backdrop-filter:blur(14px) saturate(1.2);
  box-shadow:0 18px 40px -28px #000,inset 0 1px 0 rgba(255,255,255,.05);
  padding:10px 18px;position:relative;overflow:hidden;min-height:64px;
}
.turnbar::before{
  content:'';position:absolute;inset:0;opacity:.18;
  background:linear-gradient(100deg,transparent 30%,var(--accent,transparent) 50%,transparent 70%);
  background-size:220% 100%;animation:sweep 3.6s linear infinite;
}
@keyframes sweep{to{background-position:-220% 0}}
.turnbar .side{
  font-family:var(--f-mono);font-size:clamp(9px,1.15vmin,12px);letter-spacing:.18em;color:var(--dim);
  display:flex;flex-direction:column;gap:3px;z-index:1;
}
.turnbar .side b{font-size:clamp(13px,2vmin,20px);color:var(--txt);letter-spacing:.06em}
.turntext{
  text-align:center;font-family:var(--f-display);font-weight:700;
  font-size:clamp(20px,3.6vmin,44px);letter-spacing:.14em;z-index:1;
  color:var(--accent,var(--txt));text-shadow:0 0 30px var(--accent,transparent);
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;
}
.turntext small{display:block;font-size:.4em;letter-spacing:.3em;color:var(--dim);text-shadow:none;margin-top:2px}
.clock{
  font-family:var(--f-mono);font-variant-numeric:tabular-nums;
  font-size:clamp(18px,3vmin,34px);color:var(--txt);letter-spacing:.04em;
}
.clock.warn{color:var(--warn)}
.clock.over{color:var(--hit);animation:blink 1s ease-in-out infinite}

/* ---------- stage ---------- */
.stage{display:grid;grid-template-columns:1fr clamp(240px,20vw,330px);gap:var(--gap);min-height:0}
.teams{display:grid;gap:var(--gap);min-height:0;grid-template-columns:1fr 1fr;grid-template-rows:1fr 1fr}
.teams.row{grid-template-columns:repeat(4,1fr);grid-template-rows:1fr}
.teams.row .tname{font-size:clamp(11px,1.45vmin,19px)}
.teams.row .tp{padding:10px 11px 9px}
/* auto-fit when force-started with fewer than 4 teams: reuses the 1xN row
   layout, just with bigger text since each panel gets more room */
.teams.fit .tname{font-size:clamp(15px,2.6vmin,32px)}
.teams.fit .tp{padding:14px 16px 12px}
.teams.fit .pip{height:10px}

/* ---------- team panel ---------- */
.tp{
  position:relative;display:flex;flex-direction:column;gap:8px;min-height:0;min-width:0;
  padding:12px 14px 10px;border-radius:var(--r);
  border:1px solid var(--line);background:var(--panel);backdrop-filter:blur(14px) saturate(1.2);
  box-shadow:0 22px 48px -26px #000,inset 0 1px 0 rgba(255,255,255,.05);
  transition:border-color .3s,box-shadow .35s,transform .35s,opacity .35s,filter .35s;
  overflow:hidden;
}
.tp::before{
  content:'';position:absolute;left:0;right:0;top:0;height:3px;background:var(--c);
  opacity:.45;transition:opacity .3s,height .3s;
}
.tp.turn{
  border-color:var(--c);transform:scale(1.012);
  box-shadow:0 0 0 1px var(--c),0 18px 60px -22px var(--c),inset 0 0 70px -30px var(--c);
}
.tp.turn::before{opacity:1;height:5px;box-shadow:0 0 18px var(--c)}
.tp.turn::after{ /* scanning line so the active board reads from 20m away */
  content:'';position:absolute;inset:0;pointer-events:none;
  background:linear-gradient(180deg,transparent 45%,color-mix(in srgb,var(--c) 14%,transparent) 50%,transparent 55%);
  background-size:100% 220%;animation:scan 3.2s linear infinite;
}
@keyframes scan{from{background-position:0 -110%}to{background-position:0 110%}}
.tp.out{opacity:.5;filter:grayscale(.55) brightness(.75)}   /* v1 used .25 - invisible on a projector */
.tp.idle{opacity:.82}
.tp.win{border-color:var(--gold);box-shadow:0 0 0 1px var(--gold),0 0 70px -18px var(--gold)}
.tp.unreg .boardwrap{opacity:.35}
.tp.unreg::before{opacity:.15}

.stamp{
  position:absolute;inset:0;display:none;align-items:center;justify-content:center;
  font-family:var(--f-display);font-size:clamp(18px,3.4vmin,40px);letter-spacing:.3em;
  font-weight:700;transform:rotate(-14deg);pointer-events:none;z-index:3;
}
.tp.out .stamp.elim{display:flex;color:rgba(255,71,71,.85);text-shadow:0 0 28px rgba(255,71,71,.5)}
.tp.win .stamp.wins{display:flex;color:var(--gold);text-shadow:0 0 34px rgba(255,210,74,.6);animation:pop .8s ease}
@keyframes pop{0%{transform:rotate(-14deg) scale(.4);opacity:0}60%{transform:rotate(-14deg) scale(1.12)}100%{transform:rotate(-14deg) scale(1);opacity:1}}

.tp-head{display:flex;align-items:center;gap:8px;min-width:0}
.tag{
  font-family:var(--f-mono);font-size:clamp(9px,1.2vmin,13px);font-weight:700;
  background:var(--c);color:#04060f;border-radius:6px;padding:2px 7px;flex-shrink:0;
  box-shadow:0 0 16px -4px var(--c);
}
.tname{
  font-family:var(--f-display);font-weight:700;letter-spacing:.09em;
  font-size:clamp(12px,1.85vmin,22px);color:var(--txt);
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0;flex:1;
}
.badge{
  font-family:var(--f-mono);font-size:clamp(9px,1.2vmin,13px);letter-spacing:.1em;
  padding:3px 8px;border-radius:7px;border:1px solid var(--line);color:var(--dim);flex-shrink:0;
}
.badge.alive{color:var(--ok);border-color:rgba(61,255,136,.3);background:rgba(61,255,136,.08)}
.badge.dead{color:var(--hit);border-color:rgba(255,71,71,.3);background:rgba(255,71,71,.08)}
.badge.wait{color:var(--dimmer)}

/* HP as 9 discrete pips = instantly countable, unlike a % bar */
.hp{display:flex;align-items:center;gap:7px}
.pips{display:flex;gap:3px;flex:1;min-width:0}
.pip{
  flex:1;height:7px;border-radius:3px;background:rgba(255,255,255,.07);
  transition:background .35s,box-shadow .35s;
}
.pip.on{background:var(--c);box-shadow:0 0 10px -2px var(--c)}
.pip.low.on{background:var(--warn);box-shadow:0 0 10px -2px var(--warn)}
.pip.crit.on{background:var(--hit);box-shadow:0 0 10px -2px var(--hit)}
.pip.just{animation:pipOut .7s ease}
@keyframes pipOut{0%{background:#fff;box-shadow:0 0 16px #fff}100%{}}
.hpnum{
  font-family:var(--f-mono);font-variant-numeric:tabular-nums;font-weight:700;
  font-size:clamp(11px,1.6vmin,18px);color:var(--dim);flex-shrink:0;min-width:3.2em;text-align:right;
}

/* ---------- board ---------- */
/* stacked layout = default; .meta is display:contents so the children
   flow straight into the panel column (order fixes the board/foot order) */
.meta{display:contents}
.tp-head{order:0}.hp{order:1}.boardwrap{order:2}.tp-foot{order:4}
.boardwrap{flex:1;display:flex;align-items:center;justify-content:center;min-height:0}

/* On a projector the 2x2 panels are wide and short, so the square board is
   height-capped and leaves dead space. Move the text into a side column:
   same information, roughly 30% bigger boards. */
@media (min-width:1150px) and (min-aspect-ratio:13/10){
  .teams:not(.row) .tp{flex-direction:row;align-items:stretch;gap:16px;padding:12px 14px}
  .teams:not(.row) .meta{
    display:flex;flex-direction:column;justify-content:center;gap:14px;
    flex:0 0 clamp(140px,26%,250px);min-width:0;
  }
  .teams:not(.row) .tp-head{flex-direction:column;align-items:flex-start;gap:7px}
  .teams:not(.row) .tname{white-space:normal;line-height:1.06;font-size:clamp(17px,3vmin,38px);flex:none}
  .teams:not(.row) .tag{font-size:clamp(11px,1.5vmin,16px);padding:3px 9px}
  .teams:not(.row) .badge{font-size:clamp(10px,1.35vmin,15px);padding:4px 11px}
  .teams:not(.row) .tp-foot{font-size:clamp(10px,1.3vmin,15px)}
  .teams:not(.row) .last{padding:3px 9px}
  .teams:not(.row) .tp-foot{flex-direction:column;align-items:flex-start;gap:6px}
  .teams:not(.row) .hp{flex-wrap:wrap}
  .teams:not(.row) .hpnum{font-size:clamp(16px,2.5vmin,30px);text-align:left;min-width:0}
  .teams:not(.row) .pips{flex-basis:100%}
  .teams:not(.row) .pip{height:11px;border-radius:4px}
}
.board{
  display:grid;gap:.34em;font-size:clamp(9px,1.5vmin,17px);
  grid-template-columns:1.35em repeat(7,1fr);grid-template-rows:1.35em repeat(7,1fr);
  aspect-ratio:1/1;height:100%;max-width:100%;max-height:100%;
  padding:.5em .55em;border-radius:12px;
  background:rgba(2,5,13,.88);border:1px solid rgba(255,255,255,.07);
  box-shadow:inset 0 0 46px rgba(0,0,0,.6),inset 0 1px 0 rgba(255,255,255,.035);
}
/* keep the board perfectly square whichever side runs out first
   (without this, cells stretch into rectangles in the 1x4 layout) */
@supports (container-type:size){
  .boardwrap{container-type:size}
  .board{width:min(100cqw,100cqh);height:min(100cqw,100cqh);max-width:none;max-height:none}
}
.lab{
  display:flex;align-items:center;justify-content:center;
  font-family:var(--f-mono);font-size:.95em;font-weight:700;
  color:var(--dim);opacity:.8;letter-spacing:.02em;
}
.cell{
  border-radius:7px;position:relative;display:flex;align-items:center;justify-content:center;
  background:linear-gradient(160deg,rgba(27,46,82,.94),rgba(12,22,45,.96));
  border:1px solid rgba(132,180,235,.14);
  box-shadow:inset 0 1px 0 rgba(255,255,255,.045);
  transition:background .25s,border-color .25s,box-shadow .25s;
}
.cell.ship.reveal{background:linear-gradient(160deg,rgba(255,255,255,.16),rgba(255,255,255,.06));border-color:rgba(255,255,255,.2)}
.cell.miss{background:linear-gradient(160deg,rgba(14,21,38,.95),rgba(9,14,28,.96));border-color:rgba(138,162,204,.18)}
.cell.miss::after{content:'';width:.42em;height:.42em;border-radius:50%;background:var(--miss);opacity:.75}
.cell.hit{
  background:radial-gradient(circle at 50% 45%,rgba(255,71,71,.45),rgba(255,71,71,.16) 70%);
  border-color:rgba(255,71,71,.5);box-shadow:inset 0 0 14px -4px var(--hit);
}
.cell.hit::after{
  content:'\2715';color:#fff;font-weight:700;font-size:1.05em;
  text-shadow:0 0 12px var(--hit),0 0 26px var(--hit);
}
.cell .ring{
  position:absolute;inset:-2px;border-radius:9px;border:2px solid #fff;opacity:0;pointer-events:none;
}
.cell.fresh .ring{animation:ring 1.5s ease-out 2}
@keyframes ring{
  0%{opacity:.95;transform:scale(.55)}
  70%{opacity:0;transform:scale(1.5)}
  100%{opacity:0;transform:scale(1.5)}
}
.cell.fresh.hit{animation:shake .45s ease}
@keyframes shake{
  0%,100%{transform:translate(0,0)}
  25%{transform:translate(-2px,1px)}
  50%{transform:translate(2px,-1px)}
  75%{transform:translate(-1px,-1px)}
}

.tp-foot{
  display:flex;align-items:center;justify-content:space-between;gap:8px;
  font-family:var(--f-mono);font-size:clamp(9px,1.2vmin,13px);letter-spacing:.1em;color:var(--dimmer);
}
.inc{white-space:nowrap;line-height:1.5}
.inc b{display:block;font-weight:400}
.last{padding:2px 7px;border-radius:6px;border:1px solid var(--line);opacity:0;transition:opacity .3s;white-space:nowrap}
.last.show{opacity:1}
.last.hit{color:var(--hit);border-color:rgba(255,71,71,.35);background:rgba(255,71,71,.08)}
.last.miss{color:var(--miss);border-color:rgba(138,162,204,.25)}

/* ---------- sidebar ---------- */
.side{display:flex;flex-direction:column;gap:10px;min-height:0}
.card{
  border:1px solid var(--line);border-radius:var(--r);background:var(--panel);
  backdrop-filter:blur(14px) saturate(1.2);padding:12px 13px;display:flex;flex-direction:column;min-height:0;
  box-shadow:0 18px 40px -28px #000,inset 0 1px 0 rgba(255,255,255,.05);
}
.card h3{
  font-family:var(--f-display);font-size:clamp(9px,1.2vmin,12px);letter-spacing:.28em;
  color:var(--dim);font-weight:700;display:flex;justify-content:space-between;align-items:center;
  margin-bottom:9px;flex-shrink:0;
}
.card h3 span{font-family:var(--f-mono);letter-spacing:.1em;color:var(--dimmer);font-weight:400}
#log{flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:2px;min-height:0;scrollbar-width:thin}
#log::-webkit-scrollbar{width:4px}
#log::-webkit-scrollbar-thumb{background:rgba(255,255,255,.14);border-radius:2px}
.ln{
  display:grid;grid-template-columns:auto 1.1em 1fr;gap:6px;align-items:baseline;
  font-family:var(--f-mono);font-size:clamp(9px,1.18vmin,13px);line-height:1.45;
  padding:4px 2px;border-bottom:1px solid rgba(255,255,255,.035);color:var(--dim);
  word-break:break-word;animation:slide .35s ease;
}
@keyframes slide{from{opacity:0;transform:translateX(10px)}to{opacity:1;transform:none}}
.ln:last-child{border-bottom:none}
.ln .t{color:var(--dimmer);opacity:.75;font-size:.85em}
.ln .g{text-align:center}
.ln.hit{color:var(--warn)}    .ln.hit .g{color:var(--warn)}
.ln.miss{color:var(--miss)}
.ln.elim{color:var(--hit);font-weight:600}
.ln.win{color:var(--gold);font-weight:700}
.ln.sys{color:var(--t1)}
.ln.new{background:linear-gradient(90deg,rgba(255,255,255,.07),transparent);border-radius:6px}
.empty{color:var(--dimmer);opacity:.45;font-family:var(--f-mono);font-size:11px;padding:6px 2px}


/* ---------- overlays ---------- */
.veil{
  position:fixed;inset:0;z-index:40;display:none;align-items:center;justify-content:center;
  background:rgba(2,4,10,.68);backdrop-filter:blur(6px);
}
.veil.show{display:flex;animation:fade .35s ease}
@keyframes fade{from{opacity:0}to{opacity:1}}
.winbox{text-align:center;padding:40px 70px;border-radius:24px;border:1px solid rgba(255,210,74,.35);background:rgba(10,8,2,.6)}
.winbox .k{font-family:var(--f-mono);letter-spacing:.5em;color:var(--gold);font-size:clamp(10px,1.4vmin,14px)}
.winbox .v{
  font-family:var(--f-display);font-weight:700;letter-spacing:.1em;
  font-size:clamp(34px,8vmin,110px);color:var(--gold);
  text-shadow:0 0 50px rgba(255,210,74,.55);margin:10px 0 6px;
}
.winbox .s{font-family:var(--f-mono);color:var(--dim);letter-spacing:.2em;font-size:clamp(9px,1.2vmin,13px)}
.help{max-width:560px;width:90%;background:var(--panel-2);border:1px solid var(--line-strong);border-radius:20px;padding:26px 30px}
.help h2{font-family:var(--f-display);letter-spacing:.2em;font-size:18px;margin-bottom:14px;color:var(--t1)}
.help table{width:100%;border-collapse:collapse;font-family:var(--f-mono);font-size:13px;color:var(--dim)}
.help td{padding:6px 4px;border-bottom:1px solid rgba(255,255,255,.05)}
.help td:first-child{width:90px;color:var(--txt)}
.help p{font-family:var(--f-mono);font-size:11px;color:var(--dimmer);margin-top:14px;line-height:1.6}

.toast{
  position:fixed;left:50%;top:16%;transform:translateX(-50%);z-index:45;
  font-family:var(--f-display);letter-spacing:.2em;font-weight:700;
  font-size:clamp(18px,3.2vmin,40px);padding:16px 44px;border-radius:14px;
  border:1px solid currentColor;background:rgba(6,8,18,.85);backdrop-filter:blur(6px);
  opacity:0;pointer-events:none;transition:opacity .3s,transform .3s;
}
.toast.show{opacity:1;transform:translateX(-50%) translateY(6px)}

.demoflag{
  position:fixed;left:10px;bottom:8px;z-index:50;font-family:var(--f-mono);font-size:10px;
  letter-spacing:.2em;color:var(--warn);opacity:.65;border:1px solid rgba(255,167,43,.3);
  padding:3px 9px;border-radius:999px;display:none;
}
body.demo .demoflag{display:block}

/* ---------- small screens / laptop mirroring ---------- */
@media (max-aspect-ratio:1/1),(max-width:900px){
  body{overflow:auto}
  .deck{height:auto;min-height:100vh}
  .stage{grid-template-columns:1fr}
  .teams{grid-template-columns:1fr 1fr;grid-template-rows:auto}
  .side{max-height:46vh}
  .board{font-size:12px}
}
/* reclaim header width on smaller screens before it wraps to two rows */
@media (max-width:1500px){ .brand i{display:none} }
@media (max-width:1400px){ .btn kbd{display:none} }
@media (max-width:620px){ .teams{grid-template-columns:1fr} }
</style>
</head>
<body>
<div class="bg-grid"></div>
<div class="bg-beams"></div>
<div class="bg-noise"></div>
<canvas id="fx"></canvas>

<div class="deck">

  <!-- ============ HEADER ============ -->
  <header class="top">
    <div class="brand"><b>BATTLESHIP</b><i>ELECTRONICS CLUB</i></div>
    <div class="chips">
      <span class="chip" id="conn"><span class="dot off" id="conndot"></span><span id="conntxt">CONNECTING</span></span>
      <span class="chip phase" id="phase" data-p="0">SETUP</span>
      <span class="chip" id="sync">NO DATA YET</span>
      <span class="chip reveal" id="revflag" style="display:none">SHIPS VISIBLE</span>
      <div class="ctl">
        <button class="btn b-start" onclick="UI.cmd('START')">Start <kbd>S</kbd></button>
        <button class="btn b-force" onclick="UI.forceStart()">Force <kbd>G</kbd></button>
        <button class="btn b-skip"  onclick="UI.cmd('SKIP_TURN')">Skip <kbd>K</kbd></button>
        <button class="btn b-reset" onclick="UI.reset()">Reset <kbd>R</kbd></button>
        <button class="btn b-ico" id="bsound"  onclick="UI.toggleSound()" title="Sound (M)">&#9835;</button>
        <button class="btn b-ico" id="blayout" onclick="UI.toggleLayout()" title="Layout 2x2 / 1x4 (L)">&#9638;</button>
        <button class="btn b-ico" onclick="UI.fullscreen()" title="Fullscreen (F)">&#9974;</button>
        <button class="btn b-ico" onclick="UI.help()" title="Shortcuts (?)">?</button>
      </div>
    </div>
  </header>

  <!-- ============ TURN BAR ============ -->
  <div class="turnbar" id="turnbar">
    <div class="side"><span id="aliveLabel">REGISTERED</span><b id="aliveCount">0 / 4</b></div>
    <div class="turntext" id="turntext">WAITING FOR TEAMS TO REGISTER<small id="turnsub">POWER ON EACH TEAM NODE TO REGISTER</small></div>
    <div class="side" style="text-align:right;align-items:flex-end"><span>TURN TIME</span><b class="clock" id="clock">0:00</b></div>
  </div>

  <!-- ============ STAGE ============ -->
  <div class="stage">
    <div class="teams" id="teams"></div>

    <aside class="side">
      <div class="card" style="flex:1">
        <h3>BATTLE LOG <span id="logcount">0</span></h3>
        <div id="log"><div class="empty">no events yet</div></div>
      </div>
    </aside>
  </div>
</div>

<div class="toast" id="toast"></div>
<div class="veil" id="winveil" onclick="UI.closeWin()">
  <div class="winbox">
    <div class="k">VICTORY</div>
    <div class="v" id="winname">TEAM</div>
    <div class="s">CLICK ANYWHERE TO DISMISS</div>
  </div>
</div>
<div class="veil" id="helpveil" onclick="UI.help(false)">
  <div class="help" onclick="event.stopPropagation()">
    <h2>ORGANIZER SHORTCUTS</h2>
    <table>
      <tr><td>S</td><td>Start round</td></tr>
      <tr><td>G</td><td>Force start (fewer than 4 teams)</td></tr>
      <tr><td>K</td><td>Skip current turn</td></tr>
      <tr><td>R</td><td>Reset round (asks to confirm)</td></tr>
      <tr><td>M</td><td>Mute / unmute sound effects</td></tr>
      <tr><td>L</td><td>Layout: 2x2 grid / 1x4 row</td></tr>
      <tr><td>F</td><td>Fullscreen (use this before projecting)</td></tr>
      <tr><td>P</td><td>Performance mode (drops particles + blur)</td></tr>
      <tr><td>V</td><td>Reveal ships - ORGANIZER DEBUG ONLY</td></tr>
      <tr><td>Esc</td><td>Close this / the victory overlay</td></tr>
    </table>
    <p>Backend untouched: this page only calls GET /api/state, GET /events and POST /api/command, exactly like the original dashboard.</p>
  </div>
</div>
<div class="demoflag">DEMO MODE - NO CENTRAL CONNECTED</div>

<script>
/* =====================================================================
   FRONTEND LOGIC
   Contract with the existing Python backend (unchanged):
     GET  /api/state  -> {round_state,current_turn,teams[],log[],central_connected}
     GET  /events     -> SSE of {type:"state"|"event"|"conn", ...}
     POST /api/command {cmd:"START"|"FORCE_START"|"SKIP_TURN"|"RESET"|"GET_STATE"}
   Team object: {id,name,registered,eliminated,remaining,grid[7][7]}
   Cell codes : 0 water, 1 ship (hidden), 2 miss, 3 hit
   ===================================================================== */

const COLORS = {1:'#27e6ff',2:'#ff3d84',3:'#b06bff',4:'#3dff88'};
const GRID   = 7;                               // board is GRID x GRID cells
const AXIS   = Array.from({length:GRID},(_,i)=>String(i+1));   // both axes are numbered
const coordOf = (r,c) => 'R'+(r+1)+' C'+(c+1);  // how a cell is named in the UI
const PHASES = {0:'SETUP',1:'READY',2:'LIVE',3:'GAME OVER'};
const SHIPS  = 9;
const SOFT_TURN_LIMIT = 60;   // seconds - clock turns amber
const HARD_TURN_LIMIT = 90;   // seconds - clock turns red (display only, no auto-skip)

const store = {
  get(k,d){ try{ const v=localStorage.getItem(k); return v===null?d:JSON.parse(v);}catch(e){return d;} },
  set(k,v){ try{ localStorage.setItem(k,JSON.stringify(v)); }catch(e){} }
};

const UI = {
  state:{round_state:0,current_turn:0,teams:[],log:[]},
  prevGrid:{},          // id -> flat copy of last grid, for detecting new shots
  prevRemaining:{},     // id -> last remaining, for pip flash
  panels:{},            // id -> {root, cells[25], ...}
  logView:[],           // [{text, ts}]
  turnStart:0,
  lastSync:0,
  sound:store.get('mz_sound',true),
  layoutRow:store.get('mz_row',false),
  perf:store.get('mz_perf',false),
  reveal:false,
  demo:false,
  seenWin:false,

  /* ---------- boot ---------- */
  init(){
    this.buildPanels();
    document.getElementById('teams').classList.toggle('row',this.layoutRow);
    document.body.classList.toggle('perf',this.perf);
    document.getElementById('bsound').classList.toggle('active',this.sound);
    document.getElementById('blayout').classList.toggle('active',this.layoutRow);
    this.render();
    setInterval(()=>this.tick(),250);
    this.connect();
    Particles.start();
    window.addEventListener('keydown',e=>this.key(e));
  },

  /* Demo data must NEVER reach the projector by accident: it can only run
     when the page is opened without a backend (file:// or srcdoc) or with ?demo=1.
     If Flask dies mid-match we show DASHBOARD OFFLINE and auto-reconnect instead. */
  demoAllowed: /[?&]demo=1/.test(location.search) || !/^https?:$/.test(location.protocol),
  everConnected:false,
  es:null,

  connect(){
    this.poll();
    this.openSSE();
    setInterval(()=>{
      if(this.demo) return;
      if(Date.now()-this.lastSync>8000) this.poll();
      if(!this.es || this.es.readyState===2) this.openSSE();
    },4000);
    setTimeout(()=>{ if(!this.everConnected && this.demoAllowed) Demo.start(); },1800);
  },

  poll(){
    try{
      fetch('/api/state',{cache:'no-store'}).then(r=>r.json()).then(d=>{
        this.everConnected=true; this.serverUp=true;
        this.apply(d,!this.gotFirst); this.gotFirst=true;
        this.setConn(!!d.central_connected);
      }).catch(()=>this.offline());
    }catch(e){ this.offline(); }
  },

  openSSE(){
    if(this.demo) return;
    try{ if(this.es) this.es.close(); }catch(e){}
    try{
      const es=new EventSource('/events'); this.es=es;
      es.onmessage=ev=>{
        this.everConnected=true; this.serverUp=true;
        let m; try{ m=JSON.parse(ev.data); }catch(e){ return; }
        if(m.type==='state'){ this.apply(m,!this.gotFirst); this.gotFirst=true;
          if('central_connected' in m) this.setConn(m.central_connected); }
        else if(m.type==='event'){ this.pushLog(m.text,true); this.lastSync=Date.now(); this.render(); }
        else if(m.type==='conn'){ this.setConn(m.connected); }
      };
      es.onerror=()=>{ if(this.everConnected) this.offline(); };
    }catch(e){ this.offline(); }
  },

  /* the dashboard process itself is unreachable - different failure from
     "Central is unplugged", so it gets its own label */
  offline(){
    if(this.demo) return;
    this.serverUp=false;
    document.getElementById('conndot').className='dot off';
    document.getElementById('conntxt').textContent='DASHBOARD OFFLINE';
    document.getElementById('conn').className='chip dead';
    if(!this.everConnected && this.demoAllowed) Demo.start();
  },

  /* ---------- state in ---------- */
  apply(msg,first){
    const prevTurn=this.state.current_turn, prevPhase=this.state.round_state;
    this.state=Object.assign({round_state:0,current_turn:0,teams:[],log:[]},msg);
    this.lastSync=Date.now();
    this.mergeLog(this.state.log||[]);
    if(this.state.round_state!==prevPhase || this.state.current_turn!==prevTurn) this.turnStart=Date.now();
    if(first) this.turnStart=Date.now();
    if(this.state.round_state!==3) this.seenWin=false;
    this.render(first);
  },

  /* keep client timestamps while accepting the server log as the source of truth */
  mergeLog(server){
    const cur=this.logView.map(e=>e.text);
    if(cur.length===server.length && cur.every((t,i)=>t===server[i])) return;
    const out=[];
    for(let i=server.length-1,j=cur.length-1;i>=0;i--,j--){
      const ts=(j>=0 && cur[j]===server[i]) ? this.logView[j].ts : null;
      out.unshift({text:server[i],ts:ts});
    }
    this.logView=out;
  },
  pushLog(text,live){
    this.logView.push({text:text,ts:live?Date.now():null});
    this.logView=this.logView.slice(-40);
    this.reactTo(text);
  },

  /* audio / toast reactions driven purely by the log text */
  reactTo(text){
    const t=(text||'').toLowerCase();
    if(t.includes('eliminated')||t.includes('destroyed')){ Sfx.elim(); this.toast(text.toUpperCase(),'#ff4747'); }
    else if(t.includes('wins')||t.includes('winner')){ Sfx.win(); }
    else if(t.includes('registered')||t.includes('joined')){ Sfx.blip(); }
  },

  setConn(c){
    if(this.serverUp===false && !this.demo) return;
    document.getElementById('conndot').className='dot '+(c?'on':'off');
    document.getElementById('conntxt').textContent=c?'CENTRAL ONLINE':'CENTRAL OFFLINE';
    document.getElementById('conn').className='chip '+(c?'live':'dead');
  },

  /* ---------- build the 4 panels ONCE, then patch in place ---------- */
  buildPanels(){
    const host=document.getElementById('teams'); host.innerHTML='';
    for(let id=1;id<=4;id++){
      const c=COLORS[id];
      const p=document.createElement('div');
      p.className='tp'; p.style.setProperty('--c',c);
      p.innerHTML=
        '<div class="stamp elim">ELIMINATED</div>'+
        '<div class="stamp wins">WINNER</div>'+
        '<div class="meta">'+
          '<div class="tp-head"><span class="tag">T'+id+'</span>'+
            '<span class="tname">AWAITING NODE</span><span class="badge wait">WAITING</span></div>'+
          '<div class="hp"><div class="pips"></div><span class="hpnum">-/9</span></div>'+
          '<div class="tp-foot"><span class="inc">INCOMING 0<b>HITS 0</b></span><span class="last">--</span></div>'+
        '</div>'+
        '<div class="boardwrap"><div class="board"></div></div>';
      const pips=p.querySelector('.pips');
      for(let i=0;i<SHIPS;i++){ const d=document.createElement('div'); d.className='pip'; pips.appendChild(d); }
      const b=p.querySelector('.board');
      b.appendChild(el('div','lab'));
      AXIS.forEach(L=>b.appendChild(el('div','lab',L)));
      const cells=[];
      for(let r=0;r<GRID;r++){
        b.appendChild(el('div','lab',String(r+1)));
        for(let col=0;col<GRID;col++){
          const cell=el('div','cell'); cell.appendChild(el('div','ring'));
          b.appendChild(cell); cells.push(cell);
        }
      }
      host.appendChild(p);
      this.panels[id]={root:p,cells:cells,pips:[...pips.children],
        name:p.querySelector('.tname'),badge:p.querySelector('.badge'),
        hpnum:p.querySelector('.hpnum'),inc:p.querySelector('.inc'),last:p.querySelector('.last')};
    }
  },

  /* Auto layout: once a round is live/over, only registered teams take part
     (that's what FORCE_START does), so show just those, in one row. */
  applyLayout(){
    const host=document.getElementById('teams'), s=this.state;
    const active=(s.round_state>=2)
      ? (s.teams||[]).filter(t=>t.registered).map(t=>t.id)
      : [1,2,3,4];
    const n=active.length;
    const fit = s.round_state>=2 && n>=2 && n<4;
    for(let id=1;id<=4;id++){
      this.panels[id].root.style.display = (fit && !active.includes(id)) ? 'none' : '';
    }
    host.classList.toggle('fit',fit);
    host.classList.toggle('row',fit || this.layoutRow);
    host.style.gridTemplateColumns = fit ? 'repeat('+n+',1fr)' : '';
  },

  /* ---------- render ---------- */
  render(first){
    const s=this.state;
    this.applyLayout();
    const byId={}; (s.teams||[]).forEach(t=>byId[t.id]=t);

    let alive=0,reg=0,winner=null;

    for(let id=1;id<=4;id++){
      const P=this.panels[id];
      const t=byId[id]||{id:id,name:'',registered:false,eliminated:false,remaining:0,grid:null};
      const grid=t.grid||null;
      if(t.registered) reg++;
      if(t.registered && !t.eliminated){ alive++; winner=t; }

      const isTurn = s.round_state===2 && id===s.current_turn && !t.eliminated && t.registered;
      P.root.classList.toggle('turn',isTurn);
      P.root.classList.toggle('out',!!t.eliminated);
      P.root.classList.toggle('idle',s.round_state===2 && !isTurn && !t.eliminated);
      P.root.classList.toggle('unreg',!t.registered);

      P.name.textContent = t.registered ? (t.name||('TEAM '+id)).toUpperCase() : 'AWAITING NODE';
      P.name.style.color = t.registered ? '' : 'var(--dimmer)';

      if(t.eliminated){ P.badge.className='badge dead'; P.badge.textContent='DESTROYED'; }
      else if(t.registered){ P.badge.className='badge alive'; P.badge.textContent=isTurn?'FIRING':'READY'; }
      else { P.badge.className='badge wait'; P.badge.textContent=s.round_state>=2?'NOT IN ROUND':'WAITING'; }

      /* HP pips */
      const rem=t.registered?(t.remaining|0):0;
      const dropped=this.prevRemaining[id]!==undefined && rem<this.prevRemaining[id];
      P.pips.forEach((pip,i)=>{
        const on=i<rem;
        pip.className='pip'+(on?' on':'')+(rem<=3?' crit':rem<=5?' low':'');
        if(dropped && i===rem) { pip.classList.add('just'); setTimeout(()=>pip.classList.remove('just'),700); }
      });
      this.prevRemaining[id]=rem;
      P.hpnum.textContent = t.registered ? rem+'/9' : '-/9';
      P.hpnum.style.color = !t.registered?'var(--dimmer)':rem<=3?'var(--hit)':rem<=5?'var(--warn)':'var(--dim)';

      /* board + new-shot detection (diff against the previous snapshot) */
      let inc=0,myHits=0;
      const prev=this.prevGrid[id];
      const flat=[];
      for(let r=0;r<GRID;r++) for(let c=0;c<GRID;c++){
        const v = grid && grid[r] ? (grid[r][c]|0) : 0;
        flat.push(v);
        const cell=P.cells[r*GRID+c];
        let cls='cell';
        if(v===3){cls+=' hit'; inc++; myHits++;}
        else if(v===2){cls+=' miss'; inc++;}
        else if(v===1){cls+=' ship'+(this.reveal?' reveal':'');}
        if(prev && prev[r*GRID+c]!==v && (v===2||v===3) && !first){
          cls+=' fresh';
          const coord=coordOf(r,c);
          const rc=cell.getBoundingClientRect();
          Particles.ping(rc.left+rc.width/2,rc.top+rc.height/2,
                         v===3?'255,71,71':'138,162,204',v===3?.7:.35,v===3?260:150);
          if(v===3) Particles.bubbleBurst(rc.left+rc.width/2,rc.top+rc.height/2);
          this.flashLast(P,coord,v===3);
          if(v===3) Sfx.hit(); else Sfx.miss();
          setTimeout(()=>cell.classList.remove('fresh'),3200);
        } else if(cell.classList.contains('fresh')) cls+=' fresh';
        cell.className=cls;
      }
      this.prevGrid[id]=flat;
      P.inc.innerHTML='INCOMING '+inc+'<b>HITS '+myHits+'</b>';
    }

    /* header chips */
    document.getElementById('phase').textContent=PHASES[s.round_state]||'?';
    document.getElementById('phase').dataset.p=s.round_state;
    const live = s.round_state>=2;
    document.getElementById('aliveLabel').textContent = live ? 'ALIVE' : 'REGISTERED';
    document.getElementById('aliveCount').textContent = live ? (alive+' / '+(reg||4)) : (reg+' / 4');

    /* turn bar */
    const tb=document.getElementById('turnbar'), tt=document.getElementById('turntext'), sub=document.getElementById('turnsub');
    if(s.round_state===2){
      const c=COLORS[s.current_turn]||'#fff';
      tb.style.setProperty('--accent',c);
      const t=byId[s.current_turn];
      tt.firstChild.nodeValue='\u25b6 '+((t&&t.name)?t.name.toUpperCase():('TEAM '+s.current_turn))+'  TO FIRE';
      sub.textContent='TEAM '+s.current_turn+' \u00b7 SELECT A TARGET AND A COORDINATE';
    } else if(s.round_state===3){
      tb.style.setProperty('--accent','var(--gold)');
      tt.firstChild.nodeValue=winner?('\u2605 '+((winner.name||('TEAM '+winner.id)).toUpperCase())+' WINS'):'GAME OVER';
      sub.textContent='PRESS RESET TO ARM THE NEXT ROUND';
      if(winner && !this.seenWin){ this.seenWin=true; this.celebrate(winner); }
    } else if(s.round_state===1){
      tb.style.setProperty('--accent','var(--warn)');
      tt.firstChild.nodeValue='ALL TEAMS READY';
      sub.textContent='PRESS START WHEN THE CROWD IS SETTLED';
    } else {
      tb.style.setProperty('--accent','var(--t1)');
      tt.firstChild.nodeValue='WAITING FOR TEAMS TO REGISTER';
      sub.textContent=reg+' OF 4 NODES ONLINE';
    }
    for(let id=1;id<=4;id++) this.panels[id].root.classList.toggle('win',s.round_state===3&&winner&&winner.id===id);

    this.renderLog();
  },

  flashLast(P,coord,isHit){
    P.last.textContent=coord+(isHit?' HIT':' MISS');
    P.last.className='last show '+(isHit?'hit':'miss');
    clearTimeout(P._lt); P._lt=setTimeout(()=>{P.last.classList.remove('show');},6000);
  },

  renderLog(){
    const box=document.getElementById('log');
    document.getElementById('logcount').textContent=this.logView.length;
    if(!this.logView.length){ box.innerHTML='<div class="empty">no events yet</div>'; return; }
    box.innerHTML='';
    for(let i=this.logView.length-1;i>=0;i--){
      const e=this.logView[i], t=(e.text||'').toLowerCase();
      let cls='ln',g='\u25b8';
      if(t.includes('eliminated')||t.includes('destroyed')){cls+=' elim';g='\u2620';}
      else if(t.includes('wins')||t.includes('winner')||t.includes('game over')){cls+=' win';g='\u2605';}
      else if(t.includes('hit')){cls+=' hit';g='\u2739';}
      else if(t.includes('miss')){cls+=' miss';g='\u25cb';}
      else if(t.includes('reset')||t.includes('start')||t.includes('registered')){cls+=' sys';g='\u25b8';}
      if(i===this.logView.length-1) cls+=' new';
      const ln=el('div',cls);
      ln.appendChild(el('span','t',e.ts?new Date(e.ts).toLocaleTimeString([],{hour12:false}):''));
      ln.appendChild(el('span','g',g));
      ln.appendChild(el('span','x',e.text));
      box.appendChild(ln);
    }
  },

  /* ---------- 4 Hz housekeeping: clock + stale-feed warning ---------- */
  tick(){
    const c=document.getElementById('clock');
    if(this.state.round_state===2){
      const s=Math.floor((Date.now()-this.turnStart)/1000);
      c.textContent=Math.floor(s/60)+':'+String(s%60).padStart(2,'0');
      c.className='clock'+(s>=HARD_TURN_LIMIT?' over':s>=SOFT_TURN_LIMIT?' warn':'');
    } else { c.textContent=this.state.round_state===3?'FINAL':'--:--'; c.className='clock'; }

    const sy=document.getElementById('sync');
    if(this.lastSync){
      const age=Math.round((Date.now()-this.lastSync)/1000);
      sy.textContent=age<3?'SYNCED NOW':('SYNCED '+age+'s AGO');
      sy.className='chip'+(age>8?' dead':'');
    }
  },

  /* ---------- organizer actions ---------- */
  cmd(c){
    if(this.demo) return Demo.cmd(c);
    fetch('/api/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({cmd:c})})
      .catch(()=>this.toast('COMMAND FAILED - CHECK THE DASHBOARD','#ff4747'));
  },
  reset(){ if(confirm('Reset the current round? This starts a new match log file.')) this.cmd('RESET'); },
  forceStart(){
    const teams=this.state.teams||[];
    const reg=teams.filter(t=>t.registered).length;
    if(reg<2){ alert('Need at least 2 registered teams to force-start.'); return; }
    const msg=reg<4 ? ('Start with '+reg+' of 4 teams? The other '+(4-reg)+' will sit this round out.')
                    : 'All 4 teams are registered. Start the round?';
    if(confirm(msg)) this.cmd('FORCE_START');
  },

  toggleSound(){ this.sound=!this.sound; store.set('mz_sound',this.sound);
    document.getElementById('bsound').classList.toggle('active',this.sound);
    this.toast(this.sound?'SOUND ON':'SOUND OFF','#27e6ff'); if(this.sound) Sfx.blip(); },
  toggleLayout(){ this.layoutRow=!this.layoutRow; store.set('mz_row',this.layoutRow);
    this.applyLayout();
    document.getElementById('blayout').classList.toggle('active',this.layoutRow); },
  togglePerf(){ this.perf=!this.perf; store.set('mz_perf',this.perf);
    document.body.classList.toggle('perf',this.perf);
    this.toast(this.perf?'PERFORMANCE MODE':'FULL EFFECTS','#27e6ff'); },
  toggleReveal(){ this.reveal=!this.reveal;
    document.getElementById('revflag').style.display=this.reveal?'':'none';
    this.toast(this.reveal?'SHIPS REVEALED - DO NOT PROJECT':'SHIPS HIDDEN',this.reveal?'#ffa72b':'#3dff88');
    this.render(); },
  fullscreen(){ try{ document.fullscreenElement?document.exitFullscreen():document.documentElement.requestFullscreen(); }catch(e){} },
  help(show){ const v=document.getElementById('helpveil');
    v.classList.toggle('show', show===undefined ? !v.classList.contains('show') : !!show); },

  key(e){
    if(e.target.tagName==='INPUT'||e.target.tagName==='TEXTAREA') return;
    const k=e.key.toLowerCase();
    if(k==='s') this.cmd('START');
    else if(k==='g') this.forceStart();
    else if(k==='k') this.cmd('SKIP_TURN');
    else if(k==='r') this.reset();
    else if(k==='m') this.toggleSound();
    else if(k==='l') this.toggleLayout();
    else if(k==='p') this.togglePerf();
    else if(k==='v') this.toggleReveal();
    else if(k==='f') this.fullscreen();
    else if(k==='?'||k==='/') this.help();
    else if(k==='escape'){ this.help(false); this.closeWin(); }
  },

  toast(text,color){
    const t=document.getElementById('toast');
    t.textContent=text; t.style.color=color||'#fff'; t.classList.add('show');
    clearTimeout(this._tt); this._tt=setTimeout(()=>t.classList.remove('show'),2600);
  },
  celebrate(w){
    document.getElementById('winname').textContent=(w.name||('TEAM '+w.id)).toUpperCase();
    document.getElementById('winveil').classList.add('show');
    Particles.confetti(COLORS[w.id]);
    clearTimeout(this._wt); this._wt=setTimeout(()=>this.closeWin(),11000);
  },
  closeWin(){ document.getElementById('winveil').classList.remove('show'); }
};

function el(tag,cls,text){ const d=document.createElement(tag); if(cls)d.className=cls; if(text!==undefined)d.textContent=text; return d; }

/* =====================================================================
   SFX - synthesised with WebAudio, so there are no asset files to ship
   ===================================================================== */
const Sfx={
  ctx:null,
  on(){ if(!UI.sound) return null;
    try{ if(!this.ctx) this.ctx=new (window.AudioContext||window.webkitAudioContext)();
      if(this.ctx.state==='suspended') this.ctx.resume(); return this.ctx; }catch(e){ return null; } },
  tone(f1,f2,dur,type,gain){
    const c=this.on(); if(!c) return;
    const o=c.createOscillator(),g=c.createGain();
    o.type=type||'sine'; o.frequency.setValueAtTime(f1,c.currentTime);
    if(f2) o.frequency.exponentialRampToValueAtTime(Math.max(30,f2),c.currentTime+dur);
    g.gain.setValueAtTime(gain||0.09,c.currentTime);
    g.gain.exponentialRampToValueAtTime(0.0008,c.currentTime+dur);
    o.connect(g); g.connect(c.destination); o.start(); o.stop(c.currentTime+dur+0.02);
  },
  noise(dur,gain){
    const c=this.on(); if(!c) return;
    const n=Math.floor(c.sampleRate*dur), buf=c.createBuffer(1,n,c.sampleRate), d=buf.getChannelData(0);
    for(let i=0;i<n;i++) d[i]=(Math.random()*2-1)*Math.pow(1-i/n,2);
    const src=c.createBufferSource(); src.buffer=buf;
    const g=c.createGain(); g.gain.value=gain||0.12;
    const f=c.createBiquadFilter(); f.type='lowpass'; f.frequency.value=900;
    src.connect(f); f.connect(g); g.connect(c.destination); src.start();
  },
  hit(){ this.tone(220,50,0.42,'square',0.10); this.noise(0.35,0.16); },
  miss(){ this.tone(680,420,0.14,'sine',0.05); },
  blip(){ this.tone(880,1200,0.1,'triangle',0.05); },
  elim(){ this.tone(180,45,1.0,'sawtooth',0.12); this.noise(0.8,0.14); },
  win(){ [523,659,784,1047].forEach((f,i)=>setTimeout(()=>this.tone(f,f,0.3,'triangle',0.09),i*130)); }
};

/* =====================================================================
   PARTICLES + CONFETTI (auto-throttles itself on weak laptops)
   ===================================================================== */
const Particles={
  cv:null,ctx:null,W:0,H:0,dots:[],dust:[],bubbles:[],conf:[],pings:[],sprites:{},frames:0,t0:0,
  start(){
    this.cv=document.getElementById('fx'); this.ctx=this.cv.getContext('2d');
    const rs=()=>{ this.W=this.cv.width=innerWidth; this.H=this.cv.height=innerHeight; };
    addEventListener('resize',rs); rs();
    const cols=['39,230,255','255,61,132','176,107,255','61,255,136'];
    cols.forEach(col=>{
      const cv=document.createElement('canvas'); cv.width=cv.height=96;
      const g=cv.getContext('2d');
      const grd=g.createRadialGradient(48,48,0,48,48,48);
      grd.addColorStop(0,'rgba('+col+',1)'); grd.addColorStop(.55,'rgba('+col+',.28)');
      grd.addColorStop(1,'rgba('+col+',0)');
      g.fillStyle=grd; g.fillRect(0,0,96,96); this.sprites[col]=cv;
    });
    /* far parallax layer - big soft blobs drifting behind everything */
    for(let i=0;i<13;i++) this.dust.push({x:Math.random()*this.W,y:Math.random()*this.H,
      r:Math.random()*70+38,dx:(Math.random()-.5)*.10,dy:(Math.random()-.5)*.08,
      a:Math.random()*.05+.025,c:cols[i%4]});
    /* rising bubbles - slow, peripheral, nothing to read into */
    for(let i=0;i<16;i++) this.bubbles.push({x:Math.random()*this.W,y:Math.random()*this.H,
      r:Math.random()*5+2,v:Math.random()*.42+.16,ph:Math.random()*6.3,
      sw:Math.random()*16+6,a:Math.random()*.10+.05});
    for(let i=0;i<54;i++) this.dots.push({x:Math.random()*this.W,y:Math.random()*this.H,
      r:Math.random()*1.7+.5,dx:(Math.random()-.5)*.26,dy:(Math.random()-.5)*.26,
      a:Math.random()*.38+.10,c:cols[i%4]});
    /* idle sonar contacts so the screen never looks frozen between turns */
    setInterval(()=>{ if(!UI.perf && document.visibilityState==='visible')
      this.ping(Math.random()*this.W,Math.random()*this.H,'39,230,255',.22,70); },4200);
    this.t0=performance.now(); this.loop();
  },
  /* expanding sonar ring - also fired at the exact cell of every new shot */
  ping(x,y,color,alpha,max){
    this.pings.push({x:x,y:y,r:4,max:max||150,c:color||'39,230,255',a:alpha||.55});
  },
  /* small trail of bubbles from a struck cell */
  bubbleBurst(x,y){
    if(UI.perf) return;
    for(let i=0;i<7;i++) this.bubbles.push({x:x+(Math.random()-.5)*26,y:y+(Math.random()-.5)*10,
      r:Math.random()*4+1.6,v:Math.random()*.7+.35,ph:Math.random()*6.3,
      sw:Math.random()*10+4,a:Math.random()*.16+.08,tmp:true});
  },
  confetti(color){
    this.cv.style.zIndex=44;   /* above the victory veil (z-40) while it rains */
    for(let i=0;i<160;i++) this.conf.push({x:this.W/2+(Math.random()-.5)*260,y:this.H*0.42,
      vx:(Math.random()-.5)*13,vy:Math.random()*-13-4,g:.34,
      w:Math.random()*8+3,h:Math.random()*5+2,rot:Math.random()*6.3,vr:(Math.random()-.5)*.35,
      c:[color,'#ffd24a','#ffffff','#27e6ff'][i%4],life:1});
  },
  loop(){
    const c=this.ctx; if(!c) return;
    c.clearRect(0,0,this.W,this.H);
    if(!UI.perf){
      for(const d of this.dust){
        d.x+=d.dx; d.y+=d.dy;
        if(d.x<-d.r)d.x=this.W+d.r; if(d.x>this.W+d.r)d.x=-d.r;
        if(d.y<-d.r)d.y=this.H+d.r; if(d.y>this.H+d.r)d.y=-d.r;
        c.globalAlpha=d.a; c.drawImage(this.sprites[d.c],d.x-d.r,d.y-d.r,d.r*2,d.r*2);
      }
      c.globalAlpha=1;
      for(const p of this.dots){
        p.x+=p.dx;p.y+=p.dy;
        if(p.x<0)p.x=this.W; if(p.x>this.W)p.x=0; if(p.y<0)p.y=this.H; if(p.y>this.H)p.y=0;
        c.beginPath(); c.arc(p.x,p.y,p.r,0,6.283); c.fillStyle='rgba('+p.c+','+p.a+')'; c.fill();
      }
    }
    if(!UI.perf){
      for(let i=this.bubbles.length-1;i>=0;i--){
        const b=this.bubbles[i];
        b.y-=b.v; b.ph+=.012;
        const bx=b.x+Math.sin(b.ph)*b.sw;
        if(b.y<-12){
          if(b.tmp){ this.bubbles.splice(i,1); continue; }
          b.y=this.H+Math.random()*90; b.x=Math.random()*this.W;
        }
        c.beginPath(); c.arc(bx,b.y,b.r,0,6.283);
        c.strokeStyle='rgba(150,220,255,'+b.a+')'; c.lineWidth=1; c.stroke();
        c.beginPath(); c.arc(bx-b.r*.32,b.y-b.r*.34,Math.max(.6,b.r*.26),0,6.283);
        c.fillStyle='rgba(205,240,255,'+(b.a*.95)+')'; c.fill();
      }
    }
    for(let i=this.pings.length-1;i>=0;i--){
      const g=this.pings[i]; g.r+=g.max/70;
      const k=1-g.r/g.max;
      if(k<=0){ this.pings.splice(i,1); continue; }
      c.beginPath(); c.arc(g.x,g.y,g.r,0,6.283);
      c.strokeStyle='rgba('+g.c+','+(g.a*k*k)+')'; c.lineWidth=1.6; c.stroke();
    }
    for(let i=this.conf.length-1;i>=0;i--){
      const p=this.conf[i]; p.vy+=p.g; p.x+=p.vx; p.y+=p.vy; p.rot+=p.vr; p.life-=0.004;
      c.save(); c.translate(p.x,p.y); c.rotate(p.rot); c.globalAlpha=Math.max(0,p.life);
      c.fillStyle=p.c; c.fillRect(-p.w/2,-p.h/2,p.w,p.h); c.restore();
      if(p.y>this.H+40||p.life<=0) this.conf.splice(i,1);
    }
    if(!this.conf.length && this.cv.style.zIndex==='44') this.cv.style.zIndex='';
    /* auto perf-mode if the laptop is struggling while projecting */
    this.frames++;
    if(this.frames===120){
      const fps=120000/(performance.now()-this.t0);
      if(fps<26 && !UI.perf){ UI.perf=true; document.body.classList.add('perf'); }
    }
    requestAnimationFrame(()=>this.loop());
  }
};

/* =====================================================================
   DEMO MODE - only runs when /api/state is unreachable (no backend).
   Lets you rehearse the projection before the hardware is wired up.
   ===================================================================== */
const Demo={
  s:null,timer:null,
  names:['CIRCUIT BREAKERS','OHM RAIDERS','FLUX CAPACITORS','NULL POINTERS'],
  start(){
    if(UI.demo) return;
    UI.demo=true; UI.serverUp=true; document.body.classList.add('demo');
    try{ if(UI.es) UI.es.close(); }catch(e){}
    UI.setConn(true);
    this.reset(); this.timer=setInterval(()=>this.step(),1100);
  },
  reset(){
    this.s={round_state:0,current_turn:0,teams:[1,2,3,4].map(id=>({
      id:id,name:'',registered:false,eliminated:false,remaining:SHIPS,grid:this.blank()})),log:[]};
    this.phaseTick=0; this.push('Game reset - waiting for teams to register'); this.flush();
  },
  blank(){ return Array.from({length:GRID},()=>Array(GRID).fill(0)); },
  place(t){
    let n=0; while(n<SHIPS){ const r=(Math.random()*GRID)|0,c=(Math.random()*GRID)|0;
      if(t.grid[r][c]===0){t.grid[r][c]=1;n++;} } },
  push(x){ this.s.log.push(x); this.s.log=this.s.log.slice(-20); UI.pushLog(x,true); },
  flush(){ UI.apply(JSON.parse(JSON.stringify(Object.assign({type:'state',central_connected:true},this.s)))); },
  cmd(c){
    if(c==='RESET'){ this.reset(); }
    else if(c==='START'||c==='FORCE_START'){
      const first=this.s.teams.find(t=>t.registered);
      if(this.s.round_state<2 && first){ this.s.round_state=2; this.s.current_turn=first.id;
        this.push('Round started'); this.flush(); } }
    else if(c==='SKIP_TURN'){ this.push('Turn skipped by organizer'); this.next(); this.flush(); }
  },
  next(){
    const alive=this.s.teams.filter(t=>t.registered&&!t.eliminated);
    if(alive.length<=1){ this.s.round_state=3;
      if(alive[0]) this.push(alive[0].name+' wins the round'); return; }
    let i=alive.findIndex(t=>t.id===this.s.current_turn);
    this.s.current_turn=alive[(i+1)%alive.length].id;
  },
  step(){
    const s=this.s;
    if(s.round_state===0){
      const nxt=s.teams.find(t=>!t.registered);
      if(nxt){ nxt.registered=true; nxt.name=this.names[nxt.id-1]; this.place(nxt);
        this.push('Team '+nxt.id+' registered as '+nxt.name); }
      else { s.round_state=1; this.push('All teams registered - ready to start'); }
    } else if(s.round_state===1){ this.cmd('START'); return; }
    else if(s.round_state===2){
      const me=s.teams.find(t=>t.id===s.current_turn);
      const foes=s.teams.filter(t=>t.registered&&!t.eliminated&&t.id!==s.current_turn);
      if(!foes.length){ this.next(); this.flush(); return; }
      const foe=foes[(Math.random()*foes.length)|0];
      const open=[]; for(let r=0;r<GRID;r++) for(let c=0;c<GRID;c++) if(foe.grid[r][c]<2) open.push([r,c]);
      if(!open.length){ this.next(); this.flush(); return; }
      /* hunt/target AI: after a hit, probe next to it - keeps the demo moving */
      this.hunt=this.hunt||{};
      let q=(this.hunt[foe.id]||[]).filter(([a,b])=>foe.grid[a][b]<2);
      let r,c;
      if(q.length && Math.random()<0.7){ [r,c]=q.shift(); }
      else { [r,c]=open[(Math.random()*open.length)|0]; }
      this.hunt[foe.id]=q;
      const isHit=foe.grid[r][c]===1;
      foe.grid[r][c]=isHit?3:2;
      const coord=coordOf(r,c);
      if(isHit){ foe.remaining--; this.push(me.name+' hit '+foe.name+' at '+coord);
        [[r-1,c],[r+1,c],[r,c-1],[r,c+1]].forEach(([a,b])=>{
          if(a>=0&&a<GRID&&b>=0&&b<GRID&&foe.grid[a][b]<2) this.hunt[foe.id].push([a,b]); }); }
      else this.push(me.name+' missed '+foe.name+' at '+coord);
      if(foe.remaining<=0&&!foe.eliminated){ foe.eliminated=true; this.push(foe.name+' eliminated'); }
      this.next();
    } else if(s.round_state===3){
      this.phaseTick=(this.phaseTick||0)+1;
      if(this.phaseTick>8){ this.reset(); return; }
    }
    this.flush();
  }
};

window.UI=UI; window.Demo=Demo; window.Sfx=Sfx;
UI.init();
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
