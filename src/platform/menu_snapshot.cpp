// Recharged's menus over the game's (the development switch "Override retail
// menus", tweak patch 0070): reads the game's menu scenes after a frame (the
// match's PauseMenuScene, the front end's OptionsScene), keeps what the front
// end hands over of its players' cursors while the host draws the menu (the
// cursors are then drawn by the host, on top of it), counts the game's buttons
// Recharged's pages take, and does what the host asks in the game's next
// frame. Reading changes nothing in the game.
#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error The menu snapshot belongs to the isolated original game module
#endif
#include "platform/menu_snapshot.h"

#include "Game/BaseGameSceneManager.h"
#include "Game/DB/SaveLoad.h"
#include "Game/DB/UserOptions.h"
#include "Game/FE/FEAudio.h"
#include "Game/FE/feCupFlow.h"
#include "Game/FE/feDPD.h"
#include "Game/FE/feRender.h"
#include "Game/FE/feFinder.h"
#include "Game/FE/feFinder_impl.h"
#include "Game/FE/fePackage.h"
#include "Game/FE/fePointerButton.h"
#include "Game/FE/fePresentation.h"
#include "Game/FE/feScene.h"
#include "Game/FE/feTextureResource.h"
#include "Game/FE/tlComponentInstance.h"
#include "Game/FE/tlImageInstance.h"
#include "Game/FE/tlTextInstance.h"
#include "Game/GameInfo.h"
#include "Game/GameSceneManager.h"
#include "Game/Render/RLViewLayers.h"
#include "Game/FE/fePopupMenu.h"
#include "Game/NetworkSession.h"
#include "Game/SH/SHMainMenu.h"
#include "Game/SH/SHNavigation.h"
#include "Game/SH/SHOptions.h"
#include "Game/SH/SHPause.h"
#include "NL/gl/glDraw2.h"
#include "NL/gl/glDraw3.h"
#include "NL/gl/glMatrix.h"
#include "NL/gl/glStruct.h"
#include "NL/glx/glxTexture.h"
#include "NL/nlString.h"
#include "mscharged_tweaks.h"

#include <algorithm>
#include <iterator>
#include <cstddef>
#include <cstdint>
#include <cstring>

class cGame;
extern cGame* g_pGame;

