#include "display_options.hpp"
#include "config.hpp"
#include "indep/inc/FMenus.hpp"
#include "indep/inc/Flags.hpp"
#include "indep/inc/FlailerDB.hpp"
#include "bware/inc/bWare.hpp"

#include <cstdio>
#include <string>

namespace hp2::nx {
namespace {
constexpr std::uint32_t rowKeys[]={0x52e039f3u,0x52e039f4u,0x52e039f5u,0x52e039f6u,
    0x52e039f7u,0x52e039f8u,0x52e039f9u,0x52e039fau,0x52e039fbu,
    0xaee77883u,0xaee77884u,0xaee77885u,0xaee77886u,0xaee77887u};
std::uint32_t item(int number) noexcept {return rowKeys[number];}
}
std::uint32_t FrameLimitItem(std::uint32_t screen) noexcept {return item(screen==pauseDisplay ? 12:9);}
std::uint32_t ShowFpsItem(std::uint32_t screen) noexcept {return item(screen==pauseDisplay ? 13:10);}
void RefreshDisplayMenu(frontend::FlailerResources& resources,std::uint32_t screen) {
    if(userSettings.frameLimit)resources.Printf(screen,frameLimitValue,"%d FPS",userSettings.frameLimit);
    else resources.Printf(screen,frameLimitValue,"Uncapped");
    resources.Printf(screen,showFpsValue,"%s",userSettings.showFps ? "On":"Off");
}
std::uint32_t DisplayMenuItem(std::uint32_t screen,std::uint32_t ordinal,std::uint32_t fallback) noexcept {
    if(screen!=frontendDisplay && screen!=pauseDisplay)return fallback;
    if(ordinal==0)return FrameLimitItem(screen);
    if(ordinal==1)return ShowFpsItem(screen);
    const auto count=screen==pauseDisplay ? 12u:9u;
    return ordinal-2<count ? rowKeys[ordinal-2]:0;
}
int DisplayMenuPosition(std::uint32_t screen,std::uint32_t key) noexcept {
    if(screen!=frontendDisplay && screen!=pauseDisplay)return -1;
    for(unsigned i=0;i<(screen==pauseDisplay ? 14u:11u);++i)
        if(DisplayMenuItem(screen,i,0)==key)return static_cast<int>(i);
    return -1;
}
void PrepareDisplayMenu(frontend::FlailerUiState& ui,std::uint32_t screen) {
    if(screen!=frontendDisplay && screen!=pauseDisplay)return;
    auto& resources=*ui.resources;
    if(!resources.FindObject(screen,FrameLimitItem(screen))) {
        const int count=screen==pauseDisplay ? 12:9;
        const auto* first=resources.FindObject(screen,item(0),1);
        const auto* second=resources.FindObject(screen,item(1),1);
        const auto* last=resources.FindObject(screen,item(count-1),1);
        Require(first && second && last,"Switch Display menu has no authored row templates");
        const float top=first->topLeftY,step=second->topLeftY-top,bottom=last->topLeftY;
        const auto valueTemplate=screen==pauseDisplay ? 0x5b472b16u:0x55cb09f2u;
        resources.AddNativeText(screen,item(0),FrameLimitItem(screen),first->parentKey,"Frame Rate",first->topLeftX,top);
        resources.AddNativeText(screen,valueTemplate,frameLimitValue,first->parentKey,"30 FPS",-1,top);
        resources.AddNativeText(screen,item(0),ShowFpsItem(screen),first->parentKey,"Show FPS",first->topLeftX,top+step);
        resources.AddNativeText(screen,valueTemplate,showFpsValue,first->parentKey,"Off",-1,top+step);
        auto* page=resources.FindScreen(screen);
        // Existing keys keep their callbacks. Their spatial and navigation
        // order both follow the two new rows, preserving all old options.
        for(auto* object=page->objects.next;object!=&page->objects;object=object->next) {
            if(object->kind!=1)continue;
            const bool native=object->key==FrameLimitItem(screen) || object->key==ShowFpsItem(screen) ||
                object->key==frameLimitValue || object->key==showFpsValue;
            const auto position=DisplayMenuPosition(screen,object->key);
            const bool value=screen==pauseDisplay ? object->parentKey==first->parentKey &&
                object->topLeftY>=top && object->topLeftY<=bottom+3 :
                object->key==0x55cb09f2u || object->key==0x2e931f1bu || object->key==0x03d90172u ||
                object->key==0x5610f2f4u || object->key==0x422313cbu || object->key==0x8fe4086bu;
            if(!native && (position>=2 || value))object->SetTopLeftY(object->topLeftY+2*step);
        }
        if(screen==pauseDisplay) {
            const float scale=(bottom-top)/(bottom-top+2*step);
            for(auto* object=page->objects.next;object!=&page->objects;object=object->next)
                if(object->kind==1 && object->parentKey==first->parentKey)
                    object->SetTopLeftY(top+(object->topLeftY-top)*scale);
        }
    }
    RefreshDisplayMenu(resources,screen);
}
bool HandleDisplaySetting(frontend::FlailerResources& resources,std::uint32_t screen,std::uint32_t key,int direction) {
    if(key==FrameLimitItem(screen))CycleFrameLimit(direction);
    else if(key==ShowFpsItem(screen)) {userSettings.showFps=!userSettings.showFps;applicationShowFrameRate=userSettings.showFps;}
    else return false;
    RefreshDisplayMenu(resources,screen);return true;
}
}
