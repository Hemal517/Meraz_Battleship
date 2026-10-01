"""
=====================================================================
 virtual_display.py  -  a fake 2.4" TFT screen in your web browser
=====================================================================

WHY THIS EXISTS
  Until the real touch displays arrive, you can still test the whole
  participant UI. Flash participant_node.ino with
      #define USE_VIRTUAL_DISPLAY
  switched on. The board then sends its draw commands over the USB
  cable instead of to a screen, and this script draws them on a
  240x320 "screen" in your browser. Clicking the screen sends a touch
  back to the board, exactly like a finger on the real panel.

  The real UI code runs unchanged, so what you see here is the real
  screen layout, the real button positions, and the real touch logic.

HOW TO RUN
  1. pip install flask pyserial
  2. Flash a participant board with USE_VIRTUAL_DISPLAY enabled.
  3. Close the Arduino Serial Monitor (only one program can use the
     USB port at a time).
  4. python virtual_display.py COM6          (your board's port)
        optional second argument = web port, default 5001
  5. Open http://localhost:5001 in your browser.

  Testing several boards at once? Run one copy per board on different
  web ports:   python virtual_display.py COM6 5001
                python virtual_display.py COM7 5002

WHAT IT DOES NOT TEST
  Wiring, SPI speed, backlight, and touch calibration - those only
  show up on the real display. Text shapes are also approximate.

HOW IT WORKS (the wire format; same text is documented in VirtualTFT.h)
  Board -> laptop, one line per draw call, each starting with "@TFT ":
     @TFT INIT / F color / R x y w h color / r x y w h color
     @TFT Q x y w h radius color / q x y w h radius color
     @TFT T x y size fg bg text      (bg = -1 means transparent)
  Laptop -> board:
     @TOUCH x y      a tap at pixel (x, y)
     @REDRAW         "repaint the screen you're showing"
  Any other line the board prints is shown in the log panel.
=====================================================================
"""

import collections
import json
import queue
import sys
import threading
import time

import serial                                   # pip install pyserial
from flask import Flask, Response, jsonify, request   # pip install flask

# =====================================================================
# CONFIGURATION  (you can also pass the port as the first argument)
# =====================================================================
SERIAL_PORT = "COM7"      # the participant board's USB port
BAUD_RATE = 115200        # must match Serial.begin() in the sketch
WEB_PORT = 5001           # 5000 is taken by dashboard.py
SCREEN_W, SCREEN_H = 240, 320
DISPLAY_SCALE = 2         # browser pixels per screen pixel
LOG_LINES_KEPT = 300

if len(sys.argv) > 1:
    SERIAL_PORT = sys.argv[1]
if len(sys.argv) > 2:
    WEB_PORT = int(sys.argv[2])

# =====================================================================
# SHARED STATE (touched by the serial thread and the web handlers)
# =====================================================================
state_lock = threading.Lock()
frame = []                                    # draw commands since the last full-screen clear
log = collections.deque(maxlen=LOG_LINES_KEPT)   # recent non-draw lines from the board
status = {"connected": False, "port": SERIAL_PORT, "error": ""}
subscribers = []                              # one queue per open browser tab
serial_port = None                            # the open pyserial object (or None)
write_lock = threading.Lock()                 # only one writer to the port at a time

# How many numeric arguments each draw command carries (T is handled separately)
NUMERIC_ARGS = {"F": 1, "R": 5, "r": 5, "Q": 6, "q": 6}


def broadcast(event):
    """Send one event to every open browser tab (drops it if a tab is too slow)."""
    for q in list(subscribers):
        try:
            q.put_nowait(event)
        except queue.Full:
            pass


def parse_draw(rest):
    """Turn the text after '@TFT ' into a command dict, or None if malformed."""
    op, _, args = rest.partition(" ")
    try:
        if op == "INIT":
            return {"op": "INIT"}
        if op == "T":
            parts = args.split(" ", 5)          # x y size fg bg text(with spaces)
            if len(parts) < 6:
                return None
            x, y, size, fg, bg = (int(v) for v in parts[:5])
            return {"op": "T", "x": x, "y": y, "s": size, "fg": fg, "bg": bg, "text": parts[5]}
        if op in NUMERIC_ARGS:
            nums = [int(v) for v in args.split()]
            if len(nums) != NUMERIC_ARGS[op]:
                return None
            if op == "F":
                return {"op": "F", "c": nums[0]}
            if op in ("R", "r"):
                return {"op": op, "x": nums[0], "y": nums[1], "w": nums[2], "h": nums[3], "c": nums[4]}
            return {"op": op, "x": nums[0], "y": nums[1], "w": nums[2], "h": nums[3],
                    "r": nums[4], "c": nums[5]}
    except ValueError:
        return None          # a log line interleaved into a draw line, etc.
    return None


