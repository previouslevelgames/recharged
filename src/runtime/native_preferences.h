#pragma once
#include "resources/native_preferences.h"
#include <filesystem>
#include <memory>

namespace mscharged
{
enum class NativePreferencesScope { NativePreferences, OriginalGameSave };
enum class NativePreferencesState { Idle, DirectoryPending, FilePending, Ready, Missing, Failed, Cancelled };
struct NativePreferencesStatus
{
    NativePreferencesScope scope = NativePreferencesScope::NativePreferences;
    NativePreferencesState state = NativePreferencesState::Idle;
    bool save_enabled = true, in_operation = false, save_error = false;
    bool host_pending = false, preferences_loaded = false;
    // A successful preferences write never completes Strikers2/banner loading.
    bool original_normal_save_loaded = false, full_save_complete = false;
    unsigned directory_callbacks = 0, file_callbacks = 0;
};
// A dedicated host preferences file, NOT Strikers2, Online, a Wii banner or the
// launcher INI. Path must be absolute and its parent directory must exist.
// All public calls and destruction belong to the constructing thread. Workers
// access only immutable values/path copies; callbacks run exclusively in Poll.
// Cancel/destruction drain workers and discard an unpublished temporary file.
// StartLoad must observe the file (or its absence) before StartSave. Replacement
// compares exact prior bytes before rename; external edits require a new load.
class NativePreferences
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    using Handle = resources::NativePreferencesValues::Handle;
    explicit NativePreferences(std::filesystem::path);
    ~NativePreferences();
    NativePreferences(const NativePreferences&)=delete;
    NativePreferences& operator=(const NativePreferences&)=delete;
    void StartLoad();
    void StartSave(const resources::NativePreferencesValues&);
    void Poll();
    // Executes the original cancel flag changes after draining native work;
    // saving is disabled for this owner thereafter, as in CancelSave.
    void Cancel();
    Handle Current() const;
    NativePreferencesStatus Status() const;
    // The caller must opt into this narrow authority explicitly. Original game
    // scope throws. Admission precedes the directory callback, so host_pending
    // may be true while this exact source predicate is still false.
    bool DepartureBlocked(NativePreferencesScope expected_scope) const;
    void RethrowFailure() const;
};
}
