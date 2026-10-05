#pragma once
#include "skin_pose_fixture.h"
#include "runtime/skin_render.h"
#include "NL/gl/glState.h"
namespace skin_render_fixture
{
using namespace skin_fixture;
inline void Float(Blob& b,std::size_t offset,float x){Set(b,offset,std::bit_cast<std::uint32_t>(x));}
inline Blob Model(unsigned bone=0,float blend=1,float alpha=1,bool lit=true)
{
 auto b=Rlg();const auto v=Find(b,0x1b006),bind=Find(b,0x1b00a),params=Find(b,0x1b016),packet=Find(b,0x1b004);
 const std::array<std::array<float,3>,3> points{{{-1.5f,-1,0},{1.5f,-1,0},{0,1.25f,0}}};
 for(unsigned i=0;i<3;++i)
 {
  for(unsigned c=0;c<3;++c){Float(b,v+i*12+c*4,points[i][c]);Float(b,v+36+i*12+c*4,c==2?1:0);}
  // Both signed16 UVs are constant at first/last texel centres, independently.
  for(unsigned uv=0;uv<2;++uv){b[v+72+uv*12+i*4]=uv?3:0;b[v+73+uv*12+i*4]=128;b[v+74+uv*12+i*4]=0;b[v+75+uv*12+i*4]=128;}
  b[v+96+i*4]=bone;
 }
 for(unsigned n=0;n<2;++n)for(unsigned i=0;i<16;++i)Float(b,bind+n*68+4+i*4,i%5==0?1:0);
 Float(b,params+24,blend);Float(b,params+28,alpha);Set(b,params+36,lit);
 unsigned raster=0;glSetRasterState(raster,GLS_ColourWrite,1);glSetRasterState(raster,GLS_DepthTest,1);glSetRasterState(raster,GLS_DepthWrite,1);glSetRasterState(raster,GLS_DepthFunc,1);glSetRasterState(raster,GLS_Culling,0);Set(b,packet+28,raster);return b;
}
inline Blob Texture(unsigned r,unsigned g,unsigned b,unsigned a=255,bool split=false)
{
 Blob out(96);Set(out,0,1);Set(out,4,3);for(unsigned i=8;i<11;++i)out[i]=8;out[15]=4;out[17]=4;
 for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x){const auto at=(y*4+x)*2;out[32+at]=a;out[33+at]=split&&x>=2?40:r;out[64+at]=split&&x>=2?200:g;out[65+at]=split&&x>=2?80:b;}
 return out;
}
inline Blob Rlt(bool shock=false,bool split=false)
{
 Blob out(16);Set(out,0,0x50544c47);Set(out,4,shock?1:2);Set(out,8,1);Set(out,12,1);unsigned offset=0;
 const auto first=Texture(200,100,50,255,split),second=Texture(128,64,192,255,split);
 for(unsigned i=0;i<(shock?1:2);++i){Word(out,shock?0xe4457de5:0x12345678+i);Word(out,offset);Word(out,first.size());Word(out,0);offset+=first.size();}
 Append(out,first);if(!shock)Append(out,second);return out;
}
inline mscharged::RigidSkinAsset::Handle Asset(unsigned bone=0,float blend=1,float alpha=1,bool lit=true,bool shock=false)
{return mscharged::RigidSkinAsset::Decode(Model(bone,blend,alpha,lit),mscharged::HierarchyAsset::Decode(Rig(2,false,false,shock?"bowser":"rig")));}
inline mscharged::SkinPoseFrame::Handle Frame(mscharged::RigidSkinAsset::Handle asset,float shift=0,bool flip=false)
{
 auto pose=std::make_shared<mscharged::AnimationPoseFrame>();pose->hierarchy=asset->Hierarchy();pose->matrices.resize(2);
 for(auto& m:pose->matrices)m.SetIdentity();pose->matrices[1].m41=shift;
 if(flip)pose->matrices[1].m22=pose->matrices[1].m33=-1;
 mscharged::SkinPose skin(asset);return skin.Sample(pose);
}
}
