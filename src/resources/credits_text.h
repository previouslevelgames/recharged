#pragma once
#include "resources/binary_reader.h"
#include <memory>
#include <string>
#include <vector>
namespace mscharged::resources
{
// Source SimpleParser token order with Credits' exact separators and explicit
// AdvanceLine after each token, including its skipped one-byte final line. CopyCreditLine's @/+ and63-unit rules are a
// later scene operation, not changed by this reader. No native pointer survives.
struct CreditsText
{
    using Handle=std::shared_ptr<const CreditsText>;
    std::vector<std::string> tokens;
};
CreditsText::Handle ReadCreditsText(Bytes);
}