namespace {
// The game's text (native UTF-16 of the basic plane) as UTF-8 in `out`,
// cut at a whole character.
void CopyUtf8(const unsigned short* text, char* out, std::size_t size)
{
    std::size_t at = 0;
    for (std::size_t n = 0; text && text[n] && n < 256; ++n) {
        const unsigned code = text[n];
        char bytes[3];
        std::size_t count = 0;
        if (code < 0x20) continue; // the game's line breaks and markers
        if (code < 0x80) bytes[count++] = char(code);
        else if (code < 0x800) {
            bytes[count++] = char(0xC0 | (code >> 6));
            bytes[count++] = char(0x80 | (code & 0x3F));
        } else if (code >= 0xD800 && code <= 0xDFFF) continue; // no surrogates in the game's fonts
        else {
            bytes[count++] = char(0xE0 | (code >> 12));
            bytes[count++] = char(0x80 | ((code >> 6) & 0x3F));
            bytes[count++] = char(0x80 | (code & 0x3F));
        }
        if (at + count >= size) break;
        std::memcpy(out + at, bytes, count);
        at += count;
    }
    out[at] = '\0';
}

// The text `name` on a component's slide `slide`, or null.
TLTextInstance* TextOf(TLInstance* component, const char* slide, const char* name)
{
    if (!component) return nullptr;
    return FEFinder<TLTextInstance, 3>::Find(component, InlineHasher(slide), InlineHasher(name));
}

bool Replaced()
{
#if defined(MSCHARGED_TWEAKS)
    return mscharged_tweak_pause_menu_replaced();
#else
    return false;
#endif
}

// The scene on top of `manager`'s stack when it is `scene`, or null.
BaseSceneHandler* TopScene(const BaseGameSceneManager* manager, SceneList scene)
{
    if (!manager) return nullptr;
    const unsigned long depth = manager->mCurrentStackDepth;
    if (depth == 0 || depth > BaseGameSceneManager::MAX_SCENE_DEPTH) return nullptr;
    if (manager->m_sceneStack[depth - 1] != scene) return nullptr;
    return manager->mBaseSceneHandlerStack[depth - 1];
}

// The pause menu when it is the match overlays' top scene, or null.
PauseMenuScene* TopPauseMenu() { return static_cast<PauseMenuScene*>(TopScene(g_pOverlayManager, SCENE_PAUSE)); }

// The pause menu Recharged draws: the switch on, the menu on top, its scene
// created and no quit under way.
PauseMenuScene* ShownPauseMenu()
{
    if (!Replaced()) return nullptr;
    PauseMenuScene* scene = TopPauseMenu();
    if (!scene || !scene->IsSceneReady() || scene->mQuitDelay > 0.0f) return nullptr;
    return scene;
}

// The front end's Options (the main menu's OPTIONS, options_main_menu.fen)
// Recharged draws: the switch on, the scene on top of the front end's, its
// scene created, nothing of the overlays' over it.
OptionsScene* ShownOptions()
{
    if (!Replaced()) return nullptr;
    if (g_pOverlayManager && g_pOverlayManager->mCurrentStackDepth != 0) return nullptr;
    auto* scene = static_cast<OptionsScene*>(TopScene(GameSceneManager::Instance(), SCENE_OPTIONS));
    if (!scene || !scene->IsSceneReady()) return nullptr;
    return scene;
}

// Recharged's pages show over the game (the host's command): the players'
// cursors are the host's to draw.
bool sCursors = false;

// Whether Recharged draws a menu, whose players' cursors it draws too.
bool MenuShown() { return ShownPauseMenu() || ShownOptions() || (sCursors && Replaced()); }

// The main menu when it is the front end's top scene, or null.
SHMainMenu* TopMainMenu()
{
    if (g_pOverlayManager && g_pOverlayManager->mCurrentStackDepth != 0) return nullptr;
    return static_cast<SHMainMenu*>(TopScene(GameSceneManager::Instance(), SCENE_MAIN_MENU));
}

// The main menu's ONLINE pressed again the game's way (WII ONLINE): the hook
// lets this one through.
bool sPassMainOnline = false;

// The match against another machine running (ChargedNetworkMatch).
ChargedNetworkMatch NetworkMatch()
{
    if (!g_pGame || !g_pNetworkSession) return CHARGED_NETWORK_NONE;
    switch (g_pNetworkSession->GetSessionMode()) {
    case NET_MODE_LAN: return CHARGED_NETWORK_LAN;
    case NET_MODE_ONLINE: return CHARGED_NETWORK_ONLINE;
    default: return CHARGED_NETWORK_NONE;
    }
}

// The player forfeited the online match: its network popup (the connection
// it closed) is answered for them, until the game is back in its menus.
bool sForfeiting = false;

// The cursors' quads the front end handed over since the host last read them,
// and the cursor being drawn (its pad, -1 for none) with its scene's view
// offset (the front end adds it to every matrix).
ChargedMenuSnapshot::Quad sQuads[64];
int sQuadCount = 0;
int sCapturePad = -1;
float sViewX = 0.0f, sViewY = 0.0f;
// Recharged's menu shows: the cursor goes to the host only, the game leaves
// it out of its picture (else the host just learns the hand's look).
bool sCaptureHides = false;
// The header picture's texture (cupscreenheader_default.tga, the orange bar
// with the game's logo behind a menu's title).
constexpr unsigned long kHeaderPicture = 0xC1847BD5ul;
// The game's own menu sounds (FEAudio anim audio events), by ChargedMenuSound.
constexpr unsigned long kMenuSounds[] = {0xF6EB899Eul, 0xF0AFD586ul, 0x304FDD1Eul, 0x3021A1EEul, 0x362F2841ul};

// The game's buttons Recharged's pages took.
unsigned sTaken = 0;
int sTakenMenu = 0, sTakenButton = -1, sTakenPad = -1;

// What the host asked for, done in the game's next frame (BeginFrameTask).
ChargedMenuCommand sCommands[16];
int sCommandCount = 0;

// The front end's matrices map a row vector: (x, y, z, 1) * m.
void Transform(const float* m, float x, float y, float z, float& outX, float& outY)
{
    outX = x * m[0] + y * m[4] + z * m[8] + m[12] - sViewX;
    outY = x * m[1] + y * m[5] + z * m[9] + m[13] - sViewY;
}

ChargedMenuSnapshot::Quad* NextQuad(unsigned texture)
{
    if (sQuadCount >= int(sizeof sQuads / sizeof sQuads[0])) return nullptr;
    ChargedMenuSnapshot::Quad* quad = &sQuads[sQuadCount++];
    *quad = {};
    quad->texture = texture;
    quad->pad = sCapturePad;
    return quad;
}

// GX texture tiles (the console's order and layout) as RGBA rows.
std::uint16_t Be16(const std::uint8_t* p) { return std::uint16_t(p[0] << 8 | p[1]); }

void Put(std::uint8_t* rgba, int width, int height, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b,
         std::uint8_t a)
{
    if (x >= width || y >= height) return;
    std::uint8_t* out = rgba + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 4;
    out[0] = r, out[1] = g, out[2] = b, out[3] = a;
}

void Rgb565(std::uint16_t v, std::uint8_t c[4])
{
    c[0] = std::uint8_t(((v >> 11) & 31) * 255 / 31);
    c[1] = std::uint8_t(((v >> 5) & 63) * 255 / 63);
    c[2] = std::uint8_t((v & 31) * 255 / 31);
    c[3] = 255;
}

void Rgb5a3(std::uint16_t v, std::uint8_t c[4])
{
    if (v & 0x8000) {
        c[0] = std::uint8_t(((v >> 10) & 31) * 255 / 31);
        c[1] = std::uint8_t(((v >> 5) & 31) * 255 / 31);
        c[2] = std::uint8_t((v & 31) * 255 / 31);
        c[3] = 255;
    } else {
        c[0] = std::uint8_t(((v >> 8) & 15) * 17);
        c[1] = std::uint8_t(((v >> 4) & 15) * 17);
        c[2] = std::uint8_t((v & 15) * 17);
        c[3] = std::uint8_t(((v >> 12) & 7) * 255 / 7);
    }
}

// Level 0 of `data` (`size` bytes) in `format` into `rgba` (width * height * 4
// bytes); a CI8 texture's colours are `palette` (`entries` RGB5A3 words, the
// console's order, as the game's TLUT). False for a format it cannot read or
// too little data.
bool DecodeGx(const std::uint8_t* data, std::size_t size, eGXTextureFormat format, int width, int height,
              const std::uint8_t* palette, int entries, std::uint8_t* rgba)
{
    int tileW = 4, tileH = 4;
    std::size_t tileBytes = 32;
    switch (format) {
    case GXTex_I4: tileW = 8, tileH = 8; break;
    case GXTex_I8: case GXTex_A8: tileW = 8; break;
    case GXTex_CI8:
        if (!palette || entries <= 0) return false;
        tileW = 8;
        break;
    case GXTex_IA8: case GXTex_RGB565: case GXTex_RGB5A3: break;
    case GXTex_RGBA8: tileBytes = 64; break;
    case GXTex_CMPR: tileW = 8, tileH = 8; break;
    default: return false;
    }
    const int tilesX = (width + tileW - 1) / tileW, tilesY = (height + tileH - 1) / tileH;
    if (std::size_t(tilesX) * std::size_t(tilesY) * tileBytes > size) return false;
    const std::uint8_t* tile = data;
    for (int ty = 0; ty < tilesY; ++ty)
        for (int tx = 0; tx < tilesX; ++tx, tile += tileBytes) {
            const int x0 = tx * tileW, y0 = ty * tileH;
            switch (format) {
            case GXTex_I4:
                for (int n = 0; n < 64; ++n) {
                    const std::uint8_t byte = tile[n / 2];
                    const std::uint8_t i = std::uint8_t((n & 1 ? byte & 15 : byte >> 4) * 17);
                    Put(rgba, width, height, x0 + n % 8, y0 + n / 8, i, i, i, i);
                }
                break;
            case GXTex_I8:
                for (int n = 0; n < 32; ++n) Put(rgba, width, height, x0 + n % 8, y0 + n / 8, tile[n], tile[n], tile[n], tile[n]);
                break;
            case GXTex_A8:
                for (int n = 0; n < 32; ++n) Put(rgba, width, height, x0 + n % 8, y0 + n / 8, 255, 255, 255, tile[n]);
                break;
            case GXTex_CI8:
                for (int n = 0; n < 32; ++n) {
                    std::uint8_t c[4] = {0, 0, 0, 0};
                    if (tile[n] < entries) Rgb5a3(Be16(palette + tile[n] * 2), c);
                    Put(rgba, width, height, x0 + n % 8, y0 + n / 8, c[0], c[1], c[2], c[3]);
                }
                break;
            case GXTex_IA8:
                for (int n = 0; n < 16; ++n) {
                    const std::uint8_t a = tile[n * 2], i = tile[n * 2 + 1];
                    Put(rgba, width, height, x0 + n % 4, y0 + n / 4, i, i, i, a);
                }
                break;
            case GXTex_RGB565:
            case GXTex_RGB5A3:
                for (int n = 0; n < 16; ++n) {
                    std::uint8_t c[4];
                    if (format == GXTex_RGB565) Rgb565(Be16(tile + n * 2), c);
                    else Rgb5a3(Be16(tile + n * 2), c);
                    Put(rgba, width, height, x0 + n % 4, y0 + n / 4, c[0], c[1], c[2], c[3]);
                }
                break;
            case GXTex_RGBA8:
                for (int n = 0; n < 16; ++n)
                    Put(rgba, width, height, x0 + n % 4, y0 + n / 4, tile[n * 2 + 1], tile[32 + n * 2], tile[32 + n * 2 + 1],
                        tile[n * 2]);
                break;
            case GXTex_CMPR:
                // Four 4x4 blocks (DXT1 with big-endian colours), left to right, top to bottom.
                for (int block = 0; block < 4; ++block) {
                    const std::uint8_t* b = tile + block * 8;
                    const std::uint16_t c0 = Be16(b), c1 = Be16(b + 2);
                    std::uint8_t colours[4][4];
                    Rgb565(c0, colours[0]);
                    Rgb565(c1, colours[1]);
                    for (int k = 0; k < 3; ++k) {
                        if (c0 > c1) {
                            colours[2][k] = std::uint8_t((2 * colours[0][k] + colours[1][k]) / 3);
                            colours[3][k] = std::uint8_t((colours[0][k] + 2 * colours[1][k]) / 3);
                        } else {
                            colours[2][k] = std::uint8_t((colours[0][k] + colours[1][k]) / 2);
                            colours[3][k] = 0;
                        }
                    }
                    colours[2][3] = 255;
                    colours[3][3] = c0 > c1 ? 255 : 0;
                    const int bx = x0 + (block & 1) * 4, by = y0 + (block >> 1) * 4;
                    for (int row = 0; row < 4; ++row)
                        for (int col = 0; col < 4; ++col) {
                            const int index = (b[4 + row] >> (6 - col * 2)) & 3;
                            Put(rgba, width, height, bx + col, by + row, colours[index][0], colours[index][1],
                                colours[index][2], colours[index][3]);
                        }
                }
                break;
            default: return false;
            }
        }
    return true;
}
} // namespace

