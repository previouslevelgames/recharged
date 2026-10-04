#pragma once
#include "resources/binary_reader.h"
#include "NL/FrontendTextEscapeSteps.h"
#include <string_view>

namespace mscharged::resources
{
inline bool FrontendParagraphAt(std::u16string_view text,std::size_t at)
{return at<=text.size()&&text.size()-at>=3&&text.substr(at,3)==u"{p}";}
// Callers validate the entire retained string before invoking the original
// pointer parser. Only the exact original paragraph token enters that parser.
struct FrontendParagraphEscape
{
    static constexpr unsigned short ESCAPE_BEGIN=0x007b;
    ESCAPE_TYPE m_Type=ESC_UNKNOWN;
    unsigned short m_Extended[16]{};
    const unsigned short* m_pEnd=nullptr;
    explicit FrontendParagraphEscape(const unsigned short* str)
    {
        FrontendParseTextEscape(*this,str,[](unsigned long key){
            Require(key==0x70000000UL,"Unqualified frontend text escape");return ESC_PARAGRAPH;
        });
    }
    ESCAPE_TYPE GetType()const{return m_Type;}
    nlColour GetExtendedColour()const{throw UnsupportedResource("Frontend colour escapes remain unavailable");}
};
}
