// =====================================================================
// ESP32 PainlessMesh Unified Firmware
// Features: Mesh Chat, Live Topology, Health Monitor
// =====================================================================

#include "painlessMesh.h"
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include <map>
#include <set>
#include <vector>
#include "web_ui.h" // INDEX_HTML PROGMEM blob

// ---------------------- Mesh config ----------------------
#define MESH_PREFIX "whateverYouLike"
#define MESH_PASSWORD "somethingSneaky"
#define MESH_PORT 5555
#define WEB_PORT 80

// Limits / timing
#define CHAT_HISTORY_MAX 50
#define MAX_CHAT_LEN 256
#define HEARTBEAT_DEAD_MS 6000
#define HEARTBEAT_EVICT_MS 60000
#define HEARTBEAT_PERIOD (TASK_SECOND * 2)
#define TOPOLOGY_PERIOD (TASK_SECOND * 5)
#define PRUNE_PERIOD (TASK_SECOND * 2)

// ---------------------- Globals ----------------------
Scheduler userScheduler;
painlessMesh mesh;
AsyncWebServer server(WEB_PORT);

String nodeName; // short display name for this node

// Chat history: list of {from, to, text, ts}. to=0 means broadcast.
struct ChatEntry
{
  uint32_t from;
  uint32_t to;
  String text;
  uint32_t ts;
};
std::vector<ChatEntry> chatLog;

// Per-node heartbeat info
struct HeartbeatInfo
{
  uint32_t uptime;
  uint32_t lastSeen;
};
std::map<uint32_t, HeartbeatInfo> heartbeats;

// Per-broadcaster edge list (each node reports edges it can see in its subtree)
struct Edge { uint32_t a; uint32_t b; };
std::map<uint32_t, std::vector<Edge>> topology;

// Guards chatLog / heartbeats / topology against concurrent access from
// scheduler tasks, mesh rx callback, and ESPAsyncWebServer handlers.
// Recursive so nested calls (broadcastChat -> pushChat) don't deadlock.
SemaphoreHandle_t stateMutex = nullptr;
struct Lock {
  Lock()  { if (stateMutex) xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY); }
  ~Lock() { if (stateMutex) xSemaphoreGiveRecursive(stateMutex); }
};

// ---------------------- Forward decls ----------------------
void sendHeartbeat();
void sendTopology();
void pruneDead();
void receivedCallback(uint32_t from, String &msg);
void newConnectionCallback(uint32_t nodeId);
void changedConnectionCallback();
void nodeTimeAdjustedCallback(int32_t offset);
void setupWebServer();
static void processSerial();
static void handleCommand(const String &line);
static void broadcastChat(const String &text, uint32_t to);

// Periodic broadcast tasks
Task taskHeartbeat(HEARTBEAT_PERIOD, TASK_FOREVER, &sendHeartbeat);
Task taskTopology(TOPOLOGY_PERIOD, TASK_FOREVER, &sendTopology);
Task taskPrune(PRUNE_PERIOD, TASK_FOREVER, &pruneDead);

// ---------------------- Helpers ----------------------
static String shortName(uint32_t id)
{
  String s = String(id);
  if (s.length() > 4)
    s = s.substring(0, 4);
  return "Node-" + s;
}

static void pushChat(uint32_t from, uint32_t to, const String &text, uint32_t ts)
{
  Lock l;
  ChatEntry e{from, to, text, ts};
  chatLog.push_back(e);
  if (chatLog.size() > CHAT_HISTORY_MAX)
  {
    chatLog.erase(chatLog.begin(), chatLog.begin() + (chatLog.size() - CHAT_HISTORY_MAX));
  }
}

// ---------------------- Mesh broadcasts ----------------------

// Heartbeat: { type:"heartbeat", id, uptime }
void sendHeartbeat()
{
  JsonDocument doc;
  doc["type"] = "heartbeat";
  doc["id"] = mesh.getNodeId();
  doc["uptime"] = millis() / 1000;
  String out;
  serializeJson(doc, out);
  mesh.sendBroadcast(out);

  Lock l;
  heartbeats[mesh.getNodeId()] = {(uint32_t)(millis() / 1000), millis()};
}