#if defined(MSCHARGED_TWEAKS)
int mscharged_tweak_cursor_capture_begin(const void* instance)
{
    const int previous = sCapturePad;
    if (previous >= 0 || !instance) return previous;
    for (int pad = 0; pad < 4; ++pad)
        if (instance == GetPointerInstance(pad)) {
            if (!Replaced() || !FERender::m_pRenderScene) return previous;
            sCapturePad = pad;
            sCaptureHides = MenuShown();
            sViewX = FERender::m_pRenderScene->m_matView.e[12];
            sViewY = FERender::m_pRenderScene->m_matView.e[13];
            break;
        }
    return previous;
}

void mscharged_tweak_cursor_capture_end(int previous)
{
    sCapturePad = previous;
    if (previous < 0) sCaptureHides = false;
}

bool mscharged_tweak_cursor_capture_quad(unsigned long texture, const void* matrix, const void* quad)
{
    if (sCapturePad < 0 || !matrix || !quad) return false;
    const float* m = static_cast<const nlMatrix4*>(matrix)->e;
    const glQuad3& source = *static_cast<const glQuad3*>(quad);
    if (ChargedMenuSnapshot::Quad* out = NextQuad(unsigned(texture))) {
        for (int n = 0; n < 4; ++n) {
            Transform(m, source.m_pos[n].x, source.m_pos[n].y, source.m_pos[n].z, out->x[n], out->y[n]);
            out->u[n] = source.m_uv[n].x;
            out->v[n] = source.m_uv[n].y;
        }
        std::memcpy(out->rgba, source.m_colour[0].c, 4);
    }
    return sCaptureHides;
}

