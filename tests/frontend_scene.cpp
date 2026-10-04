#include "resources/frontend_scene.h"
#include "frontend_scene_fingerprint.h"
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <iomanip>
#include <stdexcept>
using namespace mscharged::resources;
namespace
{
unsigned checks=0;
void Check(bool good,const char* why){++checks;if(!good)throw std::runtime_error(why);}
template<class F>void Reject(F f){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid FEN accepted");}
void Word(std::vector<std::uint8_t>& b,std::size_t at,std::uint32_t value){for(int i=3;i>=0;--i)b.at(at+3-i)=std::uint8_t(value>>(i*8));}
void Float(std::vector<std::uint8_t>& b,std::size_t at,float v){Word(b,at,std::bit_cast<std::uint32_t>(v));}
struct Fixture
{
 std::vector<std::uint8_t> data=std::vector<std::uint8_t>(0x360);std::vector<std::uint32_t> pointers;
 unsigned Append(unsigned size){const unsigned at=data.size();data.resize(at+((size+3)&~3u));return at;}
 void Ptr(unsigned at,unsigned to){Word(data,at,to);pointers.push_back(at);}
 void Self(unsigned at){Ptr(at,at);Ptr(at+4,at);}
 void Name(unsigned at,const char* name){std::copy_n(name,std::char_traits<char>::length(name),data.begin()+at);}
 void Attributes(unsigned at){data[at+0x30]=1;std::fill_n(data.begin()+at+0x31,4,255);for(int i=0;i<3;++i)Float(data,at+24+i*4,1);Float(data,at+64,1);Float(data,at+68,1);}
 Fixture()
 {
  Ptr(4,0x18);Ptr(8,0x330);Ptr(12,0x2a0);Word(data,16,0x1234);Word(data,20,1);
  Ptr(0x18,0x30);Ptr(0x1c,0x30);
  Self(0x30);Ptr(0x38,0x80);Ptr(0x3c,UINT32_MAX);Float(data,0x44,10);Word(data,0x4c,1);Name(0x50,"First");Word(data,0x70,1);
  Self(0x80);Ptr(0x88,0x188);Ptr(0x8c,0x110);Float(data,0x94,10);Name(0x98,"LayerInstance");Attributes(0xbc);Word(data,0x108,1);data[0x10e]=1;
  Ptr(0x110,0x2a0);Ptr(0x114,0x2a0);Attributes(0x118);Name(0x164,"Layer");
  Self(0x188);Ptr(0x190,UINT32_MAX);Ptr(0x194,0x2a0);Float(data,0x19c,10);Name(0x1a0,"TextInstance");Attributes(0x1c4);Word(data,0x210,3);data[0x216]=1;
  Float(data,0x220,200);Float(data,0x224,40);Ptr(0x28c,0x350);
  Ptr(0x2a0,0x110);Ptr(0x2a4,0x110);Attributes(0x2a8);Name(0x2f4,"Text");Word(data,0x314,2);Ptr(0x318,0x330);Float(data,0x320,200);Float(data,0x324,40);
  Self(0x330);Word(data,0x338,1);Word(data,0x33c,0x1122);
  for(unsigned i=0;i<4;++i)data[0x351+2*i]="Test"[i];
 }
 std::vector<std::uint8_t> File()const
 {
  std::vector<std::uint8_t> file(16+data.size()+pointers.size()*4);Word(file,0,0x46454e4c);Word(file,4,1);Word(file,8,data.size());Word(file,12,pointers.size()*4);std::copy(data.begin(),data.end(),file.begin()+16);
  for(unsigned i=0;i<pointers.size();++i)Word(file,16+data.size()+i*4,pointers[i]);
  return file;
 }
};
void LinkRing(Fixture& f,const std::vector<unsigned>& nodes)
{
 for(unsigned i=0;i<nodes.size();++i){f.Ptr(nodes[i],nodes[(i+1)%nodes.size()]);f.Ptr(nodes[i]+4,nodes[(i+nodes.size()-1)%nodes.size()]);}
}
Fixture Chain(unsigned count)
{
 Fixture f;unsigned parent=0x80;
 for(unsigned i=0;i<count;++i)
 {
  const auto at=f.Append(0x90);f.Self(at);f.Ptr(at+12,0x110);Word(f.data,at+0x88,1);
  if(parent==0x80)Word(f.data,parent+8,at);else f.Ptr(parent+8,at);
  parent=at;
 }
 if(parent!=0x80)f.Ptr(parent+8,0x188);
 return f;
}
Fixture Components(unsigned count,bool cycle=false)
{
 Fixture f;std::vector<unsigned> libraries{0x110,0x2a0},components,slides,instances;
 for(unsigned i=0;i<count;++i)
 {components.push_back(f.Append(0x80));slides.push_back(f.Append(0x48));instances.push_back(f.Append(0x90));libraries.push_back(components.back());}
 // Replace the original two-node library ring's four relocation entries.
 std::erase_if(f.pointers,[](auto p){return p==0x110||p==0x114||p==0x2a0||p==0x2a4;});
 LinkRing(f,libraries);Word(f.data,12,libraries.back());
 for(unsigned i=0;i<count;++i)
 {
  const auto lib=components[i],slide=slides[i],instance=instances[i];
  Word(f.data,lib+0x74,3);f.Ptr(lib+0x78,slide);f.Ptr(lib+0x7c,slide);
  f.Self(slide);f.Ptr(slide+8,instance);f.Self(instance);
  const bool next=i+1<count||cycle;Word(f.data,instance+0x88,next?4:1);
  f.Ptr(instance+12,next?components[(i+1)%count]:0x110);
 }
 Word(f.data,0x108,4);Word(f.data,0x8c,components.front());Word(f.data,0x88,UINT32_MAX);
 return f;
}
Fixture Wide(unsigned count,bool text=false)
{
 Fixture f;std::vector<unsigned> nodes;
 for(unsigned i=0;i<count;++i)
 {
  const auto at=f.Append(text?0x114:0x90);nodes.push_back(at);
  f.Ptr(at+12,text?0x2a0:0x110);Word(f.data,at+0x88,text?3:1);
 }
 LinkRing(f,nodes);Word(f.data,0x38,nodes.back());
 if(text)
 {
  const auto string=f.Append(8192*2+2);
  for(unsigned i=0;i<8192;++i)f.data[string+2*i+1]='a';
  for(auto node:nodes)f.Ptr(node+0x104,string);
 }
 return f;
}
void GraphBounds()
{
 Check(ReadFrontendScene(Chain(60).File()).instances.size()==62,"Legal child depth rejected");
 Reject([]{ReadFrontendScene(Chain(61).File());});
 Check(ReadFrontendScene(Components(20).File()).library.size()==22,"Legal component depth rejected");
 Reject([]{ReadFrontendScene(Components(21).File());});
 Reject([]{ReadFrontendScene(Components(1,true).File());});
 Reject([]{ReadFrontendScene(Components(3,true).File());});
 {
  Fixture f;const auto other=f.Append(0x90);f.Ptr(other+12,0x110);f.Ptr(other+8,0x188);Word(f.data,other+0x88,1);
  std::erase_if(f.pointers,[](auto p){return p==0x80||p==0x84;});LinkRing(f,{0x80,other});Word(f.data,0x38,other);
  Reject([&]{ReadFrontendScene(f.File());});
 }
 {
  Fixture f;const auto other=f.Append(0x48);f.Ptr(other+8,0x80);
  std::erase_if(f.pointers,[](auto p){return p==0x30||p==0x34;});LinkRing(f,{0x30,other});Word(f.data,0x18,other);
  Reject([&]{ReadFrontendScene(f.File());});
 }
 Check(ReadFrontendScene(Wide(16378).File()).instances.size()==16378,"Legal unique node budget rejected");
 Reject([]{ReadFrontendScene(Wide(16379).File());});
 Check(ReadFrontendScene(Wide(1024,true).File()).instances.size()==1024,"Legal decoded text budget rejected");
 Reject([]{ReadFrontendScene(Wide(1025,true).File());});
}
void Generated()
{
 Fixture f;auto file=f.File();const auto scene=ReadFrontendScene(file);
 Check(scene.id==0x1234&&scene.resources.size()==1&&scene.library.size()==2&&scene.instances.size()==2&&scene.slides.size()==1,"FEN graph counts differ");
 Check(scene.active_slide==0x30&&scene.presentation_slides==std::vector<std::uint32_t>{0x30},"FEN presentation references differ");
 Check(scene.slides[0].name=="First"&&scene.slides[0].children==std::vector<std::uint32_t>{0x80},"FEN slide order differs");
 const auto text=std::find_if(scene.instances.begin(),scene.instances.end(),[](auto& n){return n.type==3;});
 Check(text!=scene.instances.end()&&text->text==u"Test"&&text->library==0x2a0,"FEN text/reference differs");
 Check(text->attributes.visible&&text->attributes.colour[0]==255&&text->attributes.scale[0]==1,"FEN attributes differ");
 Check(!text->text_scissor&&text->text_scissor_box==std::array<std::uint16_t,4>{},"Default FEN text scissor differs");
 {
  Fixture scissor;const auto at=text->offset;scissor.data[at+0x108]=1;
  Word(scissor.data,at+0x10a,0x000a0014);Word(scissor.data,at+0x10e,0x001e0028);
  const auto decoded=ReadFrontendScene(scissor.File());
  const auto value=std::find_if(decoded.instances.begin(),decoded.instances.end(),[&](auto& n){return n.offset==at;});
  Check(value->text_scissor&&value->text_scissor_box==std::array<std::uint16_t,4>{10,20,30,40},"FEN text scissor was not retained");
  scissor.data[at+0x108]=2;Reject([&]{ReadFrontendScene(scissor.File());});
 }
 std::fill(file.begin(),file.end(),0);Check(text->text==u"Test","FEN result borrows input storage");
 // Every truncation is rejected, including partial relocation words.
 for(std::size_t cut=0;cut<f.File().size();++cut){auto truncated=f.File();truncated.resize(cut);Reject([&]{ReadFrontendScene(truncated);});}
 auto mutate=[&](auto fn){Fixture bad;fn(bad);Reject([&]{ReadFrontendScene(bad.File());});};
 mutate([](auto& b){Word(b.data,0x1c4,0);b.pointers.push_back(0x1c4);});
 mutate([](auto& b){Word(b.data,0x33c,8);b.pointers.push_back(0x33c);});
 mutate([](auto& b){Word(b.data,0x1a0,20);b.pointers.push_back(0x1a0);});
 mutate([](auto& b){Word(b.data,0x28c,0);}); // text aliases the package header
 mutate([](auto& b){b.pointers.push_back(4);});mutate([](auto& b){b.pointers[0]=1;});mutate([](auto& b){b.pointers[0]=0x360;});
 mutate([](auto& b){Word(b.data,4,0x360);});mutate([](auto& b){b.pointers.erase(b.pointers.begin());});
 mutate([](auto& b){Word(b.data,20,2);});mutate([](auto& b){Word(b.data,0x114,0x110);});
 mutate([](auto& b){Word(b.data,0x88,0x80);});mutate([](auto& b){Word(b.data,0x194,0x110);});
 mutate([](auto& b){Word(b.data,0x318,0x30);});mutate([](auto& b){Word(b.data,0x1c,0x80);});
 mutate([](auto& b){Word(b.data,0x28c,0x351);});mutate([](auto& b){Word(b.data,0x350,0xd8000041);});
 mutate([](auto& b){Word(b.data,0x350,0xdc000041);});mutate([](auto& b){std::fill(b.data.begin()+0x350,b.data.end(),0x42);});
 mutate([](auto& b){std::fill_n(b.data.begin()+0x50,32,'a');});mutate([](auto& b){Word(b.data,0x210,6);});
 mutate([](auto& b){b.data[0x216]=2;});mutate([](auto& b){Float(b.data,0x44,-1);});mutate([](auto& b){Word(b.data,0x1c4,0x7fc00000);});
 file=f.File();file.push_back(0);Reject([&]{ReadFrontendScene(file);});file=f.File();Word(file,4,2);Reject([&]{ReadFrontendScene(file);});
 for(unsigned cut:{0u,7u,15u,100u}){file=f.File();file.resize(cut);Reject([&]{ReadFrontendScene(file);});}
 Fixture unicode;Word(unicode.data,0x350,0xd83dde00);Word(unicode.data,0x354,0);auto uni=ReadFrontendScene(unicode.File());
 Check(std::any_of(uni.instances.begin(),uni.instances.end(),[](auto& n){return n.text==u"\U0001f600";}),"FEN surrogate pair changed");
}
void TextBounds()
{
 for(unsigned count:{8191u,8192u,8193u})
 {
  Fixture f;f.data.resize((0x350+2*(count+1)+3)&~3u);
  std::fill(f.data.begin()+0x350,f.data.end(),0);
  for(unsigned i=0;i<count;++i)f.data[0x351+2*i]='a';
  if(count>8192)Reject([&]{ReadFrontendScene(f.File());});
  else{const auto value=ReadFrontendScene(f.File());Check(value.instances.front().text.size()==count,"Exact string budget changed");}
 }
 Fixture f;f.data.resize((0x350+2*8194+3)&~3u);std::fill(f.data.begin()+0x350,f.data.end(),0);
 for(unsigned i=0;i<8191;++i)f.data[0x351+2*i]='a';
 Word(f.data,0x350+2*8191,0xd83dde00);Reject([&]{ReadFrontendScene(f.File());});
}
void ImageFields()
{
 Fixture f;
 // Reuse the checked graph with an image instance/library/texture resource.
 // The exported texture handle and dimensions are deliberately unrelated to
 // the host resource; only the resource reference and blend word are retained.
 Word(f.data,0x210,2);Word(f.data,0x314,1);Word(f.data,0x338,0);
 f.Ptr(0x218,0x330);Word(f.data,0x21c,0xf1234567);
 Word(f.data,0x348,0xdeadbeef);Word(f.data,0x34c,0x01230456);
 std::erase(f.pointers,0x28c);
 const auto decoded=ReadFrontendScene(f.File());
 const auto image=std::find_if(decoded.instances.begin(),decoded.instances.end(),[](auto& n){return n.type==2;});
 Check(image!=decoded.instances.end()&&image->resource==0x330,"FEN image resource reference differs");
 Check(image->image_blend==0xf1234567,"FEN image blend word was truncated or discarded");
 Check(decoded.resources.front().type==0,"FEN image texture type differs");
 f.pointers.push_back(0x21c);Word(f.data,0x21c,0x330);
 Reject([&]{ReadFrontendScene(f.File());}); // Blend state is scalar, never a pointer.
}
void Owned(const std::filesystem::path& folder)
{
 std::size_t files=0,slides=0,instances=0,libraries=0,resources=0,animated=0;
 for(const auto& entry:std::filesystem::directory_iterator(folder))if(entry.path().extension()==".fen")
 {
  std::ifstream in(entry.path(),std::ios::binary);std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
  FrontendScene scene;
  try{scene=ReadFrontendScene(bytes);}catch(const std::exception& e){throw std::runtime_error(entry.path().filename().string()+": "+e.what());}
  ++files;slides+=scene.slides.size();instances+=scene.instances.size();libraries+=scene.library.size();resources+=scene.resources.size();
  for(const auto& slide:scene.slides)animated+=slide.animated;
  std::cout<<entry.path().filename().string()<<'\t'<<scene.slides.size()<<'\t'<<scene.instances.size()<<'\t'<<scene.library.size()<<'\t'<<scene.resources.size()<<'\t'<<std::hex<<std::setw(16)<<std::setfill('0')<<FrontendSceneFingerprint(scene)<<std::dec<<'\n';
 }
 Check(files==75,"Owned FEN file count changed");std::cout<<"Owned FEN: "<<files<<" files, "<<slides<<" slides ("<<animated<<" animated), "<<instances<<" instances, "<<libraries<<" library objects, "<<resources<<" resource references\n";
}
}
int main(int argc,char** argv)
{
 try{Generated();GraphBounds();TextBounds();ImageFields();if(argc==2)Owned(argv[1]);std::cout<<checks<<" frontend graph checks passed\n";}
 catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<" (check "<<checks<<")\n";return 1;}
}
