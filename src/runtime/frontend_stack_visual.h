#pragma once
#include "runtime/frontend_handler.h"

namespace mscharged
{
class FrontendSceneStack;
// Selected concrete visual ownership only. The stack retains this owner and
// supplies its one base handler; complete original SceneCreated/state6 is not
// established by implementing this interface.
class FrontendStackVisual
{
    friend class FrontendSceneStack;
    virtual std::shared_ptr<FrontendSession> StackSession() const = 0;
    virtual std::shared_ptr<FrontendHandler> StackHandler() const = 0;
    virtual unsigned StackScene() const = 0;
    virtual void AttachStack() = 0;
    virtual void UpdateStack(FrontendHandler::UpdateProof&&,
        const FrontendSession::Handle& presented, const std::function<void()>& input) = 0;
    virtual void ReleaseStack() = 0;
public:
    virtual ~FrontendStackVisual() = default;
    virtual FrontendSession::Handle Current() const = 0;
};
}
