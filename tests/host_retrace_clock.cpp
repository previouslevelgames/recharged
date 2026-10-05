#include "runtime/host_retrace_clock.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
unsigned checks=0;
void Check(bool value,const char* message)
{
    ++checks;if(!value)throw std::runtime_error(message);
}
template<class F> void Reject(F&& operation)
{
    try{operation();}catch(const std::runtime_error&){++checks;return;}
    throw std::runtime_error("Invalid retrace observation accepted");
}
void Run()
{
    using mscharged::HostRetraceClock;using mscharged::HostRetraceRate;
    HostRetraceClock ntsc(HostRetraceRate::Ntsc());
    Check(ntsc.Observe(500)==0&&ntsc.Observe(500)==0,"First/repeated sample advanced VI");
    Check(ntsc.Observe(500+16683333)==0,"NTSC retrace arrived before its rational boundary");
    Check(ntsc.Observe(500+16683334)==1,"First NTSC retrace was lost");
    Check(ntsc.Observe(500+33366666)==1&&ntsc.Observe(500+33366667)==2,
          "Fractional NTSC timing drifted across two boundaries");
    Check(ntsc.Observe(500+1001000000)==60,"NTSC cadence rounded to monitor 60Hz");
    Reject([&]{ntsc.Observe(500+1000000000);});
    Check(ntsc.Observe(500+1001000000)==60,"Rejected backward time changed VI state");
    Check(ntsc.Observe(500+1001000000000ull)==60000,"Long NTSC clock accumulated drift");

    // Independent duration oracle: 1001 seconds must contain exactly 60000
    // retraces regardless of monitor rate, skipped draws or input polling.
    for(unsigned monitor_rate:{30u,60u,75u,120u,144u,240u})
    {
        HostRetraceClock c(HostRetraceRate::Ntsc());c.Observe(0);
        const std::uint64_t observations=1001ull*monitor_rate;
        for(std::uint64_t i=1;i<=observations;++i)
            c.Observe(i*1000000000ull/monitor_rate);
        Check(c.Count()==60000,"Monitor polling frequency changed native VI cadence");
    }
    HostRetraceClock split(HostRetraceRate::Ntsc());split.Observe(0);
    std::uint64_t elapsed=0;std::uint32_t random=0x493201;
    for(unsigned i=0;i<10000;++i)
    {
        random=random*1664525u+1013904223u;elapsed+=(random%10000000u)+1;
        // These short durations fit a direct integer-product oracle, unlike
        // the production clock's maximum duration and bounded 64-bit math.
        Check(split.Observe(elapsed)==elapsed*60000/1001000000000ull,
              "Partitioned time differs from independent elapsed-period oracle");
        Check(split.Observe(elapsed)==split.Count(),"Duplicate poll advanced VI");
    }
    HostRetraceClock whole(HostRetraceRate::Ntsc());whole.Observe(0);
    Check(whole.Observe(elapsed)==split.Count(),"One delayed poll lost elapsed retraces");
    HostRetraceClock pal(HostRetraceRate::Pal());pal.Observe(0);
    Check(pal.Observe(19999999)==0&&pal.Observe(20000000)==1&&pal.Observe(1000000000)==50,
          "PAL timing did not preserve 20ms retraces");
    HostRetraceClock reduced({120000,2002});reduced.Observe(0);
    Check(reduced.Rate().numerator==60000&&reduced.Rate().denominator==1001&&
          reduced.Observe(1001000000)==60,"Equivalent timing profile was not reduced");
    HostRetraceClock maximum({240,1});maximum.Observe(0);
    Check(maximum.Observe(std::numeric_limits<std::uint64_t>::max())==4427218577690ull,
          "Maximum raw duration overflowed ordinary 64-bit clock math");
    HostRetraceClock wide_phase({239999999,1000000});wide_phase.Observe(0);
    Check(wide_phase.Observe(std::numeric_limits<std::uint64_t>::max())==4427218559243ull,
          "Largest supported coprime rational phase overflowed");
    HostRetraceClock slow({1,1000000});slow.Observe(0);
    Check(slow.Observe(std::numeric_limits<std::uint64_t>::max())==18446,
          "Minimum supported rational rate lost its fractional duration");
    const auto limit=std::numeric_limits<std::uint64_t>::max();
    HostRetraceClock overflow(HostRetraceRate::Ntsc(),limit-59);overflow.Observe(0);
    Check(overflow.Observe(1000000000)==limit,"Last representable retrace was rejected");
    Reject([&]{overflow.Observe(1001000000);});
    Check(overflow.Count()==limit&&overflow.Observe(1000000001)==limit,
          "Overflow rejection changed clock anchor or fractional phase");
    Reject([]{HostRetraceClock c({0,1});});Reject([]{HostRetraceClock c({1,0});});
    Reject([]{HostRetraceClock c({241,1});});Reject([]{HostRetraceClock c({1,1000001});});
    // Large unreduced inputs are valid when their exact reduced rate is safe.
    HostRetraceClock large({4000000000u,4000000000u});large.Observe(0);
    Check(large.Observe(1000000000)==1,"Safe equivalent large rational rate rejected");
}
}
int main()
{
    try{Run();std::cout<<checks<<" host VI rational-clock checks passed\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
