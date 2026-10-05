#include "runtime/frontend_idle_dimming.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using mscharged::FrontendIdleDimming;
namespace
{
unsigned checks=0;
void Check(bool good,const char* why){++checks;if(!good)throw std::runtime_error(why);}
template<class F>void Reject(F f){++checks;try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid idle dimming operation accepted");}
constexpr std::uint64_t second=60;
}
int main()
{
    try
    {
        FrontendIdleDimming owner(0);Check(owner.ThresholdRetraces()==18000&&owner.Opacity()==0,"Default native policy differs");
        owner.Sample(300*second-1,false);Check(owner.Opacity()==0,"Default dimming began early");
        owner.Sample(300*second,false);Check(owner.Opacity()==.5f,"Default dimming deadline failed");
        owner.Select(2);Check(owner.ThresholdRetraces()==54000&&owner.Opacity()==0,"Title did not admit fifteen-minute native policy");
        owner.Sample(900*second-1,false);Check(owner.Opacity()==0,"Title dimming began early");
        owner.Sample(900*second,false);Check(owner.Opacity()==.5f,"Title dimming deadline failed");
        owner.Sample(900*second,true);Check(owner.Opacity()==0,"Actual host activity did not wake output");
        owner.Sample(1200*second,false);Check(owner.Opacity()==0,"Title policy lost its retained timeout");
        owner.Select(0);Check(owner.Opacity()==.5f,"Default restoration reset source inactivity");
        owner.Sample(1200*second,true);Check(owner.Opacity()==0,"Activity did not reset restored default");
        Reject([&]{owner.Sample(1199*second,true);});Check(owner.Opacity()==0,"Backwards time changed native state");
        Reject([&]{owner.Select(1);});Reject([&]{owner.Select(3);});Check(owner.ThresholdRetraces()==18000,"Rejected mode changed policy");
        std::thread foreign([&]{Reject([&]{owner.Select(2);});Reject([&]{owner.Sample(1201*second,true);});Reject([&]{owner.Opacity();});});foreign.join();
        Check(owner.ThresholdRetraces()==18000&&owner.Opacity()==0,"Foreign thread changed native owner");
        const auto maximum=std::numeric_limits<std::uint64_t>::max();
        FrontendIdleDimming edge(maximum-900*second);edge.Select(2);edge.Sample(maximum-1,false);Check(edge.Opacity()==0,"Large clock dimmed early");
        edge.Sample(maximum,false);Check(edge.Opacity()==.5f,"Large clock overflowed deadline");edge.Sample(maximum,true);Check(edge.Opacity()==0,"Large clock activity failed");
        FrontendIdleDimming fresh(42);Check(fresh.Opacity()==0&&fresh.ThresholdRetraces()==18000,"New native lifetime inherited old dimming");
        std::cout<<checks<<" native idle dimming policy checks passed; physical host input remains unqualified\n";
    }
    catch(const std::exception& e){std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
