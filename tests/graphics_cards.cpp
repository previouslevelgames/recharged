// The launcher's list of graphics cards (Advanced -> Graphics card): whatever
// the PC has, even none (a CI runner), each entry is a real PCI ID with a name,
// listed once, and round-trips through the display.gpu setting.
#include "platform/graphics_cards.h"

#include <cstdio>
#include <stdexcept>

int main()
{
    try
    {
        auto require = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
        const auto cards = mscharged::platform::ListGraphicsCards();
        for (std::size_t i = 0; i < cards.size(); ++i)
        {
            const auto& card = cards[i];
            require(card.id.vendor != 0 && card.id.vendor <= 0xFFFF && card.id.device <= 0xFFFF, "A PCI ID");
            require(!card.name.empty(), "A name");
            for (std::size_t j = 0; j < i; ++j) require(cards[j].id != card.id, "Each card once");
            const auto parsed = mscharged::ParseGraphicsCard(mscharged::GraphicsCardSetting(card.id));
            require(parsed && *parsed == card.id, "The setting names the card");
        }
        std::printf("%s\n", mscharged::platform::DescribeGraphicsCards(cards).c_str());
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
