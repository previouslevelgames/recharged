#include "runtime/frontend_stack.h"
#include "runtime/startup.h"
#include "runtime/startup_files.h"
#include "NL/MemAlloc.h"
#include "NL/nlMemory.h"
#include "NL/nlFile.h"
#include "NL/nlFileGC.h"
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <source_location>
#include <thread>
using namespace mscharged;
namespace
{
unsigned checks = 0;
void Check(bool value, const char* message)
{ ++checks; if (!value) throw std::runtime_error(message); }
template<class F> void Reject(F f, std::source_location at = std::source_location::current())
{
    ++checks;
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid stack operation accepted at line " + std::to_string(at.line()));
}
void Near(float a, float b)
{ Check(std::abs(a - b) < 1e-6f, "Independent frontend clock differs"); }
FrontendStackRequest Request(unsigned scene = 0)
{ FrontendStackRequest request; request.scene = scene; return request; }
void Pump(FrontendSceneStack& stack, FrontendSceneStack::Token token)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (stack.Entry(token).state == FrontendStackState::Queued || stack.Entry(token).state == FrontendStackState::Loading)
    {
        stack.Service();
        Check(std::chrono::steady_clock::now() < deadline, "Frontend stack load timed out");
        SDL_Delay(1);
    }
}
std::vector<FrontendSceneStack::Token> Tokens(const std::vector<FrontendStackEntry>& entries)
{
    std::vector<FrontendSceneStack::Token> result;
    for (const auto& entry : entries) result.push_back(entry.token);
    return result;
}
FrontendStackCallbacks Record(std::vector<std::string>& events, const std::string& name)
{
    return {
        [&, name](auto& context) {
            events.push_back(name + ":create");
            Check(context.handler.Current() == context.session.Current(), "Presentation was not assigned before SceneCreated");
        },
        [&, name](auto& context) {
            events.push_back(name + ":children");
            context.handler.AddScreen([](const auto&, auto&) { return std::vector<resources::FrontendInstanceChange>{}; });
        },
        [&, name](auto& context, float) {
            Check(context.handler.Screens().size() == 1, "Subhandlers were not initialized before update");
            events.push_back(name + (context.handler.Button(context.handler.Current(), FrontendAction::Accept,
                FrontendButtonQuery::Held) ? ":focused" : ":blocked"));
        }
    };
}
void Catalog()
{
    Check(FrontendSceneStack::SourcePath(0) == "art/fe/sms2_start.fen", "Title association differs");
    Check(FrontendSceneStack::SourcePath(1) == "art/fe/main_menu_v3.fen", "Main association differs");
    Check(FrontendSceneStack::SourcePath(18) == "art/fe/boot_loading.fen", "Boot association differs");
    Check(FrontendSceneStack::SourcePath(22) == "art/fe/movieplayer.fen", "Movie association differs");
    Check(FrontendSceneStack::SourcePath(79) == "art/fe/e3howtoholdcontroller.fen", "Absent retail E3 path was substituted");
    unsigned count = 0;
    for (unsigned i = 0; i < 107; ++i)
    {
        if (i == 88 || i == 106) { Reject([&] { FrontendSceneStack::SourcePath(i); }); continue; }
        const auto name = FrontendSceneStack::SourcePath(i);
        Check(name.starts_with("art/fe/") && name.ends_with(".fen"), "Source catalog entry format changed"); ++count;
    }
    Check(count == 105, "Source catalog population differs");
    Reject([] { FrontendSceneStack::SourcePath(107); });
    Reject([] { FrontendSceneStack::SourcePath(UINT_MAX); });
}
void Lifecycle()
{
    FrontendInput input; std::vector<std::string> events; unsigned drains = 0;
    FrontendSceneStack* active = nullptr;
    FrontendSceneStack stack(input, [&] {
        ++drains;
        if (active) Reject([&] { active->Poll(); });
    });
    active = &stack;
    Check(stack.AllReady() && stack.RenderPlan().empty(), "Empty native stack predicate differs");
    Reject([&] { stack.QueuePop(); }); Reject([&] { stack.Bind(1, {}); });
    auto incomplete = Record(events, "invalid"); incomplete.scene_created = {};
    Reject([&] { stack.QueuePush(Request(), incomplete); });
    const auto a = stack.QueuePush(Request()), b = stack.QueuePush(Request(1));
    Reject([&] { stack.QueuePop(); });
    Check(!stack.AllReady() && Tokens(stack.Entries()) == std::vector{a, b}, "Queued FIFO order differs");
    Pump(stack, a); Pump(stack, b);
    Check(Tokens(stack.Entries()) == std::vector{b, a}, "Original AddStart stack order differs");
    Check(stack.Entry(a).state == FrontendStackState::AwaitingHandler
        && stack.Entry(b).state == FrontendStackState::AwaitingHandler && stack.RenderPlan().empty(),
        "Resource-only entries pretended to have handlers");
    auto initial = stack.Entry(a).prepared;
    Reject([&] { stack.Publish(a, initial); });
    stack.Bind(a, Record(events, "a")); stack.Poll();
    Check(events == std::vector<std::string>{"a:create", "a:children"}, "Creation order differs");
    Check(stack.Entry(a).state == FrontendStackState::AwaitingPublication, "Handler claimed graphics publication");
    auto forged = std::make_shared<FrontendSessionFrame>(*initial);
    Reject([&] { stack.Publish(a, forged); }); Reject([&] { stack.Publish(a, stack.Entry(b).prepared); });
    stack.Publish(a, initial);
    Check(drains == 1 && Tokens(stack.RenderPlan()) == std::vector{a} && !stack.AllReady(), "First native publication differs");
    auto bad = Record(events, "b");
    bad.scene_created = [&](auto&) { events.push_back("b:failure"); throw std::runtime_error("SceneCreated service failure"); };
    stack.Bind(b, std::move(bad)); stack.Poll();
    Check(stack.Entry(b).state == FrontendStackState::Failed && stack.Entry(a).published == initial,
        "Failed creation discarded a visible generation");
    Reject([&] { stack.RethrowFailure(b); }); stack.Poll();
    Check(std::count(events.begin(), events.end(), "b:failure") == 1, "Failed creation was retried as success");
    Check(stack.QueuePop() == b, "Pop did not target first unqueued handler");
    Reject([&] { stack.QueuePop(b); }); stack.Poll(); Reject([&] { stack.Entry(b); });
    Check(stack.AllReady(), "Pop left a pending native request");
    const auto c = stack.QueuePush(Request(1), Record(events, "c")); Pump(stack, c);
    stack.Publish(c, stack.Entry(c).prepared);
    Check(Tokens(stack.RenderPlan()) == std::vector{c, a}, "Native render stack order differs");
    stack.SetTopMost(a); Check(Tokens(stack.RenderPlan()) == std::vector{a, c}, "Top-most render order differs");
    stack.SetVisible(a, false); Check(Tokens(stack.RenderPlan()) == std::vector{c}, "Invisible scene was rendered");
    stack.SetVisible(a, true); stack.SetTopMost(0);
    std::array<FrontendPadSample, 4> samples{}; samples[0].connected = true;
    input.Update(samples, 0); samples[0].buttons = 0x100; input.Update(samples, 0);
    int locked; input.PushFocus(&locked);
    auto old_a = stack.Entry(a).published, old_c = stack.Entry(c).published;
    events.clear(); stack.Update(.125f);
    Check(events == std::vector<std::string>{"c:blocked", "a:blocked"}, "Focus/base update order differs");
    Near(stack.Entry(a).prepared->graph.presentation_time, .125f);
    Check(stack.Entry(a).published == old_a && stack.Entry(c).published == old_c && !stack.AllReady(),
        "Update published unregistered graphics");
    auto prepared = stack.Entry(a).prepared; stack.Update(.125f);
    Check(stack.Entry(a).prepared == prepared, "Unacknowledged generation advanced twice");
    const std::array duplicate{FrontendStackPublication{a, prepared}, FrontendStackPublication{a, prepared}};
    Reject([&] { stack.Publish(duplicate); });
    const std::array wrong{FrontendStackPublication{a, prepared}, FrontendStackPublication{c, old_c}};
    Reject([&] { stack.Publish(wrong); });
    Check(stack.Entry(a).published == old_a && stack.Entry(c).published == old_c, "Invalid batch partially published");
    const std::array batch{FrontendStackPublication{a, prepared}, FrontendStackPublication{c, stack.Entry(c).prepared}};
    const auto before_batch = drains; stack.Publish(batch);
    Check(drains == before_batch + 1 && stack.AllReady(), "Publication batch did not drain exactly once");
    input.PopFocus(&locked); events.clear(); stack.Update(.125f);
    Check(events == std::vector<std::string>{"c:focused", "a:focused"}, "Focused callbacks lost actual FE input");
    Near(stack.Entry(a).prepared->graph.presentation_time, .25f); Near(old_a->graph.presentation_time, 0);
    stack.Publish(a, stack.Entry(a).prepared); stack.Publish(c, stack.Entry(c).prepared);
    for (float dt : {-1.f, INFINITY, NAN, 61.f}) Reject([&] { stack.Update(dt); });
    stack.QueuePop(a); Check(Tokens(stack.RenderPlan()) == std::vector{c}, "Queued-pop scene remained renderable");
    stack.Poll(); Reject([&] { stack.Entry(a); });
    FrontendSceneStack foreign(input, [] {}); Reject([&] { foreign.Publish(c, stack.Entry(c).published); }); foreign.Release();
    bool wrong_thread = false;
    std::thread t([&] { try { stack.Poll(); } catch (const std::logic_error&) { wrong_thread = true; } }); t.join();
    Check(wrong_thread, "Foreign thread mutated stack");
    stack.QueuePop(); stack.Poll(); Check(stack.AllReady() && stack.Entries().empty(), "Final pop leaked a scene");
    stack.Release(); stack.Release(); active = nullptr; Reject([&] { stack.Entries(); });
    Check(initial->images && initial->visuals && old_c->graph.instances.size(), "Released stack invalidated retained frames");
}
void Failures()
{
    FrontendInput input; std::vector<std::string> events;
    unsigned drains = 0; bool fail_drain = false;
    FrontendSceneStack stack(input, [&] { ++drains; if (fail_drain) throw std::runtime_error("GPU drain failure"); });
    auto cb = Record(events, "bad"); cb.initialize_subhandlers = [](auto&) { throw std::runtime_error("subhandler initialization"); };
    auto token = stack.QueuePush(Request(), std::move(cb)); Pump(stack, token);
    Check(stack.Entry(token).state == FrontendStackState::Failed, "Subhandler failure was ignored");
    stack.Cancel(token); Reject([&] { stack.Entry(token); });
    // Complete bindings may call real native methods, but recursive stack work
    // and replacement of their borrowed resource generation are rejected.
    cb = Record(events, "guard");
    cb.scene_created = [&](auto&) { Reject([&] { stack.QueuePush(Request()); }); Reject([&] { stack.Release(); }); };
    token = stack.QueuePush(Request(), std::move(cb)); Pump(stack, token); stack.Publish(token, stack.Entry(token).prepared);
    stack.QueuePop(token); stack.Poll();
    cb = Record(events, "replace"); cb.scene_created = [](auto& c) { c.session.Pop(); };
    token = stack.QueuePush(Request(), std::move(cb)); Pump(stack, token);
    Check(stack.Entry(token).state == FrontendStackState::Failed, "Callback resource replacement was accepted"); stack.Cancel(token);
    cb = Record(events, "update"); cb.after_base_update = [](auto&, float) { throw std::runtime_error("custom update service"); };
    token = stack.QueuePush(Request(), std::move(cb)); Pump(stack, token); stack.Publish(token, stack.Entry(token).prepared);
    auto retained = stack.Entry(token).published; stack.Update(.25f);
    Check(stack.Entry(token).state == FrontendStackState::Failed && stack.Entry(token).published == retained,
        "Custom update failure lost last published frame");
    Near(retained->graph.presentation_time, 0); Reject([&] { stack.RethrowFailure(token); });
    Check(Tokens(stack.RenderPlan()) == std::vector{token}, "Failed update erased retained visible generation");
    stack.QueuePop(token); stack.Poll();
    // Drain is the final publication gate. Failed drain cannot retire the old
    // visible frame or candidate/callback captures; cleanup can be retried.
    auto capture = std::make_shared<int>(7); std::weak_ptr<int> weak = capture;
    cb = Record(events, "retained"); cb.scene_created = [capture](auto&) {}; capture.reset();
    token = stack.QueuePush(Request(), std::move(cb)); Pump(stack, token); stack.Publish(token, stack.Entry(token).prepared);
    retained = stack.Entry(token).published; stack.Update(.125f);
    fail_drain = true; Reject([&] { stack.Publish(token, stack.Entry(token).prepared); });
    Check(stack.Failed() && stack.Entry(token).published == retained && !weak.expired(), "Failed drain released a visible owner");
    Reject([&] { stack.Poll(); }); Reject([&] { stack.Release(); }); Check(!weak.expired(), "Failed cleanup destroyed callbacks");
    fail_drain = false; stack.Release(); Check(weak.expired(), "Successful cleanup retained callback storage");
    Check(retained->images && retained->visuals, "Cleanup freed retained data snapshots");
}
void Cancellation()
{
    FrontendInput input; unsigned drains = 0;
    FrontendSceneStack stack(input, [&] { ++drains; });
    auto token = stack.QueuePush(Request()); stack.Cancel(token);
    Check(stack.Entries().empty() && !nlAsyncReadsPending(nullptr), "Cancelled queued scene started reads");
    for (unsigned i = 0; i < 8; ++i)
    {
        token = stack.QueuePush(Request()); stack.Poll();
        Check(stack.Entry(token).state == FrontendStackState::Loading, "Initial asynchronous load state differs");
        stack.Cancel(token); Check(!nlAsyncReadsPending(nullptr), "Cancelled load retained async work");
    }
    token = stack.QueuePush(Request(13)); Pump(stack, token);
    Check(stack.Entry(token).state == FrontendStackState::Failed, "Malformed original-associated FEN accepted");
    Reject([&] { stack.RethrowFailure(token); }); stack.Cancel(token);
    token = stack.QueuePush(Request(14)); stack.Poll();
    Check(stack.Entry(token).state == FrontendStackState::Failed, "Missing source file claimed ready"); stack.Cancel(token);
    std::vector<FrontendSceneStack::Token> ids;
    for (unsigned i = 0; i < 32; ++i) ids.push_back(stack.QueuePush(Request()));
    Reject([&] { stack.QueuePush(Request()); });
    for (auto id : ids) stack.Cancel(id);
    Check(stack.AllReady() && stack.Entries().empty(), "Cancellation left queue records");
    // Capture destructors cannot reenter cleanup.
    struct Probe
    {
        FrontendSceneStack& stack; bool& rejected;
        Probe(FrontendSceneStack& s, bool& r) : stack(s), rejected(r) {}
        ~Probe() { try { stack.QueuePush(Request()); } catch (const std::logic_error&) { rejected = true; } }
    };
    bool rejected = false; auto probe = std::make_shared<Probe>(stack, rejected);
    FrontendStackCallbacks callbacks{[probe](auto&) {}, [](auto&) {}, [](auto&, float) {}}; probe.reset();
    token = stack.QueuePush(Request(), std::move(callbacks)); stack.Cancel(token);
    Check(rejected, "Captured destructor reentered cancellation");
    token = stack.QueuePush(Request()); stack.Poll(); stack.Release();
    Check(!nlAsyncReadsPending(nullptr), "Released stack retained pending native reads");
}
void DrainConflict()
{
    FrontendInput input; std::vector<std::string> events;
    FrontendSession* borrowed = nullptr; bool mutate = false;
    FrontendSceneStack stack(input, [&] {
        if (mutate)
        {
            FrontendSessionRequest request; request.path = "/art/fe/sms2_start.fen";
            borrowed->Begin(request); // Deliberate violation of the borrowed-callback contract.
        }
    });
    auto callbacks = Record(events, "conflict");
    callbacks.scene_created = [&](auto& context) { borrowed = &context.session; };
    auto token = stack.QueuePush(Request(), std::move(callbacks)); Pump(stack, token);
    stack.Publish(token, stack.Entry(token).prepared); const auto visible = stack.Entry(token).published;
    stack.Update(.25f); const auto pending = stack.Entry(token).prepared;
    mutate = true; Reject([&] { stack.Publish(token, pending); });
    Check(stack.Failed() && stack.Entry(token).published == visible && borrowed->Current() == pending
        && borrowed->State() == FrontendSessionState::Loading, "Post-drain load with identical Current escaped validation");
    mutate = false; stack.Release();
    Check(!nlAsyncReadsPending(nullptr) && visible->images, "Conflicting drain failed retained/read cleanup");
}
void Owned()
{
    FrontendInput input; FrontendSceneStack stack(input, [] {});
    auto title = Request(0); title.initial_slide = "regular";
    auto main = Request(1); main.initial_slide = "MAIN";
    const auto a = stack.QueuePush(title), b = stack.QueuePush(main); Pump(stack, a); Pump(stack, b);
    stack.RethrowFailure(a); stack.RethrowFailure(b);
    const auto x = stack.Entry(a), y = stack.Entry(b);
    Check(x.state == FrontendStackState::AwaitingHandler && y.state == FrontendStackState::AwaitingHandler,
        "Owned scenes pretended their concrete handlers existed");
    Check(x.prepared->images->textures.size() == 15 && y.prepared->images->textures.size() == 30,
        "Owned source-associated image catalog differs");
    Check(x.prepared->graph.slides.size() == 28 && y.prepared->graph.slides.size() == 134,
        "Owned title/main graph identities differ");
    Check(stack.RenderPlan().empty() && !stack.AllReady(), "Resource-only owned load claimed visible readiness");
    Reject([&] { stack.Publish(a, x.prepared); }); stack.QueuePop(b); stack.Poll(); stack.Cancel(a); stack.Release();
    Check(x.prepared->images->textures.size() == 15 && y.prepared->images->textures.size() == 30,
        "Owned pop invalidated retained texture data");
    std::cout << "Owned stack: title 28 slides/15 textures, main 134 slides/30 textures; both AwaitingHandler; no graphics or concrete menu claim\n";
}
struct Host
{
    bool live = false, disc = false;
    ~Host() { if (live) ResetStartupFiles(); if (disc) aurora_dvd_close(); if (live) { ResetStartupMemory(); aurora_shutdown(); } }
};
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--catalog")
        {
            for (unsigned i = 0; i < 107; ++i)
            {
                std::cout << i << ':';
                if (i == 88 || i == 106) std::cout << '-';
                else std::cout << FrontendSceneStack::SourcePath(i);
                std::cout << '\n';
            }
            return 0;
        }
        Check(argc == 4, "Supply disc, output directory and generated/owned mode");
        const bool owned = std::string_view(argv[3]) == "owned";
        Check(owned || std::string_view(argv[3]) == "generated", "Unknown stack test mode");
        const auto folder = (std::filesystem::path(argv[2]) / "frontend-stack-data").string(); std::filesystem::create_directories(folder);
        AuroraConfig config{}; config.appName = "Charged frontend stack"; config.userPath = config.cachePath = folder.c_str();
        config.resourcesPath = SDL_GetBasePath(); config.desiredBackend = BACKEND_NULL;
        config.windowWidth = 320; config.windowHeight = 240; config.windowPosX = config.windowPosY = -1; config.logLevel = LOG_WARNING;
        config.mem1Size = MEM1_DEFAULT_SIZE; config.mem2Size = 64 * 1024 * 1024;
        Host host; const auto state = aurora_initialize(argc, argv, &config); host.live = true;
        Check(state.window, "Aurora initialization failed"); InitializeStartupOS(); nlInitMemory();
        Check(aurora_dvd_open(argv[1]), "Cannot open frontend stack disc"); host.disc = true; nlInitFileSystem();
        Catalog();
        for (unsigned repeat = 0; repeat < 3; ++repeat)
        {
            const auto a = StandardAllocator.TotalFreeMemory(), b = VirtualAllocator.TotalFreeMemory();
            if (owned) Owned(); else { Lifecycle(); Failures(); Cancellation(); DrainConflict(); }
            Check(!nlAsyncReadsPending(nullptr) && StandardAllocator.TotalFreeMemory() == a && VirtualAllocator.TotalFreeMemory() == b,
                "Frontend stack failed to restore native arenas/files");
        }
        std::cout << checks << " frontend stack checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << " (check " << checks << ")\n"; return 1; }
}
