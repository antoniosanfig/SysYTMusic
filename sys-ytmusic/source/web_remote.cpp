#include "web_remote.hpp"
#include "player.hpp"
#include <switch.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

namespace {

Thread g_webThread;
volatile bool g_webRunning = false;
uint16_t g_port = 8181;

const char* HTML_PAGE = R"HTML(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>sys-ytmusic Switch Remote</title>
<style>
  :root { --bg: #0f0f13; --card: #1c1c24; --accent: #ff0033; --text: #f1f1f1; --sub: #aaa; }
  body { font-family: system-ui, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 16px; }
  .wrap { max-width: 520px; margin: 0 auto; }
  h1 { font-size: 1.25rem; display: flex; align-items: center; gap: 8px; margin-bottom: 12px; }
  .badge { background: var(--accent); font-size: 0.75rem; padding: 2px 8px; border-radius: 99px; }
  .card { background: var(--card); border-radius: 12px; padding: 14px; margin-bottom: 14px; }
  .now-title { font-weight: 700; font-size: 1.05rem; margin-bottom: 4px; }
  .now-sub { color: var(--sub); font-size: 0.85rem; margin-bottom: 12px; }
  .row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; }
  button { background: #2c2c38; color: #fff; border: none; border-radius: 8px; padding: 10px 14px; font-weight: 600; cursor: pointer; }
  button.primary { background: var(--accent); }
  input[type=text] { flex: 1; background: #121218; color: #fff; border: 1px solid #333; border-radius: 8px; padding: 10px; }
  input[type=range] { flex: 1; accent-color: var(--accent); }
  .track { display: flex; justify-content: space-between; align-items: center; padding: 8px 0; border-bottom: 1px solid #2a2a35; gap: 8px; }
  .track:last-child { border-bottom: none; }
  .t-info { flex: 1; min-width: 0; }
  .t-title { font-size: 0.92rem; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .t-artist { font-size: 0.78rem; color: var(--sub); }
</style>
</head>
<body>
<div class="wrap">
  <h1>🎵 sys-ytmusic <span class="badge">Nintendo Switch</span></h1>
  <div class="card">
    <div class="now-title" id="nowTitle">Sin reproduccion</div>
    <div class="now-sub" id="nowStatus">Listo</div>
    <div class="row" style="margin-bottom:10px">
      <button onclick="api('/api/cmd?c=prev')">⏮</button>
      <button class="primary" onclick="api('/api/cmd?c=toggle')">⏯ Play/Pausa</button>
      <button onclick="api('/api/cmd?c=next')">⏭</button>
      <button onclick="api('/api/cmd?c=save')">★ Guardar</button>
    </div>
    <div class="row">
      <span>🔊</span>
      <input type="range" id="vol" min="0" max="150" value="80" onchange="api('/api/cmd?c=vol&v='+this.value)">
      <span id="volVal">80%</span>
    </div>
  </div>

  <div class="card">
    <div class="row">
      <input type="text" id="q" placeholder="Buscar cancion o artista en YouTube Music...">
      <button class="primary" onclick="doSearch()">Buscar</button>
    </div>
  </div>

  <div class="card">
    <div style="font-weight:600;margin-bottom:8px">Resultados de Busqueda</div>
    <div id="results">Busca una cancion arriba o usa el Overlay Tesla en tu Switch.</div>
  </div>
</div>
<script>
async function api(u) {
  await fetch(u);
  setTimeout(refresh, 300);
}
async function doSearch() {
  const q = document.getElementById('q').value.trim();
  if (!q) return;
  document.getElementById('nowStatus').innerText = 'Buscando...';
  await fetch('/api/search?q=' + encodeURIComponent(q));
  setTimeout(refresh, 1200);
}
async function refresh() {
  try {
    const r = await fetch('/api/state');
    const s = await r.json();
    document.getElementById('nowTitle').innerText = s.title || 'Sin reproduccion';
    document.getElementById('nowStatus').innerText = (s.artist ? s.artist + ' • ' : '') + s.status_msg;
    document.getElementById('vol').value = s.volume;
    document.getElementById('volVal').innerText = s.volume + '%';
    const box = document.getElementById('results');
    if (s.results && s.results.length > 0) {
      box.innerHTML = s.results.map((t, i) => `
        <div class="track">
          <div class="t-info">
            <div class="t-title">${t.title}</div>
            <div class="t-artist">${t.artist}</div>
          </div>
          <button class="primary" onclick="api('/api/cmd?c=play_ix&v=${i}')">▶</button>
        </div>`).join('');
    }
  } catch (e) {}
}
setInterval(refresh, 2000);
refresh();
</script>
</body>
</html>)HTML";

std::string urlDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '+') {
            out.push_back(' ');
        } else if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = { in[i + 1], in[i + 2], 0 };
            out.push_back((char)strtoul(hex, nullptr, 16));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::string getQueryParam(const std::string& path, const std::string& key) {
    std::string pattern = key + "=";
    size_t pos = path.find(pattern);
    if (pos == std::string::npos) return "";
    size_t start = pos + pattern.size();
    size_t end = path.find('&', start);
    if (end == std::string::npos) end = path.find(' ', start);
    return urlDecode(path.substr(start, end - start));
}

std::string escapeJson(const char* s) {
    std::string out;
    for (; s && *s; s++) {
        if (*s == '"') out += "\\\"";
        else if (*s == '\\') out += "\\\\";
        else if ((unsigned char)*s >= 0x20) out.push_back(*s);
    }
    return out;
}

void handleClient(int client_fd) {
    char buf[1024] = {};
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;

    std::string req(buf, (size_t)n);
    size_t sp1 = req.find(' ');
    size_t sp2 = (sp1 != std::string::npos) ? req.find(' ', sp1 + 1) : std::string::npos;
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                           ? req.substr(sp1 + 1, sp2 - sp1 - 1)
                           : "/";

    if (path.rfind("/api/state", 0) == 0) {
        YtmSharedState st = Player::getSnapshot();
        std::string json = "{";
        json += "\"title\":\"" + escapeJson(st.current_track.title) + "\",";
        json += "\"artist\":\"" + escapeJson(st.current_track.artist) + "\",";
        json += "\"status_msg\":\"" + escapeJson(st.status_msg) + "\",";
        json += "\"volume\":" + std::to_string(st.volume_percent) + ",";
        json += "\"results\":[";
        for (uint8_t i = 0; i < st.search_count; i++) {
            if (i > 0) json += ",";
            json += "{\"title\":\"" + escapeJson(st.search_results[i].title) + "\",";
            json += "\"artist\":\"" + escapeJson(st.search_results[i].artist) + "\"}";
        }
        json += "]}";

        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n" + json;
        send(client_fd, resp.c_str(), resp.size(), 0);
    } else if (path.rfind("/api/search", 0) == 0) {
        std::string q = getQueryParam(path, "q");
        if (!q.empty()) Player::triggerSearch(q);
        const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nOK";
        send(client_fd, resp, strlen(resp), 0);
    } else if (path.rfind("/api/cmd", 0) == 0) {
        std::string c = getQueryParam(path, "c");
        std::string v = getQueryParam(path, "v");
        if (c == "toggle") Player::togglePlayPause();
        else if (c == "next") Player::next();
        else if (c == "prev") Player::prev();
        else if (c == "save") Player::saveCurrentToFavorites();
        else if (c == "vol") Player::setVolume((uint8_t)atoi(v.c_str()));
        else if (c == "play_ix") Player::playSearchResult((uint8_t)atoi(v.c_str()));

        const char* resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nOK";
        send(client_fd, resp, strlen(resp), 0);
    } else {
        std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n";
        send(client_fd, hdr.c_str(), hdr.size(), 0);
        send(client_fd, HTML_PAGE, strlen(HTML_PAGE), 0);
    }
}

void webServerThread(void* arg) {
    (void)arg;
    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) return;

    int opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_port);

    if (bind(srv_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(srv_fd);
        return;
    }
    listen(srv_fd, 4);

    // Set non-blocking so we can exit cleanly and refresh Switch IP periodically
    int flags = fcntl(srv_fd, F_GETFL, 0);
    fcntl(srv_fd, F_SETFL, flags | O_NONBLOCK);

    uint32_t ip_tick = 0;
    while (g_webRunning) {
        if (ip_tick % 100 == 0) {
            uint32_t ip = gethostid();
            if (ip != 0) {
                struct in_addr in;
                in.s_addr = ip;
                Player::setSwitchIp(inet_ntoa(in));
            }
        }
        ip_tick++;

        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int cli_fd = accept(srv_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (cli_fd >= 0) {
            handleClient(cli_fd);
            close(cli_fd);
        } else {
            svcSleepThread(50'000'000ULL); // 50ms
        }
    }

    close(srv_fd);
}

} // namespace

namespace WebRemote {

bool start(uint16_t port) {
    g_port = port;
    g_webRunning = true;
    threadCreate(&g_webThread, webServerThread, nullptr, nullptr, 0x8000, 0x30, 3);
    threadStart(&g_webThread);
    return true;
}

void stop() {
    g_webRunning = false;
    threadWaitForExit(&g_webThread);
    threadClose(&g_webThread);
}

} // namespace WebRemote
