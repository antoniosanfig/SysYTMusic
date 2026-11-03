#pragma once
#include <stdint.h>
#include <stdbool.h>

#define YTM_MAGIC 0x59544D31 // "YTM1"
#define YTM_MAX_TRACKS 12
#define YTM_TITLE_LEN  96
#define YTM_ARTIST_LEN 64
#define YTM_ID_LEN     24

#define YTM_BASE_DIR       "sdmc:/config/sys-ytmusic"
#define YTM_CACHE_DIR      "sdmc:/config/sys-ytmusic/cache"
#define YTM_CONFIG_PATH    "sdmc:/config/sys-ytmusic/config.json"
#define YTM_PLAYLISTS_PATH "sdmc:/config/sys-ytmusic/playlists.json"
#define YTM_STATE_PATH     "sdmc:/config/sys-ytmusic/ipc_state.bin"
#define YTM_CMD_PATH       "sdmc:/config/sys-ytmusic/ipc_cmd.bin"

enum YtmPlayStatus : uint8_t {
    YTM_STATUS_STOPPED   = 0,
    YTM_STATUS_BUFFERING = 1,
    YTM_STATUS_PLAYING   = 2,
    YTM_STATUS_PAUSED    = 3,
    YTM_STATUS_SEARCHING = 4,
    YTM_STATUS_ERROR     = 5
};

enum YtmCmdType : uint8_t {
    YTM_CMD_NONE           = 0,
    YTM_CMD_PLAY_PAUSE     = 1,
    YTM_CMD_STOP           = 2,
    YTM_CMD_NEXT           = 3,
    YTM_CMD_PREV           = 4,
    YTM_CMD_SET_VOLUME     = 5,
    YTM_CMD_PLAY_VIDEO_ID  = 6,
    YTM_CMD_QUEUE_VIDEO_ID = 7,
    YTM_CMD_SEARCH         = 8,
    YTM_CMD_PLAY_SEARCH_IX = 9,
    YTM_CMD_PLAY_FAV_IX    = 10,
    YTM_CMD_SAVE_CURRENT   = 11,
    YTM_CMD_TOGGLE_REPEAT  = 12,
    YTM_CMD_TOGGLE_CACHE   = 13,
    YTM_CMD_CLEAR_CACHE    = 14
};

struct YtmTrackInfo {
    char video_id[YTM_ID_LEN];
    char title[YTM_TITLE_LEN];
    char artist[YTM_ARTIST_LEN];
    uint32_t duration_sec;
    bool is_cached;
};

struct YtmSharedState {
    uint32_t magic;
    uint32_t seq;
    uint8_t status;          // YtmPlayStatus
    uint8_t volume_percent;  // 0 .. 150
    bool repeat_one;
    bool cache_enabled;
    uint32_t cache_used_kb;
    char status_msg[96];
    char switch_ip[32];
    uint16_t web_port;

    // Current playing track
    YtmTrackInfo current_track;
    uint32_t elapsed_sec;

    // Search results (populated when user searches from Overlay or Web UI)
    char last_query[64];
    uint8_t search_count;
    YtmTrackInfo search_results[YTM_MAX_TRACKS];

    // Favorites / Playlist items loaded from playlists.json
    uint8_t fav_count;
    YtmTrackInfo favorites[YTM_MAX_TRACKS];

    // Quick search presets from config.json
    uint8_t preset_count;
    char presets[8][48];
};

struct YtmCommandPacket {
    uint32_t magic;
    uint32_t cmd_id;         // Incremented on each new command
    uint8_t cmd_type;        // YtmCmdType
    uint8_t param_u8;        // Volume or index
    char video_id[YTM_ID_LEN];
    char query[64];
    char title[YTM_TITLE_LEN];
    char artist[YTM_ARTIST_LEN];
};
