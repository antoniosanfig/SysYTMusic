#include "yt_api.hpp"
#include <curl/curl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <algorithm>

namespace {

size_t writeStringCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    // Cap JSON response size at 512 KB to protect sysmodule heap
    if (str->size() + total > 512 * 1024) {
        return 0;
    }
    str->append(static_cast<const char*>(contents), total);
    return total;
}

bool httpGet(const std::string& url, std::string& body_out, long timeout_sec = 8) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    body_out.clear();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "sys-ytmusic-nx/1.0 (Nintendo Switch)");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK && http_code >= 200 && http_code < 300 && !body_out.empty());
}

std::string urlEncode(const std::string& value) {
    std::string out;
    const char* hex = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else if (c == ' ') {
            out.append("%20");
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// Unescape JSON string literals (\u0026 -> &, \" -> ", \/ -> /)
std::string jsonUnescape(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char next = raw[++i];
            if (next == '"') out.push_back('"');
            else if (next == '\\') out.push_back('\\');
            else if (next == '/') out.push_back('/');
            else if (next == 'n') out.push_back(' ');
            else if (next == 't') out.push_back(' ');
            else if (next == 'u' && i + 4 < raw.size()) {
                std::string hex = raw.substr(i + 1, 4);
                unsigned int code = (unsigned int)strtoul(hex.c_str(), nullptr, 16);
                if (code == 0x0026) out.push_back('&');
                else if (code < 0x80 && code > 0) out.push_back((char)code);
                i += 4;
            } else {
                out.push_back(next);
            }
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

// Extract string value for `"key": "value"` starting from `start_pos` within `[start_pos, end_pos)`
std::string extractJsonString(const std::string& json, const char* key, size_t start_pos, size_t end_pos) {
    std::string pattern = std::string("\"") + key + "\"";
    size_t pos = json.find(pattern, start_pos);
    if (pos == std::string::npos || pos >= end_pos) return "";
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos || pos >= end_pos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos || pos >= end_pos) return "";
    size_t val_start = pos + 1;
    size_t cur = val_start;
    while (cur < end_pos && cur < json.size()) {
        if (json[cur] == '"' && json[cur - 1] != '\\') {
            return jsonUnescape(json.substr(val_start, cur - val_start));
        }
        cur++;
    }
    return "";
}

uint32_t extractJsonUint(const std::string& json, const char* key, size_t start_pos, size_t end_pos) {
    std::string pattern = std::string("\"") + key + "\"";
    size_t pos = json.find(pattern, start_pos);
    if (pos == std::string::npos || pos >= end_pos) return 0;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos || pos >= end_pos) return 0;
    pos++;
    while (pos < end_pos && (json[pos] == ' ' || json[pos] == '"')) pos++;
    return (uint32_t)strtoul(json.c_str() + pos, nullptr, 10);
}

void extractStringArray(const std::string& json, const char* key, std::vector<std::string>& out) {
    std::string pattern = std::string("\"") + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return;
    size_t arr_start = json.find('[', pos);
    size_t arr_end = json.find(']', arr_start);
    if (arr_start == std::string::npos || arr_end == std::string::npos) return;

    size_t cur = arr_start + 1;
    while (cur < arr_end) {
        size_t q1 = json.find('"', cur);
        if (q1 == std::string::npos || q1 >= arr_end) break;
        size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= arr_end) break;
        std::string item = jsonUnescape(json.substr(q1 + 1, q2 - q1 - 1));
        if (!item.empty()) out.push_back(item);
        cur = q2 + 1;
    }
}

std::string readTextFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return "";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024) {
        fclose(f);
        return "";
    }
    std::string buf((size_t)sz, '\0');
    fread(&buf[0], 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

} // namespace

