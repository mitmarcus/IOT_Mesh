// =====================================================================
// web_ui.h — single PROGMEM HTML page for mesh node web UI
// 3 tabs: Chat / Topology / Health
// =====================================================================
#pragma once

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mesh Node</title>
<style>
  body { font-family: system-ui, sans-serif; margin:0; background:#111; color:#eee; }
  header { padding:10px 14px; background:#1d1d22; border-bottom:1px solid #333; }
  header h1 { margin:0; font-size:18px; }
  header small { color:#888; }
  nav { display:flex; background:#181820; border-bottom:1px solid #333; }
  nav button { flex:1; padding:12px; background:transparent; color:#bbb; border:0;
               cursor:pointer; font-size:14px; }
  nav button.active { background:#2a2a35; color:#fff; border-bottom:2px solid #4af; }
  .tab { display:none; padding:14px; }
  .tab.active { display:block; }
  input, textarea, select, button.action {
    background:#222; color:#eee; border:1px solid #444; padding:8px;
    border-radius:4px; font-size:14px;
  }
  button.action { cursor:pointer; background:#2a4; border-color:#2a4; color:#fff; }
  #chatLog { height:60vh; overflow-y:auto; background:#1a1a20; padding:8px;
             border:1px solid #333; border-radius:4px; margin-bottom:8px; }
  .chat-msg { margin:4px 0; padding:6px 8px; background:#222; border-radius:4px; }
  .chat-msg .who { color:#4af; font-weight:bold; margin-right:6px; }
  .chat-msg .to  { color:#fa4; margin-right:6px; }
  .chat-msg.dm   { background:#2a2230; border-left:3px solid #fa4; }
  #graph { width:100%; height:65vh; background:#1a1a20; border:1px solid #333; border-radius:4px; display:block; }
  #graph .edge { stroke:#888; stroke-width:1.5; }
  #graph .node circle { stroke:#fff; stroke-width:2; }
  #graph .node.self circle { fill:#2a4; }
  #graph .node.other circle { fill:#4af; }
  #graph .node text { fill:#eee; font-size:12px; text-anchor:middle; dominant-baseline:central; }
  table { width:100%; border-collapse:collapse; }
  th, td { text-align:left; padding:6px 8px; border-bottom:1px solid #333; }
  th { background:#1f1f25; }
  .alive { color:#4f4; font-weight:bold; }
  .dead  { color:#f44; font-weight:bold; }
  .row { display:flex; flex-wrap:wrap; gap:6px; margin-bottom:8px; }
  .row input { flex:1 1 160px; min-width:0; }
  .row select { flex:0 1 auto; max-width:40%; }
  .row button.action { flex:0 0 auto; }
</style>
</head>
<body>

<header>
  <h1 id="nodeTitle">Mesh Node</h1>
  <small id="nodeIdLabel"></small>
</header>

<nav>
  <button data-tab="chat" class="active">Chat</button>
  <button data-tab="topology">Topology</button>
  <button data-tab="health">Health</button>
</nav>

<!-- ===== Chat tab ===== -->
<section id="tab-chat" class="tab active">
  <div id="chatLog"></div>
  <div class="row">
    <select id="chatTo"><option value="0">All (broadcast)</option></select>
    <input id="chatInput" placeholder="Type a message...">
    <button class="action" onclick="sendChat()">Send</button>
  </div>
</section>

<!-- ===== Topology tab ===== -->
<section id="tab-topology" class="tab">
  <svg id="graph" xmlns="http://www.w3.org/2000/svg"></svg>
</section>

<!-- ===== Health tab ===== -->
<section id="tab-health" class="tab">
  <table>
    <thead><tr><th>Node ID</th><th>Uptime (s)</th><th>Last seen (ms ago)</th><th>Status</th></tr></thead>
    <tbody id="healthBody"></tbody>
  </table>
</section>

<script>
// ---------- Tab switching ----------
document.querySelectorAll('nav button').forEach(b => {
  b.onclick = () => {
    document.querySelectorAll('nav button').forEach(x => x.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(x => x.classList.remove('active'));
    b.classList.add('active');
    document.getElementById('tab-' + b.dataset.tab).classList.add('active');
  };
});

// ---------- Chat ----------
let selfId = 0;
async function sendChat() {
  const input = document.getElementById('chatInput');
  const to    = document.getElementById('chatTo').value || '0';
  const text  = input.value.trim();
  if (!text) return;
  await fetch('/api/chat', {
    method: 'POST',
    headers: {'Content-Type':'application/x-www-form-urlencoded'},
    body: 'text=' + encodeURIComponent(text) + '&to=' + encodeURIComponent(to)
  });
  input.value = '';
  refreshChat();
}
async function refreshChat() {
  try {
    const r = await fetch('/api/chat');
    const j = await r.json();
    selfId = j.self_id || 0;
    document.getElementById('nodeTitle').textContent    = j.self_name || 'Mesh Node';
    document.getElementById('nodeIdLabel').textContent  = 'ID: ' + selfId;
    const log = document.getElementById('chatLog');
    log.innerHTML = j.messages.map(m => {
      const dm = m.to && m.to !== 0;
      const arrow = dm ? `<span class="to">&rarr; ${escapeHtml(m.to_name)}</span>` : '';
      return `<div class="chat-msg${dm?' dm':''}"><span class="who">${m.from}</span>${arrow}${escapeHtml(m.text)}</div>`;
    }).join('');
    log.scrollTop = log.scrollHeight;
  } catch (e) {}
}

// Populate recipient dropdown from health node list
function refreshChatTargets(nodes) {
  const sel = document.getElementById('chatTo');
  const cur = sel.value;
  const others = nodes.filter(n => n.id !== selfId).sort((a,b)=>a.id-b.id);
  sel.innerHTML = '<option value="0">All (broadcast)</option>' +
    others.map(n => {
      const label = 'Node-' + String(n.id).substring(0,4) + (n.alive ? '' : ' (dead)');
      return `<option value="${n.id}"${n.alive?'':' disabled'}>${label}</option>`;
    }).join('');
  if ([...sel.options].some(o => o.value === cur)) sel.value = cur;
}

// ---------- Health ----------
async function refreshHealth() {
  try {
    const r = await fetch('/api/health');
    const j = await r.json();
    document.getElementById('healthBody').innerHTML = j.nodes.map(n => {
      const cls = n.alive ? 'alive' : 'dead';
      const txt = n.alive ? 'ALIVE' : 'DEAD';
      return `<tr><td>${n.id}</td><td>${n.uptime}</td><td>${n.last_seen_ms}</td><td class="${cls}">${txt}</td></tr>`;
    }).join('');
    refreshChatTargets(j.nodes);
  } catch (e) {}
}

// ---------- Topology graph (inline SVG, circle layout) ----------
const SVG_NS = 'http://www.w3.org/2000/svg';
async function refreshTopology() {
  try {
    const r = await fetch('/api/topology');
    const j = await r.json();

    const ids   = (j.nodes || []).slice().sort((a,b)=>a-b);
    const edges = (j.edges || []);
    const svg = document.getElementById('graph');
    const rect = svg.getBoundingClientRect();
    const W = rect.width || 600, H = rect.height || 400;
    svg.setAttribute('viewBox', `0 0 ${W} ${H}`);

    // Lay out nodes on a circle (single node centered)
    const cx = W/2, cy = H/2;
    const R  = Math.max(40, Math.min(W, H)/2 - 60);
    const pos = {};
    if (ids.length === 1) {
      pos[ids[0]] = {x: cx, y: cy};
    } else {
      ids.forEach((id, i) => {
        const a = (2*Math.PI*i)/ids.length - Math.PI/2;
        pos[id] = {x: cx + R*Math.cos(a), y: cy + R*Math.sin(a)};
      });
    }

    // Render
    while (svg.firstChild) svg.removeChild(svg.firstChild);
    edges.forEach(([a, b]) => {
      const pa = pos[a], pb = pos[b];
      if (!pa || !pb) return;
      const ln = document.createElementNS(SVG_NS, 'line');
      ln.setAttribute('class', 'edge');
      ln.setAttribute('x1', pa.x); ln.setAttribute('y1', pa.y);
      ln.setAttribute('x2', pb.x); ln.setAttribute('y2', pb.y);
      svg.appendChild(ln);
    });
    ids.forEach(id => {
      const p = pos[id];
      const g = document.createElementNS(SVG_NS, 'g');
      g.setAttribute('class', 'node ' + (id === j.self_id ? 'self' : 'other'));
      const c = document.createElementNS(SVG_NS, 'circle');
      c.setAttribute('cx', p.x); c.setAttribute('cy', p.y); c.setAttribute('r', 18);
      g.appendChild(c);
      const t = document.createElementNS(SVG_NS, 'text');
      t.setAttribute('x', p.x); t.setAttribute('y', p.y + 32);
      t.textContent = 'Node-' + String(id).substring(0,4);
      g.appendChild(t);
      svg.appendChild(g);
    });
  } catch (e) {}
}

// ---------- Util ----------
function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
  }[c]));
}

// ---------- Polling: every 2s per requirement ----------
function tick() { refreshChat(); refreshHealth(); refreshTopology(); }
tick();
setInterval(tick, 2000);
</script>
</body>
</html>
)HTML";
