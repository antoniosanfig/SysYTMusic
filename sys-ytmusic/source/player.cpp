#include "player.hpp"
#include "webm_opus.hpp"
#include <curl/curl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 2;
constexpr int NUM_WAVEBUFS = 4;
constexpr size_t SAMPLES_PER_BUF = 4800; // 100ms per buffer at 48kHz
constexpr size_t BYTES_PER_BUF = SAMPLES_PER_BUF * CHANNELS * sizeof(int16_t);
constexpr size_t AUDIO_POOL_SIZE = (BYTES_PER_BUF * NUM_WAVEBUFS + 0xFFF) & ~0xFFF;

Mutex g_stateMutex;
YtmConfig g_cfg;
YtmSharedState g_state = {};
uint32_t g_lastCmdId = 0;

Thread g_audioThread;
Thread g_workerThread;
volatile bool g_running = false;
volatile bool g_abortCurrentStream = false;

std::deque<YtmTrackInfo> g_queue;
std::vector<YtmTrackInfo> g_history;

// PCM Ring Buffer between network/decoder and audren thread
Mutex g_pcmMutex;
std::deque<int16_t> g_pcmFifo;
constexpr size_t MAX_PCM_FIFO_SAMPLES = SAMPLE_RATE * CHANNELS * 6; // Max 6 seconds buffered in RAM (~1.1 MB)

// Async worker request
Mutex g_workMutex;
bool g_hasSearchWork = false;
std::string g_pendingSearchQuery;
bool g_hasStreamWork = false;
YtmTrackInfo g_pendingTrack = {};

void writeSharedStateToDisk() {
    mutexLock(&g_stateMutex);
    g_state.magic = YTM_MAGIC;
    g_state.seq++;
    YtmSharedState copy = g_state;
    mutexUnlock(&g_stateMutex);

    FILE* f = fopen(YTM_STATE_PATH ".tmp", "wb");
    if (f) {
        fwrite(&copy, 1, sizeof(YtmSharedState), f);
        fclose(f);
        remove(YTM_STATE_PATH);
        rename(YTM_STATE_PATH ".tmp", YTM_STATE_PATH);
    }
}

void setStatusMessage(uint8_t status, const char* msg) {
    mutexLock(&g_stateMutex);
    g_state.status = status;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg), "%s", msg ? msg : "");
    mutexUnlock(&g_stateMutex);
    writeSharedStateToDisk();
}

struct CurlStreamCtx {
    WebmOpusStreamDecoder* decoder;
    FILE* cache_file;
    uint64_t total_samples_decoded;
};

size_t curlAudioStreamCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    if (!g_running || g_abortCurrentStream) return 0; // Abort transfer immediately

    size_t total = size * nmemb;
    CurlStreamCtx* ctx = static_cast<CurlStreamCtx*>(userp);

    if (ctx->cache_file && total > 0) {
        fwrite(contents, 1, total, ctx->cache_file);
    }

    std::vector<int16_t> decoded_pcm;
    ctx->decoder->feed(static_cast<const uint8_t*>(contents), total, decoded_pcm);

    if (!decoded_pcm.empty()) {
        // Throttle network download if PCM FIFO is full (keeps RAM usage low)
        while (g_running && !g_abortCurrentStream) {
            mutexLock(&g_pcmMutex);
            size_t current_size = g_pcmFifo.size();
            mutexUnlock(&g_pcmMutex);
            if (current_size + decoded_pcm.size() <= MAX_PCM_FIFO_SAMPLES) break;
            svcSleepThread(20'000'000ULL); // Sleep 20ms
        }

        mutexLock(&g_pcmMutex);
        g_pcmFifo.insert(g_pcmFifo.end(), decoded_pcm.begin(), decoded_pcm.end());
        mutexUnlock(&g_pcmMutex);

        mutexLock(&g_stateMutex);
        if (g_state.status == YTM_STATUS_BUFFERING && g_pcmFifo.size() >= SAMPLES_PER_BUF * CHANNELS) {
            g_state.status = YTM_STATUS_PLAYING;
            snprintf(g_state.status_msg, sizeof(g_state.status_msg), "Reproduciendo (YouTube Opus)");
        }
        mutexUnlock(&g_stateMutex);
    }

    return total;
}

