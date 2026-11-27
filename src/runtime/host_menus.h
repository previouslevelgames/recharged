#pragma once

#include "platform/menu_snapshot.h"
#include "runtime/menu_input.h"
#include "ui/rui_kit.h"
#include "ui/rui_pages.h"

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mscharged::runtime {
// The trophy TOURNAMENT shows (eCupPersona: the Next Level Games cup's), and
// whose ceremony ends a tournament's last game.
inline constexpr int kTournamentTrophy = 5;


// Recharged's menus in the look of the game's (the UI kit, src/ui): one TV
// set over the game, with a stack of pages in it.
// - The Recharged menu (F1 or HOME anywhere): ONLINE, OPTIONS, QUIT RECHARGED;
//   over a match in play it pauses the game. In an online match the game's
//   pause opens the match menu instead (nothing pauses).
// - A question over everything (a challenge, a set's end): the prompt page.
// With the development switch "Override retail menus" they also stand in for
// the game's own:
// - The match's pause menu: the game's own scene, undrawn, drawn here with the
//   game's header, its buttons where its pointers find them and its players'
//   hands (the game keeps its input: pointing, hovering with its sound,
//   pressing, resuming). Its Audio and Visual Options open SETTINGS on SOUND
//   or DISPLAY in the same TV (the module takes those presses).
// - The front end's Options (the main menu's OPTIONS): the game's scene runs
//   undrawn (its camera move, its way back), and the OPTIONS hub stands in
//   for it: GAMEPLAY, SETTINGS, and CREDITS and BACK handed back to it.
// - The main menu's ONLINE (the globe): the ONLINE hub stands in for the
//   game's way, which its WII ONLINE takes.
// - The ONLINE hub's TOURNAMENT: from the game's online menu (through WII
//   ONLINE from the globe) into the trophy room, and the TOURNAMENT page over
//   it once the camera is on the trophy; its BACK (or B) goes back out.
// While a page shows, the menus have the input (MenuInput::OpenPages): the
// D-pad, the arrows and the mouse light an item, A presses, left and right
// change a value, B goes back a page (to the pause menu, the cursor on the
// button that opened it), the pause button resumes the match from any page.
// The mouse and each player's Wii Remote that points at the picture draw the
// player's own hand (MenuInput::Hands); the pages follow the one that moved
// or pressed last. Use it on the presentation owner thread, between game
// frames.
class HostMenus {
public:
    using Clock = std::chrono::steady_clock;

    struct Game {
        ChargedMenuTextureFunction texture = nullptr;
        ChargedMenuCommandFunction command = nullptr;
        ChargedMenuTextureObjectsFunction texture_objects = nullptr;
    };
    // What of the pages' model the pages shown need this frame (building it
    // all costs; the rest stays as made).
    struct ModelNeeds {
        bool online = false;      // Online and Ranked: an online page is in the stack
        bool leaderboard = false; // the leaderboard shows (its data is fetched)
        bool stats = false;       // the player's records show (they are fetched)
        bool settings = false;    // the settings: the Settings page shows
    };
    // The host's side: each frame's model of the pages, and what to do with
    // a change of its settings and options.
    struct Host {
        // The pages' model (the overlay's: settings, options, Online, Ranked).
        std::function<void(ui::PageModel&, const ModelNeeds&)> model;
        // A picture's texture (an avatar, "mii:own", the flags), 0 for none.
        std::function<ImTextureID(const std::string&)> image;
        // What the player did on the online pages, by the model they were
        // drawn from; true when the menus close (the game is off to RANKED).
        std::function<bool(const overlay::OnlinePanelActions&, const overlay::RankedActions&,
                           const overlay::GameOverlayModel&)>
            online;
        // live: a slider being moved (applied now, saved once it stops).
        std::function<void(const overlay::GameSettingChange&)> setting;
        std::function<void(const overlay::GameOptionChange&)> option;
        // The game ends, as when its window is closed (QUIT RECHARGED).
        std::function<void()> quit;
        // The TOURNAMENT page's model, each frame; `open` while the room's
        // pages are in the stack (the server is asked often only then); `shown`: the
        // key of the TOURNAMENT page on top (a tournament's id, empty for the
        // player's own). None: sample data.
        std::function<void(ui::TournamentModel&, bool open, const std::string& shown)> tournament;
        // The TOURNAMENTS page's model, each frame. None: the room opens
        // TOURNAMENT straight away.
        std::function<void(ui::TournamentListModel&)> tournaments;
        // TOURNAMENT's READY: the player's next set starts (true: the game
        // goes on from the trophy room into its RANKED search, and the pages
        // go). None, or false: nothing starts.
        std::function<bool()> tournament_ready;
        // TOURNAMENT's JOIN (true) or LEAVE (false), for the profile
        // server's tournaments. None: not shown.
        std::function<void(bool join)> tournament_entry;
        // The player forfeits the online match (the match menu's FORFEIT):
        // the host tells the server, then calls LeaveMatch. None: the match
        // is left at once.
        std::function<void()> forfeit;
    };

    HostMenus(Game game, MenuInput& input, Host host);
    ~HostMenus();
    HostMenus(const HostMenus&) = delete;
    HostMenus& operator=(const HostMenus&) = delete;

    // A question over everything (a challenge, a set's end) while `prompt`
    // has one, as the prompt page; its answer once given (1 its first
    // button, 2 its second: B and BACK too), once.
    void SetPrompt(std::optional<overlay::OverlayPrompt> prompt);
    std::optional<int> TakePromptAnswer();
    // The stadium the player stepped to on the prompt's row (OverlayPrompt::
    // stadium; the prompt shows it at once), once.
    std::optional<std::string> TakePromptStadium();
    // Every page goes (a set going on into the game).
    void CloseAll();
    // The set READY started is done, or its search was left: still in the
    // trophy room, its page comes again; out of it (the walk left the
    // game's online menu, `from_match` or not), the room is asked again
    // once the game's online menu is at rest (the game comes back to it).
    void TournamentSetDone(bool from_match);
    // A tournament set READY asked for goes (both players are ready): it goes
    // on from the trophy room, which stays; only the pages go.
    void TournamentSetStarts();
    // The trophy room shows (its camera there), as the game's last snapshot says.
    bool InTrophyRoom() const;
    // The game leaves the online match the player forfeited (Host::forfeit).
    void LeaveMatch();
    // Each host frame, after the game's frame and MenuInput::Service: the
    // game's menu as it is now (null with the switch off).
    void Service(const ChargedMenuSnapshot* menu, Clock::time_point now);
    bool IsShown() const { return shown_; }
    // Inside the host overlay's ImGui frame while shown.
    void Draw(float width, float height, const ui::Fonts& fonts);

private:
    // How a page at the stack's bottom came (the pages over it came from it).
    enum class Origin {
        None,      // opened from another page
        Recharged, // the Recharged menu (F1, HOME) or the online match's menu
        Pause,     // the pause menu's Audio or Visual Options
        Anchored,  // stands in for the front end's Options scene
        Wheel,     // stands in for the main menu's globe
        TrophyRoom, // over the trophy room (TOURNAMENT)
    };
    // TOURNAMENT's way into the trophy room.
    enum class TournamentWay {
        None,
        Login,   // the game's WII ONLINE asked; the main menu not left yet
        Online,  // through the game's login, until its online menu is at rest
        Room,    // the trophy room asked; until the camera is on the trophy
    };
    struct Page {
        ui::PageId id;
        int category = 0; // the one shown of its categories
        std::string key;  // which (a player's)
        Origin origin = Origin::None;
        ui::ListState list;
    };
    // One Draw's timing (the game's MENU IN / MENU OUT) and where the game's
    // picture is.
    struct Frame {
        float elapsed, dim, drop, content, pageIn;
        float unit;    // screen pixels per front end unit
        ImVec2 middle; // the picture's middle on the screen
        float width;   // the picture's width on the screen
        ImVec2 FromUnits(float x, float y) const { return {middle.x + x * unit, middle.y - y * unit}; }
        ImVec2 ToUnits(ImVec2 at) const { return {(at.x - middle.x) / unit, (middle.y - at.y) / unit}; }
    };
    // The TV as this Draw lays it out.
    struct Tv {
        ui::Painter set, screen, body; // the frame; what sits on the screen; the page's content
        float half;                    // the frame's half width
        float screenBottom;
        float contentTop; // where the page's items start
        bool compact;     // a question's TV
    };

    // F1, HOME, F2 or the game's pause (MenuInput::SetMenuKeys).
    void MenuKey(MenuInput::MenuKey key, int channel);
    void Command(int kind, int which = 0, int value = 0);
    void Play(ui::Sound sound);
    void Push(ui::PageId id, int category = 0, std::string key = {}, Origin origin = Origin::None);
    void Pop();
    // All pages go (the Recharged menu closing, the match resuming); the game's
    // own options are saved when a page changed them.
    void ClearPages();
    // Whether a page that came from `origin` is in the stack.
    bool Opened(Origin origin) const;
    bool RechargedOpen() const { return Opened(Origin::Recharged); }
    // The Recharged menu and its pages go, back to what was open under it.
    void CloseRecharged();
    // On the way out of a forfeited match: the pages stay as they are.
    bool Leaving() const;
    void Apply(const ui::PageActions& actions, bool live, const overlay::GameOverlayModel& model);
    void Back();
    void Resume();
    // The prompt's page on top while it asks; gone once it doesn't.
    void ShowPrompt(Clock::time_point now);
    // The overlay's texture of a game texture, made once (0 when the game has
    // none or it cannot be read).
    void* Texture(unsigned key);
    void* NoiseTexture();

    // Draw's steps.
    Frame Locate(float width, float height, Clock::time_point now) const;
    Tv LayOut(const Frame& frame, Clock::time_point now);
    void DrawHeader(const Frame& frame, const Tv& tv);
    ModelNeeds Needs() const;
    void BuildModel(ui::PageModel& model);
    ui::Input ReadInput();
    // The page on top with its categories and footer, then what the player
    // did; whether the hand is over an item.
    bool DrawTopPage(const Tv& tv, const ui::Fonts& fonts, const ui::Input& in, const ui::PageModel& model,
                     Clock::time_point now);
    void DrawPauseButtons(const Frame& frame, const Tv& tv, const ui::Fonts& fonts);
    void DrawHands(const Frame& frame, const ui::Input& in, bool overItem);
    // The hand the mouse draws: the keyboard player's, else the one of the
    // player the game's menus follow, else any player's.
    const std::vector<ChargedMenuSnapshot::Quad>& MouseHand() const;

    Game game_;
    MenuInput& input_;
    Host host_;
    ChargedMenuSnapshot menu_{};
    bool known_ = false; // menu_ is the game's (else all zero)
    float feHeight_ = 448.0f; // the game's front end: its screen is 448 rows
    std::vector<Page> pages_;
    // The front end's Options hub went (BACK, CREDITS): it comes again only
    // with the scene's next visit.
    bool anchorDone_ = false;
    // CREDITS chosen over the main menu: its OPTIONS were pressed, the
    // Options' Credits follow once that scene takes input (until then).
    std::optional<Clock::time_point> creditsWalk_;
    // The prompt: its question, its answer to take.
    std::optional<overlay::OverlayPrompt> prompt_;
    std::optional<int> promptAnswer_;
    std::optional<std::string> promptStadium_;
    // TOURNAMENT's way into the trophy room, since when.
    TournamentWay tournament_ = TournamentWay::None;
    bool tournamentPage_ = false; // TournamentSetDone: the page comes again in the room
    // The way into the room ends on the player's own TOURNAMENT (their set's
    // card asked, or the room comes again after a set).
    bool tournamentOwn_ = false;
    std::string bracketPicked_; // the BRACKET's set whose games are listed
    // TournamentSetDone: the room again until then (none: not asked).
    std::optional<Clock::time_point> tournamentReturn_;
    Clock::time_point tournamentSettled_{}; // TournamentSetDone: the room's state taken from then
    ui::TournamentModel tournamentModel_;
    ui::TournamentListModel tournamentListModel_;
    // The room's first page: TOURNAMENTS (with the player's own TOURNAMENT
    // over it after a set, `own`), or TOURNAMENT alone without a list.
    void PushRoomPages(bool own);
    Clock::time_point tournamentAt_{};
    void Tournament(TournamentWay way);
    // The trophy room's way: follows the game (its login, the room's camera).
    void ServiceTournament(Clock::time_point now);
    // The game hands its players' cursors over while a page shows.
    bool cursors_ = false;
    ui::PageState pageState_;
    std::optional<unsigned> taken_; // the module's count of taken presses, once known
    int channel_ = -1;
    bool pausePress_ = false;
    bool capturePageShown_ = false; // RECHARGED_CAPTURE_PAGE opened its page
    // The player forfeited the online match: on the way out (the game leaves
    // it once the host has told the server).
    bool forfeitAsked_ = false;
    // The game's own options changed on a page: saved when it goes.
    bool gameChanged_ = false;
    // A slider moved live: its change again, saved, once it rests.
    std::optional<overlay::GameSettingChange> pendingSave_;
    Clock::time_point lastLive_{};
    // The TV: shown, since when; leaving (its MENU OUT), since when; the
    // page on top since when (its content comes in).
    bool shown_ = false, leaving_ = false;
    Clock::time_point shownAt_{}, leavingAt_{}, pageAt_{};
    // The frame's half width now (design pixels): it widens for the pages
    // with categories and narrows again.
    float halfWidth_ = ui::kStandardHalfWidth;
    // The screen's and the frame's bottom edges (a question's TV is shorter),
    // and when the TV was last drawn (it opens at its size).
    float screenBottom_ = ui::kScreenBottom, frameBottom_ = ui::kFrameBottom;
    Clock::time_point drawnAt_{};
    bool blurred_ = false;
    // The mouse moved since the pages took the input: its hand shows. Moving
    // is told from where it was when they took it (ImGui's own delta misses
    // the first event after an unknown position).
    bool pointerShown_ = false;
    std::optional<ImVec2> lastPointer_;
    // The player whose hand the pages follow (MenuInput::Hands), -1 the mouse.
    int pointerHand_ = -1;
    // The last hand of each player as the game drew it, around its pointer,
    // and this frame's (kept between frames for their room).
    std::array<std::vector<ChargedMenuSnapshot::Quad>, 4> hands_, newHands_;
    // The game's pictures as ImGui textures: the original, decoded once and
    // kept (the overlay cannot free a texture), and a texture pack's
    // replacement of it, looked up again now and then (a streamed one comes
    // in a little later, at first as a small thumbnail).
    struct GameTexture {
        void* original = nullptr;
        bool decoded = false;
        void* replacement = nullptr;
        Clock::time_point looked{};
    };
    std::unordered_map<unsigned, GameTexture> textures_;
    void* noise_ = nullptr;
    bool noiseMade_ = false;
    // The game's header picture once seen (the front end has none of its own).
    void* header_ = nullptr;
};

} // namespace mscharged::runtime
