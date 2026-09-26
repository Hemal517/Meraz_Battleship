"""
MERAZ BATTLESHIP - OFFLINE REHEARSAL SERVER  (optional, dev-only)
=================================================================
    python simulate.py      ->  http://localhost:5000

Runs the REAL dashboard UI against a FAKE Central node, so you can
rehearse the projection, test the buttons and tune the look before the
ESP32s are wired up. It re-uses the exact DASHBOARD_HTML string that
lives inside dashboard.py (read out of the file, not copy-pasted), and
exposes the same three endpoints with the same message shapes:

    GET  /api/state    GET /events (SSE)    POST /api/command

Nothing in dashboard.py is imported or modified - this file never
touches pyserial, so it runs fine on a laptop with no hardware.
Delete it before the event if you like; it is not needed to run the
real dashboard.
"""

import json
import os
import queue
import random
import re
import threading
import time

from flask import Flask, Response, jsonify, request

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- pull the UI straight out of dashboard.py (single source of truth) ----
with open(os.path.join(HERE, "dashboard.py"), encoding="utf-8") as f:
    _src = f.read()
DASHBOARD_HTML = re.search(r'DASHBOARD_HTML = r"""(.*?)"""\n', _src, re.S).group(1)

TEAM_NAMES = ["CIRCUIT BREAKERS", "OHM RAIDERS", "FLUX CAPACITORS", "NULL POINTERS"]
# cells are named R<row> C<col> - both axes are numbered in the UI
SHIPS = 9

app = Flask(__name__)
lock = threading.Lock()
subs = []
subs_lock = threading.Lock()


def blank_team(i):
    return {
        "id": i,
        "name": "",
        "registered": False,
        "eliminated": False,
        "remaining": SHIPS,
        "grid": [[0] * 5 for _ in range(5)],
    }


state = {
    "type": "state",
    "round_state": 0,
    "current_turn": 0,
    "teams": [blank_team(i) for i in range(1, 5)],
    "log": [],
    "central_connected": True,
}
hunt = {1: [], 2: [], 3: [], 4: []}


def broadcast(msg):
    with subs_lock:
        for q in subs:
            q.put(msg)


def event(text):
    with lock:
        state["log"].append(text)
        state["log"] = state["log"][-20:]
    broadcast({"type": "event", "text": text})
    print("EVENT:", text)


def push_state():
    with lock:
        broadcast(json.loads(json.dumps(state)))


def place_ships(t):
    n = 0
    while n < SHIPS:
        r, c = random.randrange(5), random.randrange(5)
        if t["grid"][r][c] == 0:
            t["grid"][r][c] = 1
            n += 1


def reset_game():
    with lock:
        state["round_state"] = 0
        state["current_turn"] = 0
        state["teams"] = [blank_team(i) for i in range(1, 5)]
        state["log"] = []
        for k in hunt:
            hunt[k] = []
    event("Game reset - waiting for teams to register")
    push_state()


def alive_teams():
    return [t for t in state["teams"] if t["registered"] and not t["eliminated"]]


def next_turn():
    a = alive_teams()
    if len(a) <= 1:
        state["round_state"] = 3
        if a:
            event(f"{a[0]['name']} wins the round")
        return
    ids = [t["id"] for t in a]
    try:
        i = ids.index(state["current_turn"])
    except ValueError:
        i = -1
    state["current_turn"] = ids[(i + 1) % len(ids)]


def start_round(force=False):
    with lock:
        reg = [t for t in state["teams"] if t["registered"]]
        if state["round_state"] >= 2 or len(reg) < 2:
            return
        state["round_state"] = 2
        state["current_turn"] = reg[0]["id"]
    event("Round started - " + ("force start" if force else "all teams ready"))
    push_state()


