// nod's C API (nod.h) over this reader: the functions the game's DVD layer and
// the host use, with nod's handle, cursor, seek and error-message behaviour.
// Not provided: nod_disc_open_stream and the nod_buf_* zero-copy reads.
#include "disc.h"

#include <nod.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>

using namespace mscharged::platform::wii_disc;

// nod.h declares this opaque; one kind per handle, as in nod's NodHandleInner.
struct NodHandle
{
    static constexpr std::uint32_t kMagic = 0x4E4F4448; // "NODH", as nod

    enum class Kind
    {
        Disc,
        Partition,
        File,
    };

    std::uint32_t magic = kMagic;
    Kind kind = Kind::Disc;
    std::shared_ptr<Disc> disc;
    const Partition* partition = nullptr;
    // Disc: offset on the disc; partition and file: offset in the partition's data.
    std::uint64_t position = 0;
    std::unique_ptr<PartitionMeta> meta; // a partition's
    std::uint64_t begin = 0, end = 0;    // a file's bytes in the partition's data

    // Kept from being optimised away, so a freed handle reads as freed.
    ~NodHandle() { *const_cast<volatile std::uint32_t*>(&magic) = 0; }
};

namespace
{
// A trivially destructible thread-local: no TLS destructors needed on Horizon.
thread_local char t_message[512];
thread_local bool t_has_message = false;

void ClearError() { t_has_message = false; }

void SetError(const std::string& message)
{
    std::snprintf(t_message, sizeof t_message, "%s", message.c_str());
    t_has_message = true;
}

NodResult Fail(NodResult result, const std::string& message)
{
    SetError(message);
    return result;
}

template <typename T> bool Aligned(const T* pointer)
{
    return reinterpret_cast<std::uintptr_t>(pointer) % alignof(T) == 0;
}

// nod's check_handle!: null, misaligned and freed handles fail with these messages.
NodHandle* CheckHandle(const NodHandle* handle)
{
    if (!handle)
    {
        SetError("null handle");
        return nullptr;
    }
    if (!Aligned(handle))
    {
        SetError("unaligned handle pointer");
        return nullptr;
    }
    if (handle->magic != NodHandle::kMagic)
    {
        SetError("use after free");
        return nullptr;
    }
    return const_cast<NodHandle*>(handle);
}

// Runs `body`, turning an exception into nod's result and message.
template <typename Body> NodResult Guard(Body&& body)
{
    try
    {
        return body();
    }
    catch (const Error& error)
    {
        return Fail(error.Result(), error.what());
    }
    catch (const std::bad_alloc&)
    {
        return Fail(NOD_RESULT_ERR_IO, "allocation failed: out of memory");
    }
    catch (const std::exception& error)
    {
        return Fail(NOD_RESULT_ERR_OTHER, error.what());
    }
}

template <typename Body> std::int64_t GuardCount(Body&& body)
{
    const std::int64_t failed = -1;
    std::int64_t count = failed;
    const NodResult result = Guard([&] {
        count = body();
        return NOD_RESULT_OK;
    });
    return result == NOD_RESULT_OK ? count : failed;
}

std::uint64_t SaturatingAdd(std::uint64_t base, std::int64_t delta)
{
    if (delta >= 0)
        return std::uint64_t(delta) > std::numeric_limits<std::uint64_t>::max() - base
            ? std::numeric_limits<std::uint64_t>::max()
            : base + std::uint64_t(delta);
    const std::uint64_t magnitude = std::uint64_t(-(delta + 1)) + 1;
    return magnitude > base ? 0 : base - magnitude;
}

std::string KindName(std::uint32_t kind)
{
    switch (kind)
    {
    case NOD_PARTITION_KIND_DATA: return "Data";
    case NOD_PARTITION_KIND_UPDATE: return "Update";
    case NOD_PARTITION_KIND_CHANNEL: return "Channel";
    default: break;
    }
    // nod shows the value and its bytes as text, bytes past ASCII as U+FFFD.
    char text[64];
    std::snprintf(text, sizeof text, "Other (%08X, ", unsigned(kind));
    std::string name = text;
    for (int shift = 24; shift >= 0; shift -= 8)
    {
        const auto byte = static_cast<unsigned char>(kind >> shift);
        name += byte < 0x80 ? std::string(1, char(byte)) : std::string("\xEF\xBF\xBD");
    }
    return name + ")";
}

NodBlob Blob(const std::vector<std::uint8_t>& bytes)
{
    return bytes.empty() ? NodBlob{nullptr, 0} : NodBlob{bytes.data(), bytes.size()};
}

NodResult OpenPartition(NodHandle* disc, const NodPartitionOptions* options, NodHandle** out,
                        const Partition* (*select)(const Disc&, std::uint32_t), std::uint32_t key)
{
    ClearError();
    if (!out) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
    if (!Aligned(out)) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "unaligned pointer argument");
    NodHandle* handle = CheckHandle(disc);
    if (!handle) return NOD_RESULT_ERR_INVALID_HANDLE;
    if (handle->kind != NodHandle::Kind::Disc) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a disc");
    if (options && options->validate_hashes)
        return Fail(NOD_RESULT_ERR_OTHER, "error: hash validation is not supported on this build");
    return Guard([&] {
        const Partition* partition = select(*handle->disc, key);
        auto opened = std::make_unique<NodHandle>();
        opened->kind = NodHandle::Kind::Partition;
        opened->disc = handle->disc;
        opened->partition = partition;
        opened->meta = std::make_unique<PartitionMeta>(handle->disc->ReadMeta(*partition));
        *out = opened.release();
        return NOD_RESULT_OK;
    });
}