def handle_line(line):
    """Process one line received from the board."""
    line = line.strip()
    if not line:
        return
    if line.startswith("@TFT "):
        cmd = parse_draw(line[5:])
        if cmd is None:
            return                                   # silently drop corrupted draw lines
        with state_lock:
            if cmd["op"] in ("INIT", "F"):
                frame.clear()                        # a full clear starts a fresh frame
            if cmd["op"] != "INIT":
                frame.append(cmd)
        broadcast({"k": "draw", "c": cmd})
    else:
        with state_lock:
            log.append(line)
        broadcast({"k": "log", "t": line})


def send_to_board(text):
    """Write one line to the board. Returns (ok, message)."""
    with write_lock:
        if serial_port is None:
            return False, "board not connected"
        try:
            serial_port.write((text + "\n").encode("utf-8"))
            return True, "sent"
        except (serial.SerialException, OSError) as exc:
            return False, str(exc)


def serial_loop():
    """Background thread: connect to the board, read lines forever, reconnect on error."""
    global serial_port
    while True:
        try:
            port = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=0.2)
        except (serial.SerialException, OSError) as exc:
            with state_lock:
                status.update(connected=False, error=str(exc))
            broadcast({"k": "status", "s": dict(status)})
            time.sleep(2)
            continue

        serial_port = port
        with state_lock:
            status.update(connected=True, error="")
        broadcast({"k": "status", "s": dict(status)})
        time.sleep(0.5)
        send_to_board("@REDRAW")          # ask the board to repaint if it's already running

        buf = b""
        try:
            while True:
                chunk = port.read(256)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    handle_line(raw.decode("utf-8", errors="replace"))
        except (serial.SerialException, OSError) as exc:
            with state_lock:
                status.update(connected=False, error=str(exc))
            broadcast({"k": "status", "s": dict(status)})
        finally:
            serial_port = None
            try:
                port.close()
            except Exception:
                pass
            time.sleep(1)


# =====================================================================
# WEB SERVER
# =====================================================================
app = Flask(__name__)


@app.route("/")
def index():
    html = PAGE.replace("__SCALE__", str(DISPLAY_SCALE)).replace("__W__", str(SCREEN_W)) \
               .replace("__H__", str(SCREEN_H))
    return Response(html, mimetype="text/html")


