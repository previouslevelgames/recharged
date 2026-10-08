// SDL_ReadFromAudioQueue regression: a read that finishes the head track must
// not keep using that destroyed track's channel map. The native AI stream has
// an input channel map and queues one track per 96-frame DMA block, so device
// reads routinely span tracks. Every SDL block lives on its own pages, which
// become inaccessible when freed, so any use after free faults deterministically.
#include <SDL3/SDL.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok && failures++ < 8) std::fprintf(stderr, "FAIL: %s\n", what);
}
constexpr std::size_t kHeader = 16; // keeps SDL's 16-byte malloc alignment
struct Block { std::size_t size, mapped; };
void* Malloc(size_t size) {
    const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
    const std::size_t mapped = (size + kHeader + page - 1) / page * page;
    void* base = mmap(nullptr, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return nullptr;
    *static_cast<Block*>(base) = {size, mapped};
    return static_cast<char*>(base) + kHeader;
}
Block* Header(void* block) { return reinterpret_cast<Block*>(static_cast<char*>(block) - kHeader); }
void Free(void* block) {
    if (block) mprotect(Header(block), Header(block)->mapped, PROT_NONE); // never reused
}
void* Calloc(size_t count, size_t size) {
    void* block = Malloc(count * size);
    if (block) std::memset(block, 0, count * size);
    return block;
}
void* Realloc(void* block, size_t size) {
    void* moved = Malloc(size);
    if (moved && block) std::memcpy(moved, block, std::min(size, Header(block)->size));
    if (moved) Free(block);
    return moved;
}
void SDLCALL Released(void*, const void*, int) {}

std::vector<float> Drain(SDL_AudioStream* stream, int request_frames) {
    std::vector<float> out, chunk(request_frames * 2);
    for (;;) {
        const int got = SDL_GetAudioStreamData(stream, chunk.data(), int(chunk.size() * sizeof(float)));
        if (got <= 0) break;
        out.insert(out.end(), chunk.begin(), chunk.begin() + got / int(sizeof(float)));
    }
    return out;
}
}

int main() {
    if (!SDL_SetMemoryFunctions(Malloc, Calloc, Realloc, Free)) return 2;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return 2;
    constexpr int kFrames = 96, kTracks = 64, kRead = 150;
    // The native AI shape: S16 stereo source resampled to the device rate.
    const SDL_AudioSpec source{SDL_AUDIO_S16, 2, 32000}, output{SDL_AUDIO_F32, 2, 48000};
    const int swap[2]{1, 0};
    SDL_AudioStream* split = SDL_CreateAudioStream(&source, &output);
    SDL_AudioStream* whole = SDL_CreateAudioStream(&source, &output);
    Check(split && whole, "create streams");
    Check(split && SDL_SetAudioStreamInputChannelMap(split, swap, 2), "set split channel map");
    Check(whole && SDL_SetAudioStreamInputChannelMap(whole, swap, 2), "set reference channel map");
    std::vector<std::vector<Sint16>> blocks(kTracks, std::vector<Sint16>(kFrames * 2));
    std::vector<Sint16> all;
    for (int t = 0; t < kTracks; ++t) {
        for (int f = 0; f < kFrames; ++f) {
            blocks[t][2 * f] = Sint16((t * 97 + f * 31) % 20000);          // left
            blocks[t][2 * f + 1] = Sint16(-((t * 53 + f * 17) % 20000) - 1); // right
        }
        all.insert(all.end(), blocks[t].begin(), blocks[t].end());
    }
    // One NoCopy track per 96-frame block, as the native AI queues DMA blocks.
    for (auto& block : blocks) {
        Check(SDL_PutAudioStreamDataNoCopy(split, block.data(), int(block.size() * sizeof(Sint16)),
                                           Released, nullptr), "queue one track per block");
    }
    Check(SDL_PutAudioStreamData(whole, all.data(), int(all.size() * sizeof(Sint16))), "queue reference");
    Check(SDL_FlushAudioStream(split) && SDL_FlushAudioStream(whole), "flush streams");
    const auto expected = Drain(whole, kRead);
    const auto actual = Drain(split, kRead);
    Check(!expected.empty() && actual.size() == expected.size(), "split tracks produce every output frame");
    std::size_t wrong = 0;
    for (std::size_t n = 0; n < std::min(actual.size(), expected.size()); ++n) {
        const float d = actual[n] - expected[n];
        if (!(d <= 1e-6f && d >= -1e-6f)) ++wrong;
    }
    Check(wrong == 0, "track boundaries do not change channel-mapped resampled output");
    if (wrong) std::fprintf(stderr, "%zu of %zu samples differ from the single-track reference\n", wrong, expected.size());
    SDL_DestroyAudioStream(split);
    SDL_DestroyAudioStream(whole);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