void playFromCachedFile(const std::string& path, WebmOpusStreamDecoder& decoder) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;

    setStatusMessage(YTM_STATUS_PLAYING, "Reproduciendo desde Cache SD");

    uint8_t buf[16384];
    std::vector<int16_t> decoded_pcm;
    while (g_running && !g_abortCurrentStream) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;

        decoded_pcm.clear();
        decoder.feed(buf, n, decoded_pcm);

        if (!decoded_pcm.empty()) {
            while (g_running && !g_abortCurrentStream) {
                mutexLock(&g_pcmMutex);
                size_t sz = g_pcmFifo.size();
                mutexUnlock(&g_pcmMutex);
                if (sz + decoded_pcm.size() <= MAX_PCM_FIFO_SAMPLES) break;
                svcSleepThread(20'000'000ULL);
            }
            mutexLock(&g_pcmMutex);
            g_pcmFifo.insert(g_pcmFifo.end(), decoded_pcm.begin(), decoded_pcm.end());
            mutexUnlock(&g_pcmMutex);
        }
    }
    fclose(f);
}

void streamTrackWorker(const YtmTrackInfo& track) {
    mutexLock(&g_pcmMutex);
    g_pcmFifo.clear();
    mutexUnlock(&g_pcmMutex);

    mutexLock(&g_stateMutex);
    g_state.current_track = track;
    g_state.elapsed_sec = 0;
    mutexUnlock(&g_stateMutex);

    WebmOpusStreamDecoder decoder;
    if (!decoder.init()) {
        setStatusMessage(YTM_STATUS_ERROR, "Error al iniciar decodificador Opus");
        return;
    }

    std::string vid(track.video_id);
    if (YtApi::isTrackCached(vid)) {
        mutexLock(&g_stateMutex);
        g_state.current_track.is_cached = true;
        mutexUnlock(&g_stateMutex);
        playFromCachedFile(YtApi::getCachePath(vid), decoder);
    } else {
        setStatusMessage(YTM_STATUS_BUFFERING, "Obteniendo stream de YouTube Music...");
        ResolvedStream resolved = YtApi::resolveAudioStream(g_cfg, vid);
        if (!resolved.ok || g_abortCurrentStream) {
            if (!g_abortCurrentStream) {
                setStatusMessage(YTM_STATUS_ERROR, resolved.error_msg.c_str());
            }
            return;
        }

        // Update metadata if resolved from video ID directly
        mutexLock(&g_stateMutex);
        if (g_state.current_track.title[0] == '\0' && !resolved.title.empty()) {
            snprintf(g_state.current_track.title, sizeof(g_state.current_track.title), "%s", resolved.title.c_str());
        }
        if (g_state.current_track.artist[0] == '\0' && !resolved.artist.empty()) {
            snprintf(g_state.current_track.artist, sizeof(g_state.current_track.artist), "%s", resolved.artist.c_str());
        }
        if (g_state.current_track.duration_sec == 0 && resolved.duration_sec > 0) {
            g_state.current_track.duration_sec = resolved.duration_sec;
        }
        bool should_cache = g_state.cache_enabled &&
                            g_state.current_track.duration_sec > 0 &&
                            g_state.current_track.duration_sec <= 900; // Solo cachear canciones <= 15 min (nunca radios en vivo)
        mutexUnlock(&g_stateMutex);
        writeSharedStateToDisk();

        std::string tmp_cache = YtApi::getCachePath(vid) + ".tmp";
        FILE* cache_fp = should_cache ? fopen(tmp_cache.c_str(), "wb") : nullptr;

        CurlStreamCtx ctx = { &decoder, cache_fp, 0 };
        CURL* curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, resolved.stream_url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlAudioStreamCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

            CURLcode res = curl_easy_perform(curl);
            curl_easy_cleanup(curl);

            if (cache_fp) {
                fclose(cache_fp);
                if (res == CURLE_OK && !g_abortCurrentStream) {
                    std::string final_cache = YtApi::getCachePath(vid);
                    remove(final_cache.c_str());
                    rename(tmp_cache.c_str(), final_cache.c_str());
                    YtApi::enforceCacheLimit(g_cfg.max_cache_mb);
                    uint32_t used_kb = YtApi::getCacheUsedKb();
                    mutexLock(&g_stateMutex);
                    g_state.current_track.is_cached = true;
                    g_state.cache_used_kb = used_kb;
                    mutexUnlock(&g_stateMutex);
                } else {
                    remove(tmp_cache.c_str());
                }
            }
        }
    }

    // Wait for remaining buffered PCM samples to finish playing
    while (g_running && !g_abortCurrentStream) {
        mutexLock(&g_pcmMutex);
        bool empty = g_pcmFifo.empty();
        mutexUnlock(&g_pcmMutex);
        if (empty) break;
        svcSleepThread(50'000'000ULL);
    }

    if (!g_abortCurrentStream && g_running) {
        mutexLock(&g_stateMutex);
        bool repeat = g_state.repeat_one;
        YtmTrackInfo cur = g_state.current_track;
        mutexUnlock(&g_stateMutex);

        if (repeat) {
            Player::playTrack(cur);
        } else if (!g_queue.empty()) {
            YtmTrackInfo next_t = g_queue.front();
            g_queue.pop_front();
            Player::playTrack(next_t);
        } else {
            setStatusMessage(YTM_STATUS_STOPPED, "Listo");
        }
    }
}

