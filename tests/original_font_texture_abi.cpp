#include "NL/glx/glxTexture.h"
#include "platform/gx_texture_compat.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
unsigned checks = 0;
void Check(bool ok, const char* message)
{ ++checks; if (!ok) throw std::runtime_error(message); }
// Independent observation of the public SDK object's retained native fields.
// These records do not implement or intercept any GX service.
struct TextureObservation
{
    std::uint32_t mode0, mode1, image0, image3;
    const void* user_data;
    const void* data;
    std::uint32_t width, height, format, tlut, identity, version;
    std::uint8_t flags;
};
struct PaletteObservation
{
    std::uint32_t tlut, load;
    std::uint16_t entries;
    const void* data;
    std::uint32_t format, identity, version;
    std::uint8_t flags;
};
static_assert(sizeof(TextureObservation) <= sizeof(GXTexObj));
static_assert(sizeof(PaletteObservation) <= sizeof(GXTlutObj));
static_assert(offsetof(TextureObservation, data) == 24);
static_assert(offsetof(PaletteObservation, data) == 16);
struct alignas(void*) GuardedTexture
{
    std::array<std::uint64_t,2> before;
    alignas(void*) std::array<std::uint32_t,sizeof(GXTexObj)/4> bytes;
    std::array<std::uint64_t,2> after;
};
struct alignas(void*) GuardedPalette
{
    std::array<std::uint64_t,2> before;
    alignas(void*) std::array<std::uint32_t,sizeof(GXTlutObj)/4> bytes;
    std::array<std::uint64_t,2> after;
};
std::vector<std::uint8_t> Read(const char* path)
{
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open authored texture bytes");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
}
int main(int argc,char** argv)
{
    try
    {
        if (argc != 3) throw std::runtime_error("Expected raw texture and independent oracle");
        auto raw=Read(argv[1]);Check(raw.size()>=64,"Raw texture too short");
        std::ifstream oracle(argv[2]);
        unsigned levels,format,width,height,entries,missing,gx_format,mode0,mode1,flags,image0;
        std::uint32_t texture_hash,offset,file_size;
        oracle>>levels>>format>>width>>height>>entries>>missing>>gx_format>>mode0>>mode1>>flags>>image0>>texture_hash>>offset>>file_size;
        Check(bool(oracle),"Independent oracle incomplete");
        const auto* header=reinterpret_cast<const GXTextureHeader*>(raw.data());
        Check(unsigned(header->numLevels)==levels,"Authored texture levels were not decoded from Wii32");
        Check(unsigned(eGXTextureFormat(header->format))==format,"Authored texture format differs");
        Check(unsigned(header->width)==width && unsigned(header->height)==height,"Authored texture dimensions differ");
        Check(unsigned(header->numEntries)==entries,"Authored palette entry count differs");
        Check(header->missingTexture==missing,"Authored missing-texture flag differs");
        Check(reinterpret_cast<const std::uint8_t*>(header+1)==raw.data()+32,"Authored payload offset changed");
        const auto* bundle=reinterpret_cast<const BundleHeader*>(raw.data()+32);
        const auto* record=reinterpret_cast<const glTexBundleDict*>(raw.data()+48);
        Check(unsigned(bundle->magic)==0x726c7462 && unsigned(bundle->numTextures)==0x12345678,"Texture bundle Wii32 words differ");
        Check(unsigned(record->hash)==texture_hash && unsigned(record->offset)==offset && unsigned(record->fileSize)==file_size,
            "Texture directory words differ");
        const std::array<std::uint64_t,2> guard{0xdeadfeedaabbccddULL,0x7ac4e2836541f0b9ULL};
        GuardedTexture object{};object.before=object.after=guard;
        GuardedPalette palette{};palette.before=palette.after=guard;
        const auto* pixels=raw.data()+64;
        Check(reinterpret_cast<std::uintptr_t>(pixels)>UINT32_MAX,"Native texture data pointer fixture lacks high bits");
        if (format==8)
            GXInitTexObjCI(object.bytes.data(),const_cast<std::uint8_t*>(pixels),width,height,gx_format,0,0,levels>1,0);
        else
            GXInitTexObj(object.bytes.data(),const_cast<std::uint8_t*>(pixels),width,height,gx_format,0,0,levels>1);
        GXInitTexObjLOD(object.bytes.data(),levels==1?1:format==8?3:5,1,0.f,float(levels-1),0.f,false,false,0);
        TextureObservation observed{};std::memcpy(&observed,object.bytes.data(),sizeof observed);
        Check(object.before==guard && object.after==guard,"Canonical GX object initialization overwrote adjacent storage");
        Check(observed.data==pixels && observed.width==width && observed.height==height && observed.format==gx_format,
            "Real GX initialization lost authored data pointer or dimensions/format");
        Check(observed.mode0==mode0 && observed.mode1==mode1 && observed.flags==flags && observed.image0==image0,
            "Real GX filter/LOD/mipmap registers differ from independent oracle");
        Check(observed.identity!=0 && observed.version==1,"Real GX object identity/version not initialized");
        auto* canonical=reinterpret_cast<GXTexObj*>(object.bytes.data());
        int marker=7;GXInitTexObjUserData(canonical,&marker);
        Check(GXGetTexObjUserData(canonical)==&marker,"Canonical GX object lost native user-data pointer");
        // Exercise the SDK numeric/opaque forwarding signatures used by the
        // original platform TU, including the local copied object path.
        GXInitTexObjWrapMode(object.bytes.data(),GX_REPEAT,GX_CLAMP);
        GXInitTexObjTlut(object.bytes.data(),17UL);
        std::memcpy(&observed,object.bytes.data(),sizeof observed);
        Check((observed.mode0&15)==1 && observed.tlut==17,"Real copied-object wrap/TLUT update differs");
        if (entries)
        {
            GXInitTlutObj(palette.bytes.data(),const_cast<std::uint8_t*>(pixels),2,entries);
            PaletteObservation table{};std::memcpy(&table,palette.bytes.data(),sizeof table);
            Check(palette.before==guard && palette.after==guard,"Canonical TLUT initialization overwrote adjacent storage");
            Check(table.data==pixels && table.entries==entries && table.format==2,"Real TLUT retained metadata differs");
            Check(table.tlut==(2U<<10) && table.load==0x64000000U && table.identity && table.version==1,
                "Real TLUT register/identity initialization differs");
        }
        // Native field writes must preserve serialized Wii record widths too.
        GXTextureHeader copy{};copy.numLevels=levels;copy.format=eGXTextureFormat(format);copy.width=width;copy.height=height;copy.numEntries=entries;
        const auto* bytes=reinterpret_cast<const std::uint8_t*>(&copy);
        Check(std::memcmp(bytes,raw.data(),8)==0 && std::memcmp(bytes+14,raw.data()+14,4)==0 && std::memcmp(bytes+20,raw.data()+20,4)==0,
            "Native record field writes changed Wii byte order");
        Check(sizeof(PlatTexture::m_TexObj)==sizeof(GXTexObj) && sizeof(PlatTexture::m_TlutObj)==sizeof(GXTlutObj),
            "Original platform texture storage differs from canonical SDK sizes");
        Check(offsetof(PlatTexture,m_TexObj)%alignof(void*)==0 && offsetof(PlatTexture,m_TlutObj)%alignof(void*)==0,
            "Original platform object storage lacks native pointer alignment");
        std::cout<<"Texture data/GX ABI: "<<checks<<" assertions passed; original platform TU compiled separately; no upload/readiness claim\n";
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
