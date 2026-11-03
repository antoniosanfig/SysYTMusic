#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <stdio.h>
#include <string.h>
#include "ipc_client.h"

static YtmSharedState g_uiState = {};
static bool g_connected = false;

static void refreshUiState() {
    g_connected = YtmClient::readState(g_uiState);
}

// Open Switch software keyboard if user wants to type a custom search query
static bool openSwitchKeyboard(char* out_buf, size_t out_len, const char* header_text) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header_text);
    swkbdConfigSetGuideText(&kbd, "Escribe artista o cancion de YouTube Music");
    Result rc = swkbdShow(&kbd, out_buf, out_len);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out_buf[0] != '\0';
}

// ============================================================================
// GUI: Search Results & Quick Presets
// ============================================================================
class SearchResultsGui : public tsl::Gui {
public:
    SearchResultsGui() {}

    virtual tsl::elm::Element* createUI() override {
        refreshUiState();
        auto frame = new tsl::elm::OverlayFrame("Buscar YouTube Music", "Resultados y Mixes Rapidos");
        auto list = new tsl::elm::List();

        // Option 1: Type with Switch Keyboard
        auto kbdItem = new tsl::elm::ListItem("⌨ Escribir busqueda con teclado...");
        kbdItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                char query[64] = {};
                if (openSwitchKeyboard(query, sizeof(query), "Buscar en YouTube Music")) {
                    YtmClient::sendCommand(YTM_CMD_SEARCH, 0, query);
                }
                return true;
            }
            return false;
        });
        list->addItem(kbdItem);

        // Section: Current Search Results
        list->addItem(new tsl::elm::CategoryHeader("Resultados de Busqueda (A = Reproducir)"));
        if (g_uiState.search_count == 0) {
            list->addItem(new tsl::elm::ListItem("Sin resultados aun (elige un Mix abajo)"));
        } else {
            for (uint8_t i = 0; i < g_uiState.search_count; i++) {
                std::string label = std::string(g_uiState.search_results[i].title);
                if (g_uiState.search_results[i].is_cached) label = "[SD] " + label;
                auto item = new tsl::elm::ListItem(label, g_uiState.search_results[i].artist);
                uint8_t idx = i;
                item->setClickListener([idx](u64 keys) {
                    if (keys & HidNpadButton_A) {
                        YtmClient::sendCommand(YTM_CMD_PLAY_SEARCH_IX, idx);
                        tsl::goBack();
                        return true;
                    }
                    return false;
                });
                list->addItem(item);
            }
        }

        // Section: Quick Presets from config.json
        list->addItem(new tsl::elm::CategoryHeader("Busquedas Rapidas (config.json)"));
        for (uint8_t i = 0; i < g_uiState.preset_count; i++) {
            std::string preset = g_uiState.presets[i];
            auto pItem = new tsl::elm::ListItem("🔍 " + preset);
            pItem->setClickListener([preset](u64 keys) {
                if (keys & HidNpadButton_A) {
                    YtmClient::sendCommand(YTM_CMD_SEARCH, 0, preset.c_str());
                    return true;
                }
                return false;
            });
            list->addItem(pItem);
        }

        frame->setContent(list);
        return frame;
    }

    virtual void update() override {
        static u32 tick = 0;
        if (++tick % 30 == 0) {
            refreshUiState();
        }
    }
};

// ============================================================================
// GUI: Favorites & Offline Cache
// ============================================================================
class FavoritesGui : public tsl::Gui {
public:
    FavoritesGui() {}

    virtual tsl::elm::Element* createUI() override {
        refreshUiState();
        auto frame = new tsl::elm::OverlayFrame("Mis Favoritos y Playlists", "sdmc:/config/sys-ytmusic/playlists.json");
        auto list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Canciones Guardadas (A = Reproducir)"));
        if (g_uiState.fav_count == 0) {
            list->addItem(new tsl::elm::ListItem("No hay favoritos guardados todavia"));
        } else {
            for (uint8_t i = 0; i < g_uiState.fav_count; i++) {
                std::string label = g_uiState.favorites[i].title;
                std::string badge = g_uiState.favorites[i].is_cached ? "[SD]" : "YT";
                auto item = new tsl::elm::ListItem(label, badge);
                uint8_t idx = i;
                item->setClickListener([idx](u64 keys) {
                    if (keys & HidNpadButton_A) {
                        YtmClient::sendCommand(YTM_CMD_PLAY_FAV_IX, idx);
                        tsl::goBack();
                        return true;
                    }
                    return false;
                });
                list->addItem(item);
            }
        }

        frame->setContent(list);
        return frame;
    }
};

