#include <switch.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "../../common/ipc_cmd.h"
#include "yt_api.hpp"
#include "player.hpp"
#include "web_remote.hpp"

extern "C" {
    // Sysmodules should not use applet hooks
    u32 __nx_applet_type = AppletType_None;
    u32 __nx_fs_num_sessions = 1;

    // 5 MB inner heap to coexist peacefully with heavy Switch games
    #define INNER_HEAP_SIZE 0x500000
    size_t nx_inner_heap_size = INNER_HEAP_SIZE;
    char   nx_inner_heap[INNER_HEAP_SIZE];

    void __libnx_initheap(void) {
        void*  addr = nx_inner_heap;
        size_t size = nx_inner_heap_size;
        extern char* fake_heap_start;
        extern char* fake_heap_end;
        fake_heap_start = (char*)addr;
        fake_heap_end   = (char*)addr + size;
    }

    void __appInit(void) {
        Result rc = smInitialize();
        if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }

        rc = fsInitialize();
        if (R_FAILED(rc)) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
        fsdevMountSdmc();

        // Compact socket configuration for sysmodule footprint (~512 KB total socket transfer memory)
        SocketInitConfig sockCfg = *socketGetDefaultInitConfig();
        sockCfg.tcp_tx_buf_size = 0x8000;
        sockCfg.tcp_rx_buf_size = 0x10000;
        sockCfg.tcp_tx_buf_max_size = 0x20000;
        sockCfg.tcp_rx_buf_max_size = 0x40000;
        sockCfg.udp_tx_buf_size = 0x2400;
        sockCfg.udp_rx_buf_size = 0xA500;
        sockCfg.sb_efficiency = 2;
        sockCfg.num_bsd_sessions = 3;
        socketInitialize(&sockCfg);
        nifmInitialize(NifmServiceType_User);
        timeInitialize();
        csrngInitialize();
    }

    void __appExit(void) {
        csrngExit();
        timeExit();
        nifmExit();
        socketExit();
        fsdevUnmountAll();
        fsExit();
        smExit();
    }
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    mkdir("sdmc:/config", 0777);
    mkdir(YTM_BASE_DIR, 0777);
    mkdir(YTM_CACHE_DIR, 0777);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    YtmConfig cfg;
    YtApi::loadConfig(cfg);

    Player::init(cfg);
    WebRemote::start(cfg.web_port);

    // Main loop: poll IPC commands from the Tesla Overlay every 50ms
    while (true) {
        Player::pollIpcCommands();
        svcSleepThread(50'000'000ULL);
    }

    WebRemote::stop();
    Player::exit();
    curl_global_cleanup();
    return 0;
}
