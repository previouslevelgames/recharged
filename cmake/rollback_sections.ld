/* Rollback: the writable data of the original game's code, gathered in two
   sections of the game module (__rbk_data_start/end, __rbk_bss_start/end) so
   a rollback can save and restore exactly the globals of the original game.

   Every object of src/Game, src/NL and src/ode joins, except code whose
   globals other threads or host devices write: audio (the audio thread and
   the DSP), the network session and its transports, files, real time, the
   low-level graphics of NL and the pads (the host pad backend, the Wii Remote
   and Nunchuk state, flick detection and the front end's pad data: a replayed
   step reads none of them, the next new step reads the pads as they are).
   Joining is the safe side: only the simulation's steps run between a save
   and its restore, so a global they leave alone is restored to the very bytes
   it holds. Leaving out a global the steps change is what breaks a rollback:
   game code beyond "simulation" joins for that reason (Game/Render holds
   Presentation, NIS, stadium hazards and items as well as drawing).

   The module is compiled with -fdata-sections, so each global is an input
   section of its own; vtables and other relocated constants (.data.rel.ro*)
   stay where the default script puts them, in RELRO. Statements of a -T
   script are matched before the default script's, and INSERT keeps the
   default layout around them (GNU ld and lld).

   PE and Mach-O linkers take no script: tools/rollback_sections.py does the
   same for the Windows and macOS modules, with the lists of files it reads
   from rbk_data here (and checks against rbk_bss's: keep those two the
   same). */

SECTIONS
{
    rbk_data :
    {
        HIDDEN(__rbk_data_start = .);
        EXCLUDE_FILE(*src/Game/Audio/* *src/Game/Network[!IR]* *src/Game/FriendManager* *src/Game/LANLobby*
                     *src/Game/Net.cpp* *src/Game/NetTournManager* *src/Game/NetMessage*
                     *src/Game/Pad/FlickDetection* *src/Game/FE/feInput* *src/Game/RumbleActions*)
        *src/Game/*(.data .data.[!r]* .data.r[!e]* .data.rel .data.rel.local .data.rel.local.*
                    .data.rel.[!rl]*)
        EXCLUDE_FILE(*src/NL/gl/* *src/NL/glx/* *src/NL/gc/* *src/NL/nlFile* *src/NL/nlAsyncFileBuffer*
                     *src/NL/nlBundleFile* *src/NL/nlDebug* *src/NL/nlTicker* *src/NL/nlTime* *src/NL/nlPrint*
                     *src/NL/nlBufferedWriter* *src/NL/plat/nlFileCache* *src/NL/plat/nlFlash*
                     *src/NL/plat/PadBackend* *src/NL/plat/WiiFreestylePad* *src/NL/plat/*Socket*
                     *src/NL/plat/Transport*)
        *src/NL/*(.data .data.[!r]* .data.r[!e]* .data.rel .data.rel.local .data.rel.local.*
                  .data.rel.[!rl]*)
        *src/ode/*(.data .data.[!r]* .data.r[!e]* .data.rel .data.rel.local .data.rel.local.*
                   .data.rel.[!rl]*)
        HIDDEN(__rbk_data_end = .);
    }
    /* The audio code's globals, never restored but compared: a test whose
       steps change them touched audio state and is not rolled back. */
    rbk_audio_data :
    {
        HIDDEN(__rbk_audio_data_start = .);
        *src/Game/Audio/*(.data .data.[!r]* .data.r[!e]* .data.rel .data.rel.local .data.rel.local.*
                          .data.rel.[!rl]*)
        HIDDEN(__rbk_audio_data_end = .);
    }
}
INSERT AFTER .data;

SECTIONS
{
    rbk_bss (NOLOAD) :
    {
        HIDDEN(__rbk_bss_start = .);
        EXCLUDE_FILE(*src/Game/Audio/* *src/Game/Network[!IR]* *src/Game/FriendManager* *src/Game/LANLobby*
                     *src/Game/Net.cpp* *src/Game/NetTournManager* *src/Game/NetMessage*
                     *src/Game/Pad/FlickDetection* *src/Game/FE/feInput* *src/Game/RumbleActions*)
        *src/Game/*(.bss .bss.* COMMON)
        EXCLUDE_FILE(*src/NL/gl/* *src/NL/glx/* *src/NL/gc/* *src/NL/nlFile* *src/NL/nlAsyncFileBuffer*
                     *src/NL/nlBundleFile* *src/NL/nlDebug* *src/NL/nlTicker* *src/NL/nlTime* *src/NL/nlPrint*
                     *src/NL/nlBufferedWriter* *src/NL/plat/nlFileCache* *src/NL/plat/nlFlash*
                     *src/NL/plat/PadBackend* *src/NL/plat/WiiFreestylePad* *src/NL/plat/*Socket*
                     *src/NL/plat/Transport*)
        *src/NL/*(.bss .bss.* COMMON)
        *src/ode/*(.bss .bss.* COMMON)
        HIDDEN(__rbk_bss_end = .);
    }
    rbk_audio_bss (NOLOAD) :
    {
        HIDDEN(__rbk_audio_bss_start = .);
        *src/Game/Audio/*(.bss .bss.* COMMON)
        HIDDEN(__rbk_audio_bss_end = .);
    }
}
INSERT AFTER .bss;