void workerThreadFunc(void* arg) {
    (void)arg;
    while (g_running) {
        bool do_search = false;
        std::string query;
        bool do_stream = false;
        YtmTrackInfo track = {};

        mutexLock(&g_workMutex);
        if (g_hasSearchWork) {
            do_search = true;
            query = g_pendingSearchQuery;
            g_hasSearchWork = false;
        } else if (g_hasStreamWork) {
            do_stream = true;
            track = g_pendingTrack;
            g_hasStreamWork = false;
            g_abortCurrentStream = false;
        }
        mutexUnlock(&g_workMutex);

        if (do_search) {
            setStatusMessage(YTM_STATUS_SEARCHING, "Buscando en YouTube Music...");
            std::vector<YtmTrackInfo> results;
            std::string err;
            bool ok = YtApi::searchMusic(g_cfg, query, results, err);

            mutexLock(&g_stateMutex);
            snprintf(g_state.last_query, sizeof(g_state.last_query), "%s", query.c_str());
            g_state.search_count = (uint8_t)results.size();
            for (size_t i = 0; i < results.size() && i < YTM_MAX_TRACKS; i++) {
                g_state.search_results[i] = results[i];
            }
            if (g_state.status == YTM_STATUS_SEARCHING) {
                g_state.status = (g_pcmFifo.empty()) ? YTM_STATUS_STOPPED : YTM_STATUS_PLAYING;
            }
            snprintf(g_state.status_msg, sizeof(g_state.status_msg), "%s",
                     ok ? "Resultados listos" : err.c_str());
            mutexUnlock(&g_stateMutex);
            writeSharedStateToDisk();
        } else if (do_stream) {
            streamTrackWorker(track);
        } else {
            svcSleepThread(40'000'000ULL); // 40ms idle
        }
    }
}

