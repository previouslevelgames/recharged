#include <revolution/ax.h>
#include <revolution/dsp.h>
#include <revolution/os.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(u32) == 4, "Wii words remain 32 bits");
_Static_assert(sizeof(AXPB) == 320, "Original DSP parameter block footprint");
_Static_assert(sizeof(AXSTUDIO) == 120, "Original packed DSP studio footprint");
_Static_assert(sizeof(AXPROFILE) == 56, "Original timestamp/profile footprint");
_Static_assert(sizeof(AXVoiceContext) == sizeof(uintptr_t), "Native callback context");
_Static_assert(sizeof(((AXVPB*)0)->userContext) == sizeof(void*), "Native retained context");
_Static_assert(sizeof(((DSPTask*)0)->iramMmemAddr) == sizeof(void*), "Native DMA metadata");
_Static_assert(sizeof(((DSPTask*)0)->iramDspAddr) == 4, "Actual DSP bus address");

enum { VOICE_COUNT = 8 };
static AXVPB voices[VOICE_COUNT];
static unsigned operation;
static unsigned checks;

static void check(int condition, const char* reason) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "AX CPU check failed: %s\n", reason);
        exit(1);
    }
}

static void voice_callback(void* object) {
    AXVPB* voice = object;
    printf("callback %u %u %u %" PRIuPTR " %u %u\n", operation,
           voice->index, voice->priority, voice->userContext,
           voice->pb.state, voice->depop);
}

static void reset_voices(void) {
    __AXAllocInit();
    memset(voices, 0, sizeof(voices));
    for (unsigned i = 0; i < VOICE_COUNT; ++i) {
        voices[i].index = i;
        voices[i].pb.state = AX_VOICE_RUN;
        voices[i].pb.itd.flag = 0xffff;
        voices[i].pb.lpf.on = 0xffff;
        voices[i].pb.biquad.on = 0xffff;
        voices[i].pb.remote = 0xffff;
        voices[i].pb.rmtSrc.currentAddressFrac = 0xffff;
        for (unsigned j = 0; j < 4; ++j)
            voices[i].pb.rmtSrc.last_samples[j] = 0xffff;
        voices[i].pb.rmtIIR.lpf.on = 0xffff;
        __AXPushFreeStack(&voices[i]);
    }
}

static void dump_voices(void) {
    for (unsigned i = 0; i < VOICE_COUNT; ++i) {
        const AXVPB* voice = &voices[i];
        printf("voice %u %u %u %" PRIuPTR " %u %u %u\n", operation, i,
               voice->priority, voice->userContext, voice->pb.state,
               voice->depop, voice->sync);
    }
    for (unsigned priority = 0; priority <= AX_PRIORITY_MAX; ++priority) {
        printf("queue %u %u", operation, priority);
        AXVPB* voice = __AXGetStackHead(priority);
        unsigned count = 0;
        while (voice) {
            check(++count <= VOICE_COUNT, "Source list is bounded and acyclic");
            printf(" %u", voice->index);
            voice = voice->next;
        }
        putchar('\n');
    }
}

static void qualify_voices(const char* path) {
    FILE* input = fopen(path, "r");
    check(input != NULL, "Generated voice operations opened");
    char command[32];
    reset_voices();
    operation = 0;
    while (fscanf(input, "%31s", command) == 1) {
        ++operation;
        if (strcmp(command, "acquire") == 0) {
            unsigned priority;
            uintptr_t context;
            check(fscanf(input, "%u %" SCNuPTR, &priority, &context) == 2,
                  "Acquire fixture has priority/context");
            check(priority >= 1 && priority <= AX_PRIORITY_MAX,
                  "Fixture uses the original valid priority domain");
            BOOL previous = OSDisableInterrupts();
            AXVPB* result = AXAcquireVoice(priority, voice_callback, context);
            check(OSDisableInterrupts() == FALSE, "Acquire retains caller interrupt mask");
            OSRestoreInterrupts(previous);
            if (result) {
                check(result->pb.state == AX_VOICE_STOP && result->pb.itd.flag == 0,
                      "Original acquired-voice defaults are installed");
                check(result->pb.lpf.on == 0 && result->pb.biquad.on == 0 &&
                          result->pb.remote == 0 && result->pb.rmtIIR.lpf.on == 0,
                      "Original filter/remote defaults are installed");
                check(result->pb.rmtSrc.currentAddressFrac == 0,
                      "Original remote fraction default");
                for (unsigned j = 0; j < 4; ++j)
                    check(result->pb.rmtSrc.last_samples[j] == 0,
                          "Original remote history default");
            }
            printf("acquire %u %d\n", operation, result ? (int)result->index : -1);
        } else if (strcmp(command, "free") == 0) {
            unsigned index;
            check(fscanf(input, "%u", &index) == 1 && index < VOICE_COUNT,
                  "Free fixture index is valid");
            AXFreeVoice(&voices[index]);
        } else if (strcmp(command, "priority") == 0) {
            unsigned index, priority;
            check(fscanf(input, "%u %u", &index, &priority) == 2 &&
                      index < VOICE_COUNT && priority >= 1 && priority <= AX_PRIORITY_MAX,
                  "Priority fixture is valid");
            AXSetVoicePriority(&voices[index], priority);
        } else if (strcmp(command, "state") == 0) {
            unsigned index, state;
            check(fscanf(input, "%u %u", &index, &state) == 2 &&
                      index < VOICE_COUNT && state <= AX_VOICE_RUN,
                  "State fixture is valid");
            AXSetVoiceState(&voices[index], (u16)state);
        } else if (strcmp(command, "callback") == 0) {
            unsigned index;
            check(fscanf(input, "%u", &index) == 1 && index < VOICE_COUNT,
                  "Callback fixture index is valid");
            __AXPushCallbackStack(&voices[index]);
        } else if (strcmp(command, "service") == 0) {
            __AXServiceCallbackStack();
        } else {
            check(0, "Known generated voice operation");
        }
        dump_voices();
    }
    check(!ferror(input) && fclose(input) == 0, "Fixture read completes");
    printf("voices-qualified %u\n", checks);
}

