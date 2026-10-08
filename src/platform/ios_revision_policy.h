#pragma once

#include <revolution/os/OSIOSRev.h>

namespace mscharged::platform {
namespace detail {
constexpr OSIOSRev NativeIOSRevisionForBuild(const char (&date)[12]) {
    constexpr char months[]="JanFebMarAprMayJunJulAugSepOctNovDec";
    unsigned month=0;
    for(unsigned i=0;i<12;++i)
        if(date[0]==months[i*3] && date[1]==months[i*3+1] && date[2]==months[i*3+2]) month=i+1;
    const unsigned day=(date[4]==' '?0u:unsigned(date[4]-'0'))*10u+unsigned(date[5]-'0');
    const unsigned year=unsigned(date[7]-'0')*1000u+unsigned(date[8]-'0')*100u+
        unsigned(date[9]-'0')*10u+unsigned(date[10]-'0');
    // NS names this host's native IOS service ABI. It is not a Wii IOS title
    // identity, installed firmware revision, or metadata obtained from the disc.
    return {0x4e,0x53,1,0,static_cast<u8>(month),static_cast<u8>(day),static_cast<u16>(year)};
}
constexpr bool ValidNativeIOSBuildDate(const OSIOSRev& revision) {
    constexpr unsigned days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    const unsigned year=revision.buildYear, month=revision.buildMon;
    if(year<2000 || year>2099 || month<1 || month>12) return false;
    const bool leap=year%4==0 && (year%100!=0 || year%400==0);
    return revision.buildDay>=1 && revision.buildDay<=days[month-1]+unsigned(month==2 && leap);
}
}
// The bus provider passes its actual compilation date to this policy. Date
// storage lives in that one TU, so independently rebuilt callers cannot select
// a different build date or violate a header inline-variable definition.
}
