#pragma once

#include "platform/frontend_package.h"

#include <cstddef>
#include <span>

// The front end's logo reads "Mario Strikers RECHARGED" (issue #63). The
// logo's word is a group of seven letter components (C, h, a, r, g, e, d),
// each an image of the letter with its own looping scale animation. The
// title screen, the main menu and Domination's captain select draw it, on
// every release of the game. Copies of the game's own R and E (letter image,
// slide and animation) go in front of C, spaced like the word's letters. The
// longer word is drawn at 90 % and moved right, to sit on the logo's black
// backdrop; the lightning around it follows and a copy of its left end frames
// the R, and the main menu's dark plate behind the word widens to the new word.
namespace mscharged::platform
{
// Rewrites a front-end scene file (the Wii FENL bytes read from the disc)
// with RECHARGED for each "charged" letter group in it. Returns the number of
// groups changed, with the new scene in `out`; 0 (and `out` empty) when the
// scene has no such group or its layout is not the one known.
int RechargeLogo(std::span<const std::byte> scene, FrontendPackageVector<std::byte>& out);
}
