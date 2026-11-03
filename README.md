# 🎵 sys-ytmusic + ovl-ytmusic (YouTube Music NX)

Reproductor de **YouTube Music en segundo plano (`sysmodule`)** + **Overlay de Tesla (`.ovl`)** para Nintendo Switch con Atmosphère (CFW).
Permite buscar y escuchar música de YouTube Music mientras juegas cualquier título de Switch sin salir del juego, con caché automática en la tarjeta SD para reproducción offline.

---

## ✨ Características Principales

1. **Streaming Nativo Ultraligero (Opus 48kHz):**
   * Corre en el núcleo de sistema de la Switch (Core 3) con un consumo de RAM menor a **5 MB**, compatible incluso con juegos pesados como *Zelda: Tears of the Kingdom*.
   * Demuxer WebM/EBML en streaming + decodificador `libopus` directo a `audren` (48,000 Hz estéreo nativo de la Switch).
2. **Sin cuentas ni PC obligatorio:**
   * Se conecta directamente a instancias públicas de **Piped API** e **Invidious API** con *failover* automático si una instancia falla.
   * También incluye un mini servidor opcional (`server-proxy/ytmusic_proxy.py`) por si algún día deseas usar tu propio servidor con `yt-dlp`.
3. **Doble Control (Overlay de Tesla + Web App Móvil):**
   * **Desde la Switch (`ovl-ytmusic.ovl`):** Abre el menú Tesla (`L + D-Pad Abajo + R3`) en medio de cualquier partida para pausar (`Y`), cambiar de canción (`L`/`R`), ajustar el volumen independiente de la música (`0%` a `150%`), buscar con el teclado o lanzar mixes rápidos y favoritos.
   * **Desde tu Celular (`http://<IP-DE-TU-SWITCH>:8181`):** Incluye una Web UI oscura integrada en el `sysmodule` para que puedas buscar canciones rápidamente con el teclado de tu teléfono y enviarlas a sonar en la Switch al instante.
4. **Modo Híbrido con Caché SD Automática:**
   * Cada canción que escuchas por streaming se guarda automáticamente en `sdmc:/config/sys-ytmusic/cache/<videoId>.webm`. La próxima vez que la reproduzcas sonará instantáneamente desde la SD (`[CACHE SD]`) sin gastar batería ni Wi-Fi.

---

## 🚀 Cómo Compilar en la Nube (GitHub Actions - Sin instalar nada en tu PC)

1. Sube esta carpeta a un repositorio en tu cuenta de **GitHub** (puede ser público o privado).
2. Ve a la pestaña **Actions** de tu repositorio en GitHub.
3. Verás que el flujo **Build sys-ytmusic & Tesla Overlay** se ejecuta automáticamente usando el contenedor oficial `devkitpro/devkita64`.
4. Al terminar (tarda ~1.5 minutos), descarga el archivo **`sys-ytmusic-SD-Package.zip`** en la sección **Artifacts**.

---

## 📦 Instalación en tu Nintendo Switch

1. Asegúrate de tener instalado **Tesla Menu (`ovlmenu.ovl`)** y **nx-ovlloader** en tu Switch.
2. Descomprime el contenido de `sys-ytmusic-SD-Package.zip` directamente en la raíz de tu tarjeta MicroSD:
   ```text
   sdmc:/
   ├── atmosphere/
   │   └── contents/
   │       └── 420000000000D609/
   │           ├── exefs.nsp
   │           ├── toolbox.json
   │           └── flags/
   │               └── boot2.flag
   ├── switch/
   │   └── .overlays/
   │       └── ovl-ytmusic.ovl
   └── config/
       └── sys-ytmusic/
           ├── config.json
           ├── playlists.json
           └── cache/
   ```
3. Reinicia tu Switch (o actívalo desde *Sysmodules Overlay* / *Kosmos Toolbox*) y abre el menú Tesla con **`L + D-Pad Abajo + R3`**.

---

## ⚙️ Personalización (`sdmc:/config/sys-ytmusic/config.json`)

Puedes editar `config.json` desde tu PC o con cualquier editor en la Switch para agregar tus propios artistas o búsquedas rápidas en `"quick_presets"`, o agregar canciones directamente a `playlists.json`.
