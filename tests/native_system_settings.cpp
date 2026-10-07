#include "platform/system.h"
#include <revolution/sc.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <type_traits>

static_assert(std::is_same_v<decltype(&SCGetLanguage), u8(*)(void)>);
static_assert(std::is_same_v<decltype(&SCGetSoundMode), u8(*)(void)>);
static_assert(std::is_same_v<decltype(&SCCheckStatus), u32(*)(void)>);
static_assert(SC_STATUS_OK==0 && SC_STATUS_BUSY==1 && SC_STATUS_FATAL==2);
static int checks;
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);++checks;}
template<class Fn> void Reject(Fn fn,const char* message){bool rejected=false;try{fn();}catch(const std::logic_error&){rejected=true;}Check(rejected,message);}
static void TestSimpleAddress();

int main()
{
    using namespace mscharged;
    try
    {
        Check(SCCheckStatus()==SC_STATUS_FATAL,"unconfigured endpoint must not claim ready");
        Reject([]{SCGetLanguage();},"unconfigured language must not invent English");
        Reject([]{SCGetProgressiveMode();},"unconfigured progressive mode");
        Reject([]{SCGetEuRgb60Mode();},"unconfigured EURGB60 mode");
        Reject([]{SCGetAspectRatio();},"unconfigured aspect ratio");
        Reject([]{SCGetSoundMode();},"unconfigured sound mode");
        Reject([]{SCInit();},"unconfigured init must fail");
        Reject([]{ConfigureNativeSystemSettings({SC_LANG_MAX,0,0,0});},"invalid language");
        Reject([]{ConfigureNativeSystemSettings({SC_LANG_FR,2,0,0});},"invalid progressive");
        Reject([]{ConfigureNativeSystemSettings({SC_LANG_FR,0,2,0});},"invalid EURGB60");
        Reject([]{ConfigureNativeSystemSettings({SC_LANG_FR,0,0,2});},"invalid aspect");
        Reject([]{ConfigureNativeSystemSettings({SC_LANG_FR,0,0,0,3});},"invalid sound mode");
        Check(SCCheckStatus()==SC_STATUS_FATAL,"rejected setup must not publish status");
        ConfigureNativeSystemSettings({SC_LANG_FR,SC_PROGRESSIVE,SC_EURGB_60_HZ,SC_ASPECT_WIDE});
        Check(SCCheckStatus()==SC_STATUS_BUSY,"staged endpoint awaits original SCInit");
        Check(SCGetLanguage()==SC_LANG_FR,"original pre-init language query reads supplied record");
        Check(SCGetProgressiveMode()==SC_PROGRESSIVE,"explicit supplied progressive record");
        Check(SCGetEuRgb60Mode()==SC_EURGB_60_HZ,"explicit supplied EURGB60 record");
        Check(SCGetAspectRatio()==SC_ASPECT_WIDE,"explicit supplied widescreen record");
        Check(SCGetSoundMode()==SC_SND_STEREO,"existing diagnostic settings default to stereo");
        Reject([]{ConfigureNativeSystemSettings({SC_LANG_MAX,0,0,0});},"bad replacement rejected");
        Check(SCGetLanguage()==SC_LANG_FR && SCGetProgressiveMode()==SC_PROGRESSIVE,"bad replacement preserves snapshot");
        std::atomic<unsigned> foreignRejects=0;
        std::thread foreign([&]{
            try{SCInit();}catch(const std::logic_error&){++foreignRejects;}
            try{ConfigureNativeSystemSettings({SC_LANG_EN,0,0,0});}catch(const std::logic_error&){++foreignRejects;}
            try{ShutdownNativeSystemSettings();}catch(const std::logic_error&){++foreignRejects;}
        });foreign.join();
        Check(foreignRejects==3,"foreign lifecycle mutation must reject");
        Check(SCCheckStatus()==SC_STATUS_BUSY,"foreign init must not publish ready");
        SCInit();
        Check(SCCheckStatus()==SC_STATUS_OK,"real source init publishes supplied endpoint");
        SCInit();
        Check(SCCheckStatus()==SC_STATUS_OK,"source initialization is idempotent");
        Reject([]{ConfigureNativeSystemSettings({SC_LANG_EN,0,0,0});},"active source snapshot immutable");
        std::atomic<bool> coherent=false;
        std::thread reader([&]{coherent=SCGetLanguage()==SC_LANG_FR && SCGetProgressiveMode()==SC_PROGRESSIVE
                && SCGetEuRgb60Mode()==SC_EURGB_60_HZ && SCGetAspectRatio()==SC_ASPECT_WIDE
                && SCCheckStatus()==SC_STATUS_OK;});reader.join();
        Check(coherent,"native readers share configured snapshot");
        ShutdownNativeSystemSettings();
        Check(SCCheckStatus()==SC_STATUS_FATAL,"retired endpoint not ready");
        Reject([]{SCGetLanguage();},"retired snapshot unavailable");
        ConfigureNativeSystemSettings({SC_LANG_KR,SC_INTERLACED,SC_EURGB_50_HZ,SC_ASPECT_STD});
        Check(SCGetLanguage()==SC_LANG_KR,"retail upper language boundary retained");
        SCInit();
        Check(SCGetProgressiveMode()==SC_INTERLACED && SCGetEuRgb60Mode()==SC_EURGB_50_HZ
                && SCGetAspectRatio()==SC_ASPECT_STD,"second session supplied video records");
        ShutdownNativeSystemSettings();
        SetStartupSystemLanguage(SC_LANG_SP);
        Check(SCGetLanguage()==SC_LANG_SP && SCCheckStatus()==SC_STATUS_BUSY,"legacy diagnostic explicitly stages language");
        Check(SCGetProgressiveMode()==SC_INTERLACED && SCGetEuRgb60Mode()==SC_EURGB_50_HZ
                && SCGetAspectRatio()==SC_ASPECT_STD,"legacy diagnostic video defaults explicit");
        ShutdownNativeSystemSettings();
        for (u8 mode : {SC_SND_MONO, SC_SND_STEREO, SC_SND_SURROUND})
        {
            ConfigureNativeSystemSettings({SC_LANG_EN,SC_INTERLACED,SC_EURGB_50_HZ,SC_ASPECT_STD,mode});
            SCInit();
            Check(SCGetSoundMode()==mode,"original audio query reads the exact supplied Wii sound setting");
            ShutdownNativeSystemSettings();
        }
        Reject([]{SCGetSoundMode();},"retired sound snapshot unavailable");
        TestSimpleAddress();
        ShutdownNativeSystemSettings();
        Check(SCCheckStatus()==SC_STATUS_FATAL,"retirement is idempotent");
        std::printf("237 native SC endpoint PASS:%d checks; explicit backing records/readiness/lifetime, no source/game/video decisions changed.\n",checks);
    }
    catch(const std::exception& e){std::fprintf(stderr,"237 FAIL:%s\n",e.what());return 1;}
}

