#pragma once
#include <cstdint>
namespace mscharged { class FrontendPacketRenderer; }
namespace mscharged::resources
{
// Native FETextureResource runtime binding, distinct from its serialized name
// hash. Constructed only by actual movie texture registration. This metadata
// alone does not establish a live drawable or a presented movie frame.
class FrontendMovieImage final
{
    friend class mscharged::FrontendPacketRenderer;
    std::uint32_t width_=0,height_=0,instance_=0,resource_=0;
    FrontendMovieImage()=default;
public:
    FrontendMovieImage(const FrontendMovieImage&)=delete;
    FrontendMovieImage& operator=(const FrontendMovieImage&)=delete;
    std::uint32_t Width()const{return width_;}
    std::uint32_t Height()const{return height_;}
    std::uint32_t Instance()const{return instance_;}
    std::uint32_t Resource()const{return resource_;}
};
}
