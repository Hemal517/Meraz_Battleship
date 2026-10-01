# MERAZ BATTLESHIP — dashboard, in plain terms

## The four files

| File | Need it on event day? | What it is |
|---|---|---|
| **`dashboard.py`** | **YES — this is the one you run** | Your original backend (Serial reader, Flask routes, match logs) with the new UI embedded inside it. One file = the whole dashboard. |
| `meraz_dashboard_v2.html` | No | The same UI as a standalone page. Double-click it and it runs a fake demo match. Good for showing teammates / testing on the projector with no laptop setup. |
| `simulate.py` | No (dev only) | A **fake Central node**. Runs the real UI against a pretend game so you can rehearse without any ESP32 plugged in. |
| `UI_NOTES.md` | No | Write-up of what changed vs the old UI and why. |

Nothing else is needed. `match_logs/` gets created automatically next to `dashboard.py`.

---

## Event day — 4 steps

**1. Install the two libraries (once, with internet):**
```
pip install pyserial flask
```

**2. Find the Central ESP32's port and put it in `dashboard.py` line 66:**

- **Windows:** Device Manager → *Ports (COM & LPT)* → e.g. `COM5`
  → `SERIAL_PORT = "COM5"`
- **Linux:** `ls /dev/ttyUSB*` → `SERIAL_PORT = "/dev/ttyUSB0"`
- **Mac:** `ls /dev/cu.*` → `SERIAL_PORT = "/dev/cu.usbserial-0001"`

> **Close the Arduino IDE Serial Monitor first.** Only one program can hold the
> port. If the Monitor is open you get `could not open port` / `Access is denied`.

**3. Run it:**
```
python dashboard.py
```
You should see `Connected to Central on COM5` and `Match log for this round: ...`.

**4. Open `http://localhost:5000`, press `F` for fullscreen, project that tab.**

Top-left chip must read **CENTRAL ONLINE** + **UPDATED JUST NOW**. If it says
`CENTRAL OFFLINE`, the ESP32 isn't on that port. If it says `DASHBOARD OFFLINE`,
the Python script died — restart it, the page reconnects by itself.

---

## Rehearsing with no hardware

```
python simulate.py      →  http://localhost:5000
```
A fake match registers 4 teams, fires shots, eliminates teams and declares a
winner on a loop. All buttons work. **Stop it before running `dashboard.py`** —
both use port 5000.

Even simpler: just double-click `meraz_dashboard_v2.html`.

> Demo data can never appear by accident during the real match: it only runs from
> a `file://` page or with `?demo=1` in the URL, never when `dashboard.py` is serving.

---

## Keys for whoever drives the laptop

| Key | Action |
|---|---|
| `S` | Start round |
| `G` | Force start (fewer than 4 teams showed up) |
| `K` | Skip current turn |
| `R` | Reset round (asks first, starts a new log file) |
| `F` | Fullscreen — do this before projecting |
| `L` | Layout: 2×2 grids ↔ 1×4 row |
| `M` | Mute / unmute sound effects |
| `P` | Performance mode (kill effects if the laptop lags) |
| `V` | **Debug only** — reveal ship positions. Never leave this on while projecting; a loud `SHIPS VISIBLE` chip warns you. |
| `?` | Show this shortcut list |
| `Esc` | Close overlays |

The buttons in the top-right do the same things, so a mouse works fine too.

---

## Things you might want to tweak

| What | Where |
|---|---|
| Serial port / baud | `dashboard.py` lines 66–67 |
| Web port (if 5000 is taken) | `dashboard.py` line 69 |
| Turn clock amber/red thresholds (60 s / 90 s) | `dashboard.py` lines 909–910 |
| Team colours | `dashboard.py`, search `--t1:` (CSS) and `const COLORS` (JS) |
| The whole UI | `dashboard.py` lines 322–1520 — everything between `DASHBOARD_HTML = r"""` and the closing `"""` |

To put this UI into a `dashboard.py` you've already edited, copy the contents of
`meraz_dashboard_v2.html` and paste it between those triple quotes. Nothing above
line 322 needs to change — the backend and `central_node.ino` are untouched.

---

## If something goes wrong mid-event

| Symptom | Cause / fix |
|---|---|
| `CENTRAL OFFLINE` | Wrong port, cable unplugged, or Serial Monitor is open. Fix and it reconnects on its own — no restart needed. |
| `DASHBOARD OFFLINE` | The Python script crashed/closed. Run it again; the browser tab recovers itself. |
| Page frozen but chips look fine | Check the `LAST UPDATE …s AGO` chip — over ~8 s means no data is arriving. |
| Grids lag the log by a second or two | Expected: events stream instantly, full grids arrive on the 2 s `GET_STATE` poll. |
| Laggy animation on the projector | Press `P`. |
| Match history | `match_logs/round_N_<timestamp>.txt`, a new file each RESET. Survives crashes and browser closes. |
