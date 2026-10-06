#include "NL/nlBind.h"
#include "NL/nlBind_impl.h"
#include "NL/nlFunction.inl"
#include "Game/EventDispatcher.inl"
#include "Game/Audio/AudioScriptRuntime.h"
#include "Game/Audio/AudioSystem.h"
#include "Game/FE/feCharacterPDAComponent.h"
#include "Game/SH/SHMoviePlayer.h"
#include "Game/SH/SHNetworkStart.h"
#include "Game/SH/SHOnlineFriends.h"

#include <cstdio>
#include <cstdlib>
#include <type_traits>

using mscharged::function_abi::WiiCapture;
using mscharged::function_abi::Descriptor;

// Method types are declarations from the real reconstructed classes; no actor,
// audio manager, timer, event or source pool is constructed in this layout gate.
using NoArgMember = decltype(MemFun(&MoviePlayerScene::OnHBMHide));
using PointerMember = decltype(MemFun(&NetworkStartScene::SelectMenuItem));
using IntMember = decltype(MemFun(&SHOnlineFriends::DeleteFriend));
using TwoPointerMember = decltype(MemFun(&FEScrollBar::OnPointerEnter));

using Member0 = BindExp1<void, NoArgMember, MoviePlayerScene*>;
using MemberPlaceholder = BindExp2<void, PointerMember, NetworkStartScene*, Placeholder<0>>;
using MemberInt = BindExp2<void, IntMember, SHOnlineFriends*, int>;
using MemberTwoPlaceholders = BindExp3<void, TwoPointerMember, FEScrollBar*, Placeholder<0>, Placeholder<1>>;
using AudioUpdate = decltype(Bind<bool>(MemFun(&AudioSystem::UpdateSoundSource),
    static_cast<AudioSystem*>(nullptr), 0.1f, Placeholder<0>()));
using MenuUpdate = decltype(Bind<void>(MemFun(&NetworkStartScene::OnMenuItemApply),
    static_cast<NetworkStartScene*>(nullptr), Placeholder<0>(), 1));
using PdaUpdate = decltype(Bind<void>(MemFun(&FECharacterPDAComponent::StepAttributeBar),
    static_cast<FECharacterPDAComponent*>(nullptr), Placeholder<0>(),
    static_cast<FEAttributeBar*>(nullptr), 4));
using AudioEffectUpdate = decltype(Bind<bool>(MemFun(&AudioEffectBinding::UpdateEffect),
    static_cast<AudioEffectBinding*>(nullptr), Placeholder<0>(), Placeholder<1>(),
    static_cast<AudioEffectBinding::UpdateState*>(nullptr)));
using FreePointer = BindExp2<void, void (*)(void*, void*), Placeholder<0>, void*>;
using FreeInstance = BindExp3<bool, bool (*)(unsigned const&, void**, unsigned), Placeholder<0>, Placeholder<1>, unsigned>;
using FreeAudioPointer = BindExp3<bool, bool (*)(unsigned const&, void**, void*), Placeholder<0>, Placeholder<1>, void*>;

struct ShotAtGoalData;
using DataBase = UnidentifiedQueuedEventBase<ShotAtGoalData>;
using DataDispatch = void (DataBase::*)(ShotAtGoalData*, Function<ShotAtGoalData*>, unsigned char);
using DataBinding = decltype(Bind<void>(MemFun(std::declval<DataDispatch>()),
    std::declval<DataBase*>(), std::declval<ShotAtGoalData*>(),
    std::declval<Function<ShotAtGoalData*>>(), std::declval<Placeholder<0>>()));

using VoidBase = UnidentifiedQueuedEventBase<UnidentifiedEventNoData>;
using VoidDispatch = void (VoidBase::*)(VoidBase::Callback, unsigned char);
using VoidBinding = decltype(Bind<void>(MemFun(std::declval<VoidDispatch>()),
    std::declval<VoidBase*>(), std::declval<VoidBase::Callback>(),
    std::declval<Placeholder<0>>()));

template <typename Capture, typename Functor>
void Check(const char* family, unsigned wii_capture, unsigned wii_functor,
           unsigned native_capture, unsigned native_functor, unsigned category) {
    // Expected words below are hand-authored from the original declared fields
    // and independent native layout observations, not Descriptor-derived values.
    using D = Descriptor<Capture, Functor>;
    if (WiiCapture<Capture>::size != wii_capture || D::wii_size != wii_functor
            || sizeof(Capture) != native_capture || sizeof(Functor) != native_functor
            || D::category != category) {
        std::fprintf(stderr, "Original function layout failed: %s actual Wii=%zu/%zu native=%zu/%zu category=%zu\n",
            family, WiiCapture<Capture>::size, D::wii_size, sizeof(Capture), sizeof(Functor), D::category);
        std::exit(1);
    }
    std::printf("%s Wii=%zu/%zu native=%zu/%zu category=%zu stride=%zu\n",
        family, WiiCapture<Capture>::size, D::wii_size,
        sizeof(Capture), sizeof(Functor), D::category, D::stride);
}

int main() {
    static_assert(sizeof(void*) == 8, "This qualifier records the ELF64 native ABI");
    Check<Member0, Function0<void>::FunctorImpl<Member0>>("member0", 16, 20, 24, 32, 32);
    Check<MemberPlaceholder, Function1<void, TLComponentInstance*>::FunctorImpl<MemberPlaceholder>>("member-placeholder", 20, 24, 32, 40, 32);
    Check<MemberInt, Function0<void>::FunctorImpl<MemberInt>>("member-int", 20, 24, 32, 40, 32);
    Check<MemberTwoPlaceholders, Function2<void, int, void*>::FunctorImpl<MemberTwoPlaceholders>>("member-two-placeholders", 20, 24, 32, 40, 32);
    Check<AudioUpdate, Function1<bool, Plat3dSoundSrc&>::FunctorImpl<AudioUpdate>>("audio-dt-placeholder", 24, 28, 32, 40, 32);
    Check<MenuUpdate, Function1<void, TLComponentInstance*>::FunctorImpl<MenuUpdate>>("menu-placeholder-int", 24, 28, 32, 40, 32);
    Check<PdaUpdate, Function1<void, FETimer*>::FunctorImpl<PdaUpdate>>("pda-placeholder-pointer-int", 28, 32, 48, 56, 32);
    Check<AudioEffectUpdate, Function2<bool, unsigned const&, AudioEffectBase**>::FunctorImpl<AudioEffectUpdate>>("audio-placeholders-state", 24, 28, 40, 48, 32);
    Check<FreePointer, Function1<void, void*>::FunctorImpl<FreePointer>>("free-placeholder-pointer", 12, 16, 24, 32, 16);
    Check<FreeInstance, Function2<bool, unsigned const&, void**>::FunctorImpl<FreeInstance>>("free-placeholders-instance", 12, 16, 16, 24, 16);
    Check<FreeAudioPointer, Function2<bool, unsigned const&, void**>::FunctorImpl<FreeAudioPointer>>("free-placeholders-pointer", 12, 16, 24, 32, 16);
    Check<DataBinding, Function1<void, bool>::FunctorImpl<DataBinding>>("queue-data", 32, 36, 56, 64, 64);
    Check<VoidBinding, Function1<void, bool>::FunctorImpl<VoidBinding>>("queue-void", 28, 32, 48, 56, 32);
    Check<AudioEffectSoundStartedVisitor, Function2<bool, unsigned const&, bool*>::FunctorImpl<AudioEffectSoundStartedVisitor>>("audio-visitor", 12, 16, 24, 32, 16);
    std::puts("Original typed layouts only; no source pool/registry/AX/scene readiness.");
}
