#!/usr/bin/env python3
"""
Servidor Proxy Opcional para sys-ytmusic (Nintendo Switch)
----------------------------------------------------------
Por defecto, sys-ytmusic usa instancias publicas de Piped e Invidious sin
necesidad de PC. Si en algun momento las instancias publicas estan lentas,
puedes correr este script en tu PC, Raspberry Pi o servidor gratuito:

  pip install yt-dlp
  python ytmusic_proxy.py

Y en tu Switch editar sdmc:/config/sys-ytmusic/config.json:
  "custom_proxy_url": "http://IP-DE-TU-PC:8282"
"""

import json
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer
import yt_dlp

PORT = 8282

class YtmProxyHandler(BaseHTTPRequestHandler):
    def _send_json(self, data, status=200):
        raw = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        qs = urllib.parse.parse_qs(parsed.query)

        if parsed.path == "/search":
            q = qs.get("q", [""])[0].strip()
            if not q:
                return self._send_json({"items": []})
            ydl_opts = {"quiet": True, "extract_flat": True, "skip_download": True}
            items = []
            with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                info = ydl.extract_info(f"ytsearch10:{q}", download=False)
                for entry in info.get("entries", []) or []:
                    vid = entry.get("id", "")
                    if len(vid) == 11:
                        items.append({
                            "url": f"/watch?v={vid}",
                            "videoId": vid,
                            "title": entry.get("title", "Sin titulo"),
                            "uploaderName": entry.get("uploader") or entry.get("channel") or "YouTube Music",
                            "duration": int(entry.get("duration") or 0)
                        })
            return self._send_json({"items": items})

        if parsed.path.startswith("/streams/"):
            video_id = parsed.path.split("/streams/")[-1][:11]
            ydl_opts = {
                "quiet": True,
                "format": "251/250/249/bestaudio[acodec=opus]/bestaudio",
                "skip_download": True
            }
            with yt_dlp.YoutubeDL(ydl_opts) as ydl:
                info = ydl.extract_info(f"https://www.youtube.com/watch?v={video_id}", download=False)
                return self._send_json({
                    "audio_url": info.get("url", ""),
                    "title": info.get("title", ""),
                    "uploader": info.get("uploader", "YouTube Music"),
                    "duration": int(info.get("duration") or 0)
                })

        self._send_json({"status": "sys-ytmusic proxy activo"})


if __name__ == "__main__":
    print(f"[sys-ytmusic proxy] Escuchando en http://0.0.0.0:{PORT}")
    HTTPServer(("0.0.0.0", PORT), YtmProxyHandler).serve_forever()