const Partition* PartitionAt(const Disc& disc, std::uint32_t index)
{
    if (index >= disc.Partitions().size()) ThrowFormat("Partition " + std::to_string(index) + " not found");
    return &disc.Partitions()[index];
}

const Partition* PartitionOfKind(const Disc& disc, std::uint32_t kind)
{
    for (const Partition& partition : disc.Partitions())
        if (partition.kind == kind) return &partition;
    ThrowFormat("Partition type " + KindName(kind) + " not found");
}

// A partition handle's FST, with nod's message when it is not valid.
bool ParseFst(const NodHandle& partition, FstView& fst)
{
    std::string error;
    if (fst.Parse(partition.meta->fst, error)) return true;
    SetError(error);
    return false;
}
} // namespace

extern "C" {

const char* nod_error_message(void) { return t_has_message ? t_message : nullptr; }

NodResult nod_disc_open(const char* path, const NodDiscOptions* options, NodHandle** out)
{
    ClearError();
    if (!path || !out) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
    if (!Aligned(out)) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "unaligned pointer argument");
    if (!IsValidUtf8(path, std::strlen(path))) return Fail(NOD_RESULT_ERR_OTHER, "invalid UTF-8 in path");
    // Preloader threads only change how nod reads ahead; reads here are synchronous.
    if (options && options->partition_encryption != NOD_PARTITION_ENCRYPTION_ORIGINAL)
        return Fail(NOD_RESULT_ERR_OTHER, "error: this build reads Wii partitions only as stored "
                                          "(NOD_PARTITION_ENCRYPTION_ORIGINAL)");
    return Guard([&] {
        auto handle = std::make_unique<NodHandle>();
        handle->disc = std::make_shared<Disc>(path);
        *out = handle.release();
        return NOD_RESULT_OK;
    });
}

NodResult nod_disc_open_partition(NodHandle* disc, uint32_t index, const NodPartitionOptions* options,
                                  NodHandle** out)
{
    return OpenPartition(disc, options, out, PartitionAt, index);
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions* options,
                                       NodHandle** out)
{
    return OpenPartition(disc, options, out, PartitionOfKind, kind);
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out)
{
    ClearError();
    if (!out) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
    if (!Aligned(out)) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "unaligned pointer argument");
    NodHandle* handle = CheckHandle(partition);
    if (!handle) return NOD_RESULT_ERR_INVALID_HANDLE;
    if (handle->kind != NodHandle::Kind::Partition)
        return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a partition");
    FstView fst;
    if (!ParseFst(*handle, fst)) return NOD_RESULT_ERR_FORMAT;
    if (fst_index >= fst.Count())
    {
        char message[96];
        std::snprintf(message, sizeof message, "FST index %u out of range (total: %u)", unsigned(fst_index),
                      unsigned(fst.Count()));
        return Fail(NOD_RESULT_ERR_NOT_FOUND, message);
    }
    if (!fst.IsFile(fst_index)) return Fail(NOD_RESULT_ERR_FORMAT, "node is not a file");
    return Guard([&] {
        auto opened = std::make_unique<NodHandle>();
        opened->kind = NodHandle::Kind::File;
        opened->disc = handle->disc;
        opened->partition = handle->partition;
        opened->begin = std::uint64_t(fst.Offset(fst_index)) << 2; // Wii file offsets are stored >> 2
        opened->end = opened->begin + fst.Length(fst_index);
        opened->position = opened->begin;
        *out = opened.release();
        return NOD_RESULT_OK;
    });
}