// Walks subConnectionJson tree (painlessMesh 1.5+ format), emits parent->child edges.
// Top-level: {nodeId:self, root:true, subs:[{nodeId, subs:[...]}, ...]}
static void collectEdges(JsonObject node, std::vector<Edge> &out)
{
  uint32_t parent = node["nodeId"] | 0u;
  if (!parent)
    return;
  for (JsonObject child : node["subs"].as<JsonArray>())
  {
    uint32_t cid = child["nodeId"] | 0u;
    if (cid)
      out.push_back({parent, cid});
    collectEdges(child, out);
  }
}

// Topology: { type:"topology", id, edges:[[a,b],...] }
// Each node walks its own subtree view and broadcasts every edge it sees.
// Union across all broadcasters reconstructs full mesh graph including
// edges a leaf would otherwise miss (its parent link is in parent's view).
// Skips broadcast if local subtree unchanged since last tick.
void sendTopology()
{
  String subTree = mesh.subConnectionJson(false);

  JsonDocument subDoc;
  DeserializationError err = deserializeJson(subDoc, subTree);

  std::vector<Edge> edges;
  if (!err)
    collectEdges(subDoc.as<JsonObject>(), edges);

  static std::vector<Edge> last;
  bool same = (edges.size() == last.size());
  for (size_t i = 0; same && i < edges.size(); ++i)
    same = (edges[i].a == last[i].a && edges[i].b == last[i].b);
  if (same) return;
  last = edges;

  JsonDocument doc;
  doc["type"] = "topology";
  doc["id"] = mesh.getNodeId();
  JsonArray arr = doc["edges"].to<JsonArray>();
  for (auto &e : edges)
  {
    JsonArray pair = arr.add<JsonArray>();
    pair.add(e.a);
    pair.add(e.b);
  }

  String out;
  serializeJson(doc, out);
  mesh.sendBroadcast(out);

  Lock l;
  topology[mesh.getNodeId()] = edges;
}

// Periodic cleanup. Drops topology entries from stale broadcasters and
// evicts long-silent heartbeats so maps don't grow unbounded.
void pruneDead()
{
  Lock l;
  uint32_t now = millis();
  uint32_t self = mesh.getNodeId();

  for (auto it = topology.begin(); it != topology.end();) {
    auto h = heartbeats.find(it->first);
    bool stale = (it->first != self) &&
                 (h == heartbeats.end() || (now - h->second.lastSeen) > HEARTBEAT_DEAD_MS);
    if (stale) it = topology.erase(it);
    else ++it;
  }

  for (auto it = heartbeats.begin(); it != heartbeats.end();) {
    if (it->first != self && (now - it->second.lastSeen) > HEARTBEAT_EVICT_MS)
      it = heartbeats.erase(it);
    else ++it;
  }
}

// Sends a chat message originated locally. to=0 broadcasts, else unicast via mesh routing.
static void broadcastChat(const String &text, uint32_t to)
{
  uint32_t ts = mesh.getNodeTime();
  JsonDocument doc;
  doc["type"] = "chat";
  doc["id"] = mesh.getNodeId();
  doc["to"] = to;
  doc["text"] = text;
  doc["ts"] = ts;
  String out;
  serializeJson(doc, out);
  bool ok;
  if (to == 0)
    ok = mesh.sendBroadcast(out);
  else
    ok = mesh.sendSingle(to, out);
  Serial.printf("[CHAT TX] to=%u (%s) ok=%d text=\"%s\"\n",
                to, to == 0 ? "broadcast" : "unicast", ok, text.c_str());
  pushChat(mesh.getNodeId(), to, text, ts);
}

// ---------------------- Mesh callbacks ----------------------

// Parses incoming JSON and routes by "type"
void receivedCallback(uint32_t from, String &msg)
{
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err)
  {
    Serial.printf("JSON parse fail from %u: %s\n", from, err.c_str());
    return;
  }
  const char *type = doc["type"] | "";

  if (strcmp(type, "chat") == 0)
  {
    uint32_t to = doc["to"] | 0u;
    String text = String((const char *)(doc["text"] | ""));
    uint32_t ts = doc["ts"] | mesh.getNodeTime();
    Serial.printf("[CHAT RX] from=%u to=%u (%s) text=\"%s\"\n",
                  from, to,
                  to == 0 ? "broadcast" : (to == mesh.getNodeId() ? "me" : "other"),
                  text.c_str());
    pushChat(from, to, text, ts);
  }
  else if (strcmp(type, "heartbeat") == 0)
  {
    uint32_t id = doc["id"] | from;
    uint32_t uptime = doc["uptime"] | 0;
    Lock l;
    heartbeats[id] = {uptime, millis()};
  }
  else if (strcmp(type, "topology") == 0)
  {
    uint32_t id = doc["id"] | from;
    std::vector<Edge> edges;
    for (JsonArray pair : doc["edges"].as<JsonArray>())
    {
      if (pair.size() >= 2)
        edges.push_back({pair[0].as<uint32_t>(), pair[1].as<uint32_t>()});
    }
    Lock l;
    topology[id] = edges;
  }
}

