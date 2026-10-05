#pragma once
#include <dolphin/gx.h>
#include <bit>
#include <string>
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
namespace specular_gx_record {
struct Event { std::string name;std::vector<std::uint64_t> args; };
inline std::vector<Event> events;
inline unsigned begin_count=0,fail_begin=0;
template<class T>std::uint64_t Value(T value) { if constexpr(std::is_pointer_v<T>)return reinterpret_cast<std::uintptr_t>(value);else if constexpr(std::is_floating_point_v<T>)return std::bit_cast<std::uint32_t>(value);else return std::uint64_t(value); }
inline std::uint64_t Value(GXColor c){return(std::uint32_t(c.r)<<24)|(std::uint32_t(c.g)<<16)|(std::uint32_t(c.b)<<8)|c.a;}
inline std::uint64_t Value(GXColorS10 c){return(std::uint64_t(std::uint16_t(c.r))<<48)|(std::uint64_t(std::uint16_t(c.g))<<32)|(std::uint64_t(std::uint16_t(c.b))<<16)|std::uint16_t(c.a);}
template<class...T>void Record(const char* name,T... args) { events.push_back({name,{Value(args)...}});if(std::string(name)=="GXBegin"&&++begin_count==fail_begin)throw std::runtime_error("Injected genuine source GX transport rejection");}
inline void Reset(){events.clear();begin_count=0;fail_begin=0;}
}
extern "C" void __wrap_GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts){specular_gx_record::Record("GXBegin",type,vtxfmt,nverts);}
extern "C" void __wrap_GXCallDisplayList(const void* list, u32 nbytes){specular_gx_record::Record("GXCallDisplayList",list,nbytes);}
extern "C" void __wrap_GXClearVtxDesc(void){specular_gx_record::Record("GXClearVtxDesc");}
extern "C" void __wrap_GXColor1x16(u16 index){specular_gx_record::Record("GXColor1x16",index);}
extern "C" void __wrap_GXEnd(void){specular_gx_record::Record("GXEnd");}
extern "C" void __wrap_GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2){specular_gx_record::Record("GXInitLightAttn",lt_obj,a0,a1,a2,k0,k1,k2);}
extern "C" void __wrap_GXInitLightAttnA(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2){specular_gx_record::Record("GXInitLightAttnA",lt_obj,a0,a1,a2);}
extern "C" void __wrap_GXInitLightColor(GXLightObj* lt_obj, GXColor color){specular_gx_record::Record("GXInitLightColor",lt_obj,color);}
extern "C" void __wrap_GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_distance, f32 ref_brightness, GXDistAttnFn dist_func){specular_gx_record::Record("GXInitLightDistAttn",lt_obj,ref_distance,ref_brightness,dist_func);}
extern "C" void __wrap_GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z){specular_gx_record::Record("GXInitLightPos",lt_obj,x,y,z);}
extern "C" void __wrap_GXInitSpecularDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz){specular_gx_record::Record("GXInitSpecularDir",lt_obj,nx,ny,nz);}
extern "C" void __wrap_GXInitTexObj(GXTexObj* obj, const void* data, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap){specular_gx_record::Record("GXInitTexObj",obj,data,width,height,format,wrapS,wrapT,mipmap);}
extern "C" void __wrap_GXInitTexObjCI(GXTexObj* obj, const void* data, u16 width, u16 height, GXCITexFmt format, GXTexWrapMode wrapS, GXTexWrapMode wrapT, GXBool mipmap, u32 tlut){specular_gx_record::Record("GXInitTexObjCI",obj,data,width,height,format,wrapS,wrapT,mipmap,tlut);}
extern "C" void __wrap_GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod, GXAnisotropy max_aniso){specular_gx_record::Record("GXInitTexObjLOD",obj,min_filt,mag_filt,min_lod,max_lod,lod_bias,bias_clamp,do_edge_lod,max_aniso);}
extern "C" void __wrap_GXInitTlutObj(GXTlutObj* obj, const void* data, GXTlutFmt format, u16 entries){specular_gx_record::Record("GXInitTlutObj",obj,data,format,entries);}
extern "C" void __wrap_GXLoadLightObjImm(GXLightObj* lt_obj, GXLightID light){specular_gx_record::Record("GXLoadLightObjImm",lt_obj,light);}
extern "C" void __wrap_GXLoadNrmMtxImm(const void* mtx, u32 id){specular_gx_record::Record("GXLoadNrmMtxImm",mtx,id);}
extern "C" void __wrap_GXLoadPosMtxImm(const void* mtx, u32 id){specular_gx_record::Record("GXLoadPosMtxImm",mtx,id);}
extern "C" void __wrap_GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type){specular_gx_record::Record("GXLoadTexMtxImm",mtx,id,type);}
extern "C" void __wrap_GXLoadTexObj(GXTexObj* obj, GXTexMapID id){specular_gx_record::Record("GXLoadTexObj",obj,id);}
extern "C" void __wrap_GXLoadTlut(const GXTlutObj* obj, u32 idx){specular_gx_record::Record("GXLoadTlut",obj,idx);}
extern "C" void __wrap_GXNormal1x16(u16 index){specular_gx_record::Record("GXNormal1x16",index);}
extern "C" void __wrap_GXParam1u8(const u8 x){specular_gx_record::Record("GXParam1u8",x);}
extern "C" void __wrap_GXPosition1x16(u16 index){specular_gx_record::Record("GXPosition1x16",index);}
extern "C" void __wrap_GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1){specular_gx_record::Record("GXSetAlphaCompare",comp0,ref0,op,comp1,ref1);}
extern "C" void __wrap_GXSetAlphaUpdate(GXBool update_enable){specular_gx_record::Record("GXSetAlphaUpdate",update_enable);}
extern "C" void __wrap_GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool le){specular_gx_record::Record("GXSetArray",attr,data,size,stride,le);}
extern "C" void __wrap_GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op){specular_gx_record::Record("GXSetBlendMode",type,src_factor,dst_factor,op);}
extern "C" void __wrap_GXSetChanAmbColor(GXChannelID chan, GXColor amb_color){specular_gx_record::Record("GXSetChanAmbColor",chan,amb_color);}
extern "C" void __wrap_GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn){specular_gx_record::Record("GXSetChanCtrl",chan,enable,amb_src,mat_src,light_mask,diff_fn,attn_fn);}
extern "C" void __wrap_GXSetChanMatColor(GXChannelID chan, GXColor mat_color){specular_gx_record::Record("GXSetChanMatColor",chan,mat_color);}
extern "C" void __wrap_GXSetColorUpdate(GXBool update_enable){specular_gx_record::Record("GXSetColorUpdate",update_enable);}
extern "C" void __wrap_GXSetCullMode(GXCullMode mode){specular_gx_record::Record("GXSetCullMode",mode);}
extern "C" void __wrap_GXSetCurrentMtx(u32 id){specular_gx_record::Record("GXSetCurrentMtx",id);}
extern "C" void __wrap_GXSetNumChans(u8 nChans){specular_gx_record::Record("GXSetNumChans",nChans);}
extern "C" void __wrap_GXSetNumTevStages(u8 nStages){specular_gx_record::Record("GXSetNumTevStages",nStages);}
extern "C" void __wrap_GXSetNumTexGens(u8 nTexGens){specular_gx_record::Record("GXSetNumTexGens",nTexGens);}
extern "C" void __wrap_GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d){specular_gx_record::Record("GXSetTevAlphaIn",stage,a,b,c,d);}
extern "C" void __wrap_GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg){specular_gx_record::Record("GXSetTevAlphaOp",stage,op,bias,scale,clamp,out_reg);}
extern "C" void __wrap_GXSetTevColor(GXTevRegID id, GXColor color){specular_gx_record::Record("GXSetTevColor",id,color);}
extern "C" void __wrap_GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d){specular_gx_record::Record("GXSetTevColorIn",stage,a,b,c,d);}
extern "C" void __wrap_GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out_reg){specular_gx_record::Record("GXSetTevColorOp",stage,op,bias,scale,clamp,out_reg);}
extern "C" void __wrap_GXSetTevColorS10(GXTevRegID id, GXColorS10 color){specular_gx_record::Record("GXSetTevColorS10",id,color);}
extern "C" void __wrap_GXSetTevDirect(GXTevStageID tev_stage){specular_gx_record::Record("GXSetTevDirect",tev_stage);}
extern "C" void __wrap_GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel){specular_gx_record::Record("GXSetTevKAlphaSel",stage,sel);}
extern "C" void __wrap_GXSetTevKColor(GXTevKColorID id, GXColor color){specular_gx_record::Record("GXSetTevKColor",id,color);}
extern "C" void __wrap_GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel){specular_gx_record::Record("GXSetTevKColorSel",stage,sel);}
extern "C" void __wrap_GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color){specular_gx_record::Record("GXSetTevOrder",stage,coord,map,color);}
extern "C" void __wrap_GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel){specular_gx_record::Record("GXSetTevSwapMode",stage,ras_sel,tex_sel);}
extern "C" void __wrap_GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx, GXBool normalize, u32 postmtx){specular_gx_record::Record("GXSetTexCoordGen2",dst_coord,func,src_param,mtx,normalize,postmtx);}
extern "C" void __wrap_GXSetTexCoordScaleManually(GXTexCoordID coord, GXBool enable, u16 ss, u16 ts){specular_gx_record::Record("GXSetTexCoordScaleManually",coord,enable,ss,ts);}
extern "C" void __wrap_GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac){specular_gx_record::Record("GXSetVtxAttrFmt",vtxfmt,attr,cnt,type,frac);}
extern "C" void __wrap_GXSetVtxDesc(GXAttr attr, GXAttrType type){specular_gx_record::Record("GXSetVtxDesc",attr,type);}
extern "C" void __wrap_GXSetZCompLoc(GXBool before_tex){specular_gx_record::Record("GXSetZCompLoc",before_tex);}
extern "C" void __wrap_GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable){specular_gx_record::Record("GXSetZMode",compare_enable,func,update_enable);}
extern "C" void __wrap_GXTexCoord1x16(u16 index){specular_gx_record::Record("GXTexCoord1x16",index);}