static void TestSimpleAddress()
{
    using namespace mscharged;

 Reject([]{SCGetSimpleAddressID();},"unconfigured read");
 Reject([]{ConfigureNativeSystemSimpleAddress(0);},"no settings owner");
 Check(SCCheckStatus()==SC_STATUS_FATAL,"no fabricated status");
 ConfigureNativeSystemSettings({SC_LANG_EN,SC_INTERLACED,SC_EURGB_50_HZ,SC_ASPECT_STD,SC_SND_STEREO});
 Reject([]{SCGetSimpleAddressID();},"absent ID does not default0");
 ConfigureNativeSystemSimpleAddress(0x41010203u);
 Check(SCGetSimpleAddressID()==0x41010203u,"exact staged ID");
 Check(SCGetSimpleAddressID()>>24==0x41,"country high8");
 Check(((SCGetSimpleAddressID()>>16)&0xff)==1,"region bits");
 Check((SCGetSimpleAddressID()&0xffff)==0x0203,"city bits");
 std::atomic<unsigned> foreign{0};std::thread t([&]{try{ConfigureNativeSystemSimpleAddress(0x5f000001);}catch(const std::logic_error&){++foreign;}});t.join();
 Check(foreign==1,"owner-only mutation");
 Check(SCGetSimpleAddressID()==0x41010203u,"failed mutation retains original record");
 SCInit();
 Check(SCCheckStatus()==SC_STATUS_OK,"source init unchanged");
 Reject([]{ConfigureNativeSystemSimpleAddress(0);},"initialized immutable");
 Check(SCGetSimpleAddressID()==0x41010203u,"initialized exact ID");
 ShutdownNativeSystemSettings();
 Reject([]{SCGetSimpleAddressID();},"retired record");
 struct Oracle {u32 supplied;u32 expected;};
 const Oracle oracle[]={{0,0xffffffffu},{0xff010203u,0xffffffffu},
  {0x41ff1234u,0xffffffffu},{0xffffffffu,0xffffffffu},
  {0x5fabcdefu,0x5fabcdefu},{0x41000203u,0x41000203u}};
 for(auto row:oracle){u32 id=row.supplied;
  ConfigureNativeSystemSettings({SC_LANG_EN,0,0,0,SC_SND_MONO});
  Reject([]{SCGetSimpleAddressID();},"new owner does not inherit record");
  ConfigureNativeSystemSimpleAddress(id);SCInit();
  Check(SCGetSimpleAddressID()==row.expected,"exact original SC property result");
  Check(SCGetSoundMode()==SC_SND_MONO,"independent source sound setting unchanged");
  ShutdownNativeSystemSettings();
 }
 ConfigureNativeSystemSettings({SC_LANG_EN,0,0,0});
 ConfigureNativeSystemSimpleAddress(std::nullopt);
 Check(SCGetSimpleAddressID()==0xffffffffu,"explicit absent record uses real source sentinel");
 Check(SCCheckStatus()==SC_STATUS_BUSY,"absent address does not publish readiness");
 SCInit();Check(SCGetSimpleAddressID()==0xffffffffu,"source initialization preserves explicit absent property");
 ShutdownNativeSystemSettings();
 Check(SCCheckStatus()==SC_STATUS_FATAL,"true retirement");
}
