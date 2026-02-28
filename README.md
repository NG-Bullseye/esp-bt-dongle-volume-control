# esp-bt-dongle-volume-control

ESP8266 (NodeMCU v2) controls the volume of a Bluetooth audio dongle by simulating button presses over GPIO — no transistors needed. Exposed via TCP, integrated into Home Assistant, and callable from Alexa.

---

## Architecture

```
Alexa Voice
    │
    │  "Alexa, aktiviere Beamer Neun"
    ▼
Home Assistant Script (beamer_neun)
    │
    │  shell_command: python3 tv_volume.py SET:9
    ▼
Python TCP Client (tv_volume.py)
    │
    │  TCP :42069  →  "SET:9\n"
    ▼
ESP8266 (192.168.1.50)
    │
    │  SYNC: 16x DOWN → 0, then 10x UP → 9
    ▼
GPIO Open-Drain (D1 / D2)
    │
    │  OUTPUT+LOW = switch closed (GND)
    │  INPUT      = switch open  (high-Z)
    ▼
BT Dongle Volume Buttons
```

---

## Hardware

**NodeMCU v2 (ESP8266)** — no external transistors required.

The GPIO pins act as open-drain switches: setting a pin `OUTPUT+LOW` pulls the button contact to GND (same as pressing the button). Setting it `INPUT` leaves it floating (button released). Both boards must share a common GND.

| Pin | NodeMCU Label | Function |
|-----|---------------|----------|
| GPIO4 | D2 | Volume Down |
| GPIO5 | D1 | Volume Up |
| GPIO2 | D4 | Onboard LED (LOW during SYNC) |

**Wiring:**
- `D2` → Volume Down button contact of BT dongle
- `D1` → Volume Up button contact of BT dongle
- `GND` → GND of BT dongle (shared ground — mandatory)

---

## Network

| Key | Value |
|-----|-------|
| IP | `192.168.1.50` (static) |
| Port | `42069` (TCP) |
| WiFi | IoT 2.4GHz network |

---

## TCP Protocol

Connect, send command + newline, read response, disconnect.

| Command | Description | Response |
|---------|-------------|----------|
| `GET` | Return current tracked volume | `OK volume=9` |
| `UP` | Volume +1 | `OK volume=10` |
| `DOWN` | Volume -1 | `OK volume=8` |
| `SET:0`–`SET:15` | Sync to absolute level | `OK volume=5` |
| `SYNC` | Reset to default (9) | `OK volume=9` |
| `TEST` | Hold DOWN 2s, pause, hold UP 2s — for hardware testing | `OK test done` |

Error responses: `ERR busy`, `ERR already at max (15)`, `ERR already at min (0)`, `ERR range 0-15`, `ERR unknown command`

---

## SYNC Algorithm

The BT dongle has no feedback — the ESP tracks volume in software. To recover from drift, `SET:X` always performs a full resync:

1. Press DOWN **16 times** (MAX_VOLUME + 1) → guaranteed hardware minimum regardless of current position
2. Wait 500ms
3. Press UP **target + 1 times** → hardware ignores the first UP press after reaching minimum, so +1 compensates

Same sequence runs on boot (syncs to `DEFAULT_VOLUME = 9`).

**Timing:**
- Button press held: 100ms
- Pause between presses: 100ms

---

## Build & Flash

PlatformIO project, board `nodemcuv2`.

```bash
pio run -t upload          # build and flash via /dev/ttyUSB0
pio device monitor         # serial monitor at 115200 baud
```

Serial output on boot:
```
Connecting to WiFi....
IP: 192.168.1.50
Boot SYNC...
Boot SYNC done. volume=9
TCP server ready on port 42069
```

---

## Testing via CLI

```bash
# Quick test
echo "GET"   | nc 192.168.1.50 42069
echo "SET:5" | nc 192.168.1.50 42069
echo "SET:9" | nc 192.168.1.50 42069

# Hardware test (holds buttons 2s each — listen/feel for dongle response)
echo "TEST"  | nc 192.168.1.50 42069
```

Or use the Python client directly:

```bash
python3 tv_volume.py GET
python3 tv_volume.py SET:9
python3 tv_volume.py UP
python3 tv_volume.py DOWN
```

---

## Python Client (`tv_volume.py`)

Minimal TCP client used by Home Assistant shell commands:

```python
import socket, sys
HOST, PORT = "192.168.1.50", 42069
cmd = sys.argv[1] if len(sys.argv) > 1 else "GET"
with socket.socket() as s:
    s.settimeout(10)
    s.connect((HOST, PORT))
    s.sendall((cmd + "\n").encode())
    print(s.recv(64).decode().strip())
```

---

## Home Assistant Integration

### shell_command (`configuration.yaml`)

```yaml
shell_command:
  tv_volume_set: "python3 /config/python_scripts/tv_volume.py SET:{{ level }}"
  tv_volume_up:   python3 /config/python_scripts/tv_volume.py UP
  tv_volume_down: python3 /config/python_scripts/tv_volume.py DOWN
  tv_volume_sync: python3 /config/python_scripts/tv_volume.py SYNC
```

### Scripts (`scripts.yaml`)

16 scripts — one per volume level — callable from automations, the AI agent, or Alexa:

```yaml
beamer_null:
  alias: "Beamer Null"
  sequence:
    - action: shell_command.tv_volume_set
      data: {level: 0}

beamer_neun:
  alias: "Beamer Neun"
  sequence:
    - action: shell_command.tv_volume_set
      data: {level: 9}

# ... beamer_eins through beamer_fuenfzehn
```

### Alexa

Scripts are exposed as scenes via the HA Alexa Smart Home integration (requires Nabu Casa or equivalent).

```
"Alexa, aktiviere Beamer Neun"   →  SET:9
"Alexa, aktiviere Beamer Fuenf"  →  SET:5
```

Alternatively, via the HA Conversation Skill:

```
"Alexa, smart house, beamer auf neun"
```

### AI Agent (ha-functions MCP)

The `ha__tv_volume_set` tool is available to the Claude-based conversation agent:

```
User: "Mach den Beamer leiser, auf 6"
Agent: ha__tv_volume_set(level=6)  →  shell_command → tcp → ESP8266
```
