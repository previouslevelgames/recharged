#include "runtime/views.h"
#include "runtime/materials.h"
#include "runtime/graphics_memory.h"
#include "runtime/graphics_state.h"
#include "runtime/startup.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glMaterialProgram.h"
#include "NL/gl/glMemoryInit.h"
#include "NL/gl/glRenderList.h"
#include "NL/gl/glTextureManager.h"
#include "NL/glx/glxTarget.h"
#include "NL/glx/glxTexture.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace mscharged;
namespace
{
unsigned checks = 0, invalidations = 0;
std::vector<const glModelPacket*> packets_seen;
std::vector<unsigned long> flags_seen;
void Check(bool value, const char* message) { ++checks; if (!value) throw std::runtime_error(message); }
template<class Error, class Action> void Reject(Action action)
{
    try { action(); } catch (const Error&) { ++checks; return; }
    throw std::runtime_error("Invalid view/target operation was accepted");
}
void Invalidate() { ++invalidations; }
void Observe(GLView*, unsigned long flags, const glModelPacket* packet)
{ flags_seen.push_back(flags); if (packet) packets_seen.push_back(packet); }
void Throw(GLView*, unsigned long, const glModelPacket*) { throw std::runtime_error("callback failure"); }
void CheckOrder(GLView& view, const std::vector<const glModelPacket*>& expected)
{
    packets_seen.clear(); flags_seen.clear(); view.Iterate(Observe);
    Check(packets_seen == expected, "Original packet sorting order changed");
}
void Run()
{
    ViewMatrices matrices;
    MaterialPrograms materials;
    OriginalViews views(640, 480);
    Reject<std::invalid_argument>([] { OriginalViews duplicate(640,480); });
    const auto mem1 = StandardAllocator.TotalFreeMemory(), mem2 = VirtualAllocator.TotalFreeMemory();
    auto root_pair = glGetBackBufferTarget();
    glValidateTarget(root_pair);
    Reject<std::invalid_argument>([&] { glDestroyTarget(&root_pair); });
    Reject<std::invalid_argument>([&] { glGetTargetTexture(root_pair); });
    Reject<std::logic_error>([&] { root_pair.target->Activate(0); });
    Reject<std::invalid_argument>([&] { auto* bad = new GLView(nullptr, {}, GLViewSort_None); delete bad; });
    Reject<std::invalid_argument>([&] { auto* bad = new GLView(&matrices, {}, GLViewSort_Count); delete bad; });
    {
        GLRenderPair incomplete; incomplete.hash = 1;
        GLView view(&matrices, incomplete, GLViewSort_None);
        Reject<std::invalid_argument>([&] { view.GetRenderPair(); });
    }
    std::array<glModelPacket, 4> packet{};
    for (unsigned i = 0; i < packet.size(); ++i)
    {
        packet[i].materialProgram = glGetMaterialProgram(i & 1 ? 0x21db4385 : 0xd3e572da);
        packet[i].numUniqueVertices = 3;
        nlMatrix4 transform; transform.SetIdentity(); transform.m43 = -.125f * (i + 1);
        packet[i].matrix = glAllocSetMatrix(transform);
    }
    Check(reinterpret_cast<std::uintptr_t>(packet.data()) > UINT32_MAX, "Native packet pointer fixture needs high addresses");
    for (auto mode : {GLViewSort_Texture, GLViewSort_TransformedDepth, GLViewSort_TransformedMatrixDepth,
                     GLViewSort_None, GLViewSort_Reverse})
    {
        GLView view(&matrices, {}, mode);
        for (unsigned i : {2u, 0u, 3u, 1u}) view.AttachPacket(&packet[i], 0);
        std::vector<const glModelPacket*> expected;
        if (mode == GLViewSort_Texture) expected = {&packet[1],&packet[3],&packet[0],&packet[2]};
        else if (mode == GLViewSort_None) expected = {&packet[2],&packet[0],&packet[3],&packet[1]};
        else if (mode == GLViewSort_Reverse) expected = {&packet[1],&packet[3],&packet[0],&packet[2]};
        else expected = {&packet[0],&packet[1],&packet[2],&packet[3]};
        CheckOrder(view, expected);
        Check(view.m_TriangleCount == 4, "Original triangle count changed");
        Reject<std::runtime_error>([&] { view.Iterate(Throw); });
        CheckOrder(view, expected); // Callback failure releases the iteration guard.
        view.ResetPackets();
        view.AttachPacket(&packet[0], 0);
        view.AttachPacket(&packet[1], UINT32_MAX); // Original signed layer -1 precedes 0.
        CheckOrder(view, {&packet[1],&packet[0]});
        view.ResetPackets();
        CheckOrder(view, {});
    }
    {
        GLView depth(&matrices, {}, GLViewSort_TransformedDepth);
        nlMatrix4 matrix; matrix.SetIdentity();
        matrix.m43 = -2; packet[0].matrix = glAllocSetMatrix(matrix);
        matrix.m43 = -1.25f; packet[1].matrix = glAllocSetMatrix(matrix);
        matrix.m43 = -.125f; packet[2].matrix = glAllocSetMatrix(matrix);
        for (unsigned i : {1u,0u,2u}) depth.AttachPacket(&packet[i],0);
        CheckOrder(depth,{&packet[2],&packet[0],&packet[1]}); // Saturated Wii priorities tie by complete address.
        matrix.m43 = std::numeric_limits<float>::quiet_NaN(); packet[3].matrix = glAllocSetMatrix(matrix);
        Reject<std::invalid_argument>([&] { depth.AttachPacket(&packet[3],0); });
    }
    {
        GLView view(&matrices, {}, GLViewSort_None);
        packet[1] = packet[0]; packet[2] = packet[0];
        packet[2].matrix = glAllocSetMatrix(matrices.view);
        view.AttachPacket(&packet[0], 0); view.AttachPacket(&packet[1], 0); view.AttachPacket(&packet[2], 0);
        CheckOrder(view, {&packet[0],&packet[1],&packet[2]});
        Check(flags_seen == std::vector<unsigned long>({1,0x8E,0x80,0x84}), "Original packet dirty flags changed");
        glplatFrameAllocNextFrame();
        Reject<std::logic_error>([&] { view.Iterate(Observe); });
        view.ResetPackets(); CheckOrder(view, {});
        view.AttachPacket(&packet[0], 0); // Rebuilding a list never follows discarded frame pointers.
        glplatFrameAllocNextFrame();
        view.AttachPacket(&packet[1], 0); CheckOrder(view, {&packet[1]});
        Reject<std::invalid_argument>([&] { view.AttachPacket(nullptr, 0); });
        Reject<std::invalid_argument>([&] { view.AttachModel(nullptr, 0); });
        if (sizeof(unsigned long) > 4) Reject<std::invalid_argument>([&] { view.AttachPacket(&packet[0], UINT64_C(0x100000000)); });
    }
    {
        auto a = std::make_unique<GLView>(&matrices, GLRenderPair{}, GLViewSort_None);
        auto b = std::make_unique<GLView>(&matrices, GLRenderPair{}, GLViewSort_None);
        auto c = std::make_unique<GLView>(&matrices, GLRenderPair{}, GLViewSort_None);
        gRootView.AddChild(a.get()); gRootView.AddChild(b.get()); a->AddChild(c.get());
        std::vector<GLView*> order;
        for (GLViewIterator it(&gRootView); !it.IsDone(); it.Next()) order.push_back(it.Current());
        Check(order == std::vector<GLView*>({c.get(),a.get(),b.get(),&gRootView}), "Original child-first view traversal changed");
        Reject<std::invalid_argument>([&] { gRootView.AddChild(a.get()); });
        Reject<std::invalid_argument>([&] { c->AddChild(&gRootView); });
        Reject<std::invalid_argument>([&] { b->RemoveChild(c.get()); });
        c->SetParent(nullptr); Check(!c->m_Parent && !a->HasChildren(), "View detach retained parent or list node");
    }
    Check(!gRootView.HasChildren(), "View destruction did not unlink parents");
    {
        GLView* parent = &gRootView;
        for (unsigned i = 0; i < 7; ++i) { auto* child = new (8, false) GLView(&matrices, {}, GLViewSort_None); parent->AddChild(child); parent = child; }
        auto extra = std::make_unique<GLView>(&matrices, GLRenderPair{}, GLViewSort_None);
        Reject<std::length_error>([&] { parent->AddChild(extra.get()); });
        glViewCompact();
        delete gRootView.m_Children.m_Head->entry;
    }
    // Independent pools let targets be destroyed in any order while the global pool remains selected.
    auto* selected = glGetCurrentResourcePool();
    const auto target_mem1 = StandardAllocator.TotalFreeMemory(), target_mem2 = VirtualAllocator.TotalFreeMemory();
    for (auto format : {GLTargetFormat_RGB5A3, GLTargetFormat_RGB565, GLTargetFormat_RGBA8, GLTargetFormat_A8, GLTargetFormat_IA8})
    {
        GLTargetInfo info{}; info.width = 64; info.height = 32; info.format = format; info.clearFlags = 7;
        auto a = glCreateTarget("test/a", &info), b = glCreateTarget("test/b", &info);
        auto alias = a;
        glNativeSetViewDispatch(true);
        Reject<std::logic_error>([&] { glDestroyTarget(&a); });
        glNativeSetViewDispatch(false);
        Check(glx_GetTex(glGetTargetTexture(a)) == a.target->mTexture, "Target was not registered in the native inventory");
        Check(glCreateTarget("test/a", &info).target == a.target, "Target lookup duplicated an existing name");
        info.width = 63; Reject<std::invalid_argument>([&] { glCreateTarget("test/a", &info); }); info.width = 64;
        glDestroyTarget(&a); glDestroyTarget(&a);
        Reject<std::invalid_argument>([&] { glValidateTarget(alias); });
        a = glCreateTarget("test/a", &info);
        Check(a.nativeGeneration != alias.nativeGeneration, "Reused target handle retained its old generation");
        Reject<std::invalid_argument>([&] { glDestroyTarget(&alias); });
        glDestroyTarget(&b); glDestroyTarget(&a);
        Check(glGetCurrentResourcePool() == selected, "Target release changed the selected game pool");
        Check(StandardAllocator.TotalFreeMemory() == target_mem1 && VirtualAllocator.TotalFreeMemory() == target_mem2,
              "Target release leaked arena memory");
    }
    {
        std::vector<GLRenderPair> allocated;
        GLTargetInfo info; info.width = info.height = 8; info.format = GLTargetFormat_RGBA8;
        for (unsigned i = 0; i < 32; ++i) allocated.push_back(glCreateTarget(("full/" + std::to_string(i)).c_str(), &info));
        Reject<std::length_error>([&] { glCreateTarget("full/failure", &info); });
        for (auto& target : allocated) glDestroyTarget(&target);
        Check(glGetTextureManager()->mFreeIndices->mCount == 32, "Failed target construction leaked a texture index");
    }
    GLTargetInfo bad{}; bad.width=0; bad.height=32; bad.format=GLTargetFormat_RGBA8;
    Reject<std::invalid_argument>([&] { glCreateTarget("invalid", &bad); });
    Check(StandardAllocator.TotalFreeMemory() == mem1 && VirtualAllocator.TotalFreeMemory() == mem2,
          "View sorting or target teardown leaked persistent memory");
    views.Release(); views.Release(); materials.Release();
}
}
int main()
{
    try
    {
        Check(!gRootView.m_Sorters, "Root view allocated before game memory initialization");
        static_assert(sizeof(GLTargetInfo) == 40);
        // Keys differing only above bit 31 remain separate; high pointer bits cannot overwrite priority.
        GLPacketSortKey a{1,UINT64_C(0x100000000)}, b{1,UINT64_C(0x200000000)}, c{2,0};
        Check(a < b && b < c && !(a == b), "Native sorter truncated a pointer or corrupted priority");
        std::vector<std::uint64_t> mem1(1024*1024), mem2(1024*1024);
        ResetStartupMemory();
        StandardAllocator.Initialize(mem1.data(), mem1.size()*8); VirtualAllocator.Initialize(mem2.data(),mem2.size()*8);
        gMemoryInitialized=1;
        const auto free1=StandardAllocator.TotalFreeMemory(), free2=VirtualAllocator.TotalFreeMemory();
        for (unsigned repeat=0; repeat<2; ++repeat)
        {
            const GLMemoryRequirement requirements[]={{GLM_Header,65536},{GLM_VertexData,4096}};
            const GLMemoryConfig config{65536,4096,requirements,2,32};
            glInitMemory(&config); InitializeOriginalGraphicsState(); SetGraphicsCacheInvalidator(Invalidate);
            Run(); glShutdownMemory();
            Check(StandardAllocator.TotalFreeMemory()==free1 && VirtualAllocator.TotalFreeMemory()==free2,"View/target restart leaked game arenas");
        }
        {
            const GLMemoryRequirement requirements[]={{GLM_Header,65536}};
            const GLMemoryConfig config{4096,4096,requirements,1,8};
            glInitMemory(&config); InitializeOriginalGraphicsState();
            ViewMatrices matrices;
            OriginalViews automatic(640,480);
            gRootView.AddChild(new (8, false) GLView(&matrices, {}, GLViewSort_None));
            GLTargetInfo info; info.width=info.height=8; info.format=GLTargetFormat_RGBA8;
            auto target=glCreateTarget("shutdown/owned", &info);
            gRootView.m_Visible = false;
            glShutdownMemory(); // Registered cleanup releases the graph and its targets first.
            automatic.Release();
            Check(!gRootView.HasChildren() && !gRootView.m_Sorters && gRootView.m_Visible && !glplatGetBackBufferTarget(), "Graphics shutdown retained view/target state");
            Check(StandardAllocator.TotalFreeMemory()==free1 && VirtualAllocator.TotalFreeMemory()==free2,"Automatic view/target shutdown leaked game arenas");
            Reject<std::invalid_argument>([&] { glValidateTarget(target); });
        }
        ResetStartupMemory();
        std::cout << checks << " original view, sort, callback and target lifecycle checks passed\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
