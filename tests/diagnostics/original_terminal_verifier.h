#pragma once
#include "original_game_audio_hardware.h"
#include "platform/ai.h"
#include "platform/dsp_control.h"
#include "platform/dsp_mailbox.h"
#include "platform/stm_device.h"
#include <aurora/aurora.h>
#include <aurora/video.h>
#include "window.hpp"
#include <SDL3/SDL_video.h>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace mscharged::diagnostic {
// Borrow existing host owners. Construction follows the genuine source entry
// and successful source audio/configuration observation. No new device/lease.
class OriginalTerminalVerifier {
public:
    OriginalTerminalVerifier(OriginalGameAudioHardware& audio,
        platform::NativeAXModuleMemory& memory, SDL_Window* window)
        : audio_(audio), memory_(memory), window_(window),
          module_(memory.Status()), endpoint_(memory.Endpoint()),
          reset_count_(audio.Status().protocol.resets),
          control_generation_(platform::GetNativeDSPControlStatus().generation),
          mail_generation_(platform::GetNativeDSPMailboxStatus().generation),
          window_incarnation_(aurora::window::get_window_incarnation()),
          window_id_(window ? SDL_GetWindowID(window) : 0) {
        Require(module_.loaded && module_.reserved && !module_.retired &&
                (module_.spans == CHARGED_AX_BASE_STORAGE_COUNT || module_.spans == CHARGED_AX_HBM_STORAGE_COUNT) && module_.after.memory_initialized,
                "Terminal policy needs the genuine live source module");
        Require(audio.Status().native_initialized && platform::GetNativeAIStatus().initialized,
                "Terminal policy must follow genuine original audio initialization");
        Require(window_ && window_ == aurora_get_window() && window_id_ && window_incarnation_,
                "Terminal policy needs its actual retained game window");
        for (unsigned i = 0; i != module_.spans; ++i) words_[i] = memory_.PhysicalAddress(i);
        AuroraVIOutputState output{};
        Require(aurora_get_video_output_state(&output), "Terminal policy has no actual VI output owner");
        before_policy_present_ = output.presentations;
    }
    platform::NativeSTMPowerRemoval Policy() { return {this, Verify}; }

private:
    static void Require(bool condition, const char* error) {
        if (!condition) throw std::logic_error(error);
    }
    static void Verify(void* context, const platform::NativeSTMPowerRequest& request) {
        static_cast<const OriginalTerminalVerifier*>(context)->VerifyRemoval(request);
    }
    void VerifyRemoval(const platform::NativeSTMPowerRequest& request) const {
        using namespace platform;
        const auto ai = GetNativeAIStatus();
        Require(ai.initialized && !ai.running && !ai.interrupt_pending && !ai.callback_active &&
                !ai.retained_blocks && ai.queued_input_bytes == 0,
                "Original OS shutdown has not stopped and drained actual AI");
        const auto audio = audio_.Status();
        const auto control = GetNativeDSPControlStatus();
        const auto mail = GetNativeDSPMailboxStatus();
        // Actual CSR masks from dsp_control.cpp: RESET/PI/DSP/ARAM causes and
        // boot/ARAM transfers must be absent; HALT remains set. INIT/masks are
        // not source readiness and need not be cleared by original OS stop.
        constexpr std::uint16_t busy_or_cause = 0x0001 | 0x0002 | 0x0020 | 0x0080 | 0x0200 | 0x0400;
        Require(control.connected && control.generation == control_generation_ &&
                (control.csr & 0x0004) && !(control.csr & busy_or_cause) &&
                mail.connected && mail.generation == mail_generation_ &&
                !mail.cpu_mail_full && !mail.dsp_mail_full &&
                !audio.native_initialized && audio.protocol.phase == NativeAXBootstrapPhase::Cold &&
                audio.protocol.hardware_halted && audio.protocol.resets == reset_count_ + 1 &&
                audio.frames.phase == NativeAXFramePhase::Unavailable &&
                !audio.processed_frames && !audio.frames.processed_frames &&
                !audio.frames.completed_frames && !audio.last_active_voices,
                "Original OS stop has not genuinely reset and drained retained DSP hardware");

        const auto module = memory_.Status();
        Require(module.loaded && module.reserved && !module.retired && module.spans == module_.spans &&
                module.image_base == module_.image_base && module.reserved_bytes == module_.reserved_bytes &&
                memory_.Endpoint().generation == endpoint_.generation &&
                module.after.memory_initialized &&
                module.after.standard_address == module_.after.standard_address &&
                module.after.virtual_address == module_.after.virtual_address &&
                module.after.standard_bytes == module_.after.standard_bytes &&
                module.after.virtual_bytes == module_.after.virtual_bytes,
                "Terminal path retired or changed actual source image/arena ownership");
        // These exact extents come from NativeAXModuleMemory's existing source
        // storage contract, not inferred allocator capacities or extra pins.
        constexpr std::array<std::uint32_t, CHARGED_AX_HBM_STORAGE_COUNT> bytes{
            256, 30720, 6144, 4608, 4608, 3456, 4032, 120, 1152, 768, 1440, 64, 8192, 256};
        for (unsigned i = 0; i != module_.spans; ++i) {
            Require(memory_.PhysicalAddress(i) == words_[i], "Source static mapping was replaced");
            DSPBackendValidateMemory(endpoint_, words_[i], bytes[i], false);
        }
        Require(ai.source_address && ai.dma_bytes, "Stopped AI lost actual source DMA storage");
        // Original THPAudioMixCallback selects its native SoundBuffer BSS,
        // while original AX selects its own native static PCM buffer. AI uses
        // that native pointer directly; THP storage is not a DSP bus pin.
        Require(memory_.OwnsReadableImageExtent(
                    reinterpret_cast<const void*>(ai.source_address), ai.dma_bytes),
                "Stopped AI DMA extent has no retained source-image storage owner");

        Require(window_ == aurora_get_window() && SDL_GetWindowID(window_) == window_id_ &&
                aurora::window::get_window_incarnation() == window_incarnation_,
                "Terminal path replaced or retired its real window");
        bool enabled = true;
        AuroraVIOutputState output{};
        Require(aurora_get_video_display_enabled(&enabled) && !enabled &&
                aurora_get_video_output_state(&output) && !output.pending,
                "Original STM DCR disable/output drain did not return");
        const bool presentable = aurora::window::is_presentable();
        if (presentable) {
            AuroraVIPresentedGeometry geometry{};
            Require(aurora_get_presented_video_geometry(&geometry) &&
                    geometry.window_id == window_id_ && geometry.window_incarnation == window_incarnation_ &&
                    geometry.completion.black &&
                    geometry.completion.presentation_sequence > before_policy_present_ &&
                    geometry.completion.presentation_sequence == output.presentations,
                    "Presentable terminal window has no new actual black Present");
        }
        // No service, poll, wait, join, free or source write. 0058 itself already
        // drained the worker before the synchronous original STM ioctl. Its
        // unavailable-surface branch keeps the last completion historical.
        std::printf("Original source shutdown: native terminal owners quiescent; request=%llu.\n",
            static_cast<unsigned long long>(request.generation));
        std::puts(presentable ? "Original source shutdown: VI black Present verified."
                             : "Original source shutdown: VI disabled, surface unavailable; last Present historical.");
        std::fflush(stdout);
    }

    OriginalGameAudioHardware& audio_;
    platform::NativeAXModuleMemory& memory_;
    SDL_Window* window_;
    platform::NativeAXModuleMemoryStatus module_;
    platform::NativeDSPMemoryEndpoint endpoint_;
    std::uint64_t reset_count_, control_generation_, mail_generation_, window_incarnation_;
    std::uint32_t window_id_;
    std::array<std::uint32_t, CHARGED_AX_HBM_STORAGE_COUNT> words_{};
    std::uint64_t before_policy_present_{};
};
} // namespace mscharged::diagnostic