namespace YtApi {

std::string getCachePath(const std::string& video_id) {
    return std::string(YTM_CACHE_DIR) + "/" + video_id + ".webm";
}

bool isTrackCached(const std::string& video_id) {
    if (video_id.empty()) return false;
    std::string path = getCachePath(video_id);
    struct stat st;
    return (stat(path.c_str(), &st) == 0 && st.st_size > 16384);
}

uint32_t getCacheUsedKb() {
    DIR* d = opendir(YTM_CACHE_DIR);
    if (!d) return 0;
    uint64_t total_bytes = 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = std::string(YTM_CACHE_DIR) + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            total_bytes += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return (uint32_t)(total_bytes / 1024ULL);
}

void clearAllCache() {
    DIR* d = opendir(YTM_CACHE_DIR);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = std::string(YTM_CACHE_DIR) + "/" + ent->d_name;
        remove(full.c_str());
    }
    closedir(d);
}

void enforceCacheLimit(uint32_t max_mb) {
    if (max_mb == 0) max_mb = 250;
    uint64_t max_bytes = (uint64_t)max_mb * 1024ULL * 1024ULL;

    DIR* d = opendir(YTM_CACHE_DIR);
    if (!d) return;

    struct CachedFileEntry {
        std::string path;
        uint64_t size;
        time_t mtime;
    };
    std::vector<CachedFileEntry> files;
    uint64_t total_bytes = 0;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = std::string(YTM_CACHE_DIR) + "/" + ent->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            files.push_back({ full, (uint64_t)st.st_size, st.st_mtime });
            total_bytes += (uint64_t)st.st_size;
        }
    }
    closedir(d);

    if (total_bytes <= max_bytes) return;

    // Sort oldest first so we evict oldest cached songs until under max_bytes
    std::sort(files.begin(), files.end(), [](const CachedFileEntry& a, const CachedFileEntry& b) {
        return a.mtime < b.mtime;
    });

    for (const auto& f : files) {
        if (total_bytes <= max_bytes) break;
        if (remove(f.path.c_str()) == 0) {
            total_bytes = (total_bytes > f.size) ? (total_bytes - f.size) : 0;
        }
    }
}

void loadConfig(YtmConfig& cfg) {
    cfg.piped_instances.clear();
    cfg.invidious_instances.clear();
    cfg.quick_presets.clear();
    cfg.cache_enabled = false;
    cfg.max_cache_mb = 250;

    std::string raw = readTextFile(YTM_CONFIG_PATH);
    if (!raw.empty()) {
        uint32_t vol = extractJsonUint(raw, "default_volume", 0, raw.size());
        if (vol > 0 && vol <= 150) cfg.default_volume = (uint8_t)vol;
        uint32_t max_mb = extractJsonUint(raw, "max_cache_mb", 0, raw.size());
        if (max_mb >= 20 && max_mb <= 4096) cfg.max_cache_mb = max_mb;
        if (raw.find("\"cache_enabled\": true") != std::string::npos ||
            raw.find("\"cache_enabled\":true") != std::string::npos) {
            cfg.cache_enabled = true;
        }
        uint32_t port = extractJsonUint(raw, "web_port", 0, raw.size());
        if (port >= 1024 && port <= 65535) cfg.web_port = (uint16_t)port;
        cfg.custom_proxy_url = extractJsonString(raw, "custom_proxy_url", 0, raw.size());
        extractStringArray(raw, "piped_instances", cfg.piped_instances);
        extractStringArray(raw, "invidious_instances", cfg.invidious_instances);
        extractStringArray(raw, "quick_presets", cfg.quick_presets);
    }

    if (cfg.piped_instances.empty()) {
        cfg.piped_instances = {
            "https://pipedapi.kavin.rocks",
            "https://pipedapi.tokhmi.xyz",
            "https://pipedapi.Ativo.xyz",
            "https://api.piped.private.coffee"
        };
    }
    if (cfg.invidious_instances.empty()) {
        cfg.invidious_instances = {
            "https://inv.nadeko.net",
            "https://invidious.nerdvpn.de",
            "https://invidious.privacyredirect.com",
            "https://yewtu.be"
        };
    }
    if (cfg.quick_presets.empty()) {
        cfg.quick_presets = {
            "Lofi Hip Hop Radio",
            "Synthwave Gaming Mix",
            "Nintendo Switch OST Mix",
            "Rock Clasico Hits",
            "Bad Bunny Mix",
            "Anime Openings Mix"
        };
    }
}

