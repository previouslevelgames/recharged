#pragma once

// The trophy room in the game module (CHARGED_MENU_TROPHY_ROOM): from the
// game's online menu the front end's camera goes to the trophy of the game's
// cut online tournament and stays there in its loop, and goes back again.
// The online menu stays on the scene stack all the while. Its fe_presentation
// script does the work, as NetTournManager::TransitionOnlineMenuToTournament
// and the knockout screen's way back call it. In the game's frame, between
// the host's commands.

// Into the trophy room with the trophy `persona` (eCupPersona; another value:
// the Mushroom trophy). False unless the game's online menu is the top scene
// at rest, the presentation idle and the cut tournament inactive.
bool ChargedTrophyRoomEnter(int persona);
// Back to the online menu's camera; on the way in, once the camera is there.
void ChargedTrophyRoomLeave();
// After the game's frame: follows the presentation script, and cleans up when
// the online menu left the scene stack meanwhile.
void ChargedTrophyRoomFrame();
// Where it is (ChargedTrophyRoom).
int ChargedTrophyRoomState();
