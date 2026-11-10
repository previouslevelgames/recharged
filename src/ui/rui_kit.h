#pragma once

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The Recharged UI kit: Recharged's menus in the look of the game's own (its
// pause menu's TV set, the header bar, the plates, its pages' dark title bars
// with orange values, green bars and gold arrow buttons, its big BACK and
// ACCEPT, the hand, its sounds and its MENU IN / MENU OUT timing), with a few
// modern touches (a living grain and interference bar, smooth easing, a TV
// that widens for the pages with more in them, sharp at any size). Pure ImGui
// drawing in the caller's frame; the art is the kit's own, the game's pictures
// (its header bar, its hand) come in as textures the host made from the game.
//
// Everything is laid out in design pixels: those of a 1080-row picture, from
// the TV's middle, y down. A Painter maps them to the screen.
namespace mscharged::ui
{
inline constexpr float kDesignHeight = 1080.0f;
// Design pixels per front end unit (the game's 448-row front end in 1080
// rows, as its own layout scales it).
inline constexpr float kDesignPerUnit = 2.25f;
// The text sizes, in design pixels: the title, a plate's label (a lit one
// kLitPlateScale larger: the host makes that font too), a row's label and
// value, the small lines (paths, help).
inline constexpr float kTitleSize = 70.0f, kPlateSize = 53.0f, kRowSize = 39.5f, kSmallSize = 28.0f;
inline constexpr float kLitPlateScale = 1.08f;
// The screen's grain picture: kNoiseSize x kNoiseSize texels (NoiseTexels).
inline constexpr int kNoiseSize = 512;

// The TV set's parts, in design pixels from its middle (the game's
// pausemenu_v3 layout): the frame (its half width: the pause menu's, or a
// wide one for pages with more in them), the screen inside it, the header bar
// on the screen's top, the footer on the frame's lower lip.
inline constexpr float kFrameTop = -468.0f, kFrameBottom = 454.0f;
inline constexpr float kStandardHalfWidth = 468.0f, kWideHalfWidth = 706.0f;
inline constexpr float kBezel = 12.0f; // frame edge to screen, each side (and above it)
inline constexpr float kScreenTop = -456.0f, kScreenBottom = 359.0f;
inline constexpr float kHeaderBottom = -362.0f;
inline constexpr float kTitleY = -410.0f;
inline constexpr float kFooterY = 407.0f;

// Letters in capitals (Latin, Greek and Cyrillic too: texts::Upper), as the
// game's own labels.
std::string Upper(const char* text);
// `value` a step closer to `target` this frame (the kit's easing: fast at
// first, snapping on in the last half pixel).
void Ease(float& value, float target);

struct Fonts
{
    ImFont* title = nullptr;
    ImFont* plate = nullptr;
    ImFont* row = nullptr;
    ImFont* small = nullptr;
};

// The menus' sounds: the game's own, which the host has the game play.
enum class Sound
{
    Hover,  // the lit item changes
    Select, // a plate is pressed
    Back,   // a page goes
    Step,   // a value steps (a bar)
    Switch, // a switch or a choice changes
};

// A part of the screen, in design pixels.
struct Area
{
    float x0, x1, top, bottom;
};

// Design pixels on the screen: `s` screen pixels each, from `c` (the TV's
// middle), y down; colours faded by `fade` (0..1).
struct Painter
{
    ImDrawList* draw = nullptr;
    float s = 1.0f;
    ImVec2 c{0, 0};
    float fade = 1.0f;

    ImVec2 At(float x, float y) const;
    ImU32 Col(int r, int g, int b, int a = 255) const;
    void Rect(float x0, float y0, float x1, float y1, ImU32 col, float round = 0.0f) const;
    void Frame(float x0, float y0, float x1, float y1, ImU32 col, float round, float thickness) const;
    void Gradient(float x0, float y0, float x1, float y1, ImU32 top, ImU32 bottom) const;
    // A gradient inside rounded corners (the fill stays within them).
    void RoundGradient(float x0, float y0, float x1, float y1, ImU32 top, ImU32 bottom, float round) const;
    // Text with a dark outline `outline` design pixels wide, its anchor at
    // (x, y): `align` 0 left, 0.5 centred, 1 right; vertically centred.
    void Text(ImFont* font, float size, float x, float y, const char* text, ImU32 fill, ImU32 edge, float outline,
              float align = 0.5f) const;
    float TextWidth(ImFont* font, float size, const char* text) const; // in design pixels
    // Text wrapped at `width` design pixels from (x, y) down; its height.
    float Wrapped(ImFont* font, float size, float x, float y, float width, const char* text, ImU32 fill) const;
    // The height Wrapped gives that text (`none` without a font).
    float WrappedHeight(ImFont* font, float size, float width, const char* text, float none = 0.0f) const;
    Painter Faded(float alpha) const { return {draw, s, c, fade * alpha}; }
    // The point of the screen `screen` in design pixels.
    ImVec2 ToDesign(ImVec2 screen) const { return {(screen.x - c.x) / s, (screen.y - c.y) / s}; }
};

// How the TV is drawn this frame.
struct TvLook
{
    float half_width = kStandardHalfWidth; // the frame's (it widens for the pages with more in them)
    float elapsed = 0.0f;                  // seconds since it opened: the grain, the bar, the title's shake
    ImTextureID noise = 0;                 // NoiseTexels' picture, or 0 for none
    // The screen's and the frame's bottom edges: the full TV's (the frame's
    // lip under the screen for the footer), or a shorter one (a question).
    float screen_bottom = kScreenBottom;
    float frame_bottom = kFrameBottom;
};

// The frame with its bezel and the screen (an old TV: its colours, grain,
// darker corners, scanlines and the interference bar). `content` fades what
// sits on the screen (scanlines and bar with it).
void DrawTv(const Painter& set, const Painter& content, const TvLook& look);
// The screen's rounded corners, the bezel drawn over them: after what sits
// on the screen.
void DrawScreenMask(const Painter& set, float half_width, float screen_bottom);
// The screen's grain: kNoiseSize x kNoiseSize RGBA texels, made once.
std::vector<unsigned char> NoiseTexels();
// The header bar on the screen's top: the game's picture (`header`, its four
// corners on the screen: top left, top right, bottom right, bottom left), its
// edges drawn on to the sides of a wider screen; without it one drawn alike.
void DrawHeader(const Painter& content, float half_width, ImTextureID header, const ImVec2* corners);
// The title as the game layers it: the text, a larger ghost and a smaller
// one, each shaking by a front end unit in the game's 0.574 s loop.
void DrawTitle(const Painter& content, ImFont* font, const char* title, float elapsed);
// The players' input for a page this frame (the host reads ImGui's).
struct Input
{
    ImVec2 pointer{-1000.0f, -1000.0f}; // the mouse, in screen pixels
    bool pointer_shown = false;          // it moved since the page opened: the hand shows, items light under it
    bool pointer_moved = false;          // this frame
    bool click = false;                  // its button pressed this frame
    bool held = false;                   // its button down
    float wheel = 0.0f;                  // its wheel, in notches (up positive)
    bool up = false, down = false, left = false, right = false; // the D-pad, the stick, the arrows (repeating)
    bool accept = false; // A, Return, Space
    bool back = false;   // B, Escape, Backspace, the right button
    bool resume = false; // the game's pause button (1, the keyboard's Z)
    bool previous = false, next = false; // -, +, Q, E: the category beside
    bool repeat = false; // left or right held long enough to repeat (a slider's change is live)
};

// What a page shows starts under the header, at kContentTop.
inline constexpr float kContentTop = kHeaderBottom + 16.0f;
// A plate as the pause menu's buttons (the game's list_back plate): its
// label centred at `mid`, `half` design pixels to each side; lit, it grows and
// turns solid with a light edge, as the game's "over".
void DrawPlate(const Painter& content, ImFont* font, float half, float mid, const char* label, bool lit);

// The footer on the frame's lower lip, as the game's pages have it: big
// words with their arrows, "< BACK" at the left and the page's way on
// ("RESUME >", "CLOSE >") at the right.
struct FooterButton
{
    const char* label = ""; // "BACK", "RESUME", "CLOSE"
    bool right = false;     // at the right, its arrow after it; else at the left, its arrow before it
};
// Draws `count` of them; the one under the hand (`pointer`, if shown) is lit
// and its index returned (-1 for none), for a click to press it.
int DrawFooter(const Painter& set, const Fonts& fonts, float half_width, const FooterButton* buttons, int count,
               ImVec2 pointer, bool pointer_shown);

// A page's categories in a column at its left (retail plates): `selected` is
// the one shown, with a gold edge; the hand lights the one it is over (- and
// + switch them too). The category clicked, or -1; the column's height in
// `height`.
int DrawCategories(const Painter& content, const Fonts& fonts, const Input& input, Area area,
                   const std::vector<std::string>& names, int selected, const std::function<void(Sound)>& sound,
                   float* height = nullptr);

// A line's colour by what it says (the overlay's tones).
enum class Tone
{
    Neutral, // light grey
    Good,    // green: free to play, won, ready
    Busy,    // the game's orange: in a set, waiting
    Away,    // dim: offline
    Bad,     // red: lost, failed
};
// A person's picture: their image (a circle), else their initials on a
// colour picked by `tint`; the dot of their presence; their country.
struct Person
{
    ImTextureID image = 0;
    std::string initials;
    std::int64_t tint = 0;
    int presence = -1;   // -1 none, 0 free, 1 busy, 2 offline
    std::string country; // two capital letters (CH, DE, AT), empty for none
};
// The picture in a circle of `radius` design pixels around (cx, cy).
void DrawPerson(const Painter& content, ImFont* initials_font, float cx, float cy, float radius, const Person& person);
// A person's country as its two letters in a small outlined box (the menus'
// look, not a flag), `height` design pixels tall, its left edge at x, centred
// on y; its width (0 without one).
float DrawCountry(const Painter& content, ImFont* font, float x, float y, float height, const Person& person);

// What a list remembers between frames (the page keeps it).
struct ListState
{
    int lit = 0;              // the lit item (of those that can be lit)
    float scroll = 0.0f;      // design pixels scrolled
    float target = 0.0f;      // where scrolling goes
    int count = 0;            // items last frame
    float height = 0.0f;      // content height last frame
    // The lit item is to come into view (the D-pad moved it, the page
    // opened), and its place this frame (content pixels).
    bool reveal = true;
    float lit_top = 0.0f, lit_bottom = 0.0f;
    bool help = false;    // last frame had help lines
    bool entered = false; // the page has run a frame (no sound for its first lit item)
    // The scroll rail: an arrow held (-1 up, 1 down) since `since` (seconds,
    // ImGui's clock), the thumb dragged from `drag` (its grip, design pixels).
    int hold = 0;
    double since = 0.0;
    bool dragging = false;
    float drag = 0.0f;
};

// A page's items on the TV's screen, immediate mode, top to bottom: section
// captions, plates and rows with a value (bars with their arrow buttons, a
// switch, a choice). One item is lit (the D-pad moves it; the hand lights
// what it is over); A, a click or Return presses it; left and right (or the
// arrow buttons) change its value. The lit item's name and help show in
// `help` (else at the bottom of the rows' area). It scrolls to keep the lit
// item in view, with the wheel and with its rail (for the hand).
class List
{
public:
    List(const Painter& content, const Fonts& fonts, const Input& input, ListState& state,
         std::function<void(Sound)> sound, Area rows, std::optional<Area> help = std::nullopt);
    ~List();
    List(const List&) = delete;
    List& operator=(const List&) = delete;

    void Caption(const char* text);
    // A line of text (wrapped), not lit.
    void Note(const char* text);
    // A plate: true when pressed. `about`: its help line; `value`: a short
    // fact at its right end ("3 online", the rules it plays by).
    bool Plate(const char* label, const char* about = nullptr, bool enabled = true, const char* value = nullptr);
    // A row whose value is text, pressed as a plate (a page beside).
    bool Row(const char* label, const char* value, const char* about = nullptr);
    // A value in `bars` bars from `min` to `max` in `step`s, shown as `text`;
    // the new value when changed.
    std::optional<int> Bars(const char* label, int value, int min, int max, int step, int bars, const char* text,
                            const char* about = nullptr);
    // `off_text` and `on_text`: its two choices (none: OFF and ON).
    std::optional<bool> Switch(const char* label, bool on, const char* about = nullptr, const char* off_text = nullptr,
                               const char* on_text = nullptr);
    std::optional<int> Choice(const char* label, const std::vector<std::string>& choices, int index,
                              const char* about = nullptr);
    // A value stepped through with the arrow buttons (or left and right; A
    // steps on): -1 or +1 when stepped.
    std::optional<int> Cycle(const char* label, const char* value, const char* about = nullptr);
    // A player in a list: their picture, name and flag, a status line in its
    // tone, a tag at the right ("EU"); true when pressed (their page).
    bool PlayerRow(const Person& person, const char* name, const char* status, Tone tone, const char* tag = nullptr,
                   const char* about = nullptr);
    // A card at the top of a page: a big picture, a small orange line, the
    // title, and lines under it; with `won`/`lost` of `games` (best of) the
    // series as pips. Not lit.
    void Card(const Person* person, const char* kicker, const char* title, const std::vector<std::string>& lines,
              Tone tone = Tone::Neutral, int games = 0, int won = 0, int lost = 0);
    // A leaderboard's row: its rank, the player's name (flag), the record and
    // the points; the player's own in gold.
    void BoardRow(int rank, const char* name, const char* record, const char* points, bool you,
                  const Person* person = nullptr);
    // Whether the hand is over an item that can be pressed (for its A).
    bool PointerOverItem() const { return pointerOver_; }

private:
    struct Slot
    {
        float top, bottom; // on the screen, design pixels
        bool lit, hovered;
        int index;
    };
    Slot Place(float height, bool litable);
    // The hand over the slot lights it (`slot.lit` follows).
    void LightUnderPointer(Slot& slot);
    // A row with a value: placed, lit under the hand, its bar and label
    // drawn, its help kept; its slot.
    Slot ValueRow(const char* label, const char* about);
    void RowBar(const Slot& slot, const char* label);
    void Help(const char* label, const char* about, bool lit);
    // Pressed: A on the lit item (`right` too: a page beside), or a click on it.
    bool Pressed(const Slot& slot, bool right);
    // The "›" at a row's right, white while lit.
    void Chevron(float mid, bool lit);
    void Play(Sound sound) const
    {
        if (sound_) sound_(sound);
    }
    // The scroll rail at the right while the items are more than the screen
    // shows: arrow buttons (held, they keep going), a track that pages, a
    // thumb to drag.
    void Rail(float view);
    // A choice's button (a switch's, a choice's), its text centred; true when
    // the hand clicks it.
    bool Pill(const Slot& slot, float x0, float x1, const char* text, bool chosen);
    // An arrow button (the game's gold one; silver when it does nothing);
    // true when the hand clicks it.
    bool Arrow(const Slot& slot, float x, bool right, bool enabled);

    Painter p_;
    const Fonts& fonts_;
    const Input& in_;
    ListState& state_;
    std::function<void(Sound)> sound_;
    Area rows_;
    std::optional<Area> help_area_;
    float listBottom_;
    float y_ = 0.0f; // content pixels
    int index_ = 0;
    std::string help_; // the lit item's (a copy: the page's texts may be its own)
    std::string helpTitle_;
    bool rail_ = false;
    float railX0_ = 0.0f;
    bool anyHelp_ = false;
    bool pointerOver_ = false;
    bool clipped_ = false;
};
} // namespace mscharged::ui
