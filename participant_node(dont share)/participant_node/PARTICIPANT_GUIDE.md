# Meraz Battleship: Participant Guide

Your ESP32 has to join a live four-team naval battle run by a **Central node**. The Central node is the referee: it knows every fleet, checks every move, and decides who hits what.

Your job is to make your board talk to it. **How you set up the radio is up to you.** This guide only tells you what Central expects to hear.

New in this version: before you register, you **choose a loadout** of special powers (section 3). Your board must lock the loadout in first, and only then connect to Central.

---

## 1. What you need from the organizer

| Item | Notes |
|---|---|
| **Team ID** (1 to 4) | Your board's identity in the match |
| **Central's MAC address** | The address you send to |
| **Wi-Fi channel** | `1` |

You also **give the organizer your board's MAC address** (print it on Serial). Central only accepts boards it already knows, and a board it doesn't recognise is rejected.

Use an ESP32 board. The two sides talk over **ESP-NOW**, so no router or internet is involved.

---

## 2. The grid and your fleet

- The grid is **7 x 7**. `x` is the column (0 to 6, left to right) and `y` is the row (0 to 6, top to bottom).
- You have **exactly three ships, of sizes 1, 3 and 5**.
- A ship is described by its **first cell** (`start_x`, `start_y`), a **length**, and a **direction**. It grows from the first cell in that direction:

| `orientation` | Direction | Example (length 3 from (0,0)) |
|---|---|---|
| `0` | right | (0,0) (1,0) (2,0) |
| `1` | down | (0,0) (0,1) (0,2) |
| `2` | diagonal down-right | (0,0) (1,1) (2,2) |
| `3` | diagonal up-right | starting at (0,2): (0,2) (1,1) (2,0) |

**Your layout is rejected if:**
- a ship runs off the grid (including off the top, for direction `3`),
- two ships overlap (touching is fine),
- the sizes aren't exactly one each of 1, 3 and 5,
- the direction isn't 0 to 3.

A size-5 diagonal needs a 5 x 5 block of free cells: direction 2 can start anywhere from `(0,0)` to `(2,2)`, and direction 3 anywhere from `(0,4)` to `(2,6)`.

Your fleet is secret. Nobody but Central sees it, so choose your layout however you like, whether fixed, random, or computed.

---

## 3. Choose your loadout (before you register)

Every team fights with the same two **compulsory** moves, plus a loadout of **three special powers** it picks itself.

### Always yours (no choice needed)

| Move | What it does | Limit |
|---|---|---|
| **Single Strike** | The normal attack: one cell on one opponent | Unlimited |
| **Shield** | Protects a 3 x 3 block of your own grid (details in section 5) | Unlimited, but **never on two of your turns in a row** |

### Pick TWO attack powers

| # | Power | What it does | Uses |
|---|---|---|---|
| 1 | **Sonar Ping** | Scan a 3 x 3 area of an opponent: you learn how many ship cells are in it | 2 |
| 2 | **Salvo** | Fire at 3 cells in a straight row or column of one opponent | 2 |
| 3 | **Mine** | Hide a trap on one of your own water cells | 2 placements (one on the board at a time) |
| 4 | **Double Attack** | Fire at any 2 cells of one opponent at once | 2 |

### Pick ONE defence power

| # | Power | What it does | Uses |
|---|---|---|---|
| 1 | **Smoke Screen** | Hides every attack on you from the attacker and from the projector until your next turn | 2 |
| 2 | **Repair** | Flip one of your hit cells back to intact | 2 |

### How the loadout is sent: one byte

The loadout is **one byte**. Each power has a bit; set the bit of every power you chose:

| Bit value | Power | Pick from |
|---|---|---|
| `0x01` | Sonar Ping | attack |
| `0x02` | Salvo | attack |
| `0x04` | Mine | attack |
| `0x40` | Double Attack | attack |
| `0x08` | Repair | defence |
| `0x10` | Smoke Screen | defence |
| `0x20` | Shield | always yours (send it set or clear, it makes no difference) |

(The bit for a power is `1 << (power_id - 1)`. The power ids are Sonar 1, Salvo 2, Mine 3, Repair 4, Smoke 5, Shield 6, Double 7. You will need the ids again in section 4.)

Examples:

| Your choice | Byte |
|---|---|
| Sonar + Double Attack + Smoke Screen (+ Shield) | `0x01 + 0x40 + 0x10 + 0x20 = 0x71` |
| Salvo + Mine + Repair (+ Shield) | `0x02 + 0x04 + 0x08 + 0x20 = 0x2E` |

**A loadout is rejected if** it doesn't contain exactly 2 attack powers and exactly 1 defence power, or if it has any other bit set (for example `0x80`).

### Order of operations on your board

1. **Let the team choose first.** Show the choices (a Serial menu, a touch screen, buttons: your call) and let them pick 2 attack powers and 1 defence power.
2. **Lock the choice in** (a "Confirm" step). After this the loadout cannot change.
3. **Only now** start your radio and send the registration, with the loadout byte at the end.

The loadout is **fixed for the whole match**. If your board restarts mid-game, register again with the **same layout and the same loadout**, or Central will refuse the reconnection.

`meraz_loadout.h` is a ready-made helper (a Serial menu plus a validity check) so you don't have to build steps 1 and 2 yourself:

```cpp
#include "meraz_loadout.h"

uint8_t myLoadout;

void setup() {
  Serial.begin(115200);
  myLoadout = selectLoadoutSerial();   // blocks until the team has locked a legal loadout
  // ...only now set up ESP-NOW and send the registration, with myLoadout as its last byte
}
```

---

## 4. Message formats

Every field is a single unsigned byte (`uint8_t`) unless noted. There is **no padding and no header**. Messages are **plain packed bytes**, and Central tells them apart **by their length**, so send exactly the byte counts below. Anything else is ignored.

### You send to Central

**Registration** (34 bytes)

| Bytes | Field | Notes |
|---|---|---|
| 0 | `team_id` | Your team ID |
| 1 to 20 | `team_name` | 20 bytes of text, padded with zeros. Name must be non-empty |
| 21 to 24 | ship 1 | `ship_len, start_x, start_y, orientation` |
| 25 to 28 | ship 2 | same layout |
| 29 to 32 | ship 3 | same layout |
| 33 | `loadout` | The loadout byte from section 3 |

(The old 33-byte registration, without the loadout byte, is no longer accepted.)

**Attack** (4 bytes), the Single Strike