void nod_free(NodHandle* handle)
{
    if (handle && Aligned(handle) && handle->magic == NodHandle::kMagic) delete handle;
}

int64_t nod_read(NodHandle* handle, uint8_t* buf, size_t len)
{
    ClearError();
    if (!buf)
    {
        SetError("null pointer argument");
        return -1;
    }
    NodHandle* h = CheckHandle(handle);
    if (!h) return -1;
    // Reads fill as much as they can: nod stops at sector ends, but callers loop.
    len = std::min<std::size_t>(len, std::size_t(std::numeric_limits<std::int64_t>::max()));
    return GuardCount([&]() -> std::int64_t {
        std::size_t count = 0;
        switch (h->kind)
        {
        case NodHandle::Kind::Disc: count = h->disc->ReadRaw(h->position, buf, len); break;
        case NodHandle::Kind::Partition: count = h->disc->ReadPartition(*h->partition, h->position, buf, len); break;
        case NodHandle::Kind::File:
            if (h->position >= h->end) return 0;
            count = h->disc->ReadPartition(*h->partition, h->position, buf,
                                           std::size_t(std::min<std::uint64_t>(len, h->end - h->position)));
            break;
        }
        h->position += count;
        return std::int64_t(count);
    });
}

int64_t nod_try_read_cached_at(NodHandle* handle, uint64_t offset, uint8_t* buf, size_t len)
{
    // A miss changes nothing: no cursor, destination or error message.
    if (!handle || !Aligned(handle) || !buf || len == 0 || len > 256 * 1024) return -2;
    if (handle->magic != NodHandle::kMagic || handle->kind != NodHandle::Kind::File) return -2;
    const std::uint64_t length = handle->end - handle->begin;
    if (offset >= length) return -2;
    const std::size_t count = std::size_t(std::min<std::uint64_t>(len, length - offset));
    const std::uint64_t absolute = handle->begin + offset;
    try
    {
        if (!handle->disc->TryReadCached(*handle->partition, absolute, buf, count)) return -2;
    }
    catch (...)
    {
        return -2;
    }
    handle->position = absolute + count;
    return std::int64_t(count);
}

int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence)
{
    ClearError();
    NodHandle* h = CheckHandle(handle);
    if (!h) return -1;
    if (whence < 0 || whence > 2)
    {
        char message[64];
        std::snprintf(message, sizeof message, "invalid whence value: %d", int(whence));
        SetError(message);
        return -1;
    }
    switch (h->kind)
    {
    case NodHandle::Kind::Disc:
        if (whence == 2)
        {
            SetError("BlockIOReader: SeekFrom::End is not supported");
            return -1;
        }
        h->position = whence == 0 ? std::uint64_t(offset) : SaturatingAdd(h->position, offset);
        return std::int64_t(h->position);
    case NodHandle::Kind::Partition:
        // nod measures the end with the hash blocks, though reads skip them.
        h->position = whence == 0 ? std::uint64_t(offset)
            : whence == 1         ? SaturatingAdd(h->position, offset)
                                  : SaturatingAdd(h->partition->DataSize(), offset);
        return std::int64_t(h->position);
    case NodHandle::Kind::File:
    {
        // nod's windowed reader: a start offset wraps like Rust's release
        // arithmetic, then the position is clamped to the file.
        std::uint64_t position = whence == 0 ? h->begin + std::uint64_t(offset)
            : whence == 1                    ? SaturatingAdd(h->position, offset)
                                             : SaturatingAdd(h->end, offset);
        position = std::min(std::max(position, h->begin), h->end);
        h->position = position;
        return std::int64_t(position - h->begin);
    }
    }
    return -1;
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out)
{
    static_assert(sizeof(NodDiscHeader) == 0x400, "nod's DiscHeader is 0x400 bytes");
    ClearError();
    if (!out) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
    if (!Aligned(out)) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "unaligned pointer argument");
    const NodHandle* handle = CheckHandle(disc);
    if (!handle) return NOD_RESULT_ERR_INVALID_HANDLE;
    if (handle->kind != NodHandle::Kind::Disc) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a disc");
    std::memcpy(out, handle->disc->Boot(), sizeof *out);
    return NOD_RESULT_OK;
}

