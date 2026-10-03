// Synthetic file records built independently of the native structures.
#include "resources/static_model.h"
#include "resources/texture_bundle.h"
#include <bit>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

using namespace mscharged::resources;
namespace
{
using Buffer = std::vector<std::uint8_t>;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Put32(Buffer& data, std::size_t offset, std::uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) data.at(offset + i) = value >> (24 - i * 8); }
void Put16(Buffer& data, std::size_t offset, std::uint16_t value)
{ data.at(offset) = value >> 8; data.at(offset + 1) = value; }
void PutFloat(Buffer& data, std::size_t offset, float value) { Put32(data, offset, std::bit_cast<std::uint32_t>(value)); }
void Reject(const std::function<void()>& call, const char* reason)
{ bool failed = false; try { call(); } catch (const std::runtime_error&) { failed = true; } Check(failed, reason); }
struct ModelFixture
{
    Buffer data = Buffer(8);
    std::size_t absolute_base = 0;
    std::map<std::uint32_t, std::size_t> offsets;
    void Chunk(std::uint32_t id, const Buffer& payload, unsigned alignment = 0)
    {
        const auto header = data.size(); data.resize(header + 8);
        while ((absolute_base + data.size()) % (1u << alignment)) data.push_back(0);
        offsets[id] = data.size(); data.insert(data.end(), payload.begin(), payload.end());
        Put32(data, header, id | (alignment << 24)); Put32(data, header + 4, data.size() - header - 8);
        while (data.size() % 4) data.push_back(0);
    }
    explicit ModelFixture(bool fixed_uv = false, unsigned alignment = 5, unsigned copies = 1, unsigned index_count = 3, std::size_t base = 0, bool shadow = false)
        : absolute_base(base)
    {
        Buffer parameters(shadow ? 12 : 8); if (shadow) Put32(parameters,8,1); Put32(parameters, 0, 0x12345678); parameters[6] = 3;
        Chunk(0x1b016, parameters);
        Buffer indices(index_count * 2); Put16(indices, 2, 1); Put16(indices, 4, 2);
        Chunk(0x1b007, indices); // BE indices with a legitimate zero offset.
        Buffer vertices(shadow ? 72 : 36 + (fixed_uv ? 12 + 12 : 24));
        const float xyz[] = {-1, 0, 0, 1, 0, 0, 0, 1, 0};
        for (unsigned i = 0; i < 9; ++i) PutFloat(vertices, i * 4, xyz[i]);
        if (fixed_uv)
        {
            Put16(vertices, 36, 0xfc00); Put16(vertices, 38, 512);
            Put16(vertices, 40, 1024); Put16(vertices, 42, 0);
            Put16(vertices, 44, 512); Put16(vertices, 46, 1024);
        }
        else
        {
            const float uv[] = {0, 1, 1, 1, 0.5f, 0};
            for (unsigned i = 0; i < 6; ++i) PutFloat(vertices, (shadow ? 48 : 36) + i * 4, uv[i]);
        }
        Chunk(0x1b006, vertices, alignment); // Payload alignment pad is included in chunk size.
        Buffer streams(fixed_uv || shadow ? 24 : 16); streams[5] = 12; streams[6] = 1;
        Put32(streams, 8, 36); streams[13] = fixed_uv ? 4 : 8; streams[14] = 4;
        if (fixed_uv) { Put32(streams, 16, 48); streams[21] = 4; streams[22] = 3; }
        if (shadow) { streams[13]=4; streams[14]=3; Put32(streams,16,48); streams[21]=8; streams[22]=4; }
        Chunk(0x1b005, streams);
        Buffer packet(48); Put32(packet, 4, index_count); Put16(packet, 8, 3); packet[11] = fixed_uv || shadow ? 3 : 2;
        Put32(packet, 16, shadow ? 0x386ecbdd : fixed_uv ? 0xd3e572da : 0x21db4385);
        Buffer packets;
        for (unsigned i = 0; i < copies; ++i) packets.insert(packets.end(), packet.begin(), packet.end());
        Chunk(0x1b004, packets);
        Buffer matrix(64);
        for (unsigned i = 0; i < 4; ++i) PutFloat(matrix, (i * 4 + i) * 4, 1);
        PutFloat(matrix, 48, 2); PutFloat(matrix, 52, 3); PutFloat(matrix, 56, 4);
        Chunk(0x1b002, matrix);
        Buffer model(12); Put32(model, 0, 0x87654321); Put32(model, 4, copies);
        Chunk(0x1b003, model); Put32(data, 0, 0x8001b000); Put32(data, 4, data.size() - 8);
    }
};
Buffer TextureFixture(std::uint32_t format = 3, std::uint16_t w = 4, std::uint16_t h = 4,
    std::size_t pixel_bytes = 64, unsigned levels = 1)
{
    const unsigned palette = format == 8 ? 2 : 0;
    Buffer data(32 + 16 + 32 + pixel_bytes + palette * 2, 0);
    Put32(data, 0, 0x50544c47); Put32(data, 4, 1);
    Put32(data, 16, 0x12345678); Put32(data, 20, 16); Put32(data, 24, 32 + pixel_bytes + palette * 2);
    Put32(data, 48, levels); Put32(data, 52, format); Put16(data, 62, w); Put16(data, 64, h); Put32(data, 68, palette);
    for (std::size_t i = 80; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i);
    return data;
}
void Models()
{
    const ModelFixture file;
    auto models = ReadStaticModels(file.data);
    Check(models.size() == 1 && models[0].id == 0x87654321 && models[0].packets.size() == 1, "Model metadata");
    const auto& packet = models[0].packets[0];
    Check(packet.material.textures[0].texture == 0x12345678 && packet.material.textures[0].flags == 3 && packet.indices == std::vector<std::uint16_t>{0,1,2}, "Packet indices/binding");
    Check(packet.vertices[0].position == std::array<float,3>{1,3,4} && packet.vertices[2].position == std::array<float,3>{2,4,4}, "Big endian float/matrix transpose");
    Check(packet.vertices[2].uv == std::array<float,2>{0.5f,0}, "Float UV");
    auto rotated = file.data;
    const auto matrix = file.offsets.at(0x1b002);
    PutFloat(rotated, matrix, 0); PutFloat(rotated, matrix + 4, 1);
    PutFloat(rotated, matrix + 16, -1); PutFloat(rotated, matrix + 20, 0);
    const auto transformed = ReadStaticModels(rotated);
    Check(transformed[0].packets[0].vertices[0].position == std::array<float,3>{2,2,4}, "Original row-vector matrix rotation");
    const ModelFixture fixed(true); auto fixed_models = ReadStaticModels(fixed.data);
    Check(fixed_models[0].packets[0].vertices[0].uv == std::array<float,2>{-1,0.5f}, "Signed fixed 10-bit UV");
    const ModelFixture compact(false, 0);
    Buffer collection(8); collection.insert(collection.end(), compact.data.begin(), compact.data.end());
    Put32(collection, 0, 0x8001b100); Put32(collection, 4, collection.size() - 8);
    Check(ReadStaticModels(collection)[0].id == 0x87654321, "Static group collection");
    collection.insert(collection.end(), compact.data.begin(), compact.data.end());
    Put32(collection, 4, collection.size() - 8);
    Reject([&] { ReadStaticModels(collection); }, "Duplicate model ID in collection");
    const ModelFixture budget(false, 0, 65, 65535);
    Reject([&] { ReadStaticModels(budget.data); }, "Excessive decoded index budget");
    // Root/group malformed sizes are rejected before any host pointer can be fixed up.
    for (std::size_t size = 0; size < file.data.size(); ++size)
        Reject([&] { ReadStaticModels(Bytes(file.data).first(size)); }, "Truncated model accepted");
    auto bad = file.data;
    Put32(bad, file.offsets.at(0x1b004) + 0, 0xffffffff); Reject([&] { ReadStaticModels(bad); }, "Out of range index offset");
    bad = file.data; Put16(bad, file.offsets.at(0x1b007) + 4, 3); Reject([&] { ReadStaticModels(bad); }, "Out of range index");
    bad = file.data; PutFloat(bad, file.offsets.at(0x1b006), std::numeric_limits<float>::quiet_NaN()); Reject([&] { ReadStaticModels(bad); }, "NaN position");
    bad = file.data; Put32(bad, file.offsets.at(0x1b004) + 16, 0xdeadcafe); Reject([&] { ReadStaticModels(bad); }, "Unknown shader");
    bad = file.data; Put32(bad, file.offsets.at(0x1b004) + 40, 1); Reject([&] { ReadStaticModels(bad); }, "Skinned packet");
    bad = file.data; Put32(bad, file.offsets.at(0x1b004) + 32, 4); Reject([&] { ReadStaticModels(bad); }, "Short material record");
    bad = file.data; PutFloat(bad, file.offsets.at(0x1b002) + 12, 1); Reject([&] { ReadStaticModels(bad); }, "Projective matrix");
    bad = file.data; Put32(bad, file.offsets.at(0x1b003) + 4, 2); Reject([&] { ReadStaticModels(bad); }, "Invalid model packet count");
    bad = file.data; Put32(bad, 0, 0x8001b200); Reject([&] { ReadStaticModels(bad); }, "Vertex animation root");
    bad = file.data; Put32(bad, 8, 0x0601b016); Reject([&] { ReadStaticModels(bad); }, "Excessive alignment");
}
void Materials()
{
    ModelFixture fixture;
    fixture.data.resize(8); fixture.offsets.clear();
    Buffer parameters(48); Put32(parameters,0,10); Put32(parameters,8,11); Put32(parameters,16,12);
    parameters[6]=1; parameters[14]=2; parameters[22]=3;
    PutFloat(parameters,24,.5f); PutFloat(parameters,28,2); PutFloat(parameters,32,-3); PutFloat(parameters,36,1);
    Put32(parameters,40,1); Put32(parameters,44,1); fixture.Chunk(0x1b016,parameters);
    Buffer indices(6); Put16(indices,2,1); Put16(indices,4,2); fixture.Chunk(0x1b007,indices);
    Buffer vertices(93);
    for (unsigned i=0;i<9;++i) PutFloat(vertices,i*4,float(i));
    for (unsigned i=0;i<3;++i)
    {
        vertices[36+i*3]=64; vertices[37+i*3]=64;
        for(unsigned uv=0;uv<3;++uv) {Put16(vertices,45+uv*12+i*4,1024*(uv+1)); Put16(vertices,47+uv*12+i*4,0xfc00);}
        vertices[81+i*4]=12; vertices[82+i*4]=34; vertices[83+i*4]=56; vertices[84+i*4]=78;
    }
    fixture.Chunk(0x1b006,vertices);
    Buffer streams(48);
    const unsigned offsets[]={0,36,45,57,69,81}, strides[]={12,3,4,4,4,4}, ids[]={1,2,4,4,4,3};
    for(unsigned i=0;i<6;++i){Put32(streams,i*8,offsets[i]); streams[i*8+5]=strides[i]; streams[i*8+6]=ids[i];}
    fixture.Chunk(0x1b005,streams);
    Buffer packet(48); Put32(packet,4,3); Put16(packet,8,3); packet[11]=6; Put32(packet,16,0x32475c7d); Put32(packet,28,0xC0007);
    fixture.Chunk(0x1b004,packet);
    Buffer matrix(64); for(unsigned i=0;i<4;++i) PutFloat(matrix,i*20,1); PutFloat(matrix,0,2);
    fixture.Chunk(0x1b002,matrix);
    Buffer model(12); Put32(model,0,1); Put32(model,4,1); fixture.Chunk(0x1b003,model);
    Put32(fixture.data,0,0x8001b000); Put32(fixture.data,4,fixture.data.size()-8);
    auto converted=ReadStaticModels(fixture.data); const auto& p=converted[0].packets[0];
    Check(p.raster==0xC0007 && p.material.textures[1].texture==11 && p.material.textures[2].flags==3,"All material textures/raster retained");
    Check(p.material.scalars==std::array<float,4>{.5f,2,-3,1} && p.material.switches[1]==1,"BE material floats and switches");
    const auto& v=p.vertices[0];
    Check(v.uv==std::array<float,2>{1,-1} && v.uv1==std::array<float,2>{2,-1} && v.uv2==std::array<float,2>{3,-1},"All three UV streams retained");
    Check(v.colour==std::array<std::uint8_t,4>{12,34,56,78},"RGBA vertex bytes retained");
    Check(std::abs(v.normal[0]-1/std::sqrt(5.f))<1e-6 && std::abs(v.normal[1]-2/std::sqrt(5.f))<1e-6,"Normal uses inverse transpose, not position transform");
    auto bad=fixture.data; PutFloat(bad,fixture.offsets.at(0x1b016)+24,2); Reject([&]{ReadStaticModels(bad);},"Unsafe specular colour conversion");
    bad=fixture.data; PutFloat(bad,fixture.offsets.at(0x1b016)+28,std::numeric_limits<float>::quiet_NaN()); Reject([&]{ReadStaticModels(bad);},"Nonfinite material scalar");
    bad=fixture.data; Put32(bad,fixture.offsets.at(0x1b016)+40,2); Reject([&]{ReadStaticModels(bad);},"Nonboolean material switch");
    bad=fixture.data; bad[fixture.offsets.at(0x1b016)+6]=4; Reject([&]{ReadStaticModels(bad);},"Unknown texture flag");
    bad=fixture.data; PutFloat(bad,fixture.offsets.at(0x1b002),0); Reject([&]{ReadStaticModels(bad);},"Singular normal matrix");
    bad=fixture.data; bad[fixture.offsets.at(0x1b006)+36]=bad[fixture.offsets.at(0x1b006)+37]=0; Reject([&]{ReadStaticModels(bad);},"Zero normal");
    bad=fixture.data; bad[fixture.offsets.at(0x1b005)+21]=8; Reject([&]{ReadStaticModels(bad);},"Wrong material stream format");
    bad=fixture.data; bad[fixture.offsets.at(0x1b004)+11]=2; Reject([&]{ReadStaticModels(bad);},"Missing material vertex streams");
}
void SpecularDetail()
{
    ModelFixture fixture;
    fixture.data.resize(8); fixture.offsets.clear();
    Buffer parameters(68);
    for (unsigned i = 0; i < 4; ++i)
    {
        Put32(parameters, i * 8, 0x10000010 + i);
        parameters[i * 8 + 6] = i;
    }
    const float values[] = {.25f, .75f, 64, .2f, .4f, .6f, .8f};
    for (unsigned i = 0; i < 7; ++i) PutFloat(parameters, 32 + i * 4, values[i]);
    Put32(parameters, 60, 1); Put32(parameters, 64, 1);
    fixture.Chunk(0x1b016, parameters);
    Buffer indices(6); Put16(indices, 2, 2); Put16(indices, 4, 1);
    fixture.Chunk(0x1b007, indices);
    Buffer vertices(105);
    for (unsigned i = 0; i < 9; ++i) PutFloat(vertices, i * 4, float(i));
    for (unsigned i = 0; i < 3; ++i)
    {
        vertices[38 + i * 3] = 64;
        for (unsigned uv = 0; uv < 4; ++uv)
        {
            Put16(vertices, 45 + uv * 12 + i * 4, (uv + 1) * 1024);
            Put16(vertices, 47 + uv * 12 + i * 4, 0x8000 + i);
        }
        vertices[93 + i * 4] = 10 + i;
        vertices[96 + i * 4] = 255;
    }
    fixture.Chunk(0x1b006, vertices);
    Buffer streams(56);
    const unsigned offsets[] = {0,36,45,57,69,81,93}, strides[] = {12,3,4,4,4,4,4}, ids[] = {1,2,4,4,4,4,3};
    for (unsigned i = 0; i < 7; ++i)
    { Put32(streams, i * 8, offsets[i]); streams[i * 8 + 5] = strides[i]; streams[i * 8 + 6] = ids[i]; }
    fixture.Chunk(0x1b005, streams);
    Buffer packet(48); Put32(packet, 4, 3); Put16(packet, 8, 3); packet[11] = 7;
    Put32(packet, 16, 0x112ab470); fixture.Chunk(0x1b004, packet);
    Buffer matrix(64); for (unsigned i = 0; i < 4; ++i) PutFloat(matrix, i * 20, 1);
    fixture.Chunk(0x1b002, matrix);
    Buffer model(12); Put32(model, 0, 1); Put32(model, 4, 1); fixture.Chunk(0x1b003, model);
    Put32(fixture.data, 0, 0x8001b000); Put32(fixture.data, 4, fixture.data.size() - 8);
    const auto decoded = ReadStaticModels(fixture.data);
    const auto& p = decoded[0].packets[0];
    Check(p.material.scalars == std::array<float,4>{.25f,.75f,64,0}
        && p.material.specular_colour == std::array<float,4>{.2f,.4f,.6f,.8f}
        && p.material.switches[0] == 1 && p.material.switches[1] == 1, "Detail big-endian parameters");
    for (unsigned i = 0; i < 4; ++i)
        Check(p.material.textures[i].texture == 0x10000010 + i && p.material.textures[i].flags == i,
            "Detail four independent texture bindings");
    const auto& v = p.vertices[0];
    Check(v.uv == std::array<float,2>{1,-32} && v.uv1 == std::array<float,2>{2,-32}
        && v.uv2 == std::array<float,2>{3,-32} && v.uv3 == std::array<float,2>{4,-32}, "Detail four signed UV sets");
    Check(v.normal == std::array<float,3>{0,0,1} && v.colour[0] == 10 && v.colour[3] == 255
        && p.indices == std::vector<std::uint16_t>{0,2,1}, "Detail normal, colour and indices");
    const auto params = fixture.offsets.at(0x1b016);
    for (unsigned field = 0; field < 7; ++field)
        for (float value : {-1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), field == 2 ? 10001.f : 1.01f})
        {
            auto bad = fixture.data; PutFloat(bad, params + 32 + field * 4, value);
            Reject([&] { ReadStaticModels(bad); }, "Invalid detail float accepted");
        }
    for (unsigned field : {60u,64u})
    {
        auto bad = fixture.data; Put32(bad, params + field, 2);
        Reject([&] { ReadStaticModels(bad); }, "Invalid detail switch accepted");
    }
    for (unsigned slot = 0; slot < 4; ++slot)
    {
        auto bad = fixture.data; bad[params + slot * 8 + 6] = 4;
        Reject([&] { ReadStaticModels(bad); }, "Invalid detail binding accepted");
    }
    auto bad = fixture.data; Put32(bad, fixture.offsets.at(0x1b004) + 32, 4);
    Reject([&] { ReadStaticModels(bad); }, "Truncated detail parameters accepted");
    bad = fixture.data; bad[fixture.offsets.at(0x1b004) + 11] = 6;
    Reject([&] { ReadStaticModels(bad); }, "Missing fourth detail UV accepted");
    bad = fixture.data; bad[fixture.offsets.at(0x1b005) + 45] = 8;
    Reject([&] { ReadStaticModels(bad); }, "Invalid fourth detail UV stride accepted");
    bad = fixture.data; Put32(bad, fixture.offsets.at(0x1b005) + 40, 100);
    Reject([&] { ReadStaticModels(bad); }, "Out-of-bounds fourth detail UV accepted");
}
void ScrollingSpecular()
{
    ModelFixture f;
    f.data.resize(8); f.offsets.clear();
    Buffer params(60);
    Put32(params,0,20); params[6]=1; Put32(params,8,21); params[14]=2;
    const float values[] = {.75f,64,.2f,.4f,.6f,.8f,-.25f,.5f};
    for (unsigned i=0;i<8;++i) PutFloat(params,16+i*4,values[i]);
    Put32(params,48,1); Put32(params,52,0); Put32(params,56,1);
    f.Chunk(0x1b016,params);
    Buffer indices(6); Put16(indices,2,2); Put16(indices,4,1); f.Chunk(0x1b007,indices);
    Buffer vertices(81);
    for (unsigned i=0;i<9;++i) PutFloat(vertices,i*4,float(i));
    for (unsigned i=0;i<3;++i)
    {
        vertices[38+i*3]=64;
        Put16(vertices,45+i*4,0xfc00); Put16(vertices,47+i*4,512);
        Put16(vertices,57+i*4,2048); Put16(vertices,59+i*4,0x8000);
        vertices[69+i*4]=10+i; vertices[72+i*4]=255;
    }
    f.Chunk(0x1b006,vertices);
    Buffer streams(40);
    const unsigned offsets[]={0,36,45,57,69},strides[]={12,3,4,4,4},ids[]={1,2,4,4,3};
    for (unsigned i=0;i<5;++i)
    {Put32(streams,i*8,offsets[i]);streams[i*8+5]=strides[i];streams[i*8+6]=ids[i];}
    f.Chunk(0x1b005,streams);
    Buffer packet(48); Put32(packet,4,3); Put16(packet,8,3); packet[11]=5;
    Put32(packet,16,0x3eccd955); f.Chunk(0x1b004,packet);
    Buffer matrix(64); for(unsigned i=0;i<4;++i) PutFloat(matrix,i*20,1); f.Chunk(0x1b002,matrix);
    Buffer model(12); Put32(model,0,1); Put32(model,4,1); f.Chunk(0x1b003,model);
    Put32(f.data,0,0x8001b000); Put32(f.data,4,f.data.size()-8);
    const auto models=ReadStaticModels(f.data); const auto& p=models[0].packets[0];
    Check(p.material.scalars==std::array<float,4>{.75f,64,-.25f,.5f}
        && p.material.specular_colour==std::array<float,4>{.2f,.4f,.6f,.8f}
        && p.material.switches==std::array<std::uint32_t,5>{1,0,1,0,0},"Scrolling specular BE parameters");
    Check(p.material.textures[0].texture==20 && p.material.textures[0].flags==1
        && p.material.textures[1].texture==21 && p.material.textures[1].flags==2,"Scrolling independent bindings");
    Check(p.vertices[0].uv==std::array<float,2>{-1,.5f} && p.vertices[0].uv1==std::array<float,2>{2,-32}
        && p.vertices[0].normal==std::array<float,3>{0,0,1} && p.vertices[0].colour[0]==10
        && p.indices==std::vector<std::uint16_t>{0,2,1},"Scrolling signed UVs, normal, colour and indices");
    const auto parameters=f.offsets.at(0x1b016);
    for(unsigned field=0;field<8;++field)
    {
        for(float value:{std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity(),
                         field==1 || field>=6 ? 10001.f : 1.01f})
        {auto bad=f.data;PutFloat(bad,parameters+16+field*4,value);Reject([&]{ReadStaticModels(bad);},"Invalid scrolling float");}
        if(field<6)
        {auto bad=f.data;PutFloat(bad,parameters+16+field*4,-1);Reject([&]{ReadStaticModels(bad);},"Negative scrolling level/exponent/colour");}
    }
    for(unsigned field:{48u,52u,56u})
    {auto bad=f.data;Put32(bad,parameters+field,2);Reject([&]{ReadStaticModels(bad);},"Invalid scrolling switch");}
    for(unsigned slot:{0u,1u})
        for(unsigned byte:{6u,7u})
        {auto bad=f.data;bad[parameters+slot*8+byte]=4;Reject([&]{ReadStaticModels(bad);},"Invalid scrolling binding");}
    auto bad=f.data;Put32(bad,f.offsets.at(0x1b004)+32,4);
    Reject([&]{ReadStaticModels(bad);},"Short scrolling parameters");
    bad=f.data;bad[f.offsets.at(0x1b004)+11]=4;Reject([&]{ReadStaticModels(bad);},"Missing specular UV");
    bad=f.data;bad[f.offsets.at(0x1b005)+29]=8;Reject([&]{ReadStaticModels(bad);},"Invalid specular UV stride");
    bad=f.data;Put32(bad,f.offsets.at(0x1b005)+24,80);Reject([&]{ReadStaticModels(bad);},"Out-of-bounds specular UV");
}
void CameraOverlay(bool scrolling = false)
{
    ModelFixture f;
    f.data.resize(8); f.offsets.clear();
    Buffer params(scrolling ? 60 : 48);
    for (unsigned i = 0; i < 3; ++i) { Put32(params, i * 8, 30 + i); params[i * 8 + 6] = i; }
    PutFloat(params, 24, -2); PutFloat(params, 28, -.5f); PutFloat(params, 32, .25f);
    Put32(params, 36, 1); Put32(params, 40, 0); Put32(params, 44, 1);
    if (scrolling)
    { PutFloat(params,44,-.5f); PutFloat(params,48,.25f); Put32(params,52,0); Put32(params,56,1); }
    f.Chunk(0x1b016, params);
    Buffer indices(6); Put16(indices, 2, 2); Put16(indices, 4, 1); f.Chunk(0x1b007, indices);
    Buffer vertices(93);
    for (unsigned i = 0; i < 9; ++i) PutFloat(vertices, i * 4, float(i));
    for (unsigned i = 0; i < 3; ++i)
    {
        vertices[38 + i * 3] = 64;
        for (unsigned uv = 0; uv < 3; ++uv)
        { Put16(vertices, 45 + uv * 12 + i * 4, 1024 * (uv + 1)); Put16(vertices, 47 + uv * 12 + i * 4, 0xfc00); }
        vertices[81 + i * 4] = 10 + i; vertices[84 + i * 4] = 255;
    }
    f.Chunk(0x1b006, vertices);
    Buffer streams(48);
    const unsigned offsets[] = {0,36,45,57,69,81}, strides[] = {12,3,4,4,4,4}, ids[] = {1,2,4,4,4,3};
    for (unsigned i = 0; i < 6; ++i)
    { Put32(streams, i * 8, offsets[i]); streams[i * 8 + 5] = strides[i]; streams[i * 8 + 6] = ids[i]; }
    f.Chunk(0x1b005, streams);
    Buffer packet(48); Put32(packet, 4, 3); Put16(packet, 8, 3); packet[11] = 6;
    Put32(packet, 16, scrolling ? 0x845cad59 : 0x32bc21e8); f.Chunk(0x1b004, packet);
    Buffer matrix(64); for (unsigned i = 0; i < 4; ++i) PutFloat(matrix, i * 20, 1); f.Chunk(0x1b002, matrix);
    Buffer model(12); Put32(model, 0, 1); Put32(model, 4, 1); f.Chunk(0x1b003, model);
    Put32(f.data, 0, 0x8001b000); Put32(f.data, 4, f.data.size() - 8);
    const auto models = ReadStaticModels(f.data); const auto& p = models[0].packets[0];
    Check(p.material.scalars == std::array<float,4>{-2,-.5f,.25f,0}
        && p.material.switches == (scrolling ? std::array<std::uint32_t,5>{1,0,0,1,0} : std::array<std::uint32_t,5>{1,0,1,0,0}), "Camera overlay BE scalars/switches");
    for (unsigned i = 0; i < 3; ++i)
        Check(p.material.textures[i].texture == 30 + i && p.material.textures[i].flags == i, "Overlay three independent bindings");
    Check(p.vertices[0].uv == std::array<float,2>{1,-1} && p.vertices[0].uv1 == std::array<float,2>{2,-1}
        && p.vertices[0].uv2 == std::array<float,2>{3,-1} && p.vertices[0].normal == std::array<float,3>{0,0,1}
        && p.vertices[0].colour[0] == 10 && p.indices == std::vector<std::uint16_t>{0,2,1}, "Overlay signed streams");
    const auto par = f.offsets.at(0x1b016);
    auto valid = f.data; PutFloat(valid, par + 24, 0);
    if (scrolling) Reject([&] { ReadStaticModels(valid); }, "Scrolling camera overlay has no zero-scale fallback");
    else Check(ReadStaticModels(valid)[0].packets[0].material.scalars[0] == 0, "Original zero-scale fallback is valid");
    if (scrolling)
    {
        Check(p.material.scroll_speeds[0] == std::array<float,2>{-.5f,.25f}, "Scrolling overlay BE speed pair");
        for (unsigned i=0;i<2;++i)
            for (float value : {10001.f,-10001.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
            {auto bad=f.data;PutFloat(bad,par+44+i*4,value);Reject([&]{ReadStaticModels(bad);},"Invalid scrolling overlay speed");}
        auto bad=f.data;Put32(bad,par+52,2);Reject([&]{ReadStaticModels(bad);},"Invalid mask scroll flag");
    }
    for (unsigned field = 0; field < 3; ++field)
    {
        for (float value : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), field == 2 ? 1.01f : 10001.f})
        { auto bad = f.data; PutFloat(bad, par + 24 + field * 4, value); Reject([&] { ReadStaticModels(bad); }, "Invalid overlay scalar"); }
        auto bad = f.data; Put32(bad, par + (scrolling && field == 2 ? 56 : 36 + field * 4), 2);
        Reject([&] { ReadStaticModels(bad); }, "Invalid overlay boolean");
        bad = f.data; bad[par + field * 8 + 6] = 4;
        Reject([&] { ReadStaticModels(bad); }, "Invalid overlay texture flags");
    }
    for (float value : {std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min()})
    { auto bad = f.data; PutFloat(bad, par + 24, value); Reject([&] { ReadStaticModels(bad); }, "Overflowing inverse overlay scale"); }
    auto bad = f.data; PutFloat(bad, par + 32, -1); Reject([&] { ReadStaticModels(bad); }, "Negative overlay amount");
    bad = f.data; Put32(bad, f.offsets.at(0x1b004) + 32, 4); Reject([&] { ReadStaticModels(bad); }, "Short overlay parameters");
    bad = f.data; bad[f.offsets.at(0x1b004) + 11] = 5; Reject([&] { ReadStaticModels(bad); }, "Missing overlay mask UV");
    bad = f.data; Put32(bad, f.offsets.at(0x1b005) + 32, 92); Reject([&] { ReadStaticModels(bad); }, "Mask UV out of bounds");
}
void MaskedDetail(bool scrolling = false)
{
    ModelFixture f;
    f.data.resize(8); f.offsets.clear();
    Buffer params(scrolling ? 60 : 36);
    const unsigned blend_offset = scrolling ? 48 : 24;
    for (unsigned i = 0; i < 3; ++i) { Put32(params, i * 8, 30 + i); params[i * 8 + 6] = i; }
    PutFloat(params, blend_offset, .25f); Put32(params, blend_offset + 4, 1); Put32(params, blend_offset + 8, 0);
    if (scrolling) for (unsigned i=0;i<6;++i) PutFloat(params,24+i*4,(int(i)-3)*.25f);
    f.Chunk(0x1b016, params);
    Buffer indices(6); Put16(indices, 2, 2); Put16(indices, 4, 1); f.Chunk(0x1b007, indices);
    Buffer vertices(93);
    for (unsigned i = 0; i < 9; ++i) PutFloat(vertices, i * 4, float(i));
    for (unsigned i = 0; i < 3; ++i)
    {
        vertices[38 + i * 3] = 64;
        for (unsigned uv = 0; uv < 3; ++uv)
        { Put16(vertices, 45 + uv * 12 + i * 4, 1024 * (uv + 1)); Put16(vertices, 47 + uv * 12 + i * 4, 0xfc00); }
        vertices[81 + i * 4] = 10 + i; vertices[84 + i * 4] = 255;
    }
    f.Chunk(0x1b006, vertices);
    Buffer streams(48);
    const unsigned offsets[] = {0,36,45,57,69,81}, strides[] = {12,3,4,4,4,4}, ids[] = {1,2,4,4,4,3};
    for (unsigned i = 0; i < 6; ++i)
    { Put32(streams, i * 8, offsets[i]); streams[i * 8 + 5] = strides[i]; streams[i * 8 + 6] = ids[i]; }
    f.Chunk(0x1b005, streams);
    Buffer packet(48); Put32(packet, 4, 3); Put16(packet, 8, 3); packet[11] = 6;
    Put32(packet, 16, scrolling ? 0xf2d57ac6 : 0x09609a35); f.Chunk(0x1b004, packet);
    Buffer matrix(64); for (unsigned i = 0; i < 4; ++i) PutFloat(matrix, i * 20, 1); f.Chunk(0x1b002, matrix);
    Buffer model(12); Put32(model, 0, 1); Put32(model, 4, 1); f.Chunk(0x1b003, model);
    Put32(f.data, 0, 0x8001b000); Put32(f.data, 4, f.data.size() - 8);
    const auto models = ReadStaticModels(f.data); const auto& p = models[0].packets[0];
    Check(p.material.scalars[0] == .25f && p.material.switches[0] == 1 && p.material.switches[1] == 0,
        "Masked detail BE blend and switches");
    for (unsigned i = 0; i < 3; ++i)
        Check(p.material.textures[i].texture == 30 + i && p.material.textures[i].flags == i, "Masked detail independent bindings");
    Check(p.vertices[0].uv == std::array<float,2>{1,-1} && p.vertices[0].uv1 == std::array<float,2>{2,-1}
        && p.vertices[0].uv2 == std::array<float,2>{3,-1} && p.vertices[0].normal == std::array<float,3>{0,0,1}
        && p.vertices[0].colour[0] == 10 && p.indices == std::vector<std::uint16_t>{0,2,1}, "Masked detail signed streams");
    const auto par = f.offsets.at(0x1b016);
    if (scrolling) for (unsigned i=0;i<6;++i)
    {
        Check(p.material.scroll_speeds[i/2][i%2]==(int(i)-3)*.25f,"Six independent BE scroll speeds");
        for(float value:{10001.f,-10001.f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
        {auto bad=f.data;PutFloat(bad,par+24+i*4,value);Reject([&]{ReadStaticModels(bad);},"Invalid masked scroll speed");}
    }
    for (float value : {0.f,1.f})
    { auto valid=f.data; PutFloat(valid,par+blend_offset,value); Check(ReadStaticModels(valid)[0].packets[0].material.scalars[0]==value,"Blend endpoints"); }
    for (float value : {-1.f,1.01f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})
    { auto bad=f.data; PutFloat(bad,par+blend_offset,value); Reject([&]{ReadStaticModels(bad);},"Invalid masked detail blend"); }
    for (unsigned i=0;i<2;++i)
    { auto bad=f.data; Put32(bad,par+blend_offset+4+i*4,2); Reject([&]{ReadStaticModels(bad);},"Invalid masked detail boolean"); }
    for (unsigned i=0;i<3;++i)
    {
        auto bad=f.data; bad[par+i*8+6]=4; Reject([&]{ReadStaticModels(bad);},"Invalid masked detail texture flags");
        bad=f.data; bad[par+i*8+7]=1; Reject([&]{ReadStaticModels(bad);},"Unsupported masked detail binding metadata");
    }
    for (unsigned i=0;i<6;++i)
    {
        auto bad=f.data; ++bad[f.offsets.at(0x1b005)+i*8+5]; Reject([&]{ReadStaticModels(bad);},"Masked detail stride");
        bad=f.data; Put32(bad,f.offsets.at(0x1b005)+i*8,92); Reject([&]{ReadStaticModels(bad);},"Masked detail stream bounds");
    }
    auto bad=f.data; Put32(bad,f.offsets.at(0x1b004)+32,4); Reject([&]{ReadStaticModels(bad);},"Short masked detail parameters");
    bad=f.data; bad[f.offsets.at(0x1b004)+11]=5; Reject([&]{ReadStaticModels(bad);},"Missing masked detail stream");
}

void WorldModels()
{
    const auto texture = TextureFixture();
    const std::size_t group_start = 8 + 8 + texture.size() + 8;
    const ModelFixture shadow(false, 5, 1, 3, group_start, true);
    auto selected = ReadStaticModels(ModelFixture(false,5,1,3,0,true).data);
    Check(selected[0].packets[0].material.switches[0] == 1 && selected[0].packets[0].vertices[2].uv[0] == .5f,
          "Big-endian shadow parameter and floating UV conversion");
    auto invalid = ModelFixture(false,0,1,3,0,true);
    Put32(invalid.data,invalid.offsets.at(0x1b016)+8,2);
    Reject([&]{ReadStaticModels(invalid.data);},"Invalid shadow boolean");
    invalid = ModelFixture(false,0,1,3,0,true);
    invalid.data[invalid.offsets.at(0x1b005)+13]=8;
    Reject([&]{ReadStaticModels(invalid.data);},"Shadow colour/UV stream order");
    ModelFixture unsupported(false,5,1,3,group_start+shadow.data.size());
    Put32(unsupported.data,unsupported.offsets.at(0x1b003),0x87654322);
    Put32(unsupported.data,unsupported.offsets.at(0x1b004)+16,0xdeadbeef);
    Buffer world(16);
    Put32(world,8,0x24100); Put32(world,12,texture.size());
    world.insert(world.end(),texture.begin(),texture.end());
    const auto collection = world.size(); world.resize(collection+8);
    world.insert(world.end(),shadow.data.begin(),shadow.data.end());
    world.insert(world.end(),unsupported.data.begin(),unsupported.data.end());
    Put32(world,collection,0x8001b100); Put32(world,collection+4,world.size()-collection-8);
    Put32(world,0,0x80000001); Put32(world,4,world.size()-8);
    auto decoded=ReadStaticWorldModel(world,0x87654321);
    Check(decoded.model.id==0x87654321 && decoded.model.packets[0].vertices[0].position==std::array<float,3>{1,3,4},
          "World-relative 32-byte alignment and explicit model selection");
    Check(ReadTextureBundle(decoded.textures,{0x12345678}).textures[0].width==4,"Embedded texture container boundaries");
    Reject([&]{ReadStaticWorldModel(world,0x87654322);},"Unimplemented selected world material");
    Reject([&]{ReadStaticWorldModel(world,0);},"Missing selected world model");
    for(std::size_t n=0;n<world.size();++n)
        Reject([&]{ReadStaticWorldModel(Bytes(world).first(n),0x87654321);},"Truncated world");
    // A structurally bounded unrelated animated group must not block a selected
    // static model. Selecting that group's model still fails explicitly.
    auto animated=world;
    const auto animated_group=group_start+shadow.data.size();
    animated.insert(animated.end(),{0x80,0x01,0xb2,0x00,0,0,0,0});
    Put32(animated,animated_group+4,unsupported.data.size());
    Put32(animated,collection+4,animated.size()-collection-8);Put32(animated,4,animated.size()-8);
    Check(ReadStaticWorldModel(animated,0x87654321).model.id==0x87654321,"Unselected animated world group");
    Reject([&]{ReadStaticWorldModel(animated,0x87654322);},"Selected animated world group");
    auto bad=world;Put32(bad,group_start+shadow.data.size()+unsupported.offsets.at(0x1b003),0x87654321);
    Reject([&]{ReadStaticWorldModel(bad,0x87654321);},"Duplicate world model ID");
    bad=world;Put32(bad,8,0x24101);Reject([&]{ReadStaticWorldModel(bad,0x87654321);},"Unknown world container");
    bad=world;Put32(bad,12,0xffffffff);Reject([&]{ReadStaticWorldModel(bad,0x87654321);},"World child overflow");
}
Buffer TextureRecords(const std::vector<std::pair<std::uint32_t,Buffer>>& records)
{
    Buffer result(16 + records.size()*16); Put32(result,0,0x50544c47); Put32(result,4,records.size());
    const auto start=result.size();
    for (unsigned i=0;i<records.size();++i)
    {
        Put32(result,16+i*16,records[i].first); Put32(result,20+i*16,result.size()-start);
        Put32(result,24+i*16,records[i].second.size());
        result.insert(result.end(),records[i].second.begin(),records[i].second.end());
    }
    return result;
}
void TextureAnimations()
{
    const auto file=TextureFixture(); const Buffer image(file.begin()+48,file.end());
    Buffer animation(36+3*8); Put32(animation,0,0x5f6c6669); Put32(animation,4,0xabcd0001);
    Put32(animation,8,3); Put32(animation,12,1); Put32(animation,16,0xffffffff);
    animation[20]=1; Put32(animation,24,0x98765432); PutFloat(animation,28,.125f); Put32(animation,32,0xfedcba98);
    for (unsigned i=0;i<3;++i) { Put32(animation,36+8*i,10+i%2); PutFloat(animation,40+8*i,.25f*(i+1)); }
    auto bundle=[&](const Buffer& anim) { return TextureRecords({{0xabcd0001,anim},{10,image},{11,image},{12,image}}); };
    const auto bytes=bundle(animation), saved=bytes;
    const auto selected=ReadTextureBundle(bytes,{0xabcd0001,0xabcd0001});
    Check(selected.textures.size()==2 && selected.animations.size()==1,"Animation dependency closure/deduplication");
    const auto& a=selected.animations[0];
    Check(a.id==0xabcd0001 && a.mode==1 && a.direction==-1 && a.paused && a.elapsed==.125f
        && a.frames.size()==3 && a.frames[0].texture==10 && a.frames[1].texture==11
        && a.frames[2].texture==10 && a.frames[2].duration==.75f,"Fixed-width big-endian IFL conversion");
    Check(bytes==saved,"IFL decoder modified source data");
    Check(ReadTextureBundle(bytes,{10}).animations.empty(),"Static selection pulled unrelated animations");
    Check(ReadTextureBundle(bytes).textures.size()==3,"Full bundle static/animation split");
    auto reversed=TextureRecords({{11,image},{10,image},{0xabcd0001,animation}});
    Check(ReadTextureBundle(reversed,{0xabcd0001}).textures.size()==2,"Frame dependencies before animation record");
    for (unsigned length=0;length<animation.size();++length)
        Reject([&]{ ReadTextureBundle(bundle(Buffer(animation.begin(),animation.begin()+length)),{0xabcd0001}); },"Truncated IFL record");
    auto bad=animation;
    for (auto offset : {4u,8u,12u,16u})
    {
        bad=animation; Put32(bad,offset,offset==8 ? 4097 : 99);
        Reject([&]{ReadTextureBundle(bundle(bad));},"Invalid IFL hash/count/mode/direction");
    }
    bad=animation;Put32(bad,8,0);Reject([&]{ReadTextureBundle(bundle(bad));},"Zero animation frames");
    bad=animation;bad[20]=2;Reject([&]{ReadTextureBundle(bundle(bad));},"Invalid animation pause flag");
    for (float value : {-1.f,std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
        for (unsigned offset : {28u,40u})
        {
            bad=animation;PutFloat(bad,offset,value);Reject([&]{ReadTextureBundle(bundle(bad));},"Invalid animation duration/elapsed time");
        }
    bad=animation;PutFloat(bad,40,0);Check(ReadTextureBundle(bundle(bad)).animations[0].frames[0].duration==0,"Original zero-duration frame");
    bad=animation;Put32(bad,36,99);Reject([&]{ReadTextureBundle(bundle(bad));},"Missing animation frame");
    bad=animation;Put32(bad,36,0xabcd0001);Reject([&]{ReadTextureBundle(bundle(bad));},"Self-referencing animation");
    auto other=animation;Put32(other,4,99);bad=animation;Put32(bad,36,99);
    Reject([&]{ReadTextureBundle(TextureRecords({{0xabcd0001,bad},{99,other},{10,image},{11,image}}),{0xabcd0001});},"Nested animation");
    Reject([&]{ReadTextureBundle(TextureRecords({{0xabcd0001,animation},{0xabcd0001,animation},{10,image},{11,image}}));},"Duplicate animation hash");
}

void Textures()
{
    const auto file = TextureFixture(); const auto textures = ReadTextureBundle(file).textures;
    Check(textures.size() == 1 && textures[0].width == 4 && textures[0].gx_format == 6 && textures[0].pixels.size() == 64, "Texture metadata");
    Check(textures[0].pixels[0] == 80 && textures[0].pixels[63] == 143, "Texture tiles must retain Wii bytes");
    const unsigned sizes[] = {192,192,64,384,128,64,128,192,128};
    const unsigned gx[] = {4,5,14,6,1,0,1,3,9};
    for (unsigned format = 0; format < 9; ++format)
    {
        const auto test = TextureFixture(format, 9, 5, sizes[format]); const auto value = ReadTextureBundle(test).textures[0];
        Check(value.gx_format == gx[format] && value.pixels.size() == sizes[format], "Nonaligned tile physical size");
        if (format == 8) Check(value.palette_entries == 2 && value.palette.size() == 4 && value.palette[0] == std::uint8_t(208), "Palette byte order");
    }
    Check(ReadTextureBundle(TextureFixture(3,4,4,192,3)).textures[0].pixels.size() == 192, "Small mip tile allocation");
    for (std::size_t size = 0; size < file.size(); ++size)
        Reject([&] { ReadTextureBundle(Bytes(file).first(size)); }, "Truncated texture accepted");
    Check(ReadTextureBundle(file,{0x12345678}).textures.size()==1,"Selected texture lookup");
    Reject([&]{ReadTextureBundle(file,{0x12345679});},"Missing selected texture");
    auto mixed=file; mixed.insert(mixed.begin()+32,16,0); Put32(mixed,4,2);
    Put32(mixed,32,0xabcdef01); Put32(mixed,36,mixed.size()-48); Put32(mixed,40,4);
    mixed.insert(mixed.end(),{0x5f,0x6c,0x66,0x69});
    Check(ReadTextureBundle(mixed,{0x12345678}).textures.size()==1,"Unrequested animated global entry is not decoded");
    Reject([&]{ReadTextureBundle(mixed);},"Truncated animation in full bundle");
    auto bad = file; Put32(bad, 4, 0xffffffff); Reject([&] { ReadTextureBundle(bad); }, "Dictionary overflow");
    bad = file; Put32(bad, 20, 0xfffffff0); Reject([&] { ReadTextureBundle(bad); }, "Data-relative texture offset");
    bad = file; Put32(bad, 48, 0x5f6c6669); Reject([&] { ReadTextureBundle(bad); }, "Malformed animation header");
    bad = file; Put32(bad, 48, 4); Reject([&] { ReadTextureBundle(bad); }, "Excess mip count");
    bad = file; Put32(bad, 52, 9); Reject([&] { ReadTextureBundle(bad); }, "Unknown texture format");
    bad = file; Put16(bad, 62, 0); Reject([&] { ReadTextureBundle(bad); }, "Zero texture width");
    bad = file; bad[60] = 1; Reject([&] { ReadTextureBundle(bad); }, "Missing texture");
    bad = file; Put32(bad, 68, 2); Reject([&] { ReadTextureBundle(bad); }, "Palette on nonindexed texture");
    Buffer aliases(16 + 100 * 16 + 32 + 1024 * 1024);
    Put32(aliases, 0, 0x50544c47); Put32(aliases, 4, 100);
    for (unsigned i = 0; i < 100; ++i)
    {
        Put32(aliases, 16 + i * 16, i); Put32(aliases, 24 + i * 16, 32 + 1024 * 1024);
    }
    Put32(aliases, 1616, 1); Put32(aliases, 1620, 3); Put16(aliases, 1630, 512); Put16(aliases, 1632, 512);
    Reject([&] { ReadTextureBundle(aliases); }, "Aliased texture decoded allocation budget");
    bad = TextureFixture(8,4,4,32); Put32(bad, 68, 257); Reject([&] { ReadTextureBundle(bad); }, "Excessive CI8 palette");
}
}
int main()
{
    try { Models(); Materials(); SpecularDetail(); ScrollingSpecular(); CameraOverlay(); CameraOverlay(true); MaskedDetail(); MaskedDetail(true); WorldModels(); Textures(); TextureAnimations(); std::cout << "Static Wii resource conversion and rejection checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
