#pragma once
#include <switch.h>
#include <string>
#include <vector>
#include <deque>
#include "../../common/ipc_cmd.h"
#include "yt_api.hpp"

namespace Player {
    bool init(const YtmConfig& cfg);
    void exit();

    void playTrack(const YtmTrackInfo& track);
    void enqueueTrack(const YtmTrackInfo& track);
    void togglePlayPause();
    void stop();
    void next();
    void prev();
    void setVolume(uint8_t percent);
    void toggleRepeat();
    void toggleCache();
    void clearCache();

    void triggerSearch(const std::string& query);
    void playSearchResult(uint8_t index);
    void playFavoriteResult(uint8_t index);
    void saveCurrentToFavorites();

    void setSwitchIp(const char* ip);
    void syncSharedState();
    void pollIpcCommands();
    YtmSharedState getSnapshot();
}
