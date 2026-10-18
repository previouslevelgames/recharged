#include "platform/wii_qsort.h"
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

// ChargedWii_qsort must order equal elements as the Wii's MSL heapsort does,
// whatever the host qsort does: online, every machine sorts the same arrays
// (patch 0754). glibc keeps two equal elements as they are; the Wii swaps them.
namespace {
unsigned checks = 0;
void Check(bool condition, const std::string& what)
{
    ++checks;
    if (!condition) throw std::runtime_error("Wii qsort changed: " + what);
}

struct Entry {
    int key;
    char name;
};

int CompareKeys(const void* left, const void* right)
{
    const int l = static_cast<const Entry*>(left)->key;
    const int r = static_cast<const Entry*>(right)->key;
    return l > r ? 1 : l < r ? -1 : 0;
}

std::string Sort(const int* keys, std::size_t count)
{
    Entry entries[8];
    for (std::size_t i = 0; i < count; ++i) entries[i] = {keys[i], static_cast<char>('A' + i)};
    ChargedWii_qsort(entries, count, sizeof(entries[0]), CompareKeys);
    std::string order;
    for (std::size_t i = 0; i < count; ++i) order += entries[i].name;
    return order;
}

void Observe(const char* expected, std::initializer_list<int> keys)
{
    Check(Sort(keys.begin(), keys.size()) == expected, expected);
}
}

int main()
{
    try {
        // Equal keys: the order the Wii gives (the draft's two tied teams first).
        Observe("BA", {0, 0});
        Observe("BCDA", {0, 0, 0, 0});
        Observe("BCDEFA", {0, 0, 0, 0, 0, 0});
        Observe("BA", {1, 0});
        Observe("BDCA", {1, 0, 1, 0});
        Observe("FBDCEA", {1, 0, 1, 0, 1, 0});
        Observe("BDCA", {3, 1, 2, 1});
        Observe("FBDCEA", {3, 1, 2, 1, 3, 0});
        Observe("DCBA", {5, 5, 1, 1});
        Observe("CDFEBA", {5, 5, 1, 1, 2, 2});
        // Distinct keys come out ascending, and short arrays stay as they are.
        Observe("HGFEDCBA", {7, 6, 5, 4, 3, 2, 1, 0});
        Observe("ABCDEFGH", {0, 1, 2, 3, 4, 5, 6, 7});
        Observe("A", {9});
        Observe("", {});
        std::cout << checks << " Wii qsort checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