bool mscharged_tweak_cursor_capture_text(unsigned long texture, const void* matrix_handle, const void* quads, int count)
{
    if (sCapturePad < 0 || !quads) return false;
    nlMatrix4 matrix;
    std::memset(&matrix, 0, sizeof matrix);
    matrix.e[0] = matrix.e[5] = matrix.e[10] = matrix.e[15] = 1.0f;
    if (matrix_handle) glGetMatrix(*static_cast<const glMatrixHandle*>(matrix_handle), matrix);
    const glPoly2* polys = static_cast<const glPoly2*>(quads);
    for (int n = 0; n < count; ++n) {
        ChargedMenuSnapshot::Quad* out = NextQuad(unsigned(texture));
        if (!out) break;
        // The font's corners: top left, bottom left, bottom right, top right.
        for (int k = 0; k < 4; ++k) {
            Transform(matrix.e, polys[n].m_pos[k].x, polys[n].m_pos[k].y, 0.0f, out->x[k], out->y[k]);
            out->u[k] = polys[n].m_uv[k].x;
            out->v[k] = polys[n].m_uv[k].y;
        }
        std::memcpy(out->rgba, polys[n].m_colour[0].c, 4);
    }
    return sCaptureHides;
}
#endif

namespace {
// The scene's header, title and buttons' texts from its presentation.
void ReadPausePresentation(PauseMenuScene* scene, ChargedMenuSnapshot& s)
{
    if (!scene->mFEScene || !scene->mFEScene->m_pFEPackage) return;
    FEPresentation* presentation = scene->mFEScene->m_pFEPackage->GetPresentation();
    if (!presentation) return;
    TLComponentInstance* heading = FEFinder<TLComponentInstance, 4>::Find(
        presentation->m_currentSlide, InlineHasher("Layer"), InlineHasher("TITLE2"));
    for (const char* slide : {"challenges", "tutorials"})
        if (TLTextInstance* text = TextOf(heading, slide, "TITLE")) {
            CopyUtf8(text->GetString(), s.title, sizeof s.title);
            if (s.title[0]) break;
        }
    // The header picture: the game's quad is 100 units scaled.
    if (TLImageInstance* header = FEFinder<TLImageInstance, TLAT_IMAGE>::Find(
            presentation->m_currentSlide, InlineHasher("Layer"), InlineHasher("cupscreenheader_default"))) {
        const FETextureResource* resource = header->m_pTextureResource;
        if (resource && resource->IsValid()) {
            s.header_texture = resource->GetTextureHandle();
            s.header_x = header->GetPosition().f.x;
            s.header_y = header->GetPosition().f.y;
            s.header_width = 100.0f * header->GetScale().f.x;
            s.header_height = 100.0f * header->GetScale().f.y;
        }
    }
}

void ReadPause(PauseMenuScene* scene, ChargedMenuSnapshot& s)
{
    s.menu = CHARGED_MENU_PAUSE;
    s.ready = scene->mInitialized && !scene->mIsInTransition;
    s.leaving = scene->mIsInTransition && scene->mTransitionTo != PauseMenuScene::TT_IN;
    s.controller = -1;
    for (int n = 0; n < 7; ++n) {
        ChargedMenuSnapshot::Button& button = s.buttons[n];
        // Its pointer region once the opening slide has set it up.
        if (scene->mInitialized) {
            FEPointerButton& region = scene->mOptionButtons[n];
            button.min_x = region.GetMinX();
            button.max_x = region.GetMaxX();
            button.min_y = region.GetMinY();
            button.max_y = region.GetMaxY();
            for (int pad = 0; pad < 4; ++pad)
                if (region.GetPointerState(pad) == POINTER_BUTTON_HOVER) button.hovered = 1;
        }
        if (TLTextInstance* text = TextOf(scene->mOptionInstances[n], "off", "option"))
            CopyUtf8(text->GetString(), button.label, sizeof button.label);
    }
    ReadPausePresentation(scene, s);
}

void ReadOptions(OptionsScene* scene, ChargedMenuSnapshot& s)
{
    s.menu = CHARGED_MENU_OPTIONS;
    s.ready = scene->mScenePhase == OptionsScene::PHASE_CHOOSING;
    s.leaving = scene->mScenePhase == OptionsScene::PHASE_EXITING_TO_OPTION
        || scene->mScenePhase == OptionsScene::PHASE_EXITING_TO_MAIN_MENU;
    s.controller = int(gFEControllerIndex);
}

// The game's own options, as its save holds them.
void ReadGameOptions(ChargedMenuSnapshot& s)
{
    GameInfoManager* info = GameInfoManager::Instance();
    if (!info) return;
    if (const AudioSettings* audio = info->GetAudioSettings()) {
        s.music_volume = audio->MusicVolume;
        s.effects_volume = audio->SFXVolume;
        s.voice_volume = audio->VoiceVolume;
    }
    const VisualSettings& visual = *info->GetVisualOptions();
    s.auto_zoom = visual.mIsAutoZoomCamera ? 1 : 0;
    s.zoom_level = std::clamp(int(4.0f * visual.mCameraZoomLevel + 0.5f), 0, 4);
}

// One of the host's commands, in the game's frame.
void Run(const ChargedMenuCommand& command)
{
    GameInfoManager* info = GameInfoManager::Instance();
    switch (command.kind) {
    case CHARGED_MENU_SOUND:
        if (command.which >= 0 && command.which < int(std::size(kMenuSounds)))
            FEAudio::PlayAnimAudioEvent(kMenuSounds[command.which], 0, 0, 1);
        break;
    case CHARGED_MENU_VOICE:
        // As the game's Audio Options let the voices' volume be heard.
        if (TopPauseMenu()) FEAudio::PlaySound(1, 0x270203ED, 0, 0);
        else FEAudio::PlayAnimAudioEvent(0x1F824C84, 0, 0, 1);
        break;
    case CHARGED_MENU_VOLUME:
        if (AudioSettings* audio = info ? info->GetAudioSettings() : nullptr) {
            const int value = std::clamp(command.value, 0, 10);
            switch (command.which) {
            case 0: audio->MusicVolume = value, audio->ApplyMusicVolume(); break;
            case 1: audio->SFXVolume = value, audio->ApplySFXVolume(); break;
            case 2: audio->VoiceVolume = value, audio->ApplyVoiceVolume(); break;
            default: break;
            }
        }
        break;
    case CHARGED_MENU_AUTO_ZOOM:
        if (info) info->mUserInfo.mVisualOptions.mIsAutoZoomCamera = command.value != 0;
        break;
    case CHARGED_MENU_ZOOM:
        if (info) info->mUserInfo.mVisualOptions.mCameraZoomLevel = float(std::clamp(command.value, 0, 4)) / 4.0f;
        break;
    case CHARGED_MENU_SAVE:
        SaveLoad::StartSave(false);
        break;
    case CHARGED_MENU_RESUME:
        // As its Resume: only from the menu at rest.
        if (PauseMenuScene* scene = ShownPauseMenu();
            scene && scene->mInitialized && !scene->mIsInTransition && !scene->mSelectionMade)
            scene->OnSelectRESUME(nullptr);
        break;
    case CHARGED_MENU_PRESS:
        if (OptionsScene* scene = ShownOptions(); scene && scene->mScenePhase == OptionsScene::PHASE_CHOOSING
            && command.which >= 0 && command.which < 3) {
            scene->OnButtonPointerPress(int(gFEControllerIndex),
                                        reinterpret_cast<void*>(static_cast<std::intptr_t>(command.which)));
        }
        break;
    case CHARGED_MENU_BACK:
        // As its BACK (OptionsScene::Update): out to the main menu.
        if (OptionsScene* scene = ShownOptions(); scene && scene->mScenePhase == OptionsScene::PHASE_CHOOSING) {
            scene->mScenePhase = OptionsScene::PHASE_EXITING_TO_MAIN_MENU;
            if (SHNavigation* navigation = GetNavigationScene()) navigation->HideButtons();
            scene->mPresentation->SetActiveSlide("out", true);
            scene->mPresentation->Update(0.0f);
        }
        break;
    case CHARGED_MENU_WII_ONLINE:
        // As the globe's press (SHMainMenu::SelectItem: its checks, the
        // save's load, the Mii selection and the login follow).
        if (SHMainMenu* menu = TopMainMenu(); menu && !menu->mItemSelected && !menu->mWaitingForLoad) {
            sPassMainOnline = true;
            menu->SelectItem(gFEControllerIndex,
                             reinterpret_cast<void*>(static_cast<std::intptr_t>(MAIN_MENU_ONLINE)));
            sPassMainOnline = false;
        }
        break;
    case CHARGED_MENU_CURSORS:
        sCursors = command.value != 0;
        break;
    case CHARGED_MENU_FORFEIT:
        // As the game leaves an online match itself (the disc lid opened):
        // the loss pending, the connections closed; both games then find the
        // connection gone and go back to their menus.
        if (NetworkMatch() == CHARGED_NETWORK_ONLINE && !sForfeiting) {
            sForfeiting = true;
            g_pNetworkSession->DisconnectOnlineMatch();
        }
        break;
    default: break;
    }
}
} // namespace

