#include "ui.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using namespace nmsaves;
namespace {unsigned checks=0;void check(bool ok,const char*what){++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}bool offers(UiEnvironment e,HomeAction a){const auto v=homeActions(e);return std::find(v.begin(),v.end(),a)!=v.end();}}
int main(){
    for(const auto e:{UiEnvironment::Unknown,UiEnvironment::SysMmc,UiEnvironment::EmuMmc}){
        if(e!=UiEnvironment::SysMmc){check(!canExportUi(e,true),"Only sysMMC may export originals");check(!offers(e,HomeAction::Export),"No export entry in another environment");}
        if(e!=UiEnvironment::EmuMmc){check(!canImportUi(e,true),"A verified backup cannot authorize import outside emuMMC");check(!canUpdateUi(e),"No updater outside emuMMC");check(!offers(e,HomeAction::Import),"No import entry outside emuMMC");check(!offers(e,HomeAction::Backups),"No recovery entry outside emuMMC");}
    }
    check(homeActions(UiEnvironment::Unknown)==std::vector<HomeAction>{HomeAction::Refresh},"Unknown environment offers only inspection");
    check(!canExportUi(UiEnvironment::SysMmc,false),"Online sysMMC cannot export");
    check(canExportUi(UiEnvironment::SysMmc,true),"Offline sysMMC can request export");
    check(!canImportUi(UiEnvironment::EmuMmc,false),"No final import before backup verification");
    check(canImportUi(UiEnvironment::EmuMmc,true),"Verified emuMMC backup unlocks final review");
    check(offers(UiEnvironment::SysMmc,HomeAction::ReturnEmuMmc),"SysMMC retains return action");
    check(offers(UiEnvironment::EmuMmc,HomeAction::BringOriginals),"EmuMMC can review original access");
    check(!offers(UiEnvironment::SysMmc,HomeAction::BringOriginals),"No original-access loop from sysMMC");
    check(!offers(UiEnvironment::EmuMmc,HomeAction::ReturnEmuMmc),"No return loop from emuMMC");
    std::printf("UI environment policy: %u checks passed\n",checks);
}
