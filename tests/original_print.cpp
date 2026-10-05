#include "NL/nlPrint.h"
#include "platform/string_format.h"
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

namespace {
template<class T, std::size_t N>
void record(const char* name, int result, const std::array<T,N>& data)
{
    std::cout << name << ' ' << result;
    for (T c : data) std::cout << ' ' << static_cast<unsigned int>(static_cast<std::make_unsigned_t<T>>(c));
    std::cout << '\n';
}
int vprint(char* buffer, unsigned long count, const char* format, ...)
{
    va_list ap; va_start(ap, format);
    const int result = nlVSNPrintf(buffer, count, format, ap);
    va_end(ap); return result;
}
int host_count_zero(const char* format, ...)
{
    va_list ap; va_start(ap, format);
    const int result = mscharged::FormatString(static_cast<char*>(nullptr), 0, format, ap);
    va_end(ap); return result;
}
}
int main()
{
    std::array<char,12> small; small.fill('!');
    std::memcpy(small.data()+1,"prefix",7);
    record("narrow_alias",nlSNPrintf(small.data()+1,8,"%s_%d",small.data()+1,123),small);
    small.fill('!'); std::memcpy(small.data()+1,"prefix",7);
    record("narrow_v_alias",vprint(small.data()+1,8,"%s_%d",small.data()+1,123),small);
    small.fill('!'); record("narrow_nul",nlSNPrintf(small.data()+1,8,"%cX",0),small);
    small.fill('!'); record("narrow_empty",nlSNPrintf(small.data()+1,8,""),small);
    small.fill('!'); record("narrow_one",nlSNPrintf(small.data()+1,1,"abcd"),small);
    small.fill('!'); record("narrow_v_one",vprint(small.data()+1,1,"abcd"),small);
    std::array<char,96> narrow; narrow.fill('!');
    record("narrow_values",nlSNPrintf(narrow.data()+1,80,"%d/%u/%#x/%.*f/%s",-31,0xf0000001U,0x2aU,2,3.5,"font"),narrow);
    narrow.fill('!'); std::memcpy(narrow.data()+1,"fe/fonts/eurfonttext18",sizeof("fe/fonts/eurfonttext18"));
    record("narrow_font",nlSNPrintf(narrow.data()+1,80,"%s_%d",narrow.data()+1,1),narrow);
    narrow.fill('!'); std::memcpy(narrow.data()+1,"parent/child",13);
    record("narrow_suffix",nlSNPrintf(narrow.data()+1,80,"%s:%04x",narrow.data()+8,42),narrow);
    small.fill('!'); record("host_count_zero",host_count_zero("%s_%d","prefix",1),small);

    std::array<unsigned short,96> wide;
    auto run=[&](const char* name,const unsigned short* format,auto...args) {
        wide.fill(0x55aa);
        record(name,nlSNPrintf(wide.data()+1,80,format,args...),wide);
    };
    const unsigned short literal[]={0x41,0x03a9,0x6f22,0x25,0x25,0};
    run("wide_literal",literal);
    const unsigned short strings[]={'[','%','s',']','[','%','l','s',']',0};
    const unsigned short wtext[]={0x4d,0x03a9,0x6f22,0};
    run("wide_strings",strings,"\xc4\xe9",wtext);
    const unsigned short integers[]={'%','d','/','%','u','/','%','#','x','/','%','h','d','/','%','h','u',0};
    run("wide_integer",integers,-31,0xf0000001U,0x2aU,65535,65535);
    const unsigned short pad[]={'[','%','0','*','d',']','[','%','-','*','.','*','s',']',0};
    run("wide_padding",pad,5,42,8,3,"abcdef");
    const unsigned short characters[]={'%','l','c','/','%','c','/','%','l','c',0};
    run("wide_characters",characters,0x03a9,static_cast<int>('!'),0x6f22);
    const unsigned short decimals[]={'%','.','2','f','/','%','.','1','f','/','%','.','2','e','/','%','g',0};
    run("wide_decimal",decimals,3.5,-0.0,125.0,0.125);
    const unsigned short hex[]={'%','.','3','a','/','%','.','3','A',0};
    run("wide_hex",hex,3.5,-0.5);
    const unsigned short nonfinite[]={'%','f','/','%','F','/','%','f',0};
    run("wide_nonfinite",nonfinite,std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN());
    const unsigned short longs[]={'%','l','d','/','%','l','u','/','%','l','l','d','/','%','L','f',0};
    run("wide_native_varargs",longs,-31L,4026531841UL,1234567890123LL,3.5L);
    const unsigned short empty[]={0}; run("wide_empty",empty);
    const unsigned short zero[]={'%','l','c','X',0}; run("wide_nul",zero,0);
    const unsigned short trunc[]={'%','s','_','%','d',0};
    wide.fill(0x55aa); record("wide_truncated",nlSNPrintf(wide.data()+1,8,trunc,"prefix",123),wide);
    wide.fill(0x55aa); record("wide_one",nlSNPrintf(wide.data()+1,1,trunc,"prefix",1),wide);
    const unsigned short alias[]={'%','l','s','_','%','d',0};
    wide.fill(0x55aa); const unsigned short pref[]={'p','r','e','f','i','x',0};
    std::memcpy(wide.data()+1,pref,sizeof(pref));
    record("wide_alias",nlSNPrintf(wide.data()+1,80,alias,wide.data()+1,1),wide);
    const unsigned short counted[]={'A','B','%','n','C','%','h','n','D','%','l','n',0};
    int ni=-1;short ns=-1;long nl=-1;
    run("wide_count",counted,&ni,&ns,&nl);
    std::cout << "wide_counts " << ni << ' ' << ns << ' ' << nl << '\n';
    return nlPrintf("Actual original print source: %s %d 0x%02x\n","font",37,42)<0;
}
