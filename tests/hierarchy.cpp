#include "resources/hierarchy.h"
#include "resources/chunk_reader.h"
#include "runtime/hierarchy_assets.h"
#include "Game/SHierarchy.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>

namespace allocation_probe
{
std::atomic<std::size_t> live = 0;
thread_local std::size_t remaining = SIZE_MAX;
}
// Exercise every host allocation failure without changing the production owner.
// Counts include both retained control blocks and temporary decoded vectors.
void* operator new(std::size_t size)
{
    if (allocation_probe::remaining != SIZE_MAX)
    {
        if (!allocation_probe::remaining) throw std::bad_alloc();
        --allocation_probe::remaining;
    }
    if (void* p = std::malloc(size ? size : 1)) { ++allocation_probe::live; return p; }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { if(p) { --allocation_probe::live; std::free(p); } }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

using namespace mscharged;
using namespace mscharged::resources;
namespace
{
unsigned checks = 0;
void Check(bool good, const char* message) { ++checks; if (!good) throw std::runtime_error(message); }
template<class F> void Reject(F action)
{ ++checks; try { action(); } catch (const std::exception&) { return; } throw std::runtime_error("Malformed hierarchy accepted"); }
using Blob = std::vector<std::uint8_t>;
void Word(Blob& data, unsigned value) { for (int shift : {24, 16, 8, 0}) data.push_back(value >> shift); }
void Set(Blob& data, std::size_t at, unsigned value)
{ for (int shift : {24, 16, 8, 0}) data.at(at++) = value >> shift; }
std::size_t Pad(std::size_t size, std::size_t alignment) { return (size + alignment - 1) / alignment * alignment; }
void Chunk(Blob& file, unsigned id, const Blob& bytes, unsigned exponent)
{
    const auto at = file.size(); Word(file, id | (exponent << 24)); Word(file, 0);
    file.resize(Pad(file.size(), std::size_t(1) << exponent), 0xa5);
    file.insert(file.end(), bytes.begin(), bytes.end()); Set(file, at + 4, file.size() - at - 8);
    file.resize(Pad(file.size(), 4), 0x5a);
}
const std::vector<int> Parents{-1, 0, 1, 2, 1, 0, 5, 5, 0};
Blob Fixture(const std::vector<int>& parents = Parents, unsigned exponent = 2, const std::string& name = "fixture")
{
    const auto n = parents.size();
    std::vector<Blob> records(11);
    // Most serialized header words and both loader scratch tables deliberately
    // resemble impossible host addresses; Initialize overwrites those words.
    for (unsigned i = 0; i < 13; ++i) Word(records[0], 0xf0a0a000 + i * 4);
    Set(records[0], 4, 0xdedcba98); Set(records[0], 8, n);
    Set(records[0], 36, n > 2 ? 1 : -1); Set(records[0], 40, n > 2 ? 2 : -1);
    records[1].assign(name.begin(), name.end()); records[1].push_back(0);
    records[1].resize(Pad(records[1].size(), 4));
    for (std::size_t i = 0; i < n; ++i)
    {
        Word(records[2], 0x12340000 + i);
        Word(records[3], parents[i]);
        Word(records[4], std::count(parents.begin(), parents.end(), int(i)));
        Word(records[5], 0xdead0000 + i); Word(records[6], 0xa5a50000 + i);
        for (std::size_t child = 0; child < n; ++child)
            if (parents[child] == int(i)) Word(records[7], child);
        Word(records[8], i); // Identity is a valid mirror mapping.
        Word(records[9], i == 0 ? 0x80000000 : std::bit_cast<unsigned>(float(i) + .125f));
        Word(records[9], std::bit_cast<unsigned>(-float(i) - .25f));
        Word(records[9], i == 0 ? 1 : std::bit_cast<unsigned>(float(i) + .5f));
        records[10].push_back(i % 3 == 0 ? 255 : 0);
    }
    Blob file; Word(file, 0x80018000 | (exponent << 24)); Word(file, 0);
    file.resize(Pad(file.size(), std::size_t(1) << exponent), 0xcc);
    const unsigned ids[]{0x18001,0x18002,0x18003,0x18009,0x18004,0x18005,0x18006,0x18007,0x18008,0x18010,0x18011};
    for (unsigned i = 0; i < 11; ++i) Chunk(file, ids[i], records[i], (exponent + i) % 6);
    Set(file, 4, file.size() - 8); return file;
}
std::size_t Header(const Blob& file, unsigned id)
{
    const auto root = ReadChunk(file, 0, file.size());
    const auto end = std::size_t(root.payload.data() - file.data()) + root.payload.size();
    for (auto offset = std::size_t(root.payload.data() - file.data()); offset < end;)
    {
        const auto chunk = ReadChunk(file, offset, end);
        if (chunk.id == id) return offset;
        offset = chunk.next;
    }
    throw std::runtime_error("Test chunk is missing");
}
std::size_t Payload(const Blob& file, unsigned id)
{ const auto c = ReadChunk(file, Header(file,id), file.size()); return c.payload.data() - file.data(); }
void CheckNative(const HierarchyAsset& asset, const std::vector<int>& parents)
{
    const auto& h = asset.Data();
    Check(h.GetNumNodes() == int(parents.size()), "Native node count differs");
    std::vector<int> depths(parents.size()), stack;
    for (std::size_t i = 1; i < parents.size(); ++i) depths[i] = depths[parents[i]] + 1;
    for (std::size_t i = 0; i < parents.size(); ++i)
    {
        Check(h.GetParent(i) == parents[i], "Parent index differs");
        // This oracle uses adjacent node depths, independently of the original
        // recursive BuildPushPopFlags algorithm and serialized scratch values.
        const int expected = i + 1 < parents.size() ?
            (parents[i + 1] == int(i) ? 1 : depths[i + 1] - depths[i]) : 0;
        if(h.GetPushPop(i)!=expected)throw std::runtime_error("Push/pop node "+std::to_string(i)+" actual "+std::to_string(h.GetPushPop(i))+" expected "+std::to_string(expected));
        Check(h.GetPushPop(i) == expected, "Original push/pop differs from depth oracle");
        const int parent = stack.empty() ? -1 : stack.back();
        Check(parent == parents[i], "Original flags choose the wrong matrix parent");
        Check(stack.size() <= MaximumHierarchyDepth, "Original matrix scale index would overflow");
        const int delta = h.GetPushPop(i);
        if (delta > 0) stack.push_back(i);
        else if (delta < 0) stack.resize(stack.size() - unsigned(-delta));
        Check(h.GetNodeID(i) == 0x12340000 + i, "Node ID bits changed");
        Check(h.GetNodeIndexByID(h.GetNodeID(i)) == int(i), "Original node lookup differs");
        Check(h.GetMirroredNode(i) == int(i), "Mirror index differs");
        Check(h.PreserveBoneLength(i) == (i % 3 == 0), "Original nonzero length flag differs");
        Check(h.m_pPreserveBoneLength[i] == (i % 3 == 0 ? 255 : 0), "Length flag byte changed");
        const auto& v = h.GetTranslationOffset(i);
        Check(std::bit_cast<unsigned>(v.x) == (i == 0 ? 0x80000000 : std::bit_cast<unsigned>(float(i) + .125f)), "Translation X bits changed");
        Check(std::bit_cast<unsigned>(v.z) == (i == 0 ? 1 : std::bit_cast<unsigned>(float(i) + .5f)), "Translation Z bits changed");
        int j = 0;
        for (std::size_t child = 0; child < parents.size(); ++child)
            if (parents[child] == int(i)) Check(h.GetChild(i, j++) == int(child), "Child order changed");
        Check(h.GetNumChildren(i) == j, "Child count differs");
        Check(bool(h.m_pChildren[i]) == bool(j), "Empty original child table must be null");
    }
    Check(h.GetNodeIndexByID(0xeeeeeeee) == -1, "Missing node lookup succeeded");
}
void Valid()
{
    for (unsigned exponent = 0; exponent < 6; ++exponent)
    {
        auto bytes = Fixture(Parents, exponent);
        const auto copy = bytes;
        const auto decoded = ReadHierarchy(bytes);
        auto owned = HierarchyAsset::Decode(bytes);
        Check(decoded.name == "fixture" && decoded.hash == 0xdedcba98, "Header metadata differs");
        Check(decoded.maximum_depth == 3 && owned->MaximumDepth() == 3, "Maximum depth differs");
        Check(owned->Data().GetPelvisNodeIndex() == 1 && owned->Data().GetSpineNodeIndex() == 2, "Special nodes differ");
        Check(owned->Data().GetHashID() == decoded.hash, "Native identifier hash differs");
        Check(std::string(owned->Data().m_szName) == decoded.name, "Native name differs");
        Check(bytes == copy, "Native construction modified disk bytes");
        bytes.assign(bytes.size(), 0xcc); bytes.clear(); bytes.shrink_to_fit();
        CheckNative(*owned, Parents);
        const auto* address = &owned->Data();
        auto retained = owned; std::weak_ptr<const HierarchyAsset> weak = owned; owned.reset();
        Check(!weak.expired() && &retained->Data() == address, "Retained native identity moved");
        std::thread reader([retained, address] { if (&retained->Data() != address || retained->Data().GetNumNodes() != int(Parents.size())) std::terminate(); });
        reader.join(); retained.reset(); Check(weak.expired(), "Owner outlived its final retained handle");
    }
    for (unsigned count : {1u, 2u, 32u})
    {
        std::vector<int> chain; for (unsigned i = 0; i < count; ++i) chain.push_back(int(i) - 1);
        const auto asset = HierarchyAsset::Decode(Fixture(chain));
        Check(asset->MaximumDepth() == count - 1, "Boundary chain depth differs"); CheckNative(*asset, chain);
    }
    std::vector<int> wide(MaximumHierarchyNodes, 0); wide[0] = -1;
    const auto maximum = HierarchyAsset::Decode(Fixture(wide));
    Check(maximum->Data().GetNumChildren(0) == int(MaximumHierarchyNodes - 1), "Maximum wide hierarchy rejected");
    Check(maximum->Data().GetChild(0, MaximumHierarchyNodes - 2) == int(MaximumHierarchyNodes - 1), "Wide final child changed");
    // Original lookup is first-match and mirror access is a direct table read.
    // Preserve those contracts instead of inventing uniqueness/involution rules.
    auto aliases = Fixture(); Set(aliases, Payload(aliases,0x18003)+4, 0x12340000);
    Set(aliases, Payload(aliases,0x18008)+4, 0);
    const auto alias = HierarchyAsset::Decode(aliases);
    Check(alias->Data().GetNodeIndexByID(0x12340000) == 0, "Duplicate node hash no longer uses first match");
    Check(alias->Data().GetMirroredNode(1) == 0, "Authored mirror mapping changed");
    const auto long_name=HierarchyAsset::Decode(Fixture(Parents,5,std::string(255,'x')));
    Check(std::string(long_name->Data().m_szName).size()==255,"Maximum supported name rejected");
    auto boundaries=Fixture();Set(boundaries,Payload(boundaries,0x18001)+36,Parents.size()-1);
    Set(boundaries,Payload(boundaries,0x18001)+40,0xffffffff);
    Set(boundaries,Payload(boundaries,0x18010),std::bit_cast<unsigned>(1e7f));
    Set(boundaries,Payload(boundaries,0x18010)+4,std::bit_cast<unsigned>(-1e7f));
    const auto boundary=HierarchyAsset::Decode(boundaries);
    Check(boundary->Data().GetPelvisNodeIndex()==8&&boundary->Data().GetSpineNodeIndex()==-1,"Special node boundary changed");
    Check(boundary->Data().GetTranslationOffset(0).x==1e7f&&boundary->Data().GetTranslationOffset(0).y==-1e7f,"Finite translation boundary changed");
    static_assert(sizeof(*boundary->Data().m_pChildren) == sizeof(int*));
    Check(boundary->Data().m_pChildren[0][0]==1&&boundary->Data().m_pChildren[1][0]==2,
          "Native-width child pointer table differs");
}
void Invalid()
{
    const auto good = Fixture();
    for (std::size_t size = 0; size < good.size(); ++size)
        Reject([&] { HierarchyAsset::Decode(Bytes(good).first(size)); });
    for (unsigned count : {0u, MaximumHierarchyNodes + 1, 0xffffffffu})
    { auto bad = good; Set(bad, Payload(bad,0x18001)+8, count); Reject([&] { ReadHierarchy(bad); }); }
    for (unsigned at : {36u,40u}) for (unsigned value : {9u,0xfffffffeu,0x7fffffffu})
    { auto bad = good; Set(bad, Payload(bad,0x18001)+at, value); Reject([&] { ReadHierarchy(bad); }); }
    for (unsigned id : {0x18001u,0x18002u,0x18003u,0x18009u,0x18004u,0x18005u,0x18006u,0x18007u,0x18008u,0x18010u,0x18011u})
    {
        auto bad=good; Set(bad,Header(bad,id),0x19999); Reject([&] { ReadHierarchy(bad); });
        bad=good; Set(bad,Header(bad,id)+4,0xffffffff); Reject([&] { ReadHierarchy(bad); });
        bad=good; const auto at=Header(bad,id); Set(bad,at+4,U32(bad,at+4)+4); Reject([&] { ReadHierarchy(bad); });
        bad=good; Set(bad,Header(bad,id),id|(6u<<24)); Reject([&] { ReadHierarchy(bad); });
    }
    for (auto [node,parent] : {std::pair{0u,0u},{1u,1u},{1u,0xffffffffu},{1u,9u},{4u,0u}})
    { auto bad=good;Set(bad,Payload(bad,0x18009)+node*4,parent);Reject([&]{ReadHierarchy(bad);}); }
    for (auto [word,value] : {std::pair{0u,0u},{0u,9u},{1u,1u},{1u,8u}})
    { auto bad=good;Set(bad,Payload(bad,0x18007)+word*4,value);Reject([&]{ReadHierarchy(bad);}); }
    for (unsigned count : {0u,8u,9u,0xffffffffu})
    { auto bad=good;Set(bad,Payload(bad,0x18004),count);Reject([&]{ReadHierarchy(bad);}); }
    for (unsigned mirror : {9u,0xffffffffu})
    { auto bad=good;Set(bad,Payload(bad,0x18008),mirror);Reject([&]{ReadHierarchy(bad);}); }
    for (unsigned value : {0x7f800000u,0xff800000u,0x7fc12345u,0x7f7fffffu})
    { auto bad=good;Set(bad,Payload(bad,0x18010),value);Reject([&]{ReadHierarchy(bad);}); }
    for (unsigned value : {0u,1u,255u})
    { auto bad=good;bad[Payload(bad,0x18002)]=value;Reject([&]{ReadHierarchy(bad);}); }
    auto bad=good;bad[Payload(bad,0x18002)+7]='x';Reject([&]{ReadHierarchy(bad);});
    Reject([]{ReadHierarchy(Fixture(Parents,2,std::string(256,'x')));});
    bad=Fixture(Parents,2,"a");bad[Payload(bad,0x18002)+3]=1;Reject([&]{ReadHierarchy(bad);});
    bad=good;Set(bad,0,0x18000);Reject([&]{ReadHierarchy(bad);});
    bad=good;bad.push_back(0);Reject([&]{ReadHierarchy(bad);});
    bad=good;Chunk(bad,0x18011,{},0);Set(bad,4,bad.size()-8);Reject([&]{ReadHierarchy(bad);});
    Reject([]{ReadHierarchy(Blob(MaximumAssetBytes+1));});
    Reject([]{ReadHierarchy(Fixture({-1,0,0,1}));}); // Reciprocal tree, wrong traversal order.
    std::vector<int> deep;for(int i=0;i<33;++i)deep.push_back(i-1);
    Reject([&]{HierarchyAsset::Decode(Fixture(deep));});
    for (unsigned i=0;i<100;++i)
    {
        auto damaged=good;Set(damaged,Payload(damaged,0x18010)+12*8+8,0x7fc00000);
        Reject([&]{HierarchyAsset::Decode(damaged);});
        auto recovered=HierarchyAsset::Decode(good);Check(recovered->Data().GetNumNodes()==9,"Failed decode poisoned later ownership");
    }
}
void AllocationFailures()
{
    const auto bytes=Fixture();
    { const auto warm=HierarchyAsset::Decode(bytes); }
    const auto before=allocation_probe::live.load();
    unsigned failures=0;
    for(std::size_t at=0;at<100;++at)
    {
        bool failed=false;
        allocation_probe::remaining=at;
        try { const auto owned=HierarchyAsset::Decode(bytes); }
        catch(const std::bad_alloc&) { failed=true; }
        allocation_probe::remaining=SIZE_MAX;
        Check(allocation_probe::live==before,"Failed/successful hierarchy construction leaked host storage");
        if(!failed){Check(failures>=20,"Allocation fault probe missed native ownership stages");return;}
        ++failures;
    }
    throw std::runtime_error("Allocation fault probe never reached successful construction");
}
Blob Read(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary);
    if(!file)throw std::runtime_error("Cannot open hierarchy fixture");
    return Blob(std::istreambuf_iterator<char>(file),{});
}
void Inspect(const std::filesystem::path& path)
{
    auto bytes=Read(path);auto asset=HierarchyAsset::Decode(bytes);const auto& h=asset->Data();
    bytes.assign(bytes.size(),0xcc);bytes.clear();bytes.shrink_to_fit();
    std::cout<<"{\"name\":\"";
    for(const char* p=h.m_szName;*p;++p){if(*p=='"'||*p=='\\')std::cout<<'\\';std::cout<<*p;}
    std::cout<<"\",\"hash\":"<<h.GetHashID()<<",\"pelvis\":"<<h.GetPelvisNodeIndex()
        <<",\"spine\":"<<h.GetSpineNodeIndex()<<",\"depth\":"<<asset->MaximumDepth()<<",\"nodes\":[";
    for(int i=0;i<h.GetNumNodes();++i)
    {
        if(i)std::cout<<',';const auto& t=h.GetTranslationOffset(i);
        std::cout<<"{\"id\":"<<h.GetNodeID(i)<<",\"parent\":"<<h.GetParent(i)<<",\"mirror\":"<<h.GetMirroredNode(i)
            <<",\"push_pop\":"<<h.GetPushPop(i)<<",\"preserve\":"<<unsigned(h.m_pPreserveBoneLength[i])
            <<",\"translation_bits\":["<<std::bit_cast<unsigned>(t.x)<<','<<std::bit_cast<unsigned>(t.y)<<','<<std::bit_cast<unsigned>(t.z)
            <<"],\"children\":[";
        for(int j=0;j<h.GetNumChildren(i);++j){if(j)std::cout<<',';std::cout<<h.GetChild(i,j);}
        std::cout<<"]}";
    }
    std::cout<<"]}\n";
}
}
int main(int argc,char** argv)
{
    try
    {
        if(argc==3&&std::string(argv[1])=="--inspect"){Inspect(argv[2]);return 0;}
        if(argc!=1)throw std::runtime_error("Usage: hierarchy_tests [--inspect FILE]");
        Valid();Invalid();AllocationFailures();std::cout<<checks<<" hierarchy checks passed\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
