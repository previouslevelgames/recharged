#pragma once

#include "NL/nlFileGC.h"
#include <aurora/dvd.h>
#include <dolphin/dvd.h>
#include <stdexcept>
#include <string>

namespace mscharged::test
{
// Fixture ownership only: copy the real path after a successful mount.
inline std::string dvdTestMediumPath;
inline void ConfigureDVDTestMedium(const char* mountedDiscPath)
{
    if (!mountedDiscPath || !*mountedDiscPath || DVDGetDriveStatus() != DVD_STATE_END)
        throw std::logic_error("DVD fixture requires an actually mounted idle medium");
    dvdTestMediumPath = mountedDiscPath;
}

// Call explicitly after checking the failed/cancelled source owner's result,
// draining all its reads, and closing every borrowed file/overlay handle.
// No destructor, callback or production fault-recovery hook calls this.
inline void FinishDVDTestFaultCase(bool expectFatal)
{
    if (dvdTestMediumPath.empty() || nlAsyncReadsPending(nullptr))
        throw std::logic_error("DVD fixture remount requires configured path and no pending NL reads");
    const auto expected = expectFatal ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
    if (DVDGetDriveStatus() != expected)
        throw std::runtime_error("DVD fault case did not preserve its expected physical drive status");
    if (!expectFatal) return;
    aurora_dvd_close();
    if (DVDGetDriveStatus() != DVD_STATE_NO_DISK)
        throw std::runtime_error("DVD fixture close did not retire the failed medium");
    if (!aurora_dvd_open(dvdTestMediumPath.c_str()) || DVDGetDriveStatus() != DVD_STATE_END)
        throw std::runtime_error("DVD fixture could not establish a fresh actual media lifetime");
}
}