// ============================================================================
// GUI: Main YouTube Music Overlay
// ============================================================================
class MainYtMusicGui : public tsl::Gui {
public:
    MainYtMusicGui() {}

    virtual tsl::elm::Element* createUI() override {
        refreshUiState();
        auto frame = new tsl::elm::OverlayFrame("YouTube Music NX", "v1.0 - Reproductor en 2do Plano");
        auto list = new tsl::elm::List();

        // Live Now Playing Status Card (CustomDrawer)
        auto statusCard = new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
            if (!g_connected) {
                r->drawString("⚠ sys-ytmusic no detectado", false, x + 10, y + 25, 18, r->a(0xFFFF5555));
                r->drawString("Verifica que el sysmodule este activo.", false, x + 10, y + 50, 14, r->a(0xFFCCCCCC));
                return;
            }
            const char* title = (g_uiState.current_track.title[0] != '\0')
                                    ? g_uiState.current_track.title
                                    : "Sin cancion activa";
            const char* artist = (g_uiState.current_track.artist[0] != '\0')
                                     ? g_uiState.current_track.artist
                                     : "Elige una cancion o busca en YT Music";

            r->drawString(title, false, x + 10, y + 24, 18, r->a(0xFFFFFFFF));
            r->drawString(artist, false, x + 10, y + 46, 14, r->a(0xFFAAAAAA));

            char infoLine[128];
            snprintf(infoLine, sizeof(infoLine), "%s  |  %02u:%02u / %02u:%02u %s",
                     g_uiState.status_msg,
                     g_uiState.elapsed_sec / 60, g_uiState.elapsed_sec % 60,
                     g_uiState.current_track.duration_sec / 60, g_uiState.current_track.duration_sec % 60,
                     g_uiState.current_track.is_cached ? "[CACHE SD]" : "");
            r->drawString(infoLine, false, x + 10, y + 68, 13, r->a(0xFF00FFAA));

            char webLine[96];
            snprintf(webLine, sizeof(webLine), "📱 Control Celular: http://%s:%u",
                     g_uiState.switch_ip, g_uiState.web_port ? g_uiState.web_port : 8181);
            r->drawString(webLine, false, x + 10, y + 90, 13, r->a(0xFF88CCFF));
        });
        list->addItem(statusCard, 100);

        // Playback Controls
        list->addItem(new tsl::elm::CategoryHeader("Controles (Y = Play/Pausa | L/R = Ant/Sig)"));

        auto playPauseBtn = new tsl::elm::ListItem("⏯ Reproducir / Pausar");
        playPauseBtn->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                YtmClient::sendCommand(YTM_CMD_PLAY_PAUSE);
                return true;
            }
            return false;
        });
        list->addItem(playPauseBtn);

        auto nextBtn = new tsl::elm::ListItem("⏭ Siguiente Cancion");
        nextBtn->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                YtmClient::sendCommand(YTM_CMD_NEXT);
                return true;
            }
            return false;
        });
        list->addItem(nextBtn);

        auto prevBtn = new tsl::elm::ListItem("⏮ Cancion Anterior");
        prevBtn->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                YtmClient::sendCommand(YTM_CMD_PREV);
                return true;
            }
            return false;
        });
        list->addItem(prevBtn);

        // Volume Stepper (A = +10%, X = -10%)
        m_volItem = new tsl::elm::ListItem("🔊 Volumen Musica (A: +10% / X: -10%)",
                                           std::to_string(g_uiState.volume_percent) + "%");
        m_volItem->setClickListener([this](u64 keys) {
            int vol = (int)g_uiState.volume_percent;
            if (keys & HidNpadButton_A) {
                vol = (vol + 10 > 150) ? 150 : (vol + 10);
                YtmClient::sendCommand(YTM_CMD_SET_VOLUME, (uint8_t)vol);
                m_volItem->setValue(std::to_string(vol) + "%");
                return true;
            }
            if (keys & HidNpadButton_X) {
                vol = (vol - 10 < 0) ? 0 : (vol - 10);
                YtmClient::sendCommand(YTM_CMD_SET_VOLUME, (uint8_t)vol);
                m_volItem->setValue(std::to_string(vol) + "%");
                return true;
            }
            return false;
        });
        list->addItem(m_volItem);

        // Navigation & Library
        list->addItem(new tsl::elm::CategoryHeader("Explorar YouTube Music"));

        auto searchNav = new tsl::elm::ListItem("🔍 Buscar Canciones / Mixes Rapidos...");
        searchNav->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<SearchResultsGui>();
                return true;
            }
            return false;
        });
        list->addItem(searchNav);

        auto favNav = new tsl::elm::ListItem("⭐ Mis Favoritos y Cache SD...");
        favNav->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<FavoritesGui>();
                return true;
            }
            return false;
        });
        list->addItem(favNav);

        auto saveFavBtn = new tsl::elm::ListItem("💾 Guardar Cancion Actual en Favoritos");
        saveFavBtn->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                YtmClient::sendCommand(YTM_CMD_SAVE_CURRENT);
                return true;
            }
            return false;
        });
        list->addItem(saveFavBtn);

        m_repeatItem = new tsl::elm::ListItem("🔁 Repetir Cancion Actual",
                                              g_uiState.repeat_one ? "ON" : "OFF");
        m_repeatItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                YtmClient::sendCommand(YTM_CMD_TOGGLE_REPEAT);
                bool nextState = !g_uiState.repeat_one;
                m_repeatItem->setValue(nextState ? "ON" : "OFF");
                return true;
            }
            return false;
        });
        list->addItem(m_repeatItem);

        // Cache Management Section
        list->addItem(new tsl::elm::CategoryHeader("Memoria y Cache SD (Modo Hibrido)"));

        m_cacheToggleItem = new tsl::elm::ListItem("📦 Guardar Cache en SD",
                                                   g_uiState.cache_enabled ? "ON" : "OFF");
        m_cacheToggleItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                YtmClient::sendCommand(YTM_CMD_TOGGLE_CACHE);
                bool nextState = !g_uiState.cache_enabled;
                m_cacheToggleItem->setValue(nextState ? "ON" : "OFF");
                return true;
            }
            return false;
        });
        list->addItem(m_cacheToggleItem);

        char mbStr[32];
        snprintf(mbStr, sizeof(mbStr), "%.1f MB", (float)g_uiState.cache_used_kb / 1024.0f);
        m_clearCacheItem = new tsl::elm::ListItem("🗑️ Borrar Cache SD", mbStr);
        m_clearCacheItem->setClickListener([this](u64 keys) {
            if (keys & HidNpadButton_A) {
                YtmClient::sendCommand(YTM_CMD_CLEAR_CACHE);
                m_clearCacheItem->setValue("0.0 MB");
                return true;
            }
            return false;
        });
        list->addItem(m_clearCacheItem);

        frame->setContent(list);
        return frame;
    }

    virtual void update() override {
        static u32 tick = 0;
        if (++tick % 15 == 0) {
            refreshUiState();
            if (m_volItem) {
                m_volItem->setValue(std::to_string(g_uiState.volume_percent) + "%");
            }
            if (m_repeatItem) {
                m_repeatItem->setValue(g_uiState.repeat_one ? "ON" : "OFF");
            }
            if (m_cacheToggleItem) {
                m_cacheToggleItem->setValue(g_uiState.cache_enabled ? "ON" : "OFF");
            }
            if (m_clearCacheItem) {
                char mbStr[32];
                snprintf(mbStr, sizeof(mbStr), "%.1f MB", (float)g_uiState.cache_used_kb / 1024.0f);
                m_clearCacheItem->setValue(mbStr);
            }
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft, HidAnalogStickState joyStickPosRight) override {
        (void)keysHeld;
        (void)touchPos;
        (void)joyStickPosLeft;
        (void)joyStickPosRight;

        if (keysDown & HidNpadButton_Y) {
            YtmClient::sendCommand(YTM_CMD_PLAY_PAUSE);
            return true;
        }
        if (keysDown & HidNpadButton_R) {
            YtmClient::sendCommand(YTM_CMD_NEXT);
            return true;
        }
        if (keysDown & HidNpadButton_L) {
            YtmClient::sendCommand(YTM_CMD_PREV);
            return true;
        }
        return false;
    }

private:
    tsl::elm::ListItem* m_volItem = nullptr;
    tsl::elm::ListItem* m_repeatItem = nullptr;
    tsl::elm::ListItem* m_cacheToggleItem = nullptr;
    tsl::elm::ListItem* m_clearCacheItem = nullptr;
};

// ============================================================================
// Tesla Overlay Entrypoint
// ============================================================================
class YtMusicOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        fsdevMountSdmc();
    }

    virtual void exitServices() override {
        fsdevUnmountAll();
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainYtMusicGui>();
    }
};

int main(int argc, char **argv) {
    return tsl::loop<YtMusicOverlay>(argc, argv);
}
