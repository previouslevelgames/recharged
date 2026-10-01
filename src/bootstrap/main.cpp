#include <iostream>

#include "NL/nlEndian.h"
#include "NL/nlMath.h"

int main()
{
    unsigned short swapped = 0;
    nlSwapEndian(0x1234, &swapped);
    unsigned int seed = 0x12345678;
    const unsigned int first = nlRandom(1000, &seed);
    if (swapped != 0x3412 || first != 896)
    {
        std::cerr << "Foundation check failed.\n";
        return 1;
    }

    std::cout << "mscharged-port development bootstrap\n"
              << "Decomp revision: " << MSCHARGED_DECOMP_REVISION << '\n'
              << "Native endian and random utility checks passed.\n"
              << "The decomp is incomplete; this executable does not run the game.\n";
    return 0;
}
