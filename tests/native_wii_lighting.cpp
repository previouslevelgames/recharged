#include <aurora/aurora.h>
#include <dolphin/os.h>
#include <dolphin/gx.h>
#include "platform/interrupt_controller.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdexcept>

namespace aurora { extern AuroraConfig g_config; }
namespace aurora::gx::fifo { void init(); void shutdown(); }

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("usage: hardware52 CAPTURE_PATH");
        aurora::g_config.mem1Size = MEM1_DEFAULT_SIZE;
        aurora::g_config.mem2Size = 64u * 1024u * 1024u;
        OSInit();
        mscharged::platform::InitializeNativeInterruptController();
        aurora::gx::fifo::init();
        alignas(32) static unsigned char fifo[0x80000];
        if (!GXInit(fifo, sizeof fifo)) throw std::runtime_error("Real canonical GX init failed");

        // Generated, fully initialized hardware descriptors. These are not game
        // light/camera/actor state; no original Activate/Draw method executes.
        const float directions[][3] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
            {0, -1, 0}, {0, 0, 1}, {0, 0, -1}
        };
        const GXLightID slots[] = {GX_LIGHT0, GX_LIGHT1, GX_LIGHT2, GX_LIGHT3, GX_LIGHT6, GX_LIGHT7};
        auto* file = std::fopen(argv[1], "wb");
        if (!file) throw std::runtime_error("Hardware capture open failed");
        unsigned count = 0;
        for (unsigned i = 0; i < 6; ++i)
        {
            GXLightObj light{};
            GXInitLightColor(&light, GXColor{0x12, 0x34, 0x56, static_cast<unsigned char>(0x78 + i)});
            GXInitLightAttn(&light, 0, 0, 1, 0, 0, 1);
            GXInitSpecularDir(&light, directions[i][0], directions[i][1], directions[i][2]);
            alignas(32) unsigned char capture[2112];
            std::memset(capture, 0xA5, sizeof capture);
            GXBeginDisplayList(capture + 32, 2048);
            GXLoadLightObjImm(&light, slots[i]);
            const unsigned bytes = GXEndDisplayList();
            if (bytes != 96) throw std::runtime_error("Original 16-word light packet changed length");
            for (unsigned guard = 0; guard < 32; ++guard)
                if (capture[guard] != 0xA5 || capture[2080 + guard] != 0xA5)
                    throw std::runtime_error("Actual GX light capture exceeded its bounds");
            if (std::fwrite(capture + 32, 1, bytes, file) != bytes)
                throw std::runtime_error("Hardware capture write failed");
            ++count;
        }
        if (std::fclose(file)) throw std::runtime_error("Hardware capture close failed");
        aurora::gx::fifo::shutdown();
        std::printf("Native Wii lighting provider: %u generated axis descriptors, actual GX XF bytes/guards only; no game state, GPU, frame, Activate/Draw or CRT teardown claim.\n", count);
        std::fflush(nullptr);
        std::_Exit(0);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        std::fflush(nullptr);
        std::_Exit(1);
    }
}