void audioThreadFunc(void* arg) {
    (void)arg;
    static const AudioRendererConfig arConfig = {
        .output_rate     = AudioRendererOutputRate_48kHz,
        .num_voices      = 4,
        .num_effects     = 0,
        .num_sinks       = 1,
        .num_mix_objs    = 1,
        .num_mix_buffers = 2,
    };

    if (R_FAILED(audrenInitialize(&arConfig))) return;

    AudioDriver drv;
    if (R_FAILED(audrvCreate(&drv, &arConfig, 2))) {
        audrenExit();
        return;
    }

    void* mempool_buf = memalign(AUDREN_MEMPOOL_ALIGNMENT, AUDIO_POOL_SIZE);
    if (!mempool_buf) {
        audrvClose(&drv);
        audrenExit();
        return;
    }
    memset(mempool_buf, 0, AUDIO_POOL_SIZE);

    int mpid = audrvMemPoolAdd(&drv, mempool_buf, AUDIO_POOL_SIZE);
    audrvMemPoolAttach(&drv, mpid);

    static const uint8_t sink_channels[] = { 0, 1 };
    audrvDeviceSinkAdd(&drv, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels);
    audrvUpdate(&drv);
    audrenStartAudioRenderer();

    audrvVoiceInit(&drv, 0, CHANNELS, PcmFormat_Int16, SAMPLE_RATE);
    audrvVoiceSetDestinationMix(&drv, 0, AUDREN_FINAL_MIX_ID);
    audrvVoiceSetMixFactor(&drv, 0, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&drv, 0, 1.0f, 1, 1);
    audrvVoiceStart(&drv, 0);

    AudioDriverWaveBuf wavebufs[NUM_WAVEBUFS] = {};
    for (int i = 0; i < NUM_WAVEBUFS; i++) {
        wavebufs[i].data_pcm16 = static_cast<int16_t*>(mempool_buf);
        wavebufs[i].size = AUDIO_POOL_SIZE;
        wavebufs[i].start_sample_offset = i * SAMPLES_PER_BUF;
        wavebufs[i].end_sample_offset = (i + 1) * SAMPLES_PER_BUF;
        wavebufs[i].state = AudioDriverWaveBufState_Done;
    }

    uint64_t played_samples = 0;
    uint32_t tick_counter = 0;

    while (g_running) {
        mutexLock(&g_stateMutex);
        bool paused = (g_state.status == YTM_STATUS_PAUSED);
        float vol = (float)g_state.volume_percent / 100.0f;
        mutexUnlock(&g_stateMutex);

        audrvVoiceSetVolume(&drv, 0, paused ? 0.0f : vol);

        if (!paused) {
            for (int i = 0; i < NUM_WAVEBUFS; i++) {
                if (wavebufs[i].state == AudioDriverWaveBufState_Done ||
                    wavebufs[i].state == AudioDriverWaveBufState_Free) {
                    int16_t* dst = static_cast<int16_t*>(mempool_buf) + (i * SAMPLES_PER_BUF * CHANNELS);
                    size_t needed = SAMPLES_PER_BUF * CHANNELS;

                    mutexLock(&g_pcmMutex);
                    if (g_pcmFifo.size() >= needed) {
                        for (size_t s = 0; s < needed; s++) {
                            dst[s] = g_pcmFifo.front();
                            g_pcmFifo.pop_front();
                        }
                        mutexUnlock(&g_pcmMutex);

                        armDCacheFlush(dst, BYTES_PER_BUF);
                        audrvVoiceAppendWaveBuf(&drv, 0, &wavebufs[i]);
                        played_samples += SAMPLES_PER_BUF;
                    } else {
                        mutexUnlock(&g_pcmMutex);
                    }
                }
            }
        }

        audrvUpdate(&drv);
        audrenWaitFrame();

        tick_counter++;
        if (tick_counter % 20 == 0) {
            mutexLock(&g_stateMutex);
            if (g_state.status == YTM_STATUS_PLAYING) {
                g_state.elapsed_sec = (uint32_t)(played_samples / SAMPLE_RATE);
            } else if (g_state.status == YTM_STATUS_BUFFERING) {
                played_samples = 0;
            }
            mutexUnlock(&g_stateMutex);
            writeSharedStateToDisk();
        }
    }

    audrvClose(&drv);
    audrenExit();
    free(mempool_buf);
}

} // namespace

namespace Player {

bool init(const YtmConfig& cfg) {
    mutexInit(&g_stateMutex);
    mutexInit(&g_pcmMutex);
    mutexInit(&g_workMutex);

    g_cfg = cfg;
    memset(&g_state, 0, sizeof(g_state));
    g_state.magic = YTM_MAGIC;
    g_state.status = YTM_STATUS_STOPPED;
    g_state.volume_percent = cfg.default_volume;
    g_state.cache_enabled = cfg.cache_enabled;
    g_state.cache_used_kb = YtApi::getCacheUsedKb();
    g_state.web_port = cfg.web_port;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg), "Listo para reproducir");
    snprintf(g_state.switch_ip, sizeof(g_state.switch_ip), "127.0.0.1");

    // Populate quick search presets
    g_state.preset_count = (uint8_t)(cfg.quick_presets.size() > 8 ? 8 : cfg.quick_presets.size());
    for (uint8_t i = 0; i < g_state.preset_count; i++) {
        snprintf(g_state.presets[i], sizeof(g_state.presets[i]), "%s", cfg.quick_presets[i].c_str());
    }

    // Load favorites
    std::vector<YtmTrackInfo> favs;
    YtApi::loadFavorites(favs);
    g_state.fav_count = (uint8_t)favs.size();
    for (size_t i = 0; i < favs.size() && i < YTM_MAX_TRACKS; i++) {
        g_state.favorites[i] = favs[i];
    }

    writeSharedStateToDisk();

    g_running = true;
    threadCreate(&g_audioThread, audioThreadFunc, nullptr, nullptr, 0x8000, 0x2B, 3);
    threadStart(&g_audioThread);

    threadCreate(&g_workerThread, workerThreadFunc, nullptr, nullptr, 0x10000, 0x2F, 3);
    threadStart(&g_workerThread);

    return true;
}

void exit() {
    g_running = false;
    g_abortCurrentStream = true;
    threadWaitForExit(&g_workerThread);
    threadClose(&g_workerThread);
    threadWaitForExit(&g_audioThread);
    threadClose(&g_audioThread);
}

