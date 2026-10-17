#pragma once

namespace mscharged::platform
{
// macOS: Gatekeeper starts a downloaded app whose folder is still marked as
// quarantined from a temporary read-only copy (App Translocation), where
// Recharged.app cannot see the modules, settings and saves beside it. Then
// this offers to clear the mark on that folder and start the app again from
// it, and returns true: the program ends now with `code`. Otherwise false.
bool LeaveAppTranslocation(int& code);
} // namespace mscharged::platform
