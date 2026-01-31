# esp-bt-dongle-volume-control

ESP8266 volume control for a BT audio dongle. Two transistors simulate button presses on the dongle's remote (volume up / volume down).

## Network

| Key | Value |
|-----|-------|
| IP | `192.168.1.50` |
| Port | `42069` (TCP) |
| WiFi | Leos IoT 2,4GHz |

## GPIO Pinout

| Pin | Function |
|-----|----------|
| GPIO4 (D2) | Volume Down (transistor) |
| GPIO5 (D1) | Volume Up (transistor) |
| GPIO2 (D4) | Onboard LED (on during SYNC) |

Inverted logic: LOW = transistor ON.

## TCP Commands

| Command | Description |
|---------|-------------|
| `UP` | Volume +1 |
| `DOWN` | Volume -1 |
| `SET:0-15` | SYNC to absolute volume (15x DOWN to 0, then Nx UP) |
| `SYNC` | Reset to default volume (9) |
| `GET` | Return current volume |

## SYNC Behavior

Every `SET:X` command and every boot/restart triggers a full SYNC:
1. Press DOWN 15 times → guaranteed level 0
2. Press UP X times → deterministic target level

On boot, the ESP syncs to volume 9.

## Timing

- Button press: 50ms held
- Pause between presses: 100ms

## Build

PlatformIO project for NodeMCU v2 (ESP8266).

```bash
pio run -t upload
pio device monitor
```

## Test

```bash
echo "GET" | nc 192.168.1.50 42069
echo "SET:12" | nc 192.168.1.50 42069
```