#if defined(MSCHARGED_TWEAKS)
bool mscharged_tweak_menu_press_taken(int menu, int button, int pad)
{
    if (!Replaced()) return false;
    // The pause menu's Audio and Visual Options: Recharged's Settings on its
    // Sound and Display instead of the game's pages. The main menu's ONLINE:
    // Recharged's online menu (its WII ONLINE presses it again, let through).
    const bool pause = menu == CHARGED_MENU_PAUSE && (button == 2 || button == 3);
    const bool online = menu == CHARGED_MENU_MAIN && !sPassMainOnline;
    if (!pause && !online) return false;
    ++sTaken;
    sTakenMenu = menu;
    sTakenButton = button;
    sTakenPad = pad;
    return true;
}

bool mscharged_tweak_menu_scene_hidden(unsigned long scene)
{
    static const unsigned long pauseMenu = nlStringLowerHash("art/fe/pausemenu_v3.fen");
    static const unsigned long options = nlStringLowerHash("art/fe/options_main_menu.fen");
    return (scene == pauseMenu || scene == options) && Replaced();
}

void mscharged_tweak_menus_frame(void)
{
    // The front end's Options: once Recharged's menu stands in for them, the
    // navigation's own BACK in the corner goes (the menu has its own).
    static const OptionsScene* navigationHidden = nullptr;
    OptionsScene* options = ShownOptions();
    if (options && options != navigationHidden && options->mScenePhase == OptionsScene::PHASE_CHOOSING) {
        if (SHNavigation* navigation = GetNavigationScene()) navigation->HideButtons();
        navigationHidden = options;
    } else if (!options) navigationHidden = nullptr;
    // The main menu's own BACK (the navigation's) under Recharged's pages:
    // hidden while they show, back as the main menu sets it once they go.
    static bool mainBackHidden = false;
    SHMainMenu* mainMenu = TopMainMenu();
    if (mainMenu && sCursors && !mainBackHidden) {
        if (SHNavigation* navigation = GetNavigationScene()) navigation->HideButtons();
        mainBackHidden = true;
    } else if (mainBackHidden && !sCursors) {
        if (mainMenu && !mainMenu->mItemSelected)
            if (SHNavigation* navigation = GetNavigationScene()) navigation->SetButtons(NAVIGATION_BUTTON_BACK, true);
        mainBackHidden = false;
    } else if (!mainMenu) mainBackHidden = false;
    // Commands queued by a command's own effect wait for the next frame.
    ChargedMenuCommand commands[16];
    const int count = sCommandCount;
    std::copy(sCommands, sCommands + count, commands);
    sCommandCount = 0;
    for (int n = 0; n < count; ++n) Run(commands[n]);
}
#endif