| Byte | Field |
|---|---|
| 0 | `attacker_id` (your team ID) |
| 1 | `target_id` (the team you're shooting at) |
| 2 | `x` |
| 3 | `y` |

**Power-up** (9 bytes): use one of your powers, or Shield

| Byte | Field | Notes |
|---|---|---|
| 0 | `power` | Power id: Sonar 1, Salvo 2, Mine 3, Repair 4, Smoke 5, Shield 6, Double 7 |
| 1 | `team_id` | Your team ID |
| 2 | `target_id` | The opponent, for Sonar, Salvo and Double. `0` for everything else |
| 3, 4 | `x1`, `y1` | Meaning depends on the power (next table) |
| 5, 6 | `x2`, `y2` | Salvo and Double only |
| 7, 8 | `x3`, `y3` | Salvo only |

Send `0` in every field a power doesn't use.

| Power | `target_id` | Cells |
|---|---|---|
| 1 Sonar | opponent | `(x1,y1)` is the **centre** of the 3 x 3 area (any cell; the area is clipped at the grid edge) |
| 2 Salvo | opponent | `(x1,y1)`, `(x2,y2)`, `(x3,y3)`: three **consecutive** cells in one row or one column, in any order |
| 3 Mine | `0` | `(x1,y1)` is **your own** cell: untouched water (not a ship cell, not a cell already shot at) |
| 4 Repair | `0` | `(x1,y1)` is **your own** cell that has been hit |
| 5 Smoke | `0` | none |
| 6 Shield | `0` | `(x1,y1)` is the **centre** of the 3 x 3 block, so `x1` and `y1` must each be `1` to `5` |
| 7 Double | opponent | `(x1,y1)` and `(x2,y2)`: two **different** cells |

### Central sends to you

**Registration reply** (1 byte): `status`

| Value | Meaning |
|---|---|
| 0 | Accepted |
| 1 | Rejected (bad layout, bad name, **illegal loadout**, unknown board, or the game already started) |
| 2 | Reconnected (you were already registered and your layout **and loadout** matched) |
| 3 | Reconnect rejected (your layout or loadout doesn't match what Central stored) |

A rejection doesn't say why. Ask the organizer, who can see the reason on Central.

**Attack reply** (4 bytes): `target_id, x, y, result_code`

| `result_code` | Meaning |
|---|---|
| 0 | Miss |
| 1 | Hit |
| 2 | Invalid (see section 6) |
| 3 | Hit, and that **sank the whole team** (they're eliminated) |
| 4 | **Mine!** The cell held a mine. It counts as a miss, the mine is gone, and **you lose your next turn** |
| 5 | **Blocked** by a shield. Nothing happened and the cell is **not** marked as shot, but your turn is used |
| 6 | **Unknown.** The target is hiding behind a smoke screen, so you aren't told whether it was a hit or a miss. The cell **does** count as shot |

**Power-up reply** (7 bytes): `power, status, target_id, count, result1, result2, result3`

| Field | Meaning |
|---|---|
| `power` | The power you asked for |
| `status` | `0` accepted, `1` rejected, `2` jammed (a Sonar Ping blocked by smoke: the use and your turn are still spent) |
| `target_id` | The target you sent |
| `count` | Sonar only: how many **ship cells** are in the area (cells already hit still count) |
| `result1` to `result3` | Salvo and Double only: the result of each cell, in the order you sent them, using the same codes as an attack reply (`0, 1, 3, 4, 5, 6`). `2` means that cell **wasn't fired** (an earlier cell had already eliminated the target). For every other power these three bytes are `2`; ignore them |

Like registration, a rejected power-up doesn't say why.

**Turn update** (2 bytes): `current_turn, round_state`

Sent to **every team after every valid move**, including every power-up, and when a team registers. Invalid moves don't trigger one, because nothing changed. `current_turn` is the team ID whose turn it is (0 means nobody).

| `round_state` | Meaning |
|---|---|
| 0 | Setup (waiting for teams) |
| 1 | Ready (all registered, waiting for the organizer to start) |
| 2 | Running |
| 3 | Game over |

All of Central's replies have different lengths (1, 2, 4 and 7 bytes), so you can tell them apart by length.

---

## 5. The powers in detail

**Every power-up uses your turn**, exactly like a shot. A request that is rejected uses neither your turn nor one of your uses.

### Sonar Ping (2 uses)
Pick an opponent and a centre cell. Central counts the ship cells in the 3 x 3 area around it and tells **only you**. The projector only shows that you used Sonar. If the target is behind a smoke screen the ping is **jammed**: you learn nothing and the use is still spent.

### Salvo (2 uses)
Three consecutive cells in one row or one column of one opponent, for example `(2,3) (3,3) (4,3)` or `(5,0) (5,1) (5,2)`. Diagonals and gaps are rejected. If **any** of the three cells was already shot at, the whole Salvo is rejected and nothing is spent. Each cell is resolved like a Single Strike (hit, miss, mine, blocked, unknown); if an earlier cell eliminates the team, the rest aren't fired.

### Mine (2 placements, one on the board at a time)
- You place it **during the game, on your own turn**, and placing it uses the turn.
- It goes on one of your own cells that is **untouched water** (not a ship cell, and not a cell an opponent has already shot at).
- **Only you know where it is.** Central never tells you again, so keep your own notes. The projector says you armed a mine, never where.
- It stays on the board **until someone shoots that cell**. Then the shooter gets result `4`, the mine is gone, and the shooter loses their next turn.
- You may only have **one mine on the board at a time**. After it has been triggered you can place your second one. You can't move a mine.

### Double Attack (2 uses)
Two different cells of one opponent, fired together. If either cell was already shot at, the whole action is rejected and nothing is spent.

### Smoke Screen (2 uses)
From the moment you deploy it until **your next turn begins**, every shot at you (Single Strike, Salvo or Double) is answered with result `6` and is **hidden from the projector too**, and any Sonar Ping at you is jammed. The shots still happen: when the smoke clears, the real result appears on the projector. The smoke ends at your next turn **even if nobody shot at you**. It can't hide an elimination: a shot that sinks your last ship is reported normally and lifts the smoke.

### Repair (2 uses)
Flip one of your own **hit** cells back to an intact ship cell (your remaining ship cells go up by one). If the cell isn't a hit, the request is rejected.

### Shield (unlimited, never on two turns in a row)
- You choose the **centre** of a 3 x 3 block of your own grid, so the centre must be at `1` to `5` in both x and y.
- Until **your next turn begins**, any shot at a cell inside the block is **blocked** (result `5`): nothing is revealed, the cell is not marked, and the attacker's turn is used.
- **You can't Shield on two of your turns in a row.** After a Shield, your next turn must be something else (a Single Strike or any other power); the turn after that, you may Shield again. A turn you lose to a mine counts as that next turn.
- Where your shield is stays secret. The projector only shows that a shield is up.

---

## 6. How a game goes

1. **Choose and lock in your loadout**, then **register** before the match starts. Keep retrying until you get reply `0` (or `2`).
2. The organizer starts the match. You'll see a turn update with `round_state = 2`.
3. **Only act when `current_turn` is your team ID.** On your turn, do exactly one of: a Single Strike, a power from your loadout, or a Shield.
4. Read the reply. **You only learn about your own moves.** You won't hear about what other teams hit, so keep your own notes (including where your mine is).
5. After each valid move, everyone gets a new turn update. The turn passes to the next team that is still alive.
6. Lose all your ships and you're eliminated. **The last fleet standing wins.**

**Invalid moves** don't use up your turn, so you can correct them and try again. An attack comes back with `result_code = 2`; a power-up comes back with `status = 1`. That happens when you:
- act out of turn, or while the game isn't running,
- shoot at yourself, at an eliminated or absent team, or outside the grid,
- shoot at a cell you've **already shot at on that team**,
- use a power that **isn't in your loadout**, or one you've **used up**,
- break a power's own rules (a bent Salvo, a mine on a ship cell, a second mine while one is active, a Shield on two turns in a row, a Repair on a cell that isn't hit...).

If your board restarts mid-game, register again with the **same layout and loadout** and Central will reconnect you (reply `2`). Central remembers how many uses you have left.

---

## 7. Example bytes

Team 1 named `TEAM 1`, with a size-1 ship at (4,0) going right, a size-3 ship at (0,4) going right, a size-5 diagonal from (0,0) going down-right, and the loadout Sonar + Double Attack + Smoke Screen (`0x71`):

```
Registration (34 bytes):
01 54 45 41 4D 20 31 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 04 00 00 03 00 04 00 05 00 00 02 71

Attack, team 1 shoots team 2 at (3,4):    01 02 03 04
Attack reply, a hit on (3,4) of team 2:   02 03 04 01
Turn update, it is team 2's turn, running: 02 02

Power-up, Sonar Ping on team 2, centre (3,4):          01 01 02 03 04 00 00 00 00
Power-up reply, accepted, 2 ship cells in the area:    01 00 02 02 02 02 02

Power-up, Double Attack on team 2 at (0,3) and (6,6):  07 01 02 00 03 06 06 00 00
Power-up reply, first cell a hit, second a miss:       07 00 02 00 01 00 02

Power-up, Shield centred on (3,3):                     06 01 00 03 03 00 00 00 00
Power-up reply, accepted:                              06 00 00 00 02 02 02

Power-up, Mine on your own cell (5,6):                 03 01 00 05 06 00 00 00 00
```

---

## 8. If nothing works

- Is the **channel** `1`, the same as Central's?
- Did you **add Central as a peer** before sending to it?
- Is `centralMac` exactly right? One wrong byte means silence.
- Did you give the organizer **your** MAC?
- Are your messages **exactly** 34 (registration), 4 (attack) or 9 (power-up) bytes? Check `sizeof()`. A 33-byte registration is the old format and is ignored.
- Registration rejected? Is your loadout legal (exactly 2 attack powers, exactly 1 defence power, no stray bits)? Is it the **same loadout** you used before, if you are reconnecting?
- Power-up rejected (`status = 1`)? Check, in this order: is it your turn, is the power in your loadout, do you have uses left, are the cells and the target allowed, and (for Shield) did you Shield on your last turn?
- Does your callback signature match your **ESP32 core version** (2.x vs 3.x)? They differ.
- Print everything you send and receive on Serial.

Good luck, and may the last fleet standing win!
