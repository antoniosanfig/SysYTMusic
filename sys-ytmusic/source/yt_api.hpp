#pragma once
#include <string>
#include <vector>
#include "../../common/ipc_cmd.h"

struct YtmConfig {
    uint8_t default_volume = 80;
    bool cache_enabled = false;     // Desactivado por defecto para cuidar la MicroSD
    uint32_t max_cache_mb = 250;    // Limite maximo de seguridad (250 MB)
    uint16_t web_port = 8181;
    std::string custom_proxy_url;
    std::vector<std::string> piped_instances;
    std::vector<std::string> invidious_instances;
    std::vector<std::string> quick_presets;
};

struct ResolvedStream {
    bool ok = false;
    std::string stream_url;
    std::string title;
    std::string artist;
    uint32_t duration_sec = 0;
    std::string error_msg;
};

namespace YtApi {
    void loadConfig(YtmConfig& cfg);
    void loadFavorites(std::vector<YtmTrackInfo>& favs);
    bool saveFavorite(const YtmTrackInfo& track);

    // Search YouTube Music across configured Piped/Invidious/Custom instances
    bool searchMusic(const YtmConfig& cfg, const std::string& query, std::vector<YtmTrackInfo>& results, std::string& err_out);

    // Resolve direct Opus/WebM audio stream URL for a given YouTube videoId
    ResolvedStream resolveAudioStream(const YtmConfig& cfg, const std::string& video_id);

    // Check if a track is already cached in sdmc:/config/sys-ytmusic/cache/<video_id>.webm
    std::string getCachePath(const std::string& video_id);
    bool isTrackCached(const std::string& video_id);
    uint32_t getCacheUsedKb();
    void clearAllCache();
    void enforceCacheLimit(uint32_t max_mb);
}