void newConnectionCallback(uint32_t nodeId)
{
  Serial.printf("New connection: %u\n", nodeId);
}

void changedConnectionCallback()
{
  Serial.printf("Connections changed\n");
  sendTopology(); // converge fast on join/drop instead of waiting full period
}

void nodeTimeAdjustedCallback(int32_t offset)
{
  Serial.printf("Time adjusted, offset=%d\n", offset);
}

// ---------------------- Serial commands ----------------------
static void printHelp()
{
  Serial.println(F("Commands:"));
  Serial.println(F("  /help                  show commands"));
  Serial.println(F("  /id                    show this node id"));
  Serial.println(F("  /ip                    show AP and station IPs"));
  Serial.println(F("  /list                  list known nodes"));
  Serial.println(F("  /broadcast <text>      send chat to all"));
  Serial.println(F("  /dm <id> <text>        direct message a node"));
}

static void handleCommand(const String &line)
{
  String s = line;
  s.trim();
  if (!s.length()) return;

  if (s == "/help") {
    printHelp();
  }
  else if (s == "/id") {
    Serial.printf("id=%u name=%s\n", mesh.getNodeId(), nodeName.c_str());
  }
  else if (s == "/ip") {
    Serial.print("AP IP: ");
    Serial.println(mesh.getAPIP());
    Serial.print("STA IP: ");
    Serial.println(mesh.getStationIP());
  }
  else if (s == "/list") {
    Lock l;
    uint32_t now = millis();
    Serial.printf("%u nodes:\n", (unsigned)heartbeats.size());
    for (auto &kv : heartbeats) {
      uint32_t ago = now - kv.second.lastSeen;
      bool alive = ago <= HEARTBEAT_DEAD_MS;
      Serial.printf("  %u uptime=%us last_seen=%ums %s\n",
                    kv.first, kv.second.uptime, ago,
                    alive ? "ALIVE" : "DEAD");
    }
  }
  else if (s.startsWith("/broadcast ")) {
    String text = s.substring(11);
    text.trim();
    if (!text.length()) { Serial.println("usage: /broadcast <text>"); return; }
    if (text.length() > MAX_CHAT_LEN) text = text.substring(0, MAX_CHAT_LEN);
    broadcastChat(text, 0);
  }
  else if (s.startsWith("/dm ")) {
    String rest = s.substring(4);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp < 0) { Serial.println("usage: /dm <id> <text>"); return; }
    uint32_t id = strtoul(rest.substring(0, sp).c_str(), nullptr, 10);
    String text = rest.substring(sp + 1);
    text.trim();
    if (!id || !text.length()) { Serial.println("usage: /dm <id> <text>"); return; }
    if (text.length() > MAX_CHAT_LEN) text = text.substring(0, MAX_CHAT_LEN);
    broadcastChat(text, id);
  }
  else {
    Serial.printf("unknown command: %s (try /help)\n", s.c_str());
  }
}

// Non-blocking line reader. Drains Serial buffer, dispatches on newline.
static void processSerial()
{
  static String buf;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = buf;
      buf = "";
      handleCommand(line);
    } else {
      buf += c;
      if (buf.length() > 512) buf = ""; // overflow guard
    }
  }
}