@app.route("/stream")
def stream():
    """Server-sent events: first the current screen, then live updates."""
    q = queue.Queue(maxsize=2000)

    def gen():
        with state_lock:
            snapshot = {"k": "snapshot", "frame": list(frame), "log": list(log), "s": dict(status)}
            subscribers.append(q)
        try:
            yield "data: " + json.dumps(snapshot) + "\n\n"
            while True:
                try:
                    yield "data: " + json.dumps(q.get(timeout=15)) + "\n\n"
                except queue.Empty:
                    yield ": keep-alive\n\n"
        finally:
            if q in subscribers:
                subscribers.remove(q)

    return Response(gen(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.route("/api/state")
def api_state():
    """Plain-JSON snapshot (handy for debugging and tests)."""
    with state_lock:
        return jsonify({"frame": list(frame), "log": list(log), "status": dict(status)})


@app.route("/touch", methods=["POST"])
def touch():
    """The browser reports a click; forward it to the board as a touch."""
    data = request.get_json(silent=True) or {}
    try:
        x, y = int(data["x"]), int(data["y"])
    except (KeyError, TypeError, ValueError):
        return jsonify(ok=False, message="need integer x and y"), 400
    if not (0 <= x < SCREEN_W and 0 <= y < SCREEN_H):
        return jsonify(ok=False, message="outside the screen"), 400
    ok, msg = send_to_board("@TOUCH %d %d" % (x, y))
    return jsonify(ok=ok, message=msg), (200 if ok else 503)


@app.route("/cmd", methods=["POST"])
def cmd():
    """Send a typed Serial command (CONFIG, STATUS, '2 3 4', ...) to the board."""
    text = ((request.get_json(silent=True) or {}).get("text") or "").strip()
    if not text or len(text) > 80 or "\n" in text:
        return jsonify(ok=False, message="empty or too long"), 400
    ok, msg = send_to_board(text)
    return jsonify(ok=ok, message=msg), (200 if ok else 503)


@app.route("/redraw", methods=["POST"])
def redraw():
    ok, msg = send_to_board("@REDRAW")
    return jsonify(ok=ok, message=msg), (200 if ok else 503)


# =====================================================================
# BROWSER PAGE
# The "renderer" script is kept separate and self-contained so it can
# be tested on its own (see the test notes in the README).
# =====================================================================
PAGE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Virtual TFT</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { margin:0; background:#0b1220; color:#cfe3ff; font-family:Segoe UI, Arial, sans-serif; }
  .wrap { display:flex; gap:28px; padding:24px; flex-wrap:wrap; justify-content:center; }
  .bezel { background:#1a1f2b; border-radius:18px; padding:16px; box-shadow:0 8px 30px #000a; }
  .screen { position:relative; width:calc(__W__px * __SCALE__); height:calc(__H__px * __SCALE__); }
  canvas { position:absolute; left:0; top:0; width:100%; height:100%; image-rendering:pixelated; }
  #ov { pointer-events:none; }
  #scr { cursor:pointer; background:#000; }
  .side { width:420px; max-width:100%; }
  h1 { font-size:18px; margin:0 0 6px; color:#7fd4ff; }
  .chip { display:inline-block; padding:3px 10px; border-radius:99px; font-size:12px; margin-bottom:10px; }
  .ok { background:#0d4d2b; color:#7dffb0; } .bad { background:#5a1620; color:#ff9aa8; }
  #log { background:#070b14; border:1px solid #1d2a44; border-radius:8px; height:300px; overflow:auto;
         padding:8px; font:12px/1.45 Consolas, Menlo, monospace; white-space:pre-wrap; }
  .row { display:flex; gap:6px; margin-top:8px; flex-wrap:wrap; }
  input { flex:1; min-width:140px; background:#0f1626; color:#cfe3ff; border:1px solid #27385c;
          border-radius:6px; padding:7px; font:13px Consolas, monospace; }
  button { background:#17315c; color:#cfe3ff; border:1px solid #2b4a82; border-radius:6px;
           padding:7px 11px; cursor:pointer; } button:hover { background:#1f4380; }
  p.hint { font-size:12px; color:#8aa3c8; margin:8px 0 0; }
</style>
</head>
<body>
<div class="wrap">
  <div class="bezel"><div class="screen">
    <canvas id="scr" width="__W__" height="__H__"></canvas>
    <canvas id="ov" width="__W__" height="__H__"></canvas>
  </div></div>
  <div class="side">
    <h1>Virtual TFT &mdash; participant board</h1>
    <span id="chip" class="chip bad">connecting&hellip;</span>
    <div id="log"></div>
    <div class="row">
      <input id="cmd" placeholder="Serial command, e.g.  CONFIG   STATUS   2 3 4" maxlength="80">
      <button id="send">Send</button>
    </div>
    <div class="row">
      <button data-c="CONFIG">CONFIG</button><button data-c="STATUS">STATUS</button>
      <button data-c="HELP">HELP</button><button id="redraw">Repaint screen</button>
    </div>
    <p class="hint">Click the screen to tap it. Everything the board prints appears in the log above.</p>
  </div>
</div>

<script id="renderer">
// ---- renderer: turns draw commands into pixels (no page dependencies) ----
function rgb565(c) {
  var r = ((c >> 11) & 31) * 255 / 31, g = ((c >> 5) & 63) * 255 / 63, b = (c & 31) * 255 / 31;
  return "rgb(" + Math.round(r) + "," + Math.round(g) + "," + Math.round(b) + ")";
}
function roundRectPath(ctx, x, y, w, h, r) {
  r = Math.max(0, Math.min(r, w / 2, h / 2));
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}
function applyCmd(ctx, m) {
  switch (m.op) {
    case "F": ctx.fillStyle = rgb565(m.c); ctx.fillRect(0, 0, 240, 320); break;
    case "R": ctx.fillStyle = rgb565(m.c); ctx.fillRect(m.x, m.y, m.w, m.h); break;
    case "r": ctx.strokeStyle = rgb565(m.c); ctx.lineWidth = 1;
              ctx.strokeRect(m.x + 0.5, m.y + 0.5, m.w - 1, m.h - 1); break;
    case "Q": ctx.fillStyle = rgb565(m.c); roundRectPath(ctx, m.x, m.y, m.w, m.h, m.r); ctx.fill(); break;
    case "q": ctx.strokeStyle = rgb565(m.c); ctx.lineWidth = 1;
              roundRectPath(ctx, m.x + 0.5, m.y + 0.5, m.w - 1, m.h - 1, m.r); ctx.stroke(); break;
    case "T": {
      var cw = 6 * m.s, ch = 8 * m.s, n = m.text.length;
      if (m.bg >= 0) { ctx.fillStyle = rgb565(m.bg); ctx.fillRect(m.x, m.y, cw * n, ch); }
      ctx.fillStyle = rgb565(m.fg);
      ctx.font = "bold " + (7 * m.s) + "px Consolas, Menlo, monospace";
      ctx.textBaseline = "top"; ctx.textAlign = "center";
      for (var i = 0; i < n; i++) ctx.fillText(m.text[i], m.x + i * cw + cw / 2, m.y);
      break;
    }
  }
}
function renderFrame(ctx, frame) {
  ctx.fillStyle = "#000"; ctx.fillRect(0, 0, 240, 320);
  for (var i = 0; i < frame.length; i++) applyCmd(ctx, frame[i]);
}
</script>

<script>
var scr = document.getElementById("scr"), ctx = scr.getContext("2d");
var ov = document.getElementById("ov").getContext("2d");
var logEl = document.getElementById("log"), chip = document.getElementById("chip");

function addLog(t) {
  logEl.textContent += t + "\n";
  if (logEl.textContent.length > 20000) logEl.textContent = logEl.textContent.slice(-15000);
  logEl.scrollTop = logEl.scrollHeight;
}
function setStatus(s) {
  chip.textContent = s.connected ? "connected to " + s.port : "not connected" + (s.error ? ": " + s.error : "");
  chip.className = "chip " + (s.connected ? "ok" : "bad");
}

var es = new EventSource("/stream");
es.onmessage = function (e) {
  var m = JSON.parse(e.data);
  if (m.k === "snapshot") {
    renderFrame(ctx, m.frame); logEl.textContent = ""; m.log.forEach(addLog); setStatus(m.s);
  } else if (m.k === "draw") {
    if (m.c.op === "INIT") { ctx.fillStyle = "#000"; ctx.fillRect(0, 0, 240, 320); }
    else applyCmd(ctx, m.c);
  } else if (m.k === "log") addLog(m.t);
  else if (m.k === "status") setStatus(m.s);
};

function post(url, body) {
  return fetch(url, { method: "POST", headers: { "Content-Type": "application/json" },
                      body: JSON.stringify(body || {}) }).then(function (r) { return r.json(); });
}

scr.addEventListener("click", function (e) {
  var rect = scr.getBoundingClientRect();
  var x = Math.floor((e.clientX - rect.left) * 240 / rect.width);
  var y = Math.floor((e.clientY - rect.top) * 320 / rect.height);
  ov.clearRect(0, 0, 240, 320);
  ov.strokeStyle = "rgba(255,255,255,0.9)"; ov.lineWidth = 2;
  ov.beginPath(); ov.arc(x, y, 9, 0, 6.3); ov.stroke();
  setTimeout(function () { ov.clearRect(0, 0, 240, 320); }, 250);
  post("/touch", { x: x, y: y }).then(function (r) { if (!r.ok) addLog("[touch failed] " + r.message); });
});

function sendCmd(t) {
  if (!t) return;
  addLog("> " + t);
  post("/cmd", { text: t }).then(function (r) { if (!r.ok) addLog("[send failed] " + r.message); });
}
document.getElementById("send").onclick = function () {
  var i = document.getElementById("cmd"); sendCmd(i.value.trim()); i.value = "";
};
document.getElementById("cmd").addEventListener("keydown", function (e) {
  if (e.key === "Enter") document.getElementById("send").click();
});
Array.prototype.forEach.call(document.querySelectorAll("button[data-c]"), function (b) {
  b.onclick = function () { sendCmd(b.getAttribute("data-c")); };
});
document.getElementById("redraw").onclick = function () { post("/redraw"); };
</script>
</body>
</html>
"""

# =====================================================================
# MAIN
# =====================================================================
if __name__ == "__main__":
    threading.Thread(target=serial_loop, daemon=True).start()
    print("Virtual display for the board on %s  ->  http://localhost:%d" % (SERIAL_PORT, WEB_PORT))
    print("(Press Ctrl+C to stop. If the page says 'not connected', check the port name and")
    print(" that the Arduino Serial Monitor is closed.)")
    app.run(host="0.0.0.0", port=WEB_PORT, threaded=True, debug=False)