void playTrack(const YtmTrackInfo& track) {
    mutexLock(&g_stateMutex);
    if (g_state.current_track.video_id[0] != '\0') {
        g_history.push_back(g_state.current_track);
    }
    mutexUnlock(&g_stateMutex);

    g_abortCurrentStream = true;
    mutexLock(&g_workMutex);
    g_pendingTrack = track;
    g_hasStreamWork = true;
    mutexUnlock(&g_workMutex);
}

void enqueueTrack(const YtmTrackInfo& track) {
    mutexLock(&g_stateMutex);
    bool idle = (g_state.status == YTM_STATUS_STOPPED);
    mutexUnlock(&g_stateMutex);

    if (idle) {
        playTrack(track);
    } else {
        g_queue.push_back(track);
        setStatusMessage(g_state.status, "Cancion agregada a la cola");
    }
}

void togglePlayPause() {
    mutexLock(&g_stateMutex);
    if (g_state.status == YTM_STATUS_PLAYING) {
        g_state.status = YTM_STATUS_PAUSED;
        snprintf(g_state.status_msg, sizeof(g_state.status_msg), "Pausado");
    } else if (g_state.status == YTM_STATUS_PAUSED) {
        g_state.status = YTM_STATUS_PLAYING;
        snprintf(g_state.status_msg, sizeof(g_state.status_msg), "Reproduciendo");
    }
    mutexUnlock(&g_stateMutex);
    writeSharedStateToDisk();
}

void stop() {
    g_abortCurrentStream = true;
    mutexLock(&g_pcmMutex);
    g_pcmFifo.clear();
    mutexUnlock(&g_pcmMutex);
    setStatusMessage(YTM_STATUS_STOPPED, "Detenido");
}

void next() {
    if (!g_queue.empty()) {
        YtmTrackInfo t = g_queue.front();
        g_queue.pop_front();
        playTrack(t);
    } else {
        // Play next from search results or favorites if available
        mutexLock(&g_stateMutex);
        if (g_state.search_count > 0) {
            YtmTrackInfo t = g_state.search_results[0];
            for (uint8_t i = 0; i + 1 < g_state.search_count; i++) {
                if (strcmp(g_state.search_results[i].video_id, g_state.current_track.video_id) == 0) {
                    t = g_state.search_results[i + 1];
                    break;
                }
            }
            mutexUnlock(&g_stateMutex);
            playTrack(t);
            return;
        }
        mutexUnlock(&g_stateMutex);
    }
}

void prev() {
    if (!g_history.empty()) {
        YtmTrackInfo t = g_history.back();
        g_history.pop_back();
        playTrack(t);
    }
}

void setVolume(uint8_t percent) {
    if (percent > 150) percent = 150;
    mutexLock(&g_stateMutex);
    g_state.volume_percent = percent;
    mutexUnlock(&g_stateMutex);
    writeSharedStateToDisk();
}

void toggleRepeat() {
    mutexLock(&g_stateMutex);
    g_state.repeat_one = !g_state.repeat_one;
    mutexUnlock(&g_stateMutex);
    writeSharedStateToDisk();
}

void toggleCache() {
    mutexLock(&g_stateMutex);
    g_state.cache_enabled = !g_state.cache_enabled;
    g_cfg.cache_enabled = g_state.cache_enabled;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg),
             g_state.cache_enabled ? "Cache SD: ACTIVADA (max %u MB)" : "Cache SD: DESACTIVADA",
             g_cfg.max_cache_mb);
    mutexUnlock(&g_stateMutex);
    writeSharedStateToDisk();
}

void clearCache() {
    YtApi::clearAllCache();
    uint32_t used_kb = YtApi::getCacheUsedKb();
    mutexLock(&g_stateMutex);
    g_state.cache_used_kb = used_kb;
    g_state.current_track.is_cached = false;
    for (uint8_t i = 0; i < g_state.search_count; i++) g_state.search_results[i].is_cached = false;
    for (uint8_t i = 0; i < g_state.fav_count; i++) g_state.favorites[i].is_cached = false;
    snprintf(g_state.status_msg, sizeof(g_state.status_msg), "Cache SD eliminada (0 MB)");
    mutexUnlock(&g_stateMutex);
    writeSharedStateToDisk();
}

void triggerSearch(const std::string& query) {
    if (query.empty()) return;
    mutexLock(&g_workMutex);
    g_pendingSearchQuery = query;
    g_hasSearchWork = true;
    mutexUnlock(&g_workMutex);
}

