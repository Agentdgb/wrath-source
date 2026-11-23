#include "keybindhud.h"
#include "core/module/ModuleManager.h"
#include "core/module/Module.h"
#include "modules/render/arraylist/arraylist.h"
#include "modules/render/arraylist/font_pool.h"
#include "modules/render/arraylist/al_glass.h"
#include "vgui/bridge.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
namespace vanta::modules::keybindhud {
namespace {
struct Row { std::string name, key; BindMode mode; bool active; };
std::vector<Row> rows;
al::al_glass glass;
float x=24, y=180, w=264, h=120, scale=1;
bool valid=false, frosted=false;
std::string key_name(int vk) {
    switch(vk) {
    case VK_LBUTTON: return "MOUSE 1"; case VK_RBUTTON: return "MOUSE 2";
    case VK_MBUTTON: return "MOUSE 3"; case VK_XBUTTON1: return "MOUSE 4"; case VK_XBUTTON2: return "MOUSE 5";
    case VK_INSERT: return "INSERT"; case VK_RSHIFT: return "RSHIFT"; case VK_LSHIFT: return "LSHIFT";
    }
    wchar_t text[64]{};
    UINT scan=MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    LONG code=static_cast<LONG>((scan&255)<<16);
    if(scan&0xff00) code|=1<<24;
    if(GetKeyNameTextW(code,text,64)) {
        char utf[256]{};
        WideCharToMultiByte(CP_UTF8,0,text,-1,utf,sizeof(utf),nullptr,nullptr);
        return utf;
    }
    return "KEY " + std::to_string(vk);
}
}
void query_bbox(float& px,float& py,float& pw,float& ph) { px=x; py=y; pw=enabled?w:0; ph=enabled?h:0; }
void set_top_left(float px,float py) { x=px; y=py; }
void render_background(int width,int height,unsigned int framebuffer) {
    valid=false; frosted=false;
    if(!enabled || width<2 || height<2) return;
    scale=std::max(1.f,height/1080.f);
    rows.clear();
    for(const auto& module:ModuleManager::get().all()) {
        if(module->keybind().vk()==0 || module->keybind().mode()==BindMode::Disabled) continue;
        rows.push_back({std::string(module->name()),key_name(module->keybind().vk()),module->keybind().mode(),module->isEnabled()});
    }
    w=std::min(264.f*scale,static_cast<float>(width));
    const size_t limit=static_cast<size_t>(std::max(1.f,(height-90.f*scale)/(25.f*scale)));
    if(rows.size()>limit) rows.resize(limit);
    h=std::max(120.f,72.f+25.f*rows.size())*scale;
    x=std::clamp(x,0.f,std::max(0.f,width-w)); y=std::clamp(y,0.f,std::max(0.f,height-h));
    valid=true;
    if(vgui::bridge::isGlassEnabled() && glass.available()) {
        const float bottom=height-y-h, r=11.f*scale;
        std::vector<al::al_rect> rects; rects.emplace_back(x,bottom,w,h,r,r,r,r);
        const int pad=48;
        glass.render(rects,width,height,framebuffer,static_cast<int>(x)-pad,static_cast<int>(bottom)-pad,
            static_cast<int>(std::ceil(w))+pad*2,static_cast<int>(std::ceil(h))+pad*2,
            6,0.82f,{0.035f,0.038f,0.05f,0.48f},1.5f,10.f*scale,0.2f,1.35f);
        frosted=true;
    }
}
void render_text(int width,int height) {
    if(!enabled || !valid) return;
    auto& fonts=arraylist::shared_pool(); if(!fonts.ensure()) return;
    ImDrawList* dl=ImGui::GetForegroundDrawList(); if(!dl) return;
    dl->PushClipRect({0,0},{static_cast<float>(width),static_cast<float>(height)},false);
    float r,g,b,a; vgui::bridge::clientAccent(r,g,b,a);
    const ImU32 accent=ImGui::ColorConvertFloat4ToU32({r,g,b,1});
    const ImU32 muted=IM_COL32(139,140,153,255), white=IM_COL32(228,229,235,255);
    const float s=scale;
    dl->AddRectFilled({x,y},{x+w,y+h},frosted?IM_COL32(12,13,18,25):IM_COL32(15,16,19,245),11*s);
    dl->AddRect({x,y},{x+w,y+h},IM_COL32(255,255,255,26),11*s);
    auto text=[&](const std::string& str,float px,float py,ImU32 color,float size,float maxw) {
        ImFont* f=fonts.get(size*s); if(!f) return;
        dl->PushClipRect({px,py},{px+maxw,py+size*s+5*s},true);
        dl->PushTextureID(fonts.texture_id());
        dl->AddText(f,size*s,{px,py},color,str.c_str());
        dl->PopTextureID(); dl->PopClipRect();
    };
    // Tint the actual title glyphs, preserving atlas alpha for clean edges.
    if (ImFont* title=fonts.get(13.f*s)) {
        const float tx=x+14*s, ty=y+13*s;
        const float span=title->CalcTextSizeA(13.f*s,1000,0,"Keybind").x;
        dl->PushTextureID(fonts.texture_id());
        const int first=dl->VtxBuffer.Size;
        dl->AddText(title,13.f*s,{tx,ty},IM_COL32_WHITE,"Keybind");
        for(int i=first;i<dl->VtxBuffer.Size;++i) {
            auto& vertex=dl->VtxBuffer[i];
            const float t=std::clamp((vertex.pos.x-tx)/std::max(1.f,span),0.f,1.f);
            vertex.col=(vertex.col & IM_COL32_A_MASK) |
                (ImGui::ColorConvertFloat4ToU32({r+(1-r)*.48f*t,g+(1-g)*.48f*t,b+(1-b)*.48f*t,1}) & ~IM_COL32_A_MASK);
        }
        dl->AddText(title,13.f*s,{tx+span+4*s,ty},white,"List");
        dl->PopTextureID();
    }
    dl->AddLine({x+12*s,y+38*s},{x+w-12*s,y+38*s},IM_COL32(255,255,255,18));
    text("State",x+23*s,y+47*s,muted,10,70*s);
    text("Module",x+88*s,y+47*s,muted,10,110*s);
    text("Key",x+w-57*s,y+47*s,muted,10,55*s);
    for(size_t i=0;i<rows.size();++i) {
        const auto& row=rows[i]; float ry=y+(67.f+25.f*i)*s;
        const char* state=row.mode==BindMode::Hold ? "Hold" : row.mode==BindMode::Once ? "Click" : row.active ? "Toggled" : "Toggle";
        text(state,x+23*s,ry,row.active?accent:white,11,60*s);
        text(row.name,x+88*s,ry,white,11,std::max(1.f,w-158*s));
        text(row.key,x+w-61*s,ry,accent,10,51*s);
    }
    dl->PopClipRect();
}
}
