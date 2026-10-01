# Meraz Battleship: Participant Guide

Your ESP32 has to join a live four-team naval battle run by a **Central node**. The Central node is the referee: it knows every fleet, checks every move, and decides who hits what.

Your job is to make your board talk to it. **How you set up the radio is up to you.** This guide only tells you what Central expects to hear.

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

- The grid is **5 x 5**. `x` is the column (0 to 4, left to right) and `y` is the row (0 to 4, top to bottom).
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

A size-5 diagonal only fits corner to corner: `(0,0)` with direction 2, or `(0,4)` with direction 3.

Your fleet is secret. Nobody but Central sees it, so choose your layout however you like, whether fixed, random, or computed.

---

## 3. Message formats

Every field is a single unsigned byte (`uint8_t`) unless noted. There is **no padding and no header**. Messages are **plain packed bytes**, and Central tells them apart **by their length**, so send exactly the byte counts below. Anything else is ignored.

### You send to Central

**Registration** (33 bytes)

| Bytes | Field | Notes |
|---|---|---|
| 0 | `team_id` | Your team ID |
| 1 to 20 | `team_name` | 20 bytes of text, padded with zeros. Name must be non-empty |
| 21 to 24 | ship 1 | `ship_len, start_x, start_y, orientation` |
| 25 to 28 | ship 2 | same layout |
| 29 to 32 | ship 3 | same layout |

**Attack** (4 bytes)

| Byte | Field |
|---|---|
| 0 | `attacker_id` (your team ID) |
| 1 | `target_id` (the team you're shooting at) |
| 2 | `x` |
| 3 | `y` |

### Central sends to you

**Registration reply** (1 byte): `status`

| Value | Meaning |
|---|---|
| 0 | Accepted |
| 1 | Rejected (bad layout, bad name, unknown board, or the game already started) |
| 2 | Reconnected (you were already registered and your layout matched) |
| 3 | Reconnect rejected (your layout doesn't match what Central stored) |

A rejection doesn't say why. Ask the organizer, who can see the reason on Central.

**Attack reply** (4 bytes): `target_id, x, y, result_code`

| `result_code` | Meaning |
|---|---|
| 0 | Miss |
| 1 | Hit |
| 2 | Invalid (see below) |
| 3 | Hit, and that **sank the whole team** (they're eliminated) |

**Turn update** (2 bytes): `current_turn, round_state`

Sent to **every team after every valid move** (and when a team registers). Invalid attacks don't trigger one, because nothing changed. `current_turn` is the team ID whose turn it is (0 means nobody).

| `round_state` | Meaning |
|---|---|
| 0 | Setup (waiting for teams) |
| 1 | Ready (all registered, waiting for the organizer to start) |
| 2 | Running |
| 3 | Game over |

### Example bytes

Team 1 named `TEAM 1`, with a size-1 ship at (4,0) going right, a size-3 ship at (0,4) going right, and a size-5 diagonal from (0,0) going down-right:

```
Registration (33 bytes):
01 54 45 41 4D 20 31 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 04 00 00 03 00 04 00 05 00 00 02

Attack, team 1 shoots team 2 at (3,4):   01 02 03 04
Attack reply, a hit on (3,4) of team 2:  02 03 04 01
Turn update, it is team 2's turn, running: 02 02
```

---

## 4. How a game goes

1. **Register** before the match starts. Keep retrying until you get reply `0` (or `2`).
2. The organizer starts the match. You'll see a turn update with `round_state = 2`.
3. **Only shoot when `current_turn` is your team ID.** On your turn, send an attack.
4. Read the attack reply. **You only learn about your own shots.** You won't hear about what other teams hit, so keep your own notes.
5. After each valid move, everyone gets a new turn update. The turn passes to the next team that is still alive.
6. Lose all your ships and you're eliminated. **The last fleet standing wins.**

**Invalid attacks** (`result_code = 2`) come back when you shoot out of turn, at yourself, at an eliminated or absent team, outside the grid, or at a cell you've **already shot at on that team**. An invalid attack **doesn't use up your turn**, so you can correct it and try again.

If your board restarts mid-game, register again with the **same layout** and Central will reconnect you (reply `2`).

---

## 5. If nothing works

- Is the **channel** `1`, the same as Central's?
- Did you **add Central as a peer** before sending to it?
- Is `centralMac` exactly right? One wrong byte means silence.
- Did you give the organizer **your** MAC?
- Are your messages **exactly** 33 or 4 bytes? Check `sizeof()`.
- Does your callback signature match your **ESP32 core version** (2.x vs 3.x)? They differ.
- Print everything you send and receive on Serial.

Good luck, and may the last fleet standing win!