NodResult nod_disc_meta(const NodHandle* disc, NodDiscMeta* out)
{
    ClearError();
    if (!out) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
    if (!Aligned(out)) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "unaligned pointer argument");
    const NodHandle* handle = CheckHandle(disc);
    if (!handle) return NOD_RESULT_ERR_INVALID_HANDLE;
    if (handle->kind != NodHandle::Kind::Disc) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a disc");
    *out = handle->disc->Meta();
    return NOD_RESULT_OK;
}

uint64_t nod_disc_size(const NodHandle* disc)
{
    const NodHandle* handle = CheckHandle(disc);
    return handle && handle->kind == NodHandle::Kind::Disc ? handle->disc->Size() : 0;
}

size_t nod_disc_partitions(const NodHandle* disc, NodPartitionInfo* out, size_t cap)
{
    const NodHandle* handle = CheckHandle(disc);
    if (!handle || handle->kind != NodHandle::Kind::Disc) return 0;
    const auto& partitions = handle->disc->Partitions();
    if (out && Aligned(out))
        for (std::size_t i = 0; i < std::min(cap, partitions.size()); ++i)
            out[i] = NodPartitionInfo{partitions[i].index, partitions[i].kind, partitions[i].DataSize()};
    return partitions.size();
}

bool nod_partition_is_wii(const NodHandle* partition)
{
    const NodHandle* handle = CheckHandle(partition);
    return handle && handle->kind == NodHandle::Kind::Partition; // this build reads Wii discs only
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out)
{
    ClearError();
    if (!out) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "null pointer argument");
    if (!Aligned(out)) return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "unaligned pointer argument");
    const NodHandle* handle = CheckHandle(partition);
    if (!handle) return NOD_RESULT_ERR_INVALID_HANDLE;
    if (handle->kind != NodHandle::Kind::Partition)
        return Fail(NOD_RESULT_ERR_INVALID_HANDLE, "handle is not a partition");
    const PartitionMeta& meta = *handle->meta;
    *out = NodPartitionMeta{Blob(meta.boot), Blob(meta.bi2),    Blob(meta.apploader),  Blob(meta.dol),     Blob(meta.fst),
                            Blob(meta.ticket), Blob(meta.tmd), Blob(meta.cert_chain), Blob(meta.h3_table)};
    return NOD_RESULT_OK;
}

uint32_t nod_partition_find_file(const NodHandle* partition, const char* path, NodNodeKind* out_kind,
                                 uint32_t* out_length)
{
    ClearError();
    if (!path)
    {
        SetError("null pointer argument");
        return NOD_FST_STOP;
    }
    const NodHandle* handle = CheckHandle(partition);
    if (!handle) return NOD_FST_STOP;
    if (handle->kind != NodHandle::Kind::Partition)
    {
        SetError("handle is not a partition");
        return NOD_FST_STOP;
    }
    FstView fst;
    if (!ParseFst(*handle, fst)) return NOD_FST_STOP;
    if (!IsValidUtf8(path, std::strlen(path)))
    {
        SetError("invalid UTF-8 in path");
        return NOD_FST_STOP;
    }
    std::uint32_t node = 0;
    if (!fst.Find(path, node)) return NOD_FST_STOP;
    if (out_kind && Aligned(out_kind)) *out_kind = fst.IsDirectory(node) ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE;
    if (out_length && Aligned(out_length)) *out_length = fst.Length(node);
    return node;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data)
{
    ClearError();
    const NodHandle* handle = CheckHandle(partition);
    if (!handle) return;
    if (handle->kind != NodHandle::Kind::Partition)
    {
        SetError("handle is not a partition");
        return;
    }
    if (!callback)
    {
        SetError("null pointer argument");
        return;
    }
    FstView fst;
    if (!ParseFst(*handle, fst)) return;
    // From the first node after the root, to whichever node the callback names.
    for (std::uint32_t node = 1; node < fst.Count();)
    {
        const char* name = fst.Name(node);
        node = callback(node, fst.IsDirectory(node) ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE,
                        name ? name : "<invalid>", fst.Length(node), user_data);
    }
}

} // extern "C"
