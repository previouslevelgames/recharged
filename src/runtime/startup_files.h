#pragma once
#include <string>

namespace mscharged
{
void ResetStartupFiles();
void VerifyStartupFileReads();
std::string VerifyStartupWholeFileLoads();
std::string StartupFileSummary();
}