void playSearchResult(uint8_t index) {
    mutexLock(&g_stateMutex);
    if (index >= g_state.search_count) {
        mutexUnlock(&g_stateMutex);
        return;
    }
    YtmTrackInfo t = g_state.search_results[index];
    mutexUnlock(&g_stateMutex);
    playTrack(t);
}

void playFavoriteResult(uint8_t index) {
    mutexLock(&g_stateMutex);
    if (index >= g_state.fav_count) {
        mutexUnlock(&g_stateMutex);
        return;
    }
    YtmTrackInfo t = g_state.favorites[index];
    mutexUnlock(&g_stateMutex);
    playTrack(t);
}

void saveCurrentToFavorites() {
    mutexLock(&g_stateMutex);
    YtmTrackInfo cur = g_state.current_track;
    mutexUnlock(&g_stateMutex);

    if (cur.video_id[0] != '\0') {
        YtApi::saveFavorite(cur);
        std::vector<YtmTrackInfo> favs;
        YtApi::loadFavorites(favs);
        mutexLock(&g_stateMutex);
        g_state.fav_count = (uint8_t)favs.size();
        for (size_t i = 0; i < favs.size() && i < YTM_MAX_TRACKS; i++) {
            g_state.favorites[i] = favs[i];
        }
        snprintf(g_state.status_msg, sizeof(g_state.status_msg), "Guardado en Favoritos!");
        mutexUnlock(&g_stateMutex);
        writeSharedStateToDisk();
    }
}

void setSwitchIp(const char* ip) {
    mutexLock(&g_stateMutex);
    snprintf(g_state.switch_ip, sizeof(g_state.switch_ip), "%s", ip ? ip : "127.0.0.1");
    mutexUnlock(&g_stateMutex);
    writeSharedStateToDisk();
}

void syncSharedState() {
    writeSharedStateToDisk();
}

YtmSharedState getSnapshot() {
    mutexLock(&g_stateMutex);
    YtmSharedState copy = g_state;
    mutexUnlock(&g_stateMutex);
    return copy;
}

void pollIpcCommands() {
    FILE* f = fopen(YTM_CMD_PATH, "rb");
    if (!f) return;

    YtmCommandPacket pkt = {};
    size_t n = fread(&pkt, 1, sizeof(pkt), f);
    fclose(f);

    if (n != sizeof(pkt) || pkt.magic != YTM_MAGIC || pkt.cmd_id == g_lastCmdId) {
        return;
    }
    g_lastCmdId = pkt.cmd_id;

    switch (pkt.cmd_type) {
        case YTM_CMD_PLAY_PAUSE:
            togglePlayPause();
            break;
        case YTM_CMD_STOP:
            stop();
            break;
        case YTM_CMD_NEXT:
            next();
            break;
        case YTM_CMD_PREV:
            prev();
            break;
        case YTM_CMD_SET_VOLUME:
            setVolume(pkt.param_u8);
            break;
        case YTM_CMD_PLAY_VIDEO_ID: {
            YtmTrackInfo t = {};
            snprintf(t.video_id, sizeof(t.video_id), "%s", pkt.video_id);
            snprintf(t.title, sizeof(t.title), "%s", pkt.title);
            snprintf(t.artist, sizeof(t.artist), "%s", pkt.artist);
            playTrack(t);
            break;
        }
        case YTM_CMD_QUEUE_VIDEO_ID: {
            YtmTrackInfo t = {};
            snprintf(t.video_id, sizeof(t.video_id), "%s", pkt.video_id);
            snprintf(t.title, sizeof(t.title), "%s", pkt.title);
            snprintf(t.artist, sizeof(t.artist), "%s", pkt.artist);
            enqueueTrack(t);
            break;
        }
        case YTM_CMD_SEARCH:
            triggerSearch(pkt.query);
            break;
        case YTM_CMD_PLAY_SEARCH_IX:
            playSearchResult(pkt.param_u8);
            break;
        case YTM_CMD_PLAY_FAV_IX:
            playFavoriteResult(pkt.param_u8);
            break;
        case YTM_CMD_SAVE_CURRENT:
            saveCurrentToFavorites();
            break;
        case YTM_CMD_TOGGLE_REPEAT:
            toggleRepeat();
            break;
        case YTM_CMD_TOGGLE_CACHE:
            toggleCache();
            break;
        case YTM_CMD_CLEAR_CACHE:
            clearCache();
            break;
        default:
            break;
    }
}

} // namespace Player