static void qualify_depop(const char* path) {
    FILE* input = fopen(path, "r");
    check(input != NULL, "Independent arithmetic fixture opened");
    unsigned frame;
    int32_t authored;
    unsigned index = 0;
    while (fscanf(input, "%u %" SCNd32, &frame, &authored) == 2) {
        s32 remaining = authored;
        s32 value = INT32_C(0x12345678);
        s16 delta = 12345;
        if (frame == AX_SAMPLES_PER_FRAME)
            __AXDepopFadeMain(&remaining, &value, &delta);
        else if (frame == AX_SAMPLES_PER_FRAME_RMT)
            __AXDepopFadeRmt(&remaining, &value, &delta);
        else
            check(0, "Original frame sample count");
        printf("depop %u %" PRId32 " %" PRId32 " %d\n", index++,
               remaining, value, delta);
    }
    check(!ferror(input) && fclose(input) == 0, "Arithmetic fixture completes");
    printf("depop-qualified %u\n", index);
}

typedef struct AuxFixture {
    unsigned channel_count;
    unsigned calls;
    u32 tag;
    unsigned unique_count;
    s32* rings[3];
} AuxFixture;

static void aux_callback(void* channels, void* context) {
    AuxFixture* fixture = context;
    s32** samples = channels;
    check((uintptr_t)fixture > UINT32_MAX, "Actual native context retains upper bits");
    ++fixture->calls;
    unsigned known = 0;
    for (; known < fixture->unique_count; ++known)
        if (fixture->rings[known] == samples[0]) break;
    if (known == fixture->unique_count) {
        check(fixture->unique_count < 3, "Original three-entry auxiliary ring");
        fixture->rings[fixture->unique_count++] = samples[0];
    }
    for (unsigned channel = 0; channel < fixture->channel_count; ++channel) {
        check((u8*)samples[channel] == (u8*)samples[0] + channel * 384,
              "Original source channel selection/stride");
        for (unsigned sample = 0; sample < 96; ++sample)
            samples[channel][sample] = (s32)(fixture->tag + channel * 1000 + sample);
    }
}

static void check_ring(const s32* samples, unsigned channels, u32 tag) {
    for (unsigned channel = 0; channel < channels; ++channel)
        for (unsigned sample = 0; sample < 96; ++sample)
            check(samples[channel * 96 + sample] == (s32)(tag + channel * 1000 + sample),
                  "Actual source callback writes are retained in the selected ring");
}

static void check_zero(const void* pointer, unsigned size) {
    const u8* bytes = pointer;
    for (unsigned i = 0; i < size; ++i)
        check(bytes[i] == 0, "Original explicit ring clearing extent");
}

