#include <SDL3/SDL.h>
#include <aurora/aurora.h>
#include <aurora/gfx.hpp>
#include <aurora/pipeline_cache.h>

#include "gfx/clear.hpp"
#include "gfx/frame_packet.hpp"
#include "gfx/hash.hpp"
#include "gfx/pipeline_cache.hpp"
#include "gx/pipeline.hpp"
#include "gx/fifo.hpp"
#include "dolphin/gx/__gx.h"
#include "webgpu/gpu.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}
std::string Utf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}
struct Session {
  bool live = false;
  ~Session() { if (live) aurora_shutdown(); }
  void Stop() {
    aurora_shutdown(); live = false;
    const auto stopped = aurora_pipeline_warmup_status();
    Check(!stopped.ready && stopped.pendingPipelines == 0 && stopped.compilingPipelines == 0 &&
          stopped.totalPipelines == 0 && stopped.knownConfigs == 0,
          "Shutdown retained pipeline state");
  }
};
void Start(Session& session, int argc, char** argv, AuroraBackend backend,
           const std::filesystem::path& directory, const std::filesystem::path& resources, unsigned msaa,
           bool automaticPreparation = false) {
  std::filesystem::create_directories(directory);
  const auto path = Utf8(directory), resourcePath = Utf8(resources);
  AuroraConfig config{};
  config.appName = "Charged pipeline warmup test";
  config.userPath = config.cachePath = path.c_str();
  config.resourcesPath = resourcePath.c_str();
  config.desiredBackend = backend;
  config.windowWidth = 320; config.windowHeight = 240;
  config.windowPosX = config.windowPosY = -1;
  config.msaa = msaa;
  config.vsync = true;
  config.logLevel = LOG_WARNING;
  config.enableBackendValidation = true;
  config.automaticPipelinePreparation = automaticPreparation;
  const auto host = aurora_initialize(argc, argv, &config);
  session.live = true;
  Check(host.window && host.backend == backend, "Requested GPU backend unavailable");
  Check(aurora::webgpu::g_adapterInfo.adapterType != wgpu::AdapterType::CPU,
        "CPU adapter cannot qualify GPU pipeline preparation");
}
AuroraPipelineWarmupStatus WaitReady(std::chrono::seconds timeout = std::chrono::seconds(90)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    const auto status = aurora_pipeline_warmup_status();
    Check(status.failedPipelines == 0, "A cached pipeline failed to compile");
    if (status.ready) {
      Check(status.pendingPipelines == 0 && status.compilingPipelines == 0 &&
            status.totalPipelines == status.readyPipelines,
            "Readiness preceded final pipeline publication/maintenance");
      return status;
    }
    Check(std::chrono::steady_clock::now() < deadline, "Timed out preparing pipelines");
    SDL_PumpEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
// Include inside pipeline_warmup_gpu.cpp's anonymous namespace. The function
// uses its existing Start/Session/Check/WaitReady helpers and actual GPU cache.
void CollectionChecks(int argc, char** argv, AuroraBackend backend,
                      const std::filesystem::path& directory,
                      const std::filesystem::path& resources) {
  Session session;
  Start(session, argc, argv, backend, directory, resources, 4, true);
  Check(WaitReady().knownConfigs == 0, "Collection fixture must start without a seed");
  Check(aurora_begin_pipeline_generation(), "Automatic generation begin failed");
  aurora_update();
  Check(aurora_begin_frame(), "Collection fixture frame did not begin");
  const auto setState = [](unsigned variant) {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    // Pointer 1 is intentionally unreadable: collecting metadata must not read it.
    GXSetArray(GX_VA_POS, reinterpret_cast<const void*>(1), 0,
               variant == 0 ? 12 : 16, variant == 0);
    GXSetNumChans(0);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetNumIndStages(0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ONE);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetCullMode(variant == 0 ? GX_CULL_BACK : GX_CULL_NONE);
    GXSetZMode(GX_TRUE, variant == 0 ? GX_LEQUAL : GX_EQUAL, variant == 0);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetBlendMode(variant == 0 ? GX_BM_NONE : GX_BM_BLEND,
                   GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
  };
  std::array<aurora::gx::PipelineConfig, 2> expected{};
  for (unsigned i = 0; i < expected.size(); ++i) {
    setState(i);
    GXFlush();
    aurora::gx::fifo::drain();
    aurora::gx::populate_pipeline_config(expected[i], GX_TRIANGLES, GX_VTXFMT0);
  }
  const auto sdkBefore = *__gx;
  const auto registersBefore = aurora::gx::g_gxState.bpRegCache;
  aurora::gx::PipelineConfig before{};
  aurora::gx::populate_pipeline_config(before, GX_TRIANGLES, GX_VTXFMT0);
  const auto restored = [&] {
    Check(std::memcmp(__gx, &sdkBefore, sizeof sdkBefore) == 0, "Collection changed SDK shadow");
    Check(aurora::gx::g_gxState.bpRegCache == registersBefore, "Collection changed live BP registers");
    aurora::gx::PipelineConfig after{};
    aurora::gx::populate_pipeline_config(after, GX_TRIANGLES, GX_VTXFMT0);
    Check(std::memcmp(&before, &after, sizeof before) == 0, "Collection changed live pipeline state");
    Check(aurora::gx::fifo::get_buffer_size() == 0, "Collection published commands into real FIFO");
  };
  Check(aurora_begin_pipeline_collection(), "Collection begin failed");
  Check(!aurora_begin_pipeline_collection(), "Nested collection was accepted");
  Check(!aurora_begin_pipeline_generation(), "Generation reset inside collection was accepted");
  Check(!aurora_pipeline_warmup_status().ready, "Open discovery reported ready");
  setState(0);
  Check(aurora_collect_pipeline(GX_TRIANGLES, GX_VTXFMT0, false), "Cancelled collection failed");
  Check(aurora_end_pipeline_collection(false), "Clean cancellation failed");
  restored();
  Check(WaitReady().knownConfigs == 0, "Cancellation persisted or compiled configurations");

  Check(aurora_begin_pipeline_scope_census(), "Collection final census begin failed");
  Check(aurora_begin_pipeline_collection(), "Valid batch begin failed");
  for (unsigned i : {0u, 1u, 0u}) {
    Check(aurora_reset_pipeline_collection(), "Variant reset failed");
    setState(i);
    Check(aurora_collect_pipeline(GX_TRIANGLES, GX_VTXFMT0, false), "Variant collection failed");
  }
  Check(aurora_end_pipeline_collection(true), "Collection enqueue failed");
  restored();
  Check(aurora_seal_pipeline_generation(), "Collection final census seal failed");
  const auto prepared = WaitReady();
  Check(prepared.knownConfigs == 2 && prepared.demandMisses == 0,
        "Collection did not deduplicate/background-prepare exact configs");
  const auto layout = aurora::gfx::scene_render_target_layout();
  for (const auto& config : expected) aurora::gfx::find_pipeline(config, layout);
  const auto hits = aurora_pipeline_warmup_status();
  Check(hits.demandHits == 2 && hits.demandMisses == 0,
        "Collected keys differ from real decoder's first draw keys");

  Check(aurora_begin_pipeline_collection(), "Rejected draw batch begin failed");
  GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
  GXEnd();
  Check(!aurora_end_pipeline_collection(true), "Actual draw was accepted during collection");
  restored();

  Check(aurora_begin_pipeline_collection(), "Overflow batch begin failed");
  for (unsigned i = 0; i < 32768; ++i) GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  Check(!aurora_collect_pipeline(GX_TRIANGLES, GX_VTXFMT0, false), "Capture overflow silently succeeded");
  Check(!aurora_end_pipeline_collection(true), "Failed overflow batch submitted");
  restored();
  // A producer may fail before Emit. Reset/end must still reject the trailing
  // hardware command and must not turn an overflow into a successful empty batch.
  Check(aurora_begin_pipeline_collection(), "Trailing copy batch begin failed");
  GXCopyTex(nullptr, GX_FALSE);
  Check(!aurora_end_pipeline_collection(true), "Trailing hardware copy silently discarded at submit");
  restored();
  Check(aurora_begin_pipeline_collection(), "Reset copy batch begin failed");
  GXCopyTex(nullptr, GX_FALSE);
  Check(!aurora_reset_pipeline_collection(), "Reset silently discarded unsupported command");
  Check(!aurora_end_pipeline_collection(true), "Reset failure did not latch batch failure");
  restored();
  Check(aurora_begin_pipeline_collection(), "Reset overflow batch begin failed");
  for (unsigned i = 0; i < 32768; ++i) GXSetBlendMode(GX_BM_BLEND, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
  Check(!aurora_reset_pipeline_collection(), "Reset silently cleared capture overflow");
  Check(!aurora_end_pipeline_collection(true), "Reset overflow failure did not latch");
  restored();
  GXTexObj borrowedTexture{};
  GXInitTexObj(&borrowedTexture, reinterpret_cast<const void*>(1), 8, 8,
               GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
  const auto textureBefore = borrowedTexture;
  Check(aurora_begin_pipeline_collection(), "Texture metadata batch begin failed");
  GXLoadTexObj(&borrowedTexture, GX_TEXMAP3);
  Check(aurora_end_pipeline_collection(true), "Texture metadata collection failed");
  Check(std::memcmp(&borrowedTexture, &textureBefore, sizeof borrowedTexture) == 0,
        "Collection mutated a borrowed texture object while binding metadata");
  restored();
  Check(WaitReady().knownConfigs == 2, "Failed batches changed known configurations");
  aurora_end_frame_no_present();
  session.Stop();
}

std::array<aurora::gfx::clear::PipelineConfig, 8> Configs() {
  std::array<aurora::gfx::clear::PipelineConfig, 8> result{};
  for (unsigned i = 0; i < result.size(); ++i) {
    result[i].clearColor = (i & 1) != 0;
    result[i].clearAlpha = (i & 2) != 0;
    result[i].clearDepth = (i & 4) != 0;
  }
  return result;
}
void RequestKnown(bool bothLayouts) {
  const auto scene = aurora::gfx::scene_render_target_layout();
  auto offscreen = scene;
  offscreen.sampleCount = 1;
  offscreen.colorAttachmentCount = 1;
  aurora::gfx::detail::finalize_render_target_layout(offscreen);
  for (const auto& config : Configs()) {
    const auto ref = aurora::gfx::find_pipeline(config, scene);
    aurora::gfx::CompiledPipeline pipeline;
    Check(aurora::gfx::get_pipeline(ref, pipeline), "Known scene pipeline unavailable");
    if (bothLayouts && offscreen.key != scene.key) {
      const auto offscreenRef = aurora::gfx::find_pipeline(config, offscreen);
      Check(aurora::gfx::get_pipeline(offscreenRef, pipeline), "Known offscreen pipeline unavailable");
    }
  }
}
void VerifyHits(unsigned expectedRequests) {
  const auto before = WaitReady();
  RequestKnown(true);
  const auto after = WaitReady();
  Check(after.demandHits - before.demandHits == expectedRequests,
        "Prepared configurations did not produce demand cache hits");
  Check(after.demandMisses == before.demandMisses &&
        after.demandWaitNanoseconds == before.demandWaitNanoseconds,
        "First post-warmup requests incurred a pipeline miss/wait");
  Check(after.totalPipelines == before.totalPipelines, "Demand rebuilt a prepared pipeline");
  std::printf("Prepared requests: %u hits, 0 misses, 0 ns demand wait; ready=%llu.\n",
              expectedRequests, static_cast<unsigned long long>(after.readyPipelines));
}

// What aurora_set_pipeline_demand_miss_callback reported; the configuration
// pointer is valid only during the call.
struct DemandMissLog {
  std::vector<AuroraPipelineDemandMiss> misses;
  std::vector<std::vector<unsigned char>> configs;
};
void RecordDemandMiss(const AuroraPipelineDemandMiss* miss, void* user) {
  auto& log = *static_cast<DemandMissLog*>(user);
  log.misses.push_back(*miss);
  const auto* bytes = static_cast<const unsigned char*>(miss->config);
  log.configs.emplace_back(bytes, bytes + miss->configSize);
  log.misses.back().config = nullptr;
}
// A known configuration drawn to a render target that preparation did not cover;
// returns once it has compiled, as an encoder would.
void RequestOnOtherLayout(unsigned index) {
  auto layout = aurora::gfx::scene_render_target_layout();
  layout.colorAttachments[0].semantic = aurora::gfx::ColorAttachmentSemantic::Auxiliary;
  aurora::gfx::detail::finalize_render_target_layout(layout);
  const auto ref = aurora::gfx::find_pipeline(Configs()[index], layout);
  aurora::gfx::CompiledPipeline pipeline;
  Check(aurora::gfx::get_pipeline(ref, pipeline), "Pipeline for another render target unavailable");
}

void AutomaticSelectionChecks(int argc, char** argv, AuroraBackend backend,
                              const std::filesystem::path& directory,
                              const std::filesystem::path& resources) {
  {
    Session session;
    Start(session, argc, argv, backend, directory / "automatic-empty", resources, 1, true);
    const auto empty = WaitReady();
    Check(empty.knownConfigs == 0 && empty.totalPipelines == 0,
          "Automatic discovery imported the bundled catalogue");
    session.Stop();
  }
  {
    Session session;
    // This database contains eight descriptions recorded by normal requests in
    // the earlier session. Startup must retain them without preparing all eight.
    Start(session, argc, argv, backend, directory / "observed", resources, 1, true);
    const auto loaded = WaitReady();
    Check(loaded.knownConfigs == 8 && loaded.totalPipelines == 0,
          "Automatic startup rebuilt inactive retained configurations");
    Check(aurora_begin_pipeline_generation(), "First selection generation failed");
    const auto configs = Configs();
    const auto originalLayout = aurora::gfx::scene_render_target_layout();
    const auto oldHandle = aurora::gfx::find_pipeline(configs[0], originalLayout);
    Check(WaitReady().totalPipelines == 1, "First selected demand did not compile once");
    Check(aurora_begin_pipeline_scope_census() && aurora_seal_pipeline_generation_for_resources(1),
          "First selection scope seal failed");
    Check(aurora_begin_pipeline_generation(), "Second selection generation failed");
    aurora::gfx::find_pipeline(configs[1], originalLayout);
    Check(aurora_begin_pipeline_scope_census() && aurora_seal_pipeline_generation_for_resources(2),
          "Second selection scope seal failed");
    const auto selected = WaitReady();
    Check(selected.totalPipelines == 2 && selected.knownConfigs == 8,
          "Generation reset discarded retained handles or descriptions");
    aurora::gfx::CompiledPipeline oldPipeline;
    Check(aurora::gfx::get_pipeline(oldHandle, oldPipeline), "Generation reset invalidated an old handle");

    // Exercise the production layout-change resolver without recording a frame.
    // Only the current selection may create a new 4x key; its 1x key already exists.
    // GPU attachment resources are never used with this temporary test layout.
    const auto savedSamples = aurora::webgpu::g_graphicsConfig.msaaSamples;
    aurora::webgpu::g_graphicsConfig.msaaSamples = 4;
    aurora_prepare_pipelines();
    const auto resized = WaitReady();
    aurora::webgpu::g_graphicsConfig.msaaSamples = savedSamples;
    aurora_prepare_pipelines();
    WaitReady();
    Check(resized.totalPipelines == 3 && resized.knownConfigs == 8,
          "Layout rebuild prepared inactive configurations or forgot retained ones");
    Check(resized.demandMisses == selected.demandMisses && resized.demandHits == selected.demandHits,
          "Automatic layout preparation performed demand work");
    session.Stop();
  }
  {
    Session session;
    Start(session, argc, argv, backend, directory / "observed", resources, 1, true);
    const auto reloaded = WaitReady();
    Check(reloaded.knownConfigs == 8 && reloaded.totalPipelines == 0,
          "Inactive configurations were lost or eagerly rebuilt on later launch");
    session.Stop();
  }
}

using CatalogueConfig = std::variant<aurora::gfx::clear::PipelineConfig, aurora::gx::PipelineConfig>;
struct Catalogue {
  std::vector<CatalogueConfig> configs;
  unsigned clearCount = 0;
  unsigned gxCount = 0;
};
struct Database {
  sqlite3* handle = nullptr;
  sqlite3_stmt* statement = nullptr;
  ~Database() {
    if (statement) sqlite3_finalize(statement);
    if (handle) sqlite3_close(handle);
  }
};

template <typename Config>
Config DecodeConfig(const void* blob, int bytes, uint32_t version, int64_t storedHash,
                    aurora::gfx::ShaderType type) {
  static_assert(std::has_unique_object_representations_v<Config>);
  Check(bytes >= 0 && static_cast<size_t>(bytes) == sizeof(Config) && blob != nullptr, "Catalogue config does not match the target ABI size");
  Config config;
  std::memcpy(&config, blob, sizeof(config));
  Check(config.version == version, "Catalogue blob has an incompatible config version");
  const auto hash = aurora::xxh3_hash(config, static_cast<aurora::HashType>(type));
  Check(static_cast<uint64_t>(storedHash) == static_cast<uint64_t>(hash),
        "Stored catalogue hash differs from the native Aurora XXH3(config, type)");
  return config;
}

Catalogue ReadCatalogue(const std::filesystem::path& path) {
  Database db;
  const auto text = Utf8(path);
  Check(sqlite3_open_v2(text.c_str(), &db.handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK,
        "Cannot open catalogue read-only");
  Check(sqlite3_prepare_v2(db.handle, "SELECT value FROM aurora_schema", -1, &db.statement, nullptr) == SQLITE_OK,
        "Catalogue schema table missing");
  Check(sqlite3_step(db.statement) == SQLITE_ROW && sqlite3_column_int(db.statement, 0) == 1,
        "Catalogue SQLite schema version is incompatible");
  Check(sqlite3_step(db.statement) == SQLITE_DONE, "Catalogue has multiple schema versions");
  sqlite3_finalize(db.statement); db.statement = nullptr;
  Check(sqlite3_prepare_v2(db.handle,
        "SELECT type, hash, config_version, config_size, config, first_frame_used "
        "FROM pipeline_cache ORDER BY first_frame_used, type, hash", -1, &db.statement, nullptr) == SQLITE_OK,
        "Cannot read catalogue configurations");
  Catalogue catalogue;
  int result = SQLITE_OK;
  while ((result = sqlite3_step(db.statement)) == SQLITE_ROW) {
    const auto type = sqlite3_column_int(db.statement, 0);
    const auto hash = sqlite3_column_int64(db.statement, 1);
    const auto version = sqlite3_column_int64(db.statement, 2);
    const auto storedSize = sqlite3_column_int64(db.statement, 3);
    const auto* blob = sqlite3_column_blob(db.statement, 4);
    const auto bytes = sqlite3_column_bytes(db.statement, 4);
    const auto firstFrame = sqlite3_column_int64(db.statement, 5);
    Check(storedSize == bytes && firstFrame >= 0 && firstFrame <= UINT32_MAX,
          "Catalogue row metadata is invalid");
    if (type == static_cast<int>(aurora::gfx::ShaderType::Clear)) {
      Check(version == aurora::gfx::clear::ClearPipelineConfigVersion, "Catalogue clear config version is incompatible");
      catalogue.configs.emplace_back(DecodeConfig<aurora::gfx::clear::PipelineConfig>(
          blob, bytes, static_cast<uint32_t>(version), hash, aurora::gfx::ShaderType::Clear));
      ++catalogue.clearCount;
    } else if (type == static_cast<int>(aurora::gfx::ShaderType::GX)) {
      Check(version == aurora::gx::GXPipelineConfigVersion, "Catalogue GX config version is incompatible");
      catalogue.configs.emplace_back(DecodeConfig<aurora::gx::PipelineConfig>(
          blob, bytes, static_cast<uint32_t>(version), hash, aurora::gfx::ShaderType::GX));
      ++catalogue.gxCount;
    } else {
      throw std::runtime_error("Catalogue contains unsupported shader type");
    }
  }
  Check(result == SQLITE_DONE && !catalogue.configs.empty(), "Catalogue read failed or contains no configs");
  std::printf("Native catalogue hashes verified: clear=%u (ABI %zu), GX=%u (ABI %zu).\n",
              catalogue.clearCount, sizeof(aurora::gfx::clear::PipelineConfig),
              catalogue.gxCount, sizeof(aurora::gx::PipelineConfig));
  return catalogue;
}

void LearnedScopeChecks(int argc, char** argv, AuroraBackend backend,
                        const std::filesystem::path& directory,
                        const std::filesystem::path& resources) {
  // These are actual state-collector configs saved by the earlier fixture, not
  // hand-injected database records or copies of a shipped catalogue.
  const auto collected = ReadCatalogue(directory / "collected" / "pipeline_cache.db");
  std::vector<aurora::gx::PipelineConfig> gx;
  for (const auto& entry : collected.configs)
    if (const auto* config = std::get_if<aurora::gx::PipelineConfig>(&entry)) gx.push_back(*config);
  Check(gx.size() == 2, "Learned-scope fixture needs two genuine collected states");
  const auto a = gx[0], unknown = gx[1];
  auto c = a;
  c.cullMode = GX_CULL_FRONT; // another valid state; collector used BACK/NONE
  constexpr uint64_t matchA = 0x5354414449554d41ull;
  constexpr uint64_t matchB = 0x5354414449554d42ull;
  const auto clear = Configs();
  const auto prepare = [](std::initializer_list<aurora::gx::PipelineConfig> configs) {
    aurora::gfx::prepare_pipeline_configs({configs.begin(), configs.size()});
  };
  const auto begin = [] {
    Check(aurora_begin_pipeline_generation(), "Learned generation begin failed");
    // Incremental FE resource publication waits use readiness before final census.
    // Keeping ready false for the entire generation would deadlock that loader.
    Check(WaitReady().ready, "Unsealed generation blocked incremental resource readiness");
    Check(!aurora_seal_pipeline_generation_for_resources(matchA), "Seal without final census was accepted");
    Check(aurora_begin_pipeline_scope_census(), "Learned scope census begin failed");
    Check(!aurora_pipeline_warmup_status().ready, "Final census reported ready before seal");
  };
  const auto requestLearned = [&](bool bothLayouts) {
    const auto scene = aurora::gfx::scene_render_target_layout();
    auto offscreen = scene;
    offscreen.sampleCount = 1;
    offscreen.colorAttachmentCount = 1;
    aurora::gfx::detail::finalize_render_target_layout(offscreen);
    const auto request = [&](const auto& config) {
      aurora::gfx::CompiledPipeline pipeline;
      Check(aurora::gfx::get_pipeline(aurora::gfx::find_pipeline(config, scene), pipeline),
            "Learned scene pipeline unavailable");
      if (bothLayouts && offscreen.key != scene.key)
        Check(aurora::gfx::get_pipeline(aurora::gfx::find_pipeline(config, offscreen), pipeline),
              "Learned offscreen pipeline unavailable");
    };
    request(unknown);
    request(clear[0]);
  };
  const auto assertLearnedHits = [&](unsigned requests) {
    const auto before = WaitReady();
    requestLearned(true);
    const auto after = WaitReady();
    Check(after.demandHits == before.demandHits + requests && after.demandMisses == before.demandMisses &&
          after.demandWaitNanoseconds == before.demandWaitNanoseconds,
          "A first learned request incurred a compilation miss/wait");
  };
  {
    Session session;
    Start(session, argc, argv, backend, directory / "learned", resources, 1, true);
    Check(WaitReady().knownConfigs == 0, "Learned cache did not start empty");
    // Original startup initializes material resources without a reset hook.
    // Generation zero must accept its final census and preserve early demands.
    prepare({c});
    Check(WaitReady().readyPipelines == 1,
          "Incremental resources could not become ready before final census");
    Check(aurora_begin_pipeline_scope_census(), "Cold learned census begin failed");
    prepare({a, c});
    aurora::gfx::find_pipeline(clear[0], aurora::gfx::scene_render_target_layout());
    Check(!aurora_pipeline_warmup_status().ready, "Preseal demand incorrectly opened final census gate");
    Check(aurora_seal_pipeline_generation_for_resources(matchA), "Cold resource seal failed");
    Check(aurora_seal_pipeline_generation_for_resources(matchA), "Same resource seal was not idempotent");
    Check(!aurora_seal_pipeline_generation_for_resources(matchB) && !aurora_seal_pipeline_generation(),
          "Sealed generation accepted a different identity kind/value");
    const auto before = WaitReady();
    Check(before.totalPipelines == 3 && before.knownConfigs == 3, "Cold generated/preseal state count mismatch");
    aurora::gfx::find_pipeline(unknown, aurora::gfx::scene_render_target_layout());
    const auto after = WaitReady();
    Check(after.demandMisses == before.demandMisses + 1 && after.knownConfigs == 4 && after.totalPipelines == 4,
          "First unknown state was not observed as one real demand miss");
    session.Stop(); // joins the actual async SQLite writer; next session reopens it
  }
  {
    Session session;
    Start(session, argc, argv, backend, directory / "learned", resources, 4, true);
    const auto loaded = WaitReady();
    Check(loaded.knownConfigs == 4 && loaded.totalPipelines == 0,
          "Reload eagerly compiled or lost remembered descriptions");
    begin();
    // A stable resource identity must survive a different producer-key subset
    // (e.g. inherited source state). Learned exact states remain available.
    prepare({c, c});
    Check(aurora_seal_pipeline_generation_for_resources(matchA), "Reloaded resource seal failed");
    const auto ready = WaitReady();
    Check(ready.totalPipelines == 6 && ready.knownConfigs == 4 && ready.demandMisses == 0,
          "Learned GX/Clear states did not prepare both changed-AA layouts selectively");
    assertLearnedHits(4);
    session.Stop();
  }
  {
    Session session;
    Start(session, argc, argv, backend, directory / "learned", resources, 1, true);
    begin();
    prepare({c});
    Check(aurora_seal_pipeline_generation_for_resources(matchB), "Different resource seal failed");
    Check(WaitReady().totalPipelines == 1, "Different selected resources preloaded another match's learned states");
    aurora::gfx::find_pipeline(clear[1], aurora::gfx::scene_render_target_layout());
    Check(WaitReady().knownConfigs == 5, "Different resource observation was not retained");
    session.Stop();
  }
  {
    Session session;
    Start(session, argc, argv, backend, directory / "learned", resources, 1, true);
    begin();
    prepare({c, a, a});
    Check(aurora_seal_pipeline_generation_for_resources(matchA), "Returning resource seal failed");
    const auto ready = WaitReady();
    Check(ready.totalPipelines == 4 && ready.knownConfigs == 5,
          "Returning resources lost learned states or selected unrelated ones");
    assertLearnedHits(2);
    session.Stop();
  }
  // The generated-key fallback excludes transient loader publications, demand
  // observations, and duplicate producer calls from its identity.
  for (unsigned run = 0; run < 2; ++run) {
    Session session;
    Start(session, argc, argv, backend, directory / "learned-generated", resources, 1, true);
    Check(WaitReady().totalPipelines == 0, "Fallback startup prepared inactive configs");
    Check(aurora_begin_pipeline_generation(), "Fallback generation begin failed");
    if (run == 0) prepare({c}); // transient state absent on second launch
    Check(WaitReady().ready, "Fallback loader publication did not become ready");
    Check(aurora_begin_pipeline_scope_census(), "Fallback census begin failed");
    prepare(run == 0 ? std::initializer_list<aurora::gx::PipelineConfig>{a} :
                       std::initializer_list<aurora::gx::PipelineConfig>{a, a});
    Check(aurora_seal_pipeline_generation(), "Generated-key seal failed");
    const auto ready = WaitReady();
    Check(ready.totalPipelines == (run == 0 ? 2 : 3), "Fallback fingerprint included transient/observed states");
    if (run == 0) requestLearned(false);
    else assertLearnedHits(2);
    session.Stop();
  }
  std::puts("Learned scopes: genuine later launches preload first-use GX/Clear states; stable resource identity, "
            "changed AA, different selections, and final-only fallback census passed.");
}

void VerifyCatalogue(int argc, char** argv, AuroraBackend backend, unsigned msaa,
                     const std::filesystem::path& input, const std::filesystem::path& directory,
                     const std::filesystem::path& resources) {
  // Validate through this target's real C++ layouts and hash implementation,
  // independent of the portable catalogue generator. Input is never writable.
  const auto catalogue = ReadCatalogue(input);
  std::filesystem::copy_file(input, resources / "initial_pipeline_cache.db");
  Session session;
  const auto started = std::chrono::steady_clock::now();
  Start(session, argc, argv, backend, directory / "catalogue-cache", resources, msaa);
  const auto initialized = std::chrono::steady_clock::now();
  aurora_prepare_pipelines();
  const auto prepared = WaitReady(std::chrono::seconds(600));
  const auto readyAt = std::chrono::steady_clock::now();
  const auto scene = aurora::gfx::scene_render_target_layout();
  auto offscreen = scene;
  offscreen.sampleCount = 1;
  offscreen.colorAttachmentCount = 1;
  aurora::gfx::detail::finalize_render_target_layout(offscreen);
  const auto layoutCount = offscreen.key == scene.key ? 1u : 2u;
  const auto requests = catalogue.configs.size() * layoutCount;
  Check(prepared.knownConfigs == catalogue.configs.size() && prepared.readyPipelines == requests,
        "Full catalogue did not prepare every target scene/offscreen runtime key");
  Check(prepared.demandHits == 0 && prepared.demandMisses == 0 && prepared.demandWaitNanoseconds == 0,
        "Catalogue preparation unexpectedly performed demand work");
  for (const auto& entry : catalogue.configs) {
    std::visit([&](const auto& config) {
      const auto ref = aurora::gfx::find_pipeline(config, scene);
      aurora::gfx::CompiledPipeline pipeline;
      Check(aurora::gfx::get_pipeline(ref, pipeline), "Catalogue scene request returned no compiled pipeline");
      if (layoutCount == 2) {
        const auto offscreenRef = aurora::gfx::find_pipeline(config, offscreen);
        Check(aurora::gfx::get_pipeline(offscreenRef, pipeline), "Catalogue offscreen request returned no compiled pipeline");
      }
    }, entry);
  }
  const auto after = WaitReady();
  Check(after.demandHits == requests && after.demandMisses == 0 && after.demandWaitNanoseconds == 0,
        "A first catalogue request caused a compilation miss/wait");
  Check(after.totalPipelines == prepared.totalPipelines && after.readyPipelines == prepared.readyPipelines,
        "A first catalogue request created an additional runtime pipeline");
  const double startupSeconds = std::chrono::duration<double>(readyAt - started).count();
  const double preparationSeconds = std::chrono::duration<double>(readyAt - initialized).count();
  std::printf("Catalogue warmup: configs=%zu runtime_keys=%llu msaa=%u startup_seconds=%.6f "
              "preparation_seconds=%.6f demand_hits=%llu demand_misses=0 demand_wait_ns=0.\n",
              catalogue.configs.size(), static_cast<unsigned long long>(prepared.readyPipelines), msaa,
              startupSeconds, preparationSeconds, static_cast<unsigned long long>(after.demandHits));
  session.Stop();
}

// Measurement only (--catalog DB --demand-timing N): N catalogue GX
// configurations missed by draws, compiled one draw after another (each waited
// for before the next is recorded) and all requested at once, each in a new
// session without a seed. Run with the driver's shader cache disabled.
void DemandTiming(int argc, char** argv, AuroraBackend backend, unsigned msaa, const std::filesystem::path& input,
                  const std::filesystem::path& directory, const std::filesystem::path& resources, unsigned count) {
  const auto catalogue = ReadCatalogue(input);
  std::vector<aurora::gx::PipelineConfig> configs;
  for (const auto& entry : catalogue.configs) {
    if (const auto* config = std::get_if<aurora::gx::PipelineConfig>(&entry); config && configs.size() < count)
      configs.push_back(*config);
  }
  Check(configs.size() == count, "Catalogue holds fewer GX configurations than requested");
  unsigned run = 0;
  for (const bool together : {false, true, false, true}) {
    Session session;
    Start(session, argc, argv, backend, directory / ("timing-" + std::to_string(run++)), resources, msaa);
    aurora_prepare_pipelines();
    Check(WaitReady().knownConfigs == 0, "Timing session did not start empty");
    const auto scene = aurora::gfx::scene_render_target_layout();
    aurora::gfx::CompiledPipeline pipeline;
    const auto started = std::chrono::steady_clock::now();
    if (together) {
      std::vector<aurora::gfx::PipelineRef> refs;
      for (const auto& config : configs) refs.push_back(aurora::gfx::find_pipeline(config, scene));
      for (const auto ref : refs) Check(aurora::gfx::get_pipeline(ref, pipeline), "Timed pipeline unavailable");
    } else {
      for (const auto& config : configs)
        Check(aurora::gfx::get_pipeline(aurora::gfx::find_pipeline(config, scene), pipeline), "Timed pipeline unavailable");
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto status = WaitReady();
    std::printf("Demand timing: %u pipelines %s in %.1f ms (encoders waited %.1f ms)\n", count,
                together ? "requested at once" : "one after another", seconds * 1e3,
                status.demandWaitNanoseconds / 1e6);
    session.Stop();
  }
}
}

int main(int argc, char** argv) {
  try {
#if defined(_WIN32)
    AuroraBackend backend = BACKEND_D3D12;
#elif defined(__APPLE__)
    AuroraBackend backend = BACKEND_METAL;
#else
    AuroraBackend backend = BACKEND_VULKAN;
#endif
    auto directory = std::filesystem::temp_directory_path() /
        ("charged-pipeline-warmup-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path catalogue;
    unsigned catalogueMsaa = 1;
    unsigned demandTiming = 0;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--demand-timing" && i + 1 < argc) {
        demandTiming = static_cast<unsigned>(std::stoul(argv[++i]));
      } else if (arg == "--directory" && i + 1 < argc) {
        directory = std::filesystem::path(reinterpret_cast<const char8_t*>(argv[++i]));
      } else if (arg == "--catalog" && i + 1 < argc) {
        catalogue = std::filesystem::path(reinterpret_cast<const char8_t*>(argv[++i]));
      } else if (arg == "--msaa" && i + 1 < argc) {
        const std::string value = argv[++i];
        Check(value == "1" || value == "4", "--msaa must be 1 or 4");
        catalogueMsaa = value == "1" ? 1 : 4;
      } else if (arg == "--backend" && i + 1 < argc) {
        const std::string value = argv[++i];
        if (value == "vulkan") backend = BACKEND_VULKAN;
        else if (value == "dx12") backend = BACKEND_D3D12;
        else if (value == "metal") backend = BACKEND_METAL;
        else throw std::runtime_error("Unknown backend");
      } else throw std::runtime_error("Usage: pipeline_warmup_gpu_tests [--backend vulkan|dx12|metal] [--directory NEW_PATH] [--catalog DB --msaa 1|4 [--demand-timing N]]");
    }
    Check(!std::filesystem::exists(directory), "Fixture directory must be new (preserves existing caches)");
    const auto resources = directory / "resources";
    std::filesystem::create_directories(resources);
    if (!catalogue.empty() && demandTiming != 0) {
      DemandTiming(argc, argv, backend, catalogueMsaa, catalogue, directory, resources, demandTiming);
      return 0;
    }
    if (!catalogue.empty()) {
      VerifyCatalogue(argc, argv, backend, catalogueMsaa, catalogue, directory, resources);
      std::printf("Catalogue GPU passed (%u checks); evidence: %s\n", checks, Utf8(directory).c_str());
      return 0;
    }

    CollectionChecks(argc, argv, backend, directory / "collected", resources);

    // Establish an actual observed catalogue through normal production requests
    // and the real asynchronous SQLite writer. No saved pipeline is injected.
    {
      Session session;
      Start(session, argc, argv, backend, directory / "observed", resources, 1);
      aurora_prepare_pipelines();
      const auto empty = WaitReady();
      Check(empty.knownConfigs == 0 && empty.totalPipelines == 0, "Missing seed did not start empty");
      DemandMissLog demand;
      aurora_set_pipeline_demand_miss_callback(RecordDemandMiss, &demand);
      // Recording does not wait for missing pipelines: all eight compile at
      // once, and each draw waits only when it is encoded (get_pipeline).
      const auto configs = Configs();
      const auto scene = aurora::gfx::scene_render_target_layout();
      std::vector<aurora::gfx::PipelineRef> refs;
      for (const auto& config : configs) refs.push_back(aurora::gfx::find_pipeline(config, scene));
      Check(aurora_pipeline_warmup_status().pendingPipelines > 0, "Recording waited for missing pipelines");
      for (const auto ref : refs) {
        aurora::gfx::CompiledPipeline pipeline;
        Check(aurora::gfx::get_pipeline(ref, pipeline), "Encoding did not wait for a missing pipeline");
      }
      const auto observed = WaitReady();
      Check(observed.demandMisses == 8 && observed.demandWaitNanoseconds > 0,
            "Cold unknown pipelines did not record demand compilation waits");
      Check(observed.knownConfigs == 8, "Observed configurations were not retained");
      // Each miss was reported once, as its pipeline compiled, with its bytes.
      Check(demand.misses.size() == 8, "Cold demand misses were not reported once each");
      std::array<unsigned, 8> reported{};
      uint64_t reportedCompile = 0;
      for (size_t n = 0; n < demand.misses.size(); ++n) {
        const auto& miss = demand.misses[n];
        Check(miss.shaderType == 0 && miss.configVersion == aurora::gfx::clear::ClearPipelineConfigVersion &&
              miss.configSize == sizeof configs[0] && miss.newConfiguration && miss.compileNanoseconds > 0,
              "A cold demand miss was not reported as a new configuration");
        const auto i = static_cast<size_t>(std::find_if(configs.begin(), configs.end(), [&](const auto& config) {
          return std::memcmp(demand.configs[n].data(), &config, sizeof config) == 0;
        }) - configs.begin());
        Check(i < configs.size(), "A reported configuration differs from every drawn one");
        Check(miss.configKey == aurora::xxh3_hash(configs[i], 0), "A reported key is not the cache key");
        ++reported[i];
        reportedCompile += miss.compileNanoseconds;
      }
      Check(std::all_of(reported.begin(), reported.end(), [](unsigned count) { return count == 1; }),
            "A drawn configuration was not reported exactly once");
      // The encoder waited while the eight compiled together: less than their sum.
      Check(observed.demandWaitNanoseconds < reportedCompile, "Encoding waited longer than the compiles took");
      VerifyHits(8);
      Check(demand.misses.size() == 8, "A prepared request was reported as a demand miss");
      session.Stop();
    }
    std::filesystem::copy_file(directory / "observed" / "pipeline_cache.db",
                               resources / "initial_pipeline_cache.db");

    AutomaticSelectionChecks(argc, argv, backend, directory, resources);
    LearnedScopeChecks(argc, argv, backend, directory, resources);

    // Fresh local/Dawn caches: consume the bundled seed, then prove that the
    // first actual requests on this device are hits for scene/offscreen layouts.
    DemandMissLog afterShutdown; // set in the first session; its shutdown clears it
    for (unsigned msaa : {1u, 4u}) {
      Session session;
      Start(session, argc, argv, backend, directory / ("seeded-" + std::to_string(msaa)), resources, msaa);
      aurora_prepare_pipelines();
      const auto prepared = WaitReady();
      Check(prepared.knownConfigs == 8 && prepared.readyPipelines == (msaa == 1 ? 8u : 16u),
            "Bundled seed did not prepare the selected MSAA/offscreen layouts");
      Check(prepared.demandHits == 0 && prepared.demandMisses == 0,
            "Background preparation was counted as demand work");
      VerifyHits(msaa == 1 ? 8 : 16);
      if (msaa == 4) {
        RequestOnOtherLayout(1);
        Check(afterShutdown.misses.empty(), "Shutdown did not clear the demand miss callback");
      }
      // A known configuration on a render target the preparation did not cover.
      DemandMissLog known;
      aurora_set_pipeline_demand_miss_callback(RecordDemandMiss, &known);
      RequestOnOtherLayout(0);
      Check(known.misses.size() == 1 && !known.misses[0].newConfiguration && known.misses[0].shaderType == 0 &&
            known.misses[0].compileNanoseconds > 0, "A known configuration's demand miss was not reported as known");
      aurora_set_pipeline_demand_miss_callback(nullptr, nullptr);
      RequestOnOtherLayout(2);
      Check(known.misses.size() == 1, "A cleared demand miss callback still received reports");
      if (msaa == 1) aurora_set_pipeline_demand_miss_callback(RecordDemandMiss, &afterShutdown);
      const auto total = aurora_pipeline_warmup_status().totalPipelines;
      aurora_prepare_pipelines(); aurora_prepare_pipelines();
      Check(WaitReady().totalPipelines == total, "Repeated preparation duplicated runtime pipelines");
      session.Stop();
    }

    // Cancel preparation with the normal shutdown path. An individual driver
    // compile cannot be preempted; queued catalogue work must be discarded.
    {
      Session session;
      Start(session, argc, argv, backend, directory / "cancel", resources, 4);
      aurora_prepare_pipelines();
      session.Stop();
    }
    std::printf("Pipeline warmup GPU passed (%u checks); evidence: %s\n", checks, Utf8(directory).c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Pipeline warmup GPU failure: %s\n", error.what());
    return 1;
  }
}
