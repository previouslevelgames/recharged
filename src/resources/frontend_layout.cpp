#include "resources/frontend_layout.h"
#include "resources/frontend_images.h"
#include "Game/FE/FrontendLayoutSteps.h"
#include "Game/FE/FrontendImageSteps.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

namespace mscharged::resources
{
void ValidateFrontendImageTexture(const Texture& texture)
{
    constexpr unsigned formats[]={4,5,14,6,1,0,1,3,9};
    Require(texture.width&&texture.width<=1024&&texture.height&&texture.height<=1024
        &&texture.game_format<std::size(formats)&&texture.gx_format==formats[texture.game_format],
        "Invalid frontend image texture dimensions or format");
    unsigned maximum=1;for(unsigned size=std::max(texture.width,texture.height);size>1;size>>=1)++maximum;
    Require(texture.levels&&texture.levels<=maximum,"Invalid frontend image mip levels");
    const auto format=texture.game_format;
    Require((format==8&&texture.palette_entries&&texture.palette_entries<=256)||(format!=8&&!texture.palette_entries),
        "Invalid frontend image palette count");
    std::size_t bytes=0;
    for(unsigned level=0;level<texture.levels;++level)
    {
        const auto w=std::max(1u,unsigned(texture.width)>>level),h=std::max(1u,unsigned(texture.height)>>level);
        const unsigned bw=(format==2||format==4||format==5||format==6||format==8)?8:4;
        const unsigned bh=(format==2||format==5)?8:4;
        bytes+=((w+bw-1)/bw)*((h+bh-1)/bh)*(format==3?64:32);
    }
    Require(texture.pixels.size()==bytes&&texture.palette.size()==std::size_t(texture.palette_entries)*2,
        "Frontend image texture storage does not match its metadata");
}
namespace
{
constexpr unsigned MaximumNodes=16384;
void Scalar(float value)
{ Require(std::isfinite(value)&&std::abs(value)<=1e7f,"Frontend layout scalar exceeds its finite range"); }
struct Attributes
{
    feVector3 position,rotation,scale,pivot;
    const feVector3& GetPosition() const{return position;}
    const feVector3& GetRotation() const{return rotation;}
    const feVector3& GetScale() const{return scale;}
    const feVector3& GetPivot() const{return pivot;}
};
FrontendAttributes Effective(const FrontendInstance& instance,const FrontendLibraryObject& object)
{
    auto value=object.attributes;
    if(instance.overload_flags&1)value.position=instance.attributes.position;
    if(instance.overload_flags&2)value.rotation=instance.attributes.rotation;
    if(instance.overload_flags&4)value.scale=instance.attributes.scale;
    if(instance.overload_flags&8)value.pivot=instance.attributes.pivot;
    if(instance.overload_flags&16)value.colour=instance.attributes.colour;
    for(const auto& vector:{value.position,value.rotation,value.scale,value.pivot})for(float v:vector)Scalar(v);
    return value;
}
nlMatrix4 Combine(const FrontendAttributes& value,const nlMatrix4& parent)
{
    Attributes attributes;
    for(unsigned i=0;i<3;++i)
    {
        attributes.position.e[i]=value.position[i];attributes.rotation.e[i]=value.rotation[i];
        attributes.scale.e[i]=value.scale[i];attributes.pivot.e[i]=value.pivot[i];
    }
    nlMatrix4 matrix;FrontendPushTransform(&attributes,parent,matrix);
    // Bounded input and parent entries keep intermediate products within finite
    // float range; reject large accumulated results before recursing/drawing.
    for(float v:matrix.e)Scalar(v);
    return matrix;
}
bool ValidTime(float time,float start,float duration)
{
    Scalar(time);Scalar(start);Scalar(duration);Require(duration>=0,"Negative frontend instance duration");
    // TLInstance::IsValidAtTime including its two rounded subtractions and nlNear.
    const auto atLeast=[](float value,float bound){return value-bound>.0001f||nlNear(value,bound);};
    const auto atMost=[](float value,float bound){return bound-value>.0001f||nlNear(value,bound);};
    return atLeast(time,start)&&atMost(time-start,duration);
}
struct PlainFont
{
    const FrontendFont& font;
    TextMetrics m_Metrics{};
    std::map<unsigned short,const FontGlyph*> glyphs;
    explicit PlainFont(const FrontendFont& value):font(value)
    {
        Require(font.height&&font.height<=32767&&font.ascent<=32767&&font.internal_leading<=32767
            &&font.glyphs.size()<=65536,"Invalid frontend textbox font metrics");
        m_Metrics.Height=font.height;
        for(const auto& [key,glyph]:font.glyphs)
            Require(glyph.font_char&&glyphs.emplace(glyph.font_char,&glyph).second,"Null or duplicate font character index");
    }
    unsigned long GetCharWidth(unsigned short current,unsigned short previous) const
    {
        const auto found=glyphs.find(current),prior=glyphs.find(previous);
        Require(found!=glyphs.end(),"Textbox references a missing font character");
        return font.CharacterWidth(*found->second,prior==glyphs.end()?nullptr:prior->second);
    }
};
// Formatted strings reject before the shared original algorithm is entered.
// Keep an explicit failure boundary if that precondition changes in the future.
struct UnavailableEscape
{
    // Original nlTextEscape.cpp's sentinel; parser operations remain unavailable.
    static constexpr unsigned short ESCAPE_BEGIN=0x007b;
    const unsigned short* m_pEnd=nullptr;
    explicit UnavailableEscape(const unsigned short*){throw UnsupportedResource("Frontend text escapes are not selected");}
    unsigned GetType() const{throw UnsupportedResource("Frontend text escapes are not selected");}
};
struct Rows
{
    const PlainFont* pFont=nullptr;
    const unsigned short* String=nullptr;
    const nlMatrix4* pMatrix=nullptr;
    unsigned long DrawOptions=0;
    unsigned short RowCount=0;
    signed short YOffset=0;
    Row Rows[17]{};
};
FontLayout TextBox(std::shared_ptr<const FrontendFont> font,std::u16string_view text,
    std::array<float,2> size,std::uint32_t options)
{
    for(float v:size)Require(std::isfinite(v)&&v>=0&&v<=32767,"Textbox size exceeds checked signed offsets");
    PlainFont adapter(*font);std::vector<unsigned short> converted;converted.reserve(text.size()+1);
    for(char16_t ch:text)
    {
        const auto found=font->glyphs.find(ch);
        Require(found!=font->glyphs.end(),"Frontend text contains an unavailable authored glyph");
        converted.push_back(found->second.font_char);
    }
    converted.push_back(0);
    Rows rows;
    FrontendProcessString<PlainFont,Rows,UnavailableEscape>(converted.data(),&adapter,{size[0],size[1]},options|nlTextBox::FlipY,nullptr,rows);
    const float x=(options&15)==nlTextBox::AlignCenter?-size[0]/2:(options&15)==nlTextBox::AlignRight?-size[0]:0;
    const float y=(options&0xf0)==nlTextBox::VAlignCenter?size[1]/2:(options&0xf0)==nlTextBox::VAlignBottom?size[1]:0;
    const float leading=options&nlTextBox::UseFullHeight?font->internal_leading:0;
    FontLayout output;output.font=std::move(font);output.height=float(rows.RowCount)*output.font->height;
    for(unsigned row=0;row<rows.RowCount;++row)
    {
        const auto begin=rows.Rows[row].FirstChar,end=rows.Rows[row+1].FirstChar;
        Require(begin<=end&&end<=text.size(),"Original textbox returned an invalid row range");
        auto line=LayoutFrontendText(output.font,text.substr(begin,end-begin));
        const float dx=x+rows.Rows[row].XOffset;
        // TLTextInstance::Render's anchor followed by original DrawString's
        // FlipY, integer YOffset, ascent/leading and one Height per row.
        const float dy=-y+rows.YOffset+leading+float(row)*output.font->height;
        for(auto& quad:line.quads)
        {quad.left+=dx;quad.right+=dx;quad.top+=dy;quad.bottom+=dy;output.quads.push_back(quad);}
        output.width=std::max(output.width,line.width);
    }
    return output;
}
class Builder
{
    const FrontendScene& scene_;
    const Localization& localization_;
    std::map<std::uint32_t,const FrontendInstance*> instances_;
    std::map<std::uint32_t,const FrontendLibraryObject*> library_;
    std::map<std::uint32_t,const FrontendResource*> resources_;
    std::map<std::uint32_t,const FrontendSlide*> slides_;
    std::map<std::uint32_t,std::shared_ptr<const FrontendFont>> fonts_;
    const FrontendImageCatalog& images_;
    std::set<std::uint32_t> active_;
    unsigned visited_=0;
    std::size_t glyphs_=0;
    FrontendLayoutFrame result_;
    nlMatrix4 scene_view_;
    void Image(const FrontendInstance& instance,const FrontendLibraryObject& object,const nlMatrix4& matrix,
        const std::array<float,4>& colour)
    {
        if(!instance.resource){++result_.unavailable["image resource assigned by a scene handler"];return;}
        Require(resources_.contains(*instance.resource),"Frontend image resource is absent");
        const auto& resource=*resources_.at(*instance.resource);
        Require(resource.type==0,"Frontend image resource is not a texture");
        // Dynamic resources require their real callbacks/targets even if a
        // caller supplies an ordinary texture under the same hash.
        if(resource.hash==FrontendNameHash("movie")||resource.hash==FrontendNameHash("target/grab_texture"))
        {++result_.unavailable["dynamic movie or grab image"];return;}
        if(const auto unavailable=images_.unavailable.find(resource.hash);unavailable!=images_.unavailable.end())
        {++result_.unavailable[unavailable->second];return;}
        const auto found=images_.textures.find(resource.hash);
        if(found==images_.textures.end()){++result_.unavailable["image texture outside the supplied set"];return;}
        Require(found->second&&found->second->id==resource.hash,"Null or mismatched frontend image texture");
        ValidateFrontendImageTexture(*found->second);
        Require(instance.image_blend<=7,"Invalid frontend image blend mode");
        struct UV
        {
            std::array<float,4> values;
            float GetUVX() const{return values[0];} float GetUVY() const{return values[1];}
            float GetUVWidth() const{return values[2];} float GetUVHeight() const{return values[3];}
        } uv{object.attributes.uv};
        for(unsigned i=0;i<4;++i){if(instance.overload_flags&(0x40u<<i))uv.values[i]=instance.attributes.uv[i];Scalar(uv.values[i]);}
        nlVector2 coordinates[4];FrontendImageUV(&uv,found->second->width,found->second->height,coordinates);
        FrontendLayoutImage entry;
        entry.instance=instance.offset;entry.priority=instance.priority;entry.name=instance.name;
        entry.texture=found->second;entry.blend=instance.image_blend;
        for(unsigned i=0;i<4;++i)
        {
            Scalar(coordinates[i].x);Scalar(coordinates[i].y);
            for(float value:{coordinates[i].x,coordinates[i].y})
                Require(value*1024.f>=-32768.f&&value*1024.f<32768.f,"Frontend image UV exceeds original signed16 storage");
            entry.vertices[i]={FrontendImageQuadPositions[i].x,FrontendImageQuadPositions[i].y,coordinates[i].x,coordinates[i].y};
            entry.colour[i]=static_cast<std::uint8_t>(static_cast<int>(colour[i]*255.f));
        }
        // Original images copy the inherited matrix and add view translation;
        // they do not perform the full view multiplication used by text.
        entry.transform={matrix.m11,-matrix.m12,0,0,matrix.m21,-matrix.m22,0,0,0,0,1,0,
            matrix.m41+scene_view_.m41+320,240-(matrix.m42+scene_view_.m42),0,1};
        result_.entries.push_back(std::move(entry));
    }
    void Enter(std::uint32_t id,unsigned depth)
    {
        Require(depth<=64&&++visited_<=MaximumNodes,"Frontend frame exceeds its traversal budget");
        Require(active_.insert(id).second,"Cyclic frontend frame dependency");
    }
    void Text(const FrontendInstance& instance,const FrontendLibraryObject& object,const nlMatrix4& matrix,
        const std::array<float,4>& colour)
    {
        if(instance.text_scissor){++result_.unavailable["scissored text component"];return;}
        if(!object.resource){++result_.unavailable["font assigned by a scene handler"];return;}
        Require(resources_.contains(*object.resource),"Frontend text font resource is absent");
        const auto& resource=*resources_.at(*object.resource);
        Require(resource.type==1,"Frontend text resource is not a font");
        const auto selected=fonts_.find(resource.hash);
        if(selected==fonts_.end()){++result_.unavailable["font alias outside the supplied set"];return;}
        const auto& text=instance.text_overload_flags&8?localization_.Get(instance.localization_hash):instance.text;
        if(text.empty()){++result_.unavailable["empty or handler-assigned text"];return;}
        Require(text.size()<=4096,"Frontend textbox exceeds its string budget");
        if(std::any_of(text.begin(),text.end(),[](char16_t c){return c<32||c==127||c=='{'||c=='}'||(c>=0xd800&&c<=0xdfff);}))
        {++result_.unavailable["formatted, control or surrogate text"];return;}
        if((instance.draw_options&~0x1e33u)||(instance.draw_options&15)>2||(instance.draw_options&0xf0)>0x20)
        {++result_.unavailable["unsupported textbox draw options"];return;}
        if(std::any_of(text.begin(),text.end(),[&](char16_t c){return !selected->second->glyphs.contains(c);}))
        {++result_.unavailable["missing authored font glyph"];return;}
        FrontendLayoutText entry;
        entry.instance=instance.offset;entry.priority=instance.priority;entry.name=instance.name;entry.text=text;
        entry.layout=TextBox(selected->second,text,instance.text_box,instance.draw_options);
        Require(entry.layout.quads.size()<=262144-glyphs_,"Frontend frame glyph budget exceeded");glyphs_+=entry.layout.quads.size();
        nlMatrix4 text_matrix;nlMultMatrices(text_matrix,matrix,scene_view_);
        entry.transform={text_matrix.m11,-text_matrix.m12,0,0,-text_matrix.m21,text_matrix.m22,0,0,0,0,1,0,
            text_matrix.m41+320,240-text_matrix.m42,0,1};
        for(unsigned i=0;i<4;++i)entry.colour[i]=static_cast<std::uint8_t>(static_cast<int>(colour[i]*255.f));
        result_.entries.push_back(std::move(entry));
    }
    void Instance(std::uint32_t id,float time,const nlMatrix4& parent,std::array<float,4> colour,unsigned depth)
    {
        Enter(id,depth);Require(instances_.contains(id),"Frontend slide references a missing instance");
        const auto& instance=*instances_.at(id);
        Require(instance.library&&library_.contains(*instance.library),"Frontend instance library is absent");
        const auto& object=*library_.at(*instance.library);
        Require(instance.type>=1&&instance.type<=5&&object.type+1==instance.type,"Frontend instance/library type mismatch");
        if(!ValidTime(time,instance.start,instance.duration)||!instance.visible||!object.attributes.visible)
        {++result_.hidden;active_.erase(id);return;}
        const auto attributes=Effective(instance,object);
        if(attributes.rotation[0]!=0||attributes.rotation[1]!=0||attributes.position[2]!=0||attributes.pivot[2]!=0||attributes.scale[2]!=1)
        {++result_.unavailable["nonplanar instance branch"];active_.erase(id);return;}
        const auto matrix=Combine(attributes,parent);
        for(unsigned i=0;i<4;++i)colour[i]=(attributes.colour[i]*colour[i])/255.f;
        if(instance.type==3)Text(instance,object,matrix,colour);
        else if(instance.type==4)
        {
            Require(!object.active_slide||std::find(object.slides.begin(),object.slides.end(),*object.active_slide)!=object.slides.end(),
                "Active component slide is outside its ring");
            if(object.active_slide)Slide(*object.active_slide,matrix,colour,depth+1);
        }
        else if(instance.type==2)Image(instance,object,matrix,colour);
        for(auto child:instance.children)Instance(child,time,matrix,colour,depth+1);
        active_.erase(id);
    }
    void Slide(std::uint32_t id,const nlMatrix4& matrix,const std::array<float,4>& colour,unsigned depth)
    {
        Enter(id,depth);Require(slides_.contains(id),"Frontend active slide is absent");const auto& slide=*slides_.at(id);
        Scalar(slide.time);
        if(slide.animated&&!slide.animation_evaluated)++result_.unavailable["animated slide branch"];
        else for(auto child:slide.children)Instance(child,slide.time,matrix,colour,depth+1);
        active_.erase(id);
    }
public:
    Builder(const FrontendScene& scene,const Localization& loc,std::span<const std::shared_ptr<const FrontendFont>> fonts,
        const FrontendImageCatalog& images)
        :scene_(scene),localization_(loc),images_(images)
    {
        // Original FEScene constructor and FERender text-matrix composition.
        glMatrixLookAt(scene_view_,{0,0,600},{0,0,0},{0,1,0});
        Require(scene.instances.size()<=MaximumNodes&&scene.library.size()<=MaximumNodes&&scene.slides.size()<=MaximumNodes
            &&scene.resources.size()<=MaximumNodes&&fonts.size()<=256&&images.textures.size()<=4096&&images.unavailable.size()<=4096,
            "Frontend layout input budget exceeded");
        for(const auto& [id,texture]:images.textures)Require(!images.unavailable.contains(id),"Conflicting frontend image catalog entries");
        std::set<std::uint32_t> ids;
        const auto index=[&](const auto& input,auto& output){for(const auto& value:input){Require(ids.insert(value.offset).second,"Duplicate frontend record ID");output.emplace(value.offset,&value);}};
        index(scene.instances,instances_);index(scene.library,library_);index(scene.resources,resources_);index(scene.slides,slides_);
        for(const auto& font:fonts)Require(font&&fonts_.emplace(font->alias,font).second,"Null or duplicate frontend font alias");
    }
    FrontendLayoutFrame Build(FrontendReference selected)
    {
        if(!selected)selected=scene_.active_slide;
        Require(!selected||std::find(scene_.presentation_slides.begin(),scene_.presentation_slides.end(),*selected)!=scene_.presentation_slides.end(),
            "Selected presentation slide is outside its ring");
        if(selected){nlMatrix4 identity;identity.SetIdentity();Slide(*selected,identity,{1,1,1,1},0);}
        // Original main selects eCLV_Anark, whose GLViewSort_Reverse dispatches
        // packets in reverse submission order. Stored m_priority is not read by
        // FERender; sorting it would change the original painter order.
        std::reverse(result_.entries.begin(),result_.entries.end());
        return std::move(result_);
    }
};
}
std::size_t FrontendLayoutFrame::TextCount() const
{return std::count_if(entries.begin(),entries.end(),[](const auto& entry){return std::holds_alternative<FrontendLayoutText>(entry);});}
std::size_t FrontendLayoutFrame::ImageCount() const{return entries.size()-TextCount();}
FrontendLayoutFrame BuildFrontendLayout(const FrontendScene& scene,const Localization& loc,
    std::span<const std::shared_ptr<const FrontendFont>> fonts,FrontendReference selected)
{return Builder(scene,loc,fonts,FrontendImageCatalog{}).Build(selected);}
FrontendLayoutFrame BuildFrontendLayout(const FrontendScene& scene,const Localization& loc,
    std::span<const std::shared_ptr<const FrontendFont>> fonts,FrontendReference selected,const FrontendImageCatalog& images)
{return Builder(scene,loc,fonts,images).Build(selected);}
}
