#pragma once
#include "resources/binary_reader.h"
#include <array>
#include <memory>
#include <vector>

namespace mscharged
{
enum class ParticleFileKind : unsigned { Resident, NonResident, Geometry, Textures };
struct ParticleFiles
{
    // NonResident contains the decompressed file; other slots preserve disc bytes.
    std::array<std::vector<std::uint8_t>, 4> data;
    std::array<std::size_t, 4> source_sizes{};
    const auto& operator[](ParticleFileKind kind) const { return data.at(static_cast<unsigned>(kind)); }
};
enum class ParticleFileState { Loading, Ready, Failed, Cancelled };

// The four source-ordered requests in EmissionManager::StartLoading(false,
// false, false, true). Ready supplies owned bytes, not registered templates,
// geometry/textures, an initialized EmissionManager or completed boot service41.
// Requires initialized NL files/arenas; service and destroy on the owner thread
// before file shutdown. Retained results are independent of both game arenas.
class ParticleFileLoad
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    static constexpr std::size_t MaximumRetainedBytes = 32 * 1024 * 1024;
    ParticleFileLoad();
    ~ParticleFileLoad();
    ParticleFileLoad(const ParticleFileLoad&) = delete;
    ParticleFileLoad& operator=(const ParticleFileLoad&) = delete;
    void Poll();
    void Service();
    void Cancel();
    ParticleFileState State() const;
    unsigned CompletedFiles() const;
    std::shared_ptr<const ParticleFiles> Result() const;
};
}
