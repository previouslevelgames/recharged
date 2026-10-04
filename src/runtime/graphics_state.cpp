#include "runtime/graphics_state.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glState.h"
#include "NL/gl/glStartupStages.h"
#include <stdexcept>

namespace mscharged
{
void InitializeOriginalGraphicsState()
{
    InitializeOriginalGraphicsStartupState();
    glSetDefaultState(true);
}
void InitializeOriginalGraphicsStartupState()
{
    if (!glGetCurrentResourcePool())
        throw std::logic_error("Original graphics state requires a live resource pool");
    if (glGetIdentityMatrix() != GL_INVALID_MATRIX)
        throw std::logic_error("Original graphics matrices are already initialized");
    try
    {
        gl_StartupState();
    }
    catch (...)
    {
        gl_StateShutdown();
        gl_MatrixShutdown();
        throw;
    }
}
}
