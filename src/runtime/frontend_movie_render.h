#pragma once
#include "runtime/frontend_movie_playback.h"
#include <array>
class GLResourcePool;
class GLView;
namespace mscharged
{
struct FrontendMovieQuad
{
    // Original image callback vertex/UV order. Pixel projection belongs to view.
    std::array<std::array<float,2>,4> positions{{{0,0},{640,0},{640,480},{0,480}}};
    std::array<std::array<float,2>,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
    std::array<float,16> transform{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::array<float,4> colour{1,1,1,1};
    bool source_image_state=false; // Original FERender dynamic-image raster defaults.
};
// Retained original movie/u/v texture slots and original float mesh/material.
// No second movie owner or collision is permitted. Pool/views/material registry
// outlive owner. Submit only inside a collecting frame; FinishFrame only after
// actual send/cancel and drain. Cancellation yields no presentation receipt.
class FrontendMovieRenderer
{
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
public:
    FrontendMovieRenderer(GLResourcePool&,unsigned width,unsigned height,void(*drain)());
    ~FrontendMovieRenderer();
    FrontendMovieRenderer(const FrontendMovieRenderer&)=delete;
    FrontendMovieRenderer& operator=(const FrontendMovieRenderer&)=delete;
    void Submit(GLView&,resources::ThpMovieFrameHandle,const FrontendMovieQuad& = {});
    FrontendMoviePresentation::Handle FinishFrame();
    void Release();
};
}