static void qualify_aux(void) {
    AuxFixture a = {4, 0, 10000, 0, {0}};
    AuxFixture b = {4, 0, 20000, 0, {0}};
    AuxFixture c = {3, 0, 30000, 0, {0}};
    void* pointer;
    __AXAuxInit();
    __AXClInit();
    check(AXGetMode() == AX_OUTPUT_STEREO, "Original initial output mode");
    check(AXGetAuxAReturnVolume() == 0x8000 && AXGetAuxBReturnVolume() == 0x8000 &&
              AXGetAuxCReturnVolume() == 0x8000, "Original auxiliary return volumes");
    AXSetAuxAReturnVolume(0xffff);
    AXSetAuxBReturnVolume(1234);
    AXSetAuxCReturnVolume(2);
    check(AXGetAuxAReturnVolume() == 0xffff && AXGetAuxBReturnVolume() == 1234 &&
              AXGetAuxCReturnVolume() == 2, "Original return-volume setters retain values");
    __AXGetAuxAInput(&pointer);
    check(pointer == NULL, "Unregistered original auxiliary input stays absent");
    AXRegisterAuxACallback(aux_callback, &a);
    AXRegisterAuxBCallback(aux_callback, &b);
    AXRegisterAuxCCallback(aux_callback, &c);
    AXAuxCallback callback;
    void* context;
    AXGetAuxACallback(&callback, &context);
    check(callback == aux_callback && context == &a, "Actual source registration preserves A context");
    AXGetAuxBCallback(&callback, &context);
    check(callback == aux_callback && context == &b, "Actual source registration preserves B context");
    AXGetAuxCCallback(&callback, &context);
    check(callback == aux_callback && context == &c, "Actual source registration preserves C context");
    AXSetMode(AX_OUTPUT_DPL2);
    for (unsigned i = 0; i < 3; ++i) {
        __AXProcessAux();
        __AXGetAuxAOutput(&pointer);
        check_ring(pointer, 4, a.tag);
        __AXGetAuxBOutput(&pointer);
        check_ring(pointer, 4, b.tag);
    }
    check(a.calls == 3 && b.calls == 3 && c.calls == 0,
          "Original DPL2 suppresses only auxiliary C callback");
    check(a.unique_count == 3 && b.unique_count == 3,
          "Original CPU auxiliary visits all three rings");
    a.channel_count = b.channel_count = 3;
    AXSetMode(AX_OUTPUT_SURROUND);
    for (unsigned i = 0; i < 3; ++i) {
        __AXProcessAux();
        __AXGetAuxCOutput(&pointer);
        check_ring(pointer, 3, c.tag);
    }
    check(c.calls == 3 && c.unique_count == 3, "Original stereo/surround auxiliary C resumes");

    // Source quirk: initialization clears only 1152 bytes at each array's start,
    // even though A/B have three 1536-byte rings. Preserve the other stored bytes.
    __AXAuxInit();
    // First callback visited ring2, followed by ring0 then ring1.
    check_zero(a.rings[1], 1152);
    check_zero(b.rings[1], 1152);
    check_zero(c.rings[1], 1152);
    check_ring(a.rings[0], 4, a.tag);
    check_ring(a.rings[2], 4, a.tag);
    check_ring(b.rings[0], 4, b.tag);
    check_ring(b.rings[2], 4, b.tag);
    check_ring(c.rings[0], 3, c.tag);
    check_ring(c.rings[2], 3, c.tag);
    for (unsigned sample = 0; sample < 96; ++sample) {
        check(a.rings[1][3 * 96 + sample] == (s32)(a.tag + 3000 + sample),
              "Original partial init preserves A's fourth channel");
        check(b.rings[1][3 * 96 + sample] == (s32)(b.tag + 3000 + sample),
              "Original partial init preserves B's fourth channel");
    }
    AXRegisterAuxACallback(NULL, NULL);
    AXRegisterAuxBCallback(NULL, NULL);
    AXRegisterAuxCCallback(NULL, NULL);
    __AXGetAuxAInput(&pointer);
    check(pointer == NULL, "Unregister makes source input absent immediately");
    for (unsigned i = 0; i < 3; ++i) __AXProcessAux();
    for (unsigned i = 0; i < 3; ++i) {
        check_zero(a.rings[i], 1536);
        check_zero(b.rings[i], 1536);
        check_zero(c.rings[i], 1152);
    }
    check(a.calls == 6 && b.calls == 6 && c.calls == 3,
          "Unregister/clear never fabricates callback delivery");
    check(!AXIsInit(), "Bounded CPU qualifier did not initialize original AX");
    check(__AXGetCurrentProfile() == NULL, "Original uninitialized profile remains absent");
    printf("aux-qualified %u\n", checks);
}

int main(int argc, char** argv) {
    check(argc >= 2, "Explicit qualifier mode");
    check(!AXIsInit(), "Original AX starts uninitialized");
    if (strcmp(argv[1], "voices") == 0) {
        check(argc == 3, "Voice fixture path supplied");
        qualify_voices(argv[2]);
    } else if (strcmp(argv[1], "depop") == 0) {
        check(argc == 3, "Arithmetic fixture path supplied");
        qualify_depop(argv[2]);
    } else if (strcmp(argv[1], "aux") == 0) {
        qualify_aux();
    } else {
        check(0, "Known qualifier mode");
    }
    check(!AXIsInit(), "No false source initialized state after CPU qualification");
    return 0;
}
