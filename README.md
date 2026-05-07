# IOT_Mesh

ESP32 firmware for self-organizing WiFi mesh built on [painlessMesh](https://gitlab.com/painlessMesh/painlessMesh). Each node runs the same sketch and exposes a web UI with three tabs: **Chat**, **Topology**, **Health**.

## Features

- **Mesh chat** - broadcast or direct-message any node by ID. History capped at 50 messages per node.
- **Live topology** - every node publishes the edges it sees in its own subtree; UI unions them into a full graph (SVG).
- **Health monitor** - heartbeats every 2s, nodes marked dead after 6s silent, evicted after 60s.
- **Web UI** - served over the mesh AP on port 80, single PROGMEM HTML page.
- **Serial CLI** - `/help`, `/id`, `/list`, `/broadcast <text>`, `/dm <id> <text>`.

## Hardware

- Any ESP32 board (tested on stock dev kits).
- Two or more nodes for mesh to take place.

## Build

Arduino IDE or arduino-cli. Required libraries:

- `painlessMesh`
- `ESPAsyncWebServer`
- `AsyncTCP`
- `ArduinoJson` (v7+)

Open [iot_mesh.ino](iot_mesh.ino), select ESP32 board, flash.

## Config

Top of [iot_mesh.ino](iot_mesh.ino):

```cpp
#define MESH_PREFIX   "whateverYouLike"
#define MESH_PASSWORD "somethingSneaky"
#define MESH_PORT     5555
#define WEB_PORT      80
```

Change prefix/password before deploying. All nodes must match.

## Usage

1. Flash two or more boards.
2. Connect phone/laptop to mesh SSID (the `MESH_PREFIX` AP, password `MESH_PASSWORD`).
3. Open `http://<node-AP-IP>/` - IP printed over serial at boot.
4. Use Chat / Topology / Health tabs.

Serial monitor (115200 baud) accepts the same commands; type `/help`.

## Files

- [iot_mesh.ino](iot_mesh.ino) - mesh logic, JSON message routing, web server, CLI.
- [web_ui.h](web_ui.h) - PROGMEM HTML+CSS+JS for the UI.

## Protocol

Three JSON message types broadcast over mesh:

- `chat` - `{type, id, to, text, ts}`. `to=0` means broadcast.
- `heartbeat` - `{type, id, uptime}`. Sent every 2s.
- `topology` - `{type, id, edges:[[a,b],...]}`. Sent every 5s, only when local subtree changes.

HTTP endpoints: `GET /api/chat`, `POST /api/chat` (form: `text`, optional `to`), `GET /api/health`, `GET /api/topology`.