def game_loop():
    """Very rough stand-in for central_node.ino - just enough to drive the UI."""
    time.sleep(1.5)
    reset_game()
    while True:
        time.sleep(1.6)
        with lock:
            rs = state["round_state"]

            if rs == 0:
                nxt = next((t for t in state["teams"] if not t["registered"]), None)
                if nxt:
                    nxt["registered"] = True
                    nxt["name"] = TEAM_NAMES[nxt["id"] - 1]
                    place_ships(nxt)
                    txt = f"Team {nxt['id']} registered as {nxt['name']}"
                else:
                    state["round_state"] = 1
                    txt = "All teams registered - ready to start"

            elif rs == 1:
                state["round_state"] = 2
                state["current_turn"] = state["teams"][0]["id"]
                txt = "Round started"

            elif rs == 2:
                me = next(t for t in state["teams"] if t["id"] == state["current_turn"])
                foes = [t for t in alive_teams() if t["id"] != me["id"]]
                if not foes:
                    next_turn()
                    txt = None
                else:
                    foe = random.choice(foes)
                    q = [(r, c) for (r, c) in hunt[foe["id"]] if foe["grid"][r][c] < 2]
                    openc = [(r, c) for r in range(5) for c in range(5) if foe["grid"][r][c] < 2]
                    if not openc:
                        next_turn()
                        txt = None
                    else:
                        r, c = q.pop(0) if (q and random.random() < 0.7) else random.choice(openc)
                        hunt[foe["id"]] = q
                        hit = foe["grid"][r][c] == 1
                        foe["grid"][r][c] = 3 if hit else 2
                        coord = f"R{r + 1} C{c + 1}"
                        if hit:
                            foe["remaining"] -= 1
                            txt = f"{me['name']} hit {foe['name']} at {coord}"
                            for a, b in ((r - 1, c), (r + 1, c), (r, c - 1), (r, c + 1)):
                                if 0 <= a < 5 and 0 <= b < 5 and foe["grid"][a][b] < 2:
                                    hunt[foe["id"]].append((a, b))
                        else:
                            txt = f"{me['name']} missed {foe['name']} at {coord}"
                        if foe["remaining"] <= 0 and not foe["eliminated"]:
                            foe["eliminated"] = True
                            broadcast({"type": "event", "text": f"{foe['name']} eliminated"})
                            state["log"].append(f"{foe['name']} eliminated")
                        next_turn()
            else:  # game over -> hold, then start a new round
                txt = None
                state["_over"] = state.get("_over", 0) + 1
                if state["_over"] > 8:
                    state["_over"] = 0
                    threading.Thread(target=reset_game, daemon=True).start()

        if txt:
            event(txt)
        push_state()


@app.route("/")
def index():
    return DASHBOARD_HTML


@app.route("/api/state")
def api_state():
    with lock:
        return jsonify(state)


@app.route("/api/command", methods=["POST"])
def api_command():
    data = request.get_json(silent=True) or {}
    cmd = str(data.get("cmd", "")).upper()
    if cmd not in ("START", "FORCE_START", "RESET", "SKIP_TURN", "GET_STATE"):
        return jsonify({"ok": False, "error": "unknown command"}), 400
    print("COMMAND:", cmd)
    if cmd == "RESET":
        reset_game()
    elif cmd == "START":
        start_round()
    elif cmd == "FORCE_START":
        start_round(force=True)
    elif cmd == "SKIP_TURN":
        with lock:
            if state["round_state"] == 2:
                next_turn()
        event("Turn skipped by organizer")
        push_state()
    return jsonify({"ok": True})


@app.route("/events")
def events():
    def stream():
        q = queue.Queue()
        with subs_lock:
            subs.append(q)
        try:
            with lock:
                yield f"data: {json.dumps(state)}\n\n"
            while True:
                yield f"data: {json.dumps(q.get())}\n\n"
        finally:
            with subs_lock:
                if q in subs:
                    subs.remove(q)

    return Response(stream(), mimetype="text/event-stream")


if __name__ == "__main__":
    threading.Thread(target=game_loop, daemon=True).start()
    print("=== REHEARSAL SERVER (fake Central) -> http://localhost:5000 ===")
    app.run(host="0.0.0.0", port=5000, threaded=True, debug=False)
