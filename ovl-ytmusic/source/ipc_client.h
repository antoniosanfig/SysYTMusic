#pragma once
#include <stdio.h>
#include <string.h>
#include "../../common/ipc_cmd.h"

namespace YtmClient {

inline bool readState(YtmSharedState& out) {
    FILE* f = fopen(YTM_STATE_PATH, "rb");
    if (!f) return false;
    size_t n = fread(&out, 1, sizeof(YtmSharedState), f);
    fclose(f);
    return (n == sizeof(YtmSharedState) && out.magic == YTM_MAGIC);
}

inline uint32_t nextCmdId() {
    static uint32_t s_id = 0;
    if (s_id == 0) {
        FILE* f = fopen(YTM_CMD_PATH, "rb");
        if (f) {
            YtmCommandPacket prev = {};
            if (fread(&prev, 1, sizeof(prev), f) == sizeof(prev) && prev.magic == YTM_MAGIC) {
                s_id = prev.cmd_id;
            }
            fclose(f);
        }
    }
    return ++s_id;
}

inline bool sendCommand(uint8_t cmd_type, uint8_t param_u8 = 0, const char* query = nullptr) {
    YtmCommandPacket pkt = {};
    pkt.magic = YTM_MAGIC;
    pkt.cmd_id = nextCmdId();
    pkt.cmd_type = cmd_type;
    pkt.param_u8 = param_u8;
    if (query) {
        snprintf(pkt.query, sizeof(pkt.query), "%s", query);
    }

    FILE* f = fopen(YTM_CMD_PATH ".tmp", "wb");
    if (!f) return false;
    fwrite(&pkt, 1, sizeof(pkt), f);
    fclose(f);
    remove(YTM_CMD_PATH);
    rename(YTM_CMD_PATH ".tmp", YTM_CMD_PATH);
    return true;
}

} // namespace YtmClient