void loadFavorites(std::vector<YtmTrackInfo>& favs) {
    favs.clear();
    std::string raw = readTextFile(YTM_PLAYLISTS_PATH);
    if (raw.empty()) return;

    size_t pos = 0;
    while (favs.size() < YTM_MAX_TRACKS) {
        size_t obj_start = raw.find('{', pos);
        if (obj_start == std::string::npos) break;
        size_t obj_end = raw.find('}', obj_start);
        if (obj_end == std::string::npos) break;

        std::string vid = extractJsonString(raw, "video_id", obj_start, obj_end);
        std::string title = extractJsonString(raw, "title", obj_start, obj_end);
        std::string artist = extractJsonString(raw, "artist", obj_start, obj_end);
        uint32_t dur = extractJsonUint(raw, "duration", obj_start, obj_end);

        if (!vid.empty() && !title.empty()) {
            YtmTrackInfo t = {};
            snprintf(t.video_id, sizeof(t.video_id), "%s", vid.c_str());
            snprintf(t.title, sizeof(t.title), "%s", title.c_str());
            snprintf(t.artist, sizeof(t.artist), "%s", artist.empty() ? "YouTube Music" : artist.c_str());
            t.duration_sec = dur;
            t.is_cached = isTrackCached(vid);
            favs.push_back(t);
        }
        pos = obj_end + 1;
    }
}

