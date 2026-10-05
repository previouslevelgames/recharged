#include "resources/credits_text.h"
#include "Game/Sys/simpleparser.h"
#include <algorithm>
namespace mscharged::resources
{
CreditsText::Handle ReadCreditsText(Bytes bytes)
{
    Require(bytes.size()<=65536,"Credits text exceeds its bounded file profile");
    // Original NextToken has256-byte fixed buffers and no overflow check.
    // Limiting each physical line first also bounds comments/separated tokens.
    unsigned line=0;
    for(auto byte:bytes)
    {
        Require(byte!=0,"Credits text has an embedded NUL");
        if(byte=='\n')line=0;
        else Require(++line<=255,"Credits text exceeds original parser line storage");
    }
    auto result=std::make_shared<CreditsText>();
    if(bytes.size()<=1)return result;
    std::vector<char> data(bytes.begin(),bytes.end());
    SimpleParser parser;
    if(!parser.StartParsing(data.data(),static_cast<int>(data.size()),"\t\r\n"))return result;
    while(const auto* token=parser.NextToken(false))
    {
        Require(result->tokens.size()<4096,"Credits token count exceeds its bounded profile");
        result->tokens.emplace_back(token);
        parser.AdvanceLine();
    }
    return result;
}
}