bool ChargedMenuAnswersPopup(int popup_type)
{
    // The connection the player closed by forfeiting: back to the menus.
    return sForfeiting
        && (popup_type == POPUP_NETWORK_CONNECTION_LOST || popup_type == POPUP_NETWORK_SYNC_ERROR
            || popup_type == POPUP_NETWORK_OVERFLOW);
}

extern "C" __attribute__((visibility("default"))) bool charged_menu_texture(unsigned texture, int* width,
                                                                            int* height, int* format,
                                                                            unsigned char* rgba, unsigned capacity)
{
    if (!width || !height || !format) return false;
    const PlatTexture* source = glx_GetTex(texture);
    if (!source || !source->m_SwizzledData || source->m_Width == 0 || source->m_Height == 0) return false;
    *width = source->m_Width;
    *height = source->m_Height;
    *format = int(source->m_Format);
    if (!rgba) return true;
    const std::size_t need = std::size_t(source->m_Width) * source->m_Height * 4;
    if (capacity < need) return false;
    std::memset(rgba, 0, need);
    const int entries = source->m_PaletteData && source->m_NativePaletteBytes >= 2
        ? std::min<int>(source->m_nPaletteEntries, int(source->m_NativePaletteBytes / 2)) : 0;
    return DecodeGx(static_cast<const std::uint8_t*>(source->m_SwizzledData), source->m_NativeDataBytes,
                    source->m_Format, source->m_Width, source->m_Height,
                    reinterpret_cast<const std::uint8_t*>(source->m_PaletteData), entries, rgba);
}


