// RECHARGED_ROLLBACK_TEST_COMPARE=N checks every Nth rollback: it copies the
// memory the rollback tracks before the restore and compares it after the
// replay, when it should be as it was. It also watches what no rollback
// restores (the module's other globals, what the test leaves out of the
// arenas), which a replay should not change. <file> lists the places that
// differed in the first comparisons, <file>.summary every place with how
// often: state the replay does not bring back as it was, or moves forward a
// second time.
#include "platform/rollback_state.h"

namespace mscharged::rollback
{
namespace
{
// RECHARGED_ROLLBACK_TEST_COMPARE also watches what no rollback restores:
// the module's other globals, and what the test leaves out of the arenas but
// the audio and frame memory (the replay draws and the audio thread plays).
// A replay should not change them; what it does change goes forward twice.
constexpr const char* kModuleGlobals = "module global";
} // namespace

void BuildWatched()
{
    State& s = *sState;
    s.compare.watchedCount = 0;
    Range segments[8] = {};
    std::uintptr_t bounds[8][2];
    const int segmentCount = mscharged::platform::RollbackWritableSegments(s.compare.moduleBase, bounds, 8);
    for (int i = 0; i < segmentCount; ++i) segments[i] = {bounds[i][0], bounds[i][1], kModuleGlobals};
    Range cuts[kMaxRanges];
    std::copy(s.memory.ranges, s.memory.ranges + s.memory.rangeCount, cuts);
    std::sort(cuts, cuts + s.memory.rangeCount, [](const Range& a, const Range& b) { return a.start < b.start; });
    auto add = [&](std::uintptr_t start, std::uintptr_t end, const char* name) {
        if (start < end && s.compare.watchedCount < kMaxWatched) s.compare.watched[s.compare.watchedCount++] = {start, end, name};
    };
    for (const Range& segment : segments)
    {
        if (!segment.start) continue;
        std::uintptr_t at = segment.start;
        for (int c = 0; c < s.memory.rangeCount; ++c)
        {
            if (cuts[c].end <= at || cuts[c].start >= segment.end) continue;
            add(at, std::max(at, cuts[c].start), kModuleGlobals);
            at = std::max(at, cuts[c].end);
        }
        add(at, segment.end, kModuleGlobals);
    }
    for (int i = 0; i < s.memory.excludedCount; ++i)
    {
        const char* name = s.memory.excluded[i].name;
        if (!std::strcmp(name, "audio memory") || !std::strcmp(name, "AudioBackend") || !std::strcmp(name, "frame memory")
            || !std::strcmp(name, "audio heap")
            || !std::strncmp(name, "task ", 5) || !std::strncmp(name, "fixed steps'", 12))
            continue;
        add(s.memory.excluded[i].start, s.memory.excluded[i].end, name);
    }
}

// RECHARGED_ROLLBACK_TEST_COMPARE: the tracked memory before a restore, and
// the watched memory.
bool CopyForCompare()
{
    State& s = *sState;
    std::size_t bytes = 0;
    for (int i = 0; i < s.memory.regionCount; ++i) bytes += s.memory.regions[i].bytes;
    for (int i = 0; i < s.compare.watchedCount; ++i) bytes += s.compare.watched[i].end - s.compare.watched[i].start;
    if (bytes != s.compare.compareBytes)
    {
        mscharged::platform::RollbackUnmapPages(s.compare.compareCopy, s.compare.compareBytes);
        s.compare.compareCopy = static_cast<std::uint8_t*>(mscharged::platform::RollbackMapPages(bytes));
        s.compare.compareBytes = s.compare.compareCopy ? bytes : 0;
        if (!s.compare.compareCopy) return false;
    }
    std::uint8_t* to = s.compare.compareCopy;
    for (int i = 0; i < s.memory.regionCount; ++i)
    {
        std::memcpy(to, s.memory.regions[i].base, s.memory.regions[i].bytes);
        to += s.memory.regions[i].bytes;
    }
    for (int i = 0; i < s.compare.watchedCount; ++i)
    {
        const std::size_t length = s.compare.watched[i].end - s.compare.watched[i].start;
        std::memcpy(to, reinterpret_cast<const void*>(s.compare.watched[i].start), length);
        to += length;
    }
    return true;
}

namespace
{
bool ModuleOffset(std::uintptr_t value, std::uintptr_t& offset)
{
    if (!value || mscharged::platform::RollbackModuleBase(reinterpret_cast<void*>(value)) != sState->compare.moduleBase)
        return false;
    offset = value - sState->compare.moduleBase;
    return true;
}

const char* RangeName(std::uintptr_t address)
{
    const State& s = *sState;
    for (int i = 0; i < s.memory.rangeCount; ++i)
        if (address >= s.memory.ranges[i].start && address < s.memory.ranges[i].end) return s.memory.ranges[i].name;
    return "?";
}

Place* FindPlace(int kind, std::uintptr_t key, std::size_t size)
{
    State& s = *sState;
    for (int i = 0; i < s.compare.placeCount; ++i)
        if (s.compare.places[i].kind == kind && s.compare.places[i].key == key && s.compare.places[i].size == size) return &s.compare.places[i];
    if (s.compare.placeCount == kMaxPlaces)
    {
        ++s.compare.placesDropped;
        return nullptr;
    }
    Place& place = s.compare.places[s.compare.placeCount++];
    place = {};
    place.kind = kind;
    place.key = key;
    place.size = size;
    return &place;
}
} // namespace

void PrintPlace(FILE* file, const Place& place)
{
    switch (place.kind)
    {
    case kGlobal:
        std::fprintf(file, "global module+0x%lx (%s)", static_cast<unsigned long>(place.key), place.region);
        break;
    case kAllocation:
        std::fprintf(file, "%zu-byte allocation (%s), first differing +0x%lx, ", place.size, place.region,
                     static_cast<unsigned long>(place.firstOffset));
        if (place.key)
            std::fprintf(file, "first word module+0x%lx", static_cast<unsigned long>(place.key));
        else
            std::fprintf(file, "no vtable");
        break;
    case kNotRestored:
        if (place.region == kModuleGlobals)
            std::fprintf(file, "NOT RESTORED module global module+0x%lx", static_cast<unsigned long>(place.key));
        else
            std::fprintf(file, "NOT RESTORED %s, +0x%lx", place.region, static_cast<unsigned long>(place.firstOffset));
        break;
    default:
        std::fprintf(file, "no live allocation (%s)", place.region);
        break;
    }
}

namespace
{
void PrintBytes(FILE* file, const char* label, const std::uint8_t* bytes)
{
    std::fprintf(file, "      %s", label);
    for (int i = 0; i < 16; ++i) std::fprintf(file, " %02x", bytes[i]);
    std::fprintf(file, "\n");
}

// One range that differs, [start, end), with `before` its bytes in the copy;
// `watched` the watched range it lies in, if any.
void NoteDifference(std::uintptr_t start, std::uintptr_t end, const std::uint8_t* before, const Range* watched)
{
    State& s = *sState;
    s.compare.compareDiffBytes += end - start;
    int kind = kUnowned;
    std::uintptr_t key = 0, offset = 0;
    std::size_t size = 0;
    if (watched)
    {
        Place* place = FindPlace(kNotRestored,
                                 watched->name == kModuleGlobals ? start - s.compare.moduleBase : watched->start, 0);
        if (!place) return;
        place->bytes += end - start;
        if (place->lastCompare == s.compare.compares) return;
        place->lastCompare = s.compare.compares;
        if (!place->compares++)
        {
            place->region = watched->name;
            place->firstOffset = start - watched->start;
            const std::size_t shown = std::min<std::size_t>(16, end - start);
            std::memcpy(place->before, before, shown);
            std::memcpy(place->after, reinterpret_cast<const void*>(start), shown);
        }
        return;
    }
    const auto data = reinterpret_cast<std::uintptr_t>(__rbk_data_start);
    const auto bss = reinterpret_cast<std::uintptr_t>(__rbk_bss_start);
    mscharged::platform::GameAllocationSpan span{};
    if ((data && start >= data && start < reinterpret_cast<std::uintptr_t>(__rbk_data_end))
        || (bss && start >= bss && start < reinterpret_cast<std::uintptr_t>(__rbk_bss_end)))
    {
        kind = kGlobal;
        key = start - s.compare.moduleBase;
    }
    else if (mscharged::platform::FindGameAllocationSpan(reinterpret_cast<const void*>(start), 1, span))
    {
        kind = kAllocation;
        const auto base = reinterpret_cast<std::uintptr_t>(span.base);
        size = span.bytes;
        offset = start - base;
        std::uintptr_t first = 0;
        if (span.bytes >= sizeof(first)) std::memcpy(&first, span.base, sizeof(first));
        if (!ModuleOffset(first, key)) key = 0;
    }
    Place* place = FindPlace(kind, key, size);
    if (!place) return;
    place->bytes += end - start;
    if (place->lastCompare == s.compare.compares) return;
    place->lastCompare = s.compare.compares;
    if (!place->compares++)
    {
        place->firstOffset = offset;
        place->region = RangeName(start);
        const std::size_t shown = std::min<std::size_t>(16, end - start);
        std::memcpy(place->before, before, shown);
        std::memcpy(place->after, reinterpret_cast<const void*>(start), shown);
    }
}
} // namespace

// After a replay: what differs from the copy, in 8-byte words, ranges joined
// across gaps under 32 bytes.
void CompareAfterReplay(unsigned frame, int depth)
{
    State& s = *sState;
    ++s.compare.compares;
    const std::uint64_t bytesBefore = s.compare.compareDiffBytes;
    const int placesBefore = s.compare.placeCount;
    const std::uint8_t* copy = s.compare.compareCopy;
    for (int r = 0; r < s.memory.regionCount + s.compare.watchedCount; ++r)
    {
        const Range* watched = r < s.memory.regionCount ? nullptr : &s.compare.watched[r - s.memory.regionCount];
        const auto* live = watched ? reinterpret_cast<const std::uint8_t*>(watched->start)
                                   : static_cast<const std::uint8_t*>(s.memory.regions[r].base);
        const std::size_t bytes = watched ? watched->end - watched->start : s.memory.regions[r].bytes;
        constexpr std::size_t kPage = 4096;
        std::uintptr_t open = 0, last = 0;
        const std::uint8_t* openBefore = nullptr;
        for (std::size_t page = 0; page < bytes; page += kPage)
        {
            const std::size_t length = std::min(kPage, bytes - page);
            if (!std::memcmp(live + page, copy + page, length)) continue;
            for (std::size_t at = page; at + 8 <= page + length; at += 8)
            {
                if (!std::memcmp(live + at, copy + at, 8)) continue;
                const auto address = reinterpret_cast<std::uintptr_t>(live + at);
                if (open && address - last < 32)
                {
                    last = address + 8;
                    continue;
                }
                if (open) NoteDifference(open, last, openBefore, watched);
                open = address;
                last = address + 8;
                openBefore = copy + at;
            }
        }
        if (open) NoteDifference(open, last, openBefore, watched);
        copy += bytes;
    }
    const bool differs = s.compare.compareDiffBytes != bytesBefore;
    if (differs) ++s.compare.comparesDiffering;
    if (!differs || s.compare.comparesDiffering > kDetailedCompares || !s.out) return;
    Log("COMPARED rollback of %d steps from frame %u: %llu bytes differ; places first seen now:\n",
                 depth, frame, static_cast<unsigned long long>(s.compare.compareDiffBytes - bytesBefore));
    for (int i = placesBefore; i < s.compare.placeCount && i < placesBefore + kListedPlaces; ++i)
    {
        Log("  ");
        PrintPlace(s.out, s.compare.places[i]);
        Log("\n");
        PrintBytes(s.out, "before:", s.compare.places[i].before);
        PrintBytes(s.out, "after: ", s.compare.places[i].after);
    }
    FlushLog();
}
} // namespace mscharged::rollback