// ---------------------- Web server setup ----------------------
void setupWebServer()
{
  // Index page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req)
            { req->send_P(200, "text/html", INDEX_HTML); });

  // Chat: GET returns recent messages, POST broadcasts a new one
  server.on("/api/chat", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    JsonDocument doc;
    doc["self_id"]   = mesh.getNodeId();
    doc["self_name"] = nodeName;
    JsonArray arr = doc["messages"].to<JsonArray>();
    {
      Lock l;
      for (auto &c : chatLog) {
        JsonObject o = arr.add<JsonObject>();
        o["from"]    = shortName(c.from);
        o["from_id"] = c.from;
        o["to"]      = c.to;
        o["to_name"] = c.to ? shortName(c.to) : String("all");
        o["text"]    = c.text;
        o["ts"]      = c.ts;
      }
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out); });

  server.on("/api/chat", HTTP_POST, [](AsyncWebServerRequest *req)
            {
    if (req->hasParam("text", true)) {
      String text = req->getParam("text", true)->value();
      if (text.length() > MAX_CHAT_LEN) text = text.substring(0, MAX_CHAT_LEN);
      uint32_t to = 0;
      if (req->hasParam("to", true))
        to = strtoul(req->getParam("to", true)->value().c_str(), nullptr, 10);
      if (text.length()) broadcastChat(text, to);
    }
    req->send(200, "application/json", "{\"ok\":true}"); });

  // Health: union of heartbeats with alive/dead flag
  server.on("/api/health", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    JsonDocument doc;
    JsonArray arr = doc["nodes"].to<JsonArray>();
    uint32_t now = millis();
    {
      Lock l;
      for (auto &kv : heartbeats) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]           = kv.first;
        o["uptime"]       = kv.second.uptime;
        uint32_t ago      = now - kv.second.lastSeen;
        o["last_seen_ms"] = ago;
        o["alive"]        = (ago <= HEARTBEAT_DEAD_MS);
      }
    }
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out); });

  // Topology: union of edges from live broadcasters, deduped undirected.
  // Read-only: dead-state cleanup lives in pruneDead(). Filter here only
  // covers the window between prune ticks.
  server.on("/api/topology", HTTP_GET, [](AsyncWebServerRequest *req)
            {
    JsonDocument doc;
    uint32_t self = mesh.getNodeId();
    doc["self_id"] = self;
    JsonArray nodesArr = doc["nodes"].to<JsonArray>();
    JsonArray edgesArr = doc["edges"].to<JsonArray>();

    uint32_t now = millis();

    std::set<uint32_t> nodeSet;
    std::set<uint64_t> edgeSet; // packed (min<<32)|max for dedup

    {
      Lock l;
      std::set<uint32_t> dead;
      for (auto &kv : heartbeats) {
        if (kv.first != self && (now - kv.second.lastSeen) > HEARTBEAT_DEAD_MS)
          dead.insert(kv.first);
      }

      for (auto &kv : topology) {
        if (dead.count(kv.first)) continue;
        nodeSet.insert(kv.first);
        for (auto &e : kv.second) {
          if (dead.count(e.a) || dead.count(e.b)) continue;
          nodeSet.insert(e.a);
          nodeSet.insert(e.b);
          uint32_t lo = std::min(e.a, e.b), hi = std::max(e.a, e.b);
          uint64_t key = ((uint64_t)lo << 32) | hi;
          if (edgeSet.insert(key).second) {
            JsonArray pair = edgesArr.add<JsonArray>();
            pair.add(lo);
            pair.add(hi);
          }
        }
      }
    }

    for (auto id : nodeSet) nodesArr.add(id);

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out); });

  server.onNotFound([](AsyncWebServerRequest *req)
                    { req->send(404, "text/plain", "Not found"); });

  server.begin();
}

// ---------------------- setup / loop ----------------------
void setup()
{
  Serial.begin(115200);
  delay(50);

  stateMutex = xSemaphoreCreateRecursiveMutex();

  // Mesh init
  mesh.setDebugMsgTypes(ERROR | STARTUP);
  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  nodeName = shortName(mesh.getNodeId());
  Serial.printf("This node: %s (id=%u)\n", nodeName.c_str(), mesh.getNodeId());
  Serial.print("AP IP: ");
  Serial.println(mesh.getAPIP());

  // Periodic broadcast tasks
  userScheduler.addTask(taskHeartbeat);
  taskHeartbeat.enable();
  userScheduler.addTask(taskTopology);
  taskTopology.enable();
  userScheduler.addTask(taskPrune);
  taskPrune.enable();

  // Web server
  setupWebServer();

  printHelp();
}

void loop()
{
  mesh.update();           // drives mesh + scheduler
  userScheduler.execute(); // explicit, harmless
  processSerial();
}