extern "C" __attribute__((visibility("default"))) bool charged_menu_command(const ChargedMenuCommand* command,
                                                                            unsigned size)
{
    if (!command || size != sizeof(ChargedMenuCommand) || !Replaced()) return false;
    // In a match against another machine nothing the game shares may change
    // (its sounds draw the shared random numbers): only the cursors' drawing
    // and the forfeit, refused before they take a place in the queue.
    if (NetworkMatch() != CHARGED_NETWORK_NONE && command->kind != CHARGED_MENU_CURSORS
        && command->kind != CHARGED_MENU_FORFEIT)
        return false;
    if (sCommandCount >= int(sizeof sCommands / sizeof sCommands[0])) return false;
    sCommands[sCommandCount++] = *command;
    return true;
}

extern "C" __attribute__((visibility("default"))) bool charged_menu_snapshot(ChargedMenuSnapshot* snapshot,
                                                                             unsigned size)
{
    if (!snapshot || size != sizeof(ChargedMenuSnapshot) || !Replaced()) return false;
    ChargedMenuSnapshot s{};
    s.taken = sTaken;
    s.taken_menu = sTakenMenu;
    s.network_match = NetworkMatch();
    if (!g_pGame) sForfeiting = false;
    s.forfeiting = sForfeiting ? 1 : 0;
    s.taken_button = sTakenButton;
    s.taken_pad = sTakenPad;
    ReadGameOptions(s);
    s.front_end = g_pGame == nullptr;
    // As GetPointerPosition (feDPD.cpp) spans the picture: 854 units wide in
    // widescreen; the screen's height (also without a menu: the host's own).
    if (const gl_ScreenInfo* screen = glGetScreenInfo()) {
        s.fe_height = float(screen->ScreenHeight);
    }
    // The cursors' quads of the frame just drawn, once (also under no menu
    // of Recharged's: the host keeps each player's hand for its pages).
    s.quad_count = sQuadCount;
    std::copy(sQuads, sQuads + sQuadCount, s.quads);
    sQuadCount = 0;
    for (int pad = 0; pad < 4; ++pad) {
        s.pointer_x[pad] = gFEPointerPositions[pad].x;
        s.pointer_y[pad] = gFEPointerPositions[pad].y;
    }
    // Not before its scene is loaded and created (its buttons' instances are
    // set then); a quit under way turns the picture black (the game returns
    // to its menus).
    if (PauseMenuScene* scene = ShownPauseMenu()) {
        ReadPause(scene, s);
    } else if (OptionsScene* options = ShownOptions()) ReadOptions(options, s);
    // Else the header picture by its name, where the game has it loaded (the
    // host places it as the pause menu's): Recharged's pages over the main
    // menu, a question.
    // (The front end's texture handles are the textures' names.)
    if (!s.header_texture && glx_GetTex(kHeaderPicture)) s.header_texture = unsigned(kHeaderPicture);
    *snapshot = s;
    return true;
}