bool saveFavorite(const YtmTrackInfo& track) {
    if (track.video_id[0] == '\0') return false;
    std::vector<YtmTrackInfo> favs;
    loadFavorites(favs);

    for (const auto& existing : favs) {
        if (strcmp(existing.video_id, track.video_id) == 0) {
            return true; // Already in favorites
        }
    }
    if (favs.size() >= YTM_MAX_TRACKS) {
        favs.erase(favs.begin());
    }
    favs.push_back(track);

    FILE* f = fopen(YTM_PLAYLISTS_PATH, "wb");
    if (!f) return false;
    fprintf(f, "{\n  \"favorites\": [\n");
    for (size_t i = 0; i < favs.size(); i++) {
        fprintf(f,
                "    {\"video_id\": \"%s\", \"title\": \"%s\", \"artist\": \"%s\", \"duration\": %u}%s\n",
                favs[i].video_id,
                favs[i].title,
                favs[i].artist,
                favs[i].duration_sec,
                (i + 1 < favs.size()) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return true;
}

bool searchMusic(const YtmConfig& cfg, const std::string& query, std::vector<YtmTrackInfo>& results, std::string& err_out) {
    results.clear();
    std::string encoded = urlEncode(query);

    // 1. Try custom proxy first if configured
    if (!cfg.custom_proxy_url.empty()) {
        std::string url = cfg.custom_proxy_url + "/search?q=" + encoded + "&filter=music_songs";
        std::string body;
        if (httpGet(url, body)) {
            size_t pos = 0;
            while (results.size() < YTM_MAX_TRACKS) {
                size_t u_pos = body.find("\"url\"", pos);
                if (u_pos == std::string::npos) break;
                size_t obj_end = body.find('}', u_pos);
                if (obj_end == std::string::npos) break;
                std::string watch = extractJsonString(body, "url", u_pos, obj_end);
                size_t v_idx = watch.find("v=");
                std::string vid = (v_idx != std::string::npos) ? watch.substr(v_idx + 2, 11) : extractJsonString(body, "videoId", u_pos, obj_end);
                std::string title = extractJsonString(body, "title", u_pos, obj_end);
                std::string artist = extractJsonString(body, "uploaderName", u_pos, obj_end);
                if (!vid.empty() && !title.empty()) {
                    YtmTrackInfo t = {};
                    snprintf(t.video_id, sizeof(t.video_id), "%s", vid.c_str());
                    snprintf(t.title, sizeof(t.title), "%s", title.c_str());
                    snprintf(t.artist, sizeof(t.artist), "%s", artist.empty() ? "YT Music" : artist.c_str());
                    t.duration_sec = extractJsonUint(body, "duration", u_pos, obj_end);
                    t.is_cached = isTrackCached(vid);
                    results.push_back(t);
                }
                pos = obj_end + 1;
            }
            if (!results.empty()) return true;
        }
    }

    // 2. Try Piped API instances (filter=music_songs)
    for (const auto& inst : cfg.piped_instances) {
        std::string url = inst + "/search?q=" + encoded + "&filter=music_songs";
        std::string body;
        if (!httpGet(url, body)) continue;

        size_t pos = 0;
        while (results.size() < YTM_MAX_TRACKS) {
            size_t obj_start = body.find("{\"url\":", pos);
            if (obj_start == std::string::npos) obj_start = body.find("\"url\":\"/watch?v=", pos);
            if (obj_start == std::string::npos) break;
            size_t obj_end = body.find('}', obj_start);
            if (obj_end == std::string::npos) break;

            std::string watch = extractJsonString(body, "url", obj_start, obj_end);
            size_t v_idx = watch.find("v=");
            if (v_idx != std::string::npos && watch.size() >= v_idx + 13) {
                std::string vid = watch.substr(v_idx + 2, 11);
                std::string title = extractJsonString(body, "title", obj_start, obj_end);
                std::string artist = extractJsonString(body, "uploaderName", obj_start, obj_end);
                uint32_t dur = extractJsonUint(body, "duration", obj_start, obj_end);
                if (!title.empty()) {
                    YtmTrackInfo t = {};
                    snprintf(t.video_id, sizeof(t.video_id), "%s", vid.c_str());
                    snprintf(t.title, sizeof(t.title), "%s", title.c_str());
                    snprintf(t.artist, sizeof(t.artist), "%s", artist.empty() ? "YouTube Music" : artist.c_str());
                    t.duration_sec = dur;
                    t.is_cached = isTrackCached(vid);
                    results.push_back(t);
                }
            }
            pos = obj_end + 1;
        }
        if (!results.empty()) return true;
    }

    // 3. Fallback to Invidious API instances
    for (const auto& inst : cfg.invidious_instances) {
        std::string url = inst + "/api/v1/search?q=" + encoded + "&type=video";
        std::string body;
        if (!httpGet(url, body)) continue;

        size_t pos = 0;
        while (results.size() < YTM_MAX_TRACKS) {
            size_t vid_pos = body.find("\"videoId\"", pos);
            if (vid_pos == std::string::npos) break;
            size_t obj_start = (vid_pos > 120) ? (vid_pos - 120) : 0;
            size_t obj_end = body.find("\"videoThumbnails\"", vid_pos);
            if (obj_end == std::string::npos || obj_end > vid_pos + 1200) obj_end = vid_pos + 800;

            std::string vid = extractJsonString(body, "videoId", obj_start, obj_end);
            std::string title = extractJsonString(body, "title", obj_start, obj_end);
            std::string author = extractJsonString(body, "author", obj_start, obj_end);
            uint32_t dur = extractJsonUint(body, "lengthSeconds", obj_start, obj_end);

            if (vid.size() == 11 && !title.empty()) {
                YtmTrackInfo t = {};
                snprintf(t.video_id, sizeof(t.video_id), "%s", vid.c_str());
                snprintf(t.title, sizeof(t.title), "%s", title.c_str());
                snprintf(t.artist, sizeof(t.artist), "%s", author.empty() ? "YouTube Music" : author.c_str());
                t.duration_sec = dur;
                t.is_cached = isTrackCached(vid);
                results.push_back(t);
            }
            pos = vid_pos + 10;
        }
        if (!results.empty()) return true;
    }

    err_out = "Sin respuesta de instancias API (verifica Wi-Fi o config.json)";
    return false;
}

ResolvedStream resolveAudioStream(const YtmConfig& cfg, const std::string& video_id) {
    ResolvedStream res;

    // 1. Custom proxy if configured
    if (!cfg.custom_proxy_url.empty()) {
        std::string url = cfg.custom_proxy_url + "/streams/" + video_id;
        std::string body;
        if (httpGet(url, body)) {
            std::string direct = extractJsonString(body, "audio_url", 0, body.size());
            if (!direct.empty()) {
                res.ok = true;
                res.stream_url = direct;
                res.title = extractJsonString(body, "title", 0, body.size());
                res.artist = extractJsonString(body, "uploader", 0, body.size());
                res.duration_sec = extractJsonUint(body, "duration", 0, body.size());
                return res;
            }
        }
    }

    // 2. Piped instances: look for audioStreams with codec="opus" or mimeType="audio/webm"
    for (const auto& inst : cfg.piped_instances) {
        std::string url = inst + "/streams/" + video_id;
        std::string body;
        if (!httpGet(url, body)) continue;

        res.title = extractJsonString(body, "title", 0, 2048);
        res.artist = extractJsonString(body, "uploader", 0, 2048);
        res.duration_sec = extractJsonUint(body, "duration", 0, 2048);

        size_t as_pos = body.find("\"audioStreams\"");
        if (as_pos == std::string::npos) continue;

        size_t cur = as_pos;
        std::string fallback_url;
        while (true) {
            size_t obj_start = body.find('{', cur);
            if (obj_start == std::string::npos) break;
            size_t obj_end = body.find('}', obj_start);
            if (obj_end == std::string::npos) break;

            std::string mime = extractJsonString(body, "mimeType", obj_start, obj_end);
            std::string codec = extractJsonString(body, "codec", obj_start, obj_end);
            std::string stream_url = extractJsonString(body, "url", obj_start, obj_end);

            if (!stream_url.empty()) {
                if (mime.find("audio/webm") != std::string::npos || codec.find("opus") != std::string::npos) {
                    res.ok = true;
                    res.stream_url = stream_url;
                    return res;
                }
                if (fallback_url.empty()) fallback_url = stream_url;
            }
            cur = obj_end + 1;
        }
        if (!fallback_url.empty()) {
            res.ok = true;
            res.stream_url = fallback_url;
            return res;
        }
    }

    // 3. Invidious instances: look for adaptiveFormats with itag 251 / 250 / 249 (audio/webm; codecs="opus")
    for (const auto& inst : cfg.invidious_instances) {
        std::string url = inst + "/api/v1/videos/" + video_id;
        std::string body;
        if (!httpGet(url, body)) continue;

        res.title = extractJsonString(body, "title", 0, 2048);
        res.artist = extractJsonString(body, "author", 0, 2048);
        res.duration_sec = extractJsonUint(body, "lengthSeconds", 0, 2048);

        size_t af_pos = body.find("\"adaptiveFormats\"");
        if (af_pos == std::string::npos) continue;

        size_t cur = af_pos;
        while (true) {
            size_t obj_start = body.find('{', cur);
            if (obj_start == std::string::npos) break;
            size_t obj_end = body.find('}', obj_start);
            if (obj_end == std::string::npos) break;

            std::string type = extractJsonString(body, "type", obj_start, obj_end);
            std::string itag = extractJsonString(body, "itag", obj_start, obj_end);
            std::string stream_url = extractJsonString(body, "url", obj_start, obj_end);

            if (!stream_url.empty() &&
                (type.find("audio/webm") != std::string::npos || itag == "251" || itag == "250" || itag == "249")) {
                res.ok = true;
                res.stream_url = stream_url;
                return res;
            }
            cur = obj_end + 1;
        }
    }

    res.error_msg = "No se pudo obtener stream Opus de YouTube";
    return res;
}

} // namespace YtApi
