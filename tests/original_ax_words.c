#include <revolution/ax.h>
#include <revolution/os.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(AXPB) == 320, "Original DSP PB footprint");
_Static_assert(offsetof(AXPB, addr) == 0x6e, "Original address-field offset");
_Static_assert(offsetof(AXPB, adpcm) == 0x7e, "Original ADPCM-field offset");
_Static_assert(offsetof(AXPBADPCM, gain) == 0x20, "Original gain high-word field");
_Static_assert(offsetof(AXPBADPCM, pred_scale) == 0x22, "Original predictor low-word field");
_Static_assert(sizeof(AXSTUDIO) == 120, "Original packed studio footprint");
_Static_assert(offsetof(AXSTUDIO, R) == 6, "Original unaligned packed value");
_Static_assert(offsetof(AXSTUDIO, Aux0) == 78, "Original interleaved remote fields");

static void check(int condition, const char* reason) {
    if (!condition) {
        fprintf(stderr, "Original AX word qualifier failed: %s\n", reason);
        exit(1);
    }
}

static uint16_t read_be16(const unsigned char* bytes) {
    return (uint16_t)((uint16_t)bytes[0] * 256 + bytes[1]);
}

static uint32_t read_be32(const unsigned char* bytes) {
    return ((uint32_t)read_be16(bytes) << 16) + read_be16(bytes + 2);
}

static void emit16(uint16_t value) { printf("%02x%02x", value >> 8, value & 255); }
static void emit32(uint32_t value) { emit16((uint16_t)(value >> 16)); emit16((uint16_t)value); }

static void qualify_address(const char* path) {
    FILE* input = fopen(path, "rb");
    check(input != NULL, "Raw authored word fixture opened");
    unsigned char bytes[20];
    unsigned index = 0;
    size_t size;
    while ((size = fread(bytes, 1, sizeof(bytes), input)) != 0) {
        check(size == sizeof(bytes), "Exact Wii-word fixture stride");
        AXVPB voice = {0};
        memset(&voice.pb, 0xa5, sizeof(voice.pb));
        voice.sync = read_be32(bytes);
        // Packed original AXPBADDR admits an odd byte address. Exercise that
        // real native ABI contract instead of relying on a stack word boundary.
        unsigned char address_storage[sizeof(AXPBADDR) + 1];
        AXPBADDR* address = (AXPBADDR*)(address_storage + (index & 1));
        // Fixture transport is independent of the platform ABI accessors.
        for (unsigned i = 0; i < 8; ++i) {
            const uint16_t value = read_be16(bytes + 4 + 2 * i);
            memcpy((unsigned char*)address + 2 * i, &value, sizeof(value));
        }
        BOOL prior = OSDisableInterrupts();
        const BOOL caller_mask = (index & 1) ? FALSE : prior;
        OSRestoreInterrupts(caller_mask);
        AXSetVoiceAddr(&voice, address);
        check(OSDisableInterrupts() == caller_mask, "Source call retains the original caller mask");
        OSRestoreInterrupts(prior);
        printf("address %u ", index++);
        emit32(voice.sync);
        // The untouched PB canary has equal-byte u32 fields; the changed ranges
        // contain only authored u16 fields. This canonical field dump avoids
        // sharing the platform high/low accessor implementation with the oracle.
        for (unsigned offset = 0; offset < sizeof(voice.pb); offset += 2) {
            uint16_t value;
            memcpy(&value, (unsigned char*)&voice.pb + offset, sizeof(value));
            emit16(value);
        }
        putchar('\n');
    }
    check(!ferror(input) && fclose(input) == 0, "Raw word fixture closes");
    printf("address-qualified %u\n", index);
}

static void install_voice_depop(AXPB* voice, const unsigned char* bytes) {
    // Input order is the exact source PB layout (12 main +8 remote signed16).
    for (unsigned i = 0; i < 12; ++i) {
        const uint16_t bits = read_be16(bytes + 2 * i);
        memcpy((unsigned char*)&voice->dpop + 2 * i, &bits, sizeof(bits));
    }
    for (unsigned i = 0; i < 8; ++i) {
        const uint16_t bits = read_be16(bytes + 24 + 2 * i);
        memcpy((unsigned char*)&voice->rmtDpop + 2 * i, &bits, sizeof(bits));
    }
}

static void dump_studio(unsigned index) {
    const unsigned char* studio = (const unsigned char*)__AXGetStudio();
    printf("studio %u ", index);
    // Read actual native metadata with memcpy; serialize the original6-byte
    // value/delta fields independently for comparison with authored BE words.
    for (unsigned channel = 0; channel < 20; ++channel) {
        int32_t value;
        int16_t delta;
        memcpy(&value, studio + 6 * channel, sizeof(value));
        memcpy(&delta, studio + 6 * channel + 4, sizeof(delta));
        emit32((uint32_t)value);
        emit16((uint16_t)delta);
    }
    putchar('\n');
}

static void qualify_studio(const char* path) {
    FILE* input = fopen(path, "rb");
    check(input != NULL, "Raw studio source-operation fixture opened");
    unsigned index = 0;
    int command;
    __AXSPBInit();
    while ((command = fgetc(input)) != EOF) {
        if (command == 0) {
            __AXSPBInit();
        } else if (command == 1) {
            unsigned char bytes[40];
            check(fread(bytes, 1, sizeof(bytes), input) == sizeof(bytes), "Exact original voice-depop words");
            AXPB voice = {0};
            install_voice_depop(&voice, bytes);
            __AXDepopVoice(&voice);
        } else if (command == 2) {
            __AXPrintStudio();
            dump_studio(index++);
        } else {
            check(0, "Known actual source-operation fixture command");
        }
    }
    check(!ferror(input) && fclose(input) == 0, "Raw studio fixture closes");
    printf("studio-qualified %u\n", index);
}

int main(int argc, char** argv) {
    check(argc == 3, "Explicit method and raw fixture");
    check(!AXIsInit(), "Original AX is not initialized by a data qualifier");
    if (strcmp(argv[1], "address") == 0)
        qualify_address(argv[2]);
    else if (strcmp(argv[1], "studio") == 0)
        qualify_studio(argv[2]);
    else
        check(0, "Known original source method");
    check(!AXIsInit(), "No fabricated AX initialization after qualification");
    return 0;
}
