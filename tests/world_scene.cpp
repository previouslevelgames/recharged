#include "resources/world_scene.h"
#include <charconv>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace
{
std::vector<std::uint8_t> Read(const char* path)
{
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot open world fixture");
    return {std::istreambuf_iterator<char>(in),{}};
}
}
int main(int argc,char** argv)
{
    try
    {
        if(argc<4)throw std::runtime_error("Supply resident/temporary data and object IDs");
        auto resident=Read(argv[1]),temporary=Read(argv[2]);std::vector<std::uint32_t> ids;
        for(int i=3;i<argc;++i)
        {
            std::string_view text=argv[i];std::uint32_t id;
            auto result=std::from_chars(text.data(),text.data()+text.size(),id,16);
            if(result.ec!=std::errc{}||result.ptr!=text.data()+text.size())throw std::runtime_error("Invalid fixture ID");
            ids.push_back(id);
        }
        auto scene=mscharged::resources::ReadStaticWorldScene(resident,temporary,ids);
        std::fill(resident.begin(),resident.end(),0xcc);resident.clear();resident.shrink_to_fit();
        std::fill(temporary.begin(),temporary.end(),0xcc);temporary.clear();temporary.shrink_to_fit();
        std::cout<<std::setprecision(9)<<"{\"objects\":"<<scene.objects.size()<<",\"models\":"<<scene.models.size()
            <<",\"textures\":"<<scene.textures.textures.size()<<",\"animations\":"<<scene.textures.animations.size()<<",\"bounds\":[";
        for(float x:scene.bounds.center)std::cout<<x<<',';
        std::cout<<scene.bounds.radius<<"],\"ids\":[";
        for(std::size_t i=0;i<scene.objects.size();++i)std::cout<<(i?",":"")<<scene.objects[i].id;
        std::cout<<"],\"positions\":[";
        bool comma=false;
        for(const auto& model:scene.models)for(const auto& packet:model.packets)for(const auto& vertex:packet.vertices)
            for(float x:vertex.position){std::cout<<(comma?",":"")<<x;comma=true;}
        std::cout<<"],\"normals\":[";comma=false;
        for(const auto& model:scene.models)for(const auto& packet:model.packets)for(const auto& vertex:packet.vertices)
            for(float x:vertex.normal){std::cout<<(comma?",":"")<<x;comma=true;}
        std::cout<<"]}\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
