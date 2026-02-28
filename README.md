# esp-bt-dongle-volume-control

ESP8266 controls the volume of a BT audio dongle by simulating button presses via GPIO. The interesting part: a Claude AI agent can trigger this directly through a chain of standard interfaces — MCP tool → HA API → shell → TCP → GPIO.

---

## The Backbone (replicate this)

This is the full communication chain from AI agent to physical GPIO pin:

```
Claude AI Agent
    │
    │  ha__tv_volume_set(level=9)          ← MCP Tool Call
    ▼
ha-functions MCP Server
    │
    │  POST /api/services/shell_command/tv_volume_set
    │  {"level": 9}                         ← HA REST API
    ▼
Home Assistant
    │
    │  python3 /config/python_scripts/tv_volume.py SET:9
    │                                        ← shell_command
    ▼
tv_volume.py (Python TCP client)
    │
    │  TCP connect 192.168.1.50:42069
    │  send "SET:9\n"                        ← raw TCP
    ▼
ESP8266 (NodeMCU v2)
    │
    │  SYNC: 16x LOW on GPIO4, then 10x LOW on GPIO5
    │                                        ← GPIO active-low output
    ▼
BT Dongle Volume Buttons (GND-triggered)
```

Each layer is independent and replaceable. You only need to replicate the layers relevant to your use case.

---

## Layer 1 — GPIO (ESP8266)

The ESP8266 GPIO pin drives to GND (`OUTPUT + LOW`) to close the button contact, and releases it (`INPUT` = high-Z) to open it. No transistor needed if both boards share GND.

```cpp
void pinOn(int pin)  { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
void pinOff(int pin) { pinMode(pin, INPUT); }
```

**Wiring:**
```
ESP GND  ──────────────────  BT Dongle GND   (shared ground, mandatory)
ESP D2 (GPIO4)  ──────────  Volume DOWN button contact
ESP D1 (GPIO5)  ──────────  Volume UP button contact
```

The GPIO pin actively pulls the button contact to GND — same effect as pressing the physical button.

**Board:** NodeMCU v2, PlatformIO

```ini
; platformio.ini
[env:nodemcuv2]
platform = espressif8266
board = nodemcuv2
framework = arduino
lib_deps =
    ESP8266WiFi
    ESPAsyncTCP@^1.2.2
```

```bash
pio run -t upload
pio device monitor   # 115200 baud
```

---

## Layer 2 — TCP Server (ESP8266)

The firmware runs a non-blocking async TCP server on port 42069. Commands are single-line ASCII strings.

**Protocol:**

| Command | Action | Response |
|---------|--------|----------|
| `GET` | Return tracked volume | `OK volume=9` |
| `UP` | +1 step | `OK volume=10` |
| `DOWN` | -1 step | `OK volume=8` |
| `SET:0`–`SET:15` | Full resync to target | `OK volume=5` |
| `SYNC` | Resync to default (9) | `OK volume=9` |
| `TEST` | Hold DOWN 2s, pause, hold UP 2s | `OK test done` |

**SYNC algorithm** — required because the dongle has no feedback:
1. Press DOWN **16×** (MAX+1) → guaranteed hardware minimum
2. Press UP **target+1×** → hardware ignores first UP after hitting min, +1 compensates

**Quick test:**
```bash
echo "GET"   | nc 192.168.1.50 42069
echo "SET:9" | nc 192.168.1.50 42069
```

---

## Layer 3 — Python TCP Client

Minimal script that bridges shell → TCP. Deployed into the HA container at `/config/python_scripts/tv_volume.py`.

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

```bash
python3 tv_volume.py SET:9
python3 tv_volume.py GET
```

---

## Layer 4 — Home Assistant

### shell_command (`configuration.yaml`)

Exposes the Python script as an HA service. The `{{ level }}` template is filled at call time.

```yaml
shell_command:
  tv_volume_set:  "python3 /config/python_scripts/tv_volume.py SET:{{ level }}"
  tv_volume_up:    python3 /config/python_scripts/tv_volume.py UP
  tv_volume_down:  python3 /config/python_scripts/tv_volume.py DOWN
  tv_volume_sync:  python3 /config/python_scripts/tv_volume.py SYNC
```

### Scripts (`scripts.yaml`)

16 scripts (beamer_null → beamer_fuenfzehn) wrap the shell_command with a fixed level. Makes them callable by name from automations, Alexa, and the AI agent.

```yaml
beamer_neun:
  alias: "Beamer Neun"
  sequence:
    - action: shell_command.tv_volume_set
      data: {level: 9}
```

Alexa discovers these as scenes (requires HA Smart Home integration):
```
"Alexa, aktiviere Beamer Neun"
```

---

## Layer 5 — MCP Tool (AI Agent Interface)

The `ha__tv_volume_set` tool in the [ha-functions MCP server](https://github.com/NG-Bullseye/ha-functions-mcp) lets the Claude AI agent call the full chain with a single function call.

**Tool definition (`functions.txt`):**
```yaml
- spec:
    name: tv_volume_set
    description: "Set TV/BT-Dongle volume to exact level 0-15."
    parameters:
      type: object
      properties:
        level:
          type: integer
          description: "Volume 0-15"
      required: [level]
  function:
    type: script
    sequence:
      - service: shell_command.tv_volume_set
        data:
          level: "{{ level }}"
```

**MCP server** translates the tool call into an HA REST API call:
```
POST http://HA_HOST:8123/api/services/shell_command/tv_volume_set
Authorization: Bearer <token>
{"level": 9}
```

Claude then calls it like any other function — no extra plumbing needed:
```
User: "Mach den Beamer auf 9"
Agent: ha__tv_volume_set(level=9)
→ HA API → shell → python → TCP → ESP8266 → GPIO
```

---

## Replicate Quickly

| What you want | Start here |
|---------------|------------|
| Control any GPIO-triggered device | Layer 1–2 only: ESP8266 + TCP server |
| Add a new HA-callable device command | Layer 3–4: Python client + shell_command |
| Let the AI agent control it | Layer 5: Add entry in functions.txt |
| Voice control via Alexa | Layer 4: HA scripts + Smart Home integration |
| Test without AI | `echo "SET:9" \| nc 192.168.1.50 42069` |
