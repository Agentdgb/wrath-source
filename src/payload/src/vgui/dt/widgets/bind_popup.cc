#include "../../includes.hh"
#include "../../bridge.h"
#include "bind_popup.hh"
#include "../render/panel_glass.hh"
namespace dt::bind_popup {
namespace {
std::string module; float x=0,y=0; int opened=-1;
constexpr float width=244.f, height=86.f;
float reveal[3]={0,1,0};
void glyph(int mode,float cx,float cy,ImU32 ink) {
    auto* dl=g_render->draw_list();
    if(mode==0) { // power
        dl->PathArcTo({cx,cy},5.3f,-.90f,4.04f,24); dl->PathStroke(ink,0,1.5f);
        dl->AddLine({cx,cy-7},{cx,cy},ink,1.5f);
    } else if(mode==1) { // open circular hold indicator, matching the reference
        dl->PathArcTo({cx,cy},5.6f,-1.57f,3.8f,28); dl->PathStroke(ink,0,1.6f);
        dl->AddCircleFilled({cx,cy-5.6f},2.f,ink,16);
        dl->PathArcTo({cx,cy},2.7f,-1.1f,2.7f,18); dl->PathStroke(ink,0,1.3f);
    } else { // click pointer
        const ImVec2 points[]={{cx-4,cy-6},{cx-4,cy+5},{cx-1,cy+2},{cx+1.5f,cy+6},{cx+3.5f,cy+5},{cx+1,cy+1},{cx+5,cy+1}};
        dl->AddPolyline(points,7,ink,ImDrawFlags_Closed,1.35f);
    }
}
}
bool active() {return !module.empty();}
void close() {module.clear();}
void open(const std::string& name,float left,float top) {
    module=name;
    const int modes[]={1,0,2};
    for(int i=0;i<3;++i) reveal[i]=vgui::bridge::bindMode(module)==modes[i]?1.f:0.f;
    x=std::clamp(left,4.f,float(c_app::k_window_width)-width-4.f);
    y=std::clamp(top,4.f,float(c_app::k_window_height)-height-4.f);
    opened=ImGui::GetFrameCount();
}
void render(const frame_input_t& input) {
    if(!active()) return;
    const bool fresh=opened==ImGui::GetFrameCount();
    const bool inside=input.hovered(x,y,width,height);
    if(!fresh && (((input.m_left_clicked || input.m_right_clicked) && !inside) || ImGui::IsKeyPressed(ImGuiKey_Escape,false))) {close(); return;}
    auto* dl=g_render->draw_list(); dl->PushClipRectFullScreen();
    draw::shadow(x,y,width,height,11,20,0,4,hue::c_color(0,0,0,120));
    if(theme::glass_enabled) panel_glass::blur_game(x,y,width,height,11);
    draw::rounded_rect(x,y,width,height,11,theme::glass_enabled?hue::c_color(18,19,24,135):hue::c_color(17,18,22,255));
    draw::border(x,y,width,height,11,1,hue::c_color(120,120,138,35));
    dl->PushClipRect({x+12,y+4},{x+width-12,y+30},true);
    draw_text(module,x+14,y+18,font_face::bold,12,theme::text_primary,align_left|align_middle);
    dl->PopClipRect();
    draw::rounded_rect(x+8,y+36,width-16,40,8,hue::c_color(255,255,255,4));
    draw::border(x+8,y+36,width-16,40,8,1,hue::c_color(125,125,145,20));
    draw_text("Mode",x+33,y+56,font_face::regular,11,theme::text_secondary,align_left|align_middle);
    const ImU32 mode_ink=IM_COL32(164,164,179,255);
    dl->AddRect({x+15.5f,y+50.5f},{x+27.5f,y+59.5f},mode_ink,2.4f,0,1.35f);
    dl->AddLine({x+18.5f,y+62},{x+24.5f,y+62},mode_ink,1.35f);
    dl->AddLine({x+21.5f,y+59.5f},{x+21.5f,y+62},mode_ink,1.35f);
    const int modes[]={1,0,2}; const char* names[]={"Hold","Toggle","Click"};
    int selected=vgui::bridge::bindMode(module);
    draw::rounded_rect(x+80,y+43,148,26,6,theme::bg_element);
    float px=x+83;
    float ar,ag,ab,aa; vgui::bridge::clientAccent(ar,ag,ab,aa);
    for(int i=0;i<3;++i) {
        bool chosen=selected==modes[i];
        reveal[i]=anim::lerp(reveal[i],chosen?1.f:0.f,16.f,input.m_delta);
        float cell=31.f+48.f*reveal[i];
        bool hot=input.hovered(px,y+45,cell,22);
        if(reveal[i]>.005f) draw::rounded_rect(px,y+45,cell,22,5,theme::accent_violet.modulate(.24f*reveal[i]));
        else if(hot) draw::rounded_rect(px,y+45,cell,22,5,hue::c_color(255,255,255,15));
        glyph(modes[i],px+12,y+56,chosen?ImGui::ColorConvertFloat4ToU32({ar,ag,ab,1}):IM_COL32(153,153,168,255));
        dl->PushClipRect({px+23,y+45},{px+cell-3,y+67},true);
        if(reveal[i]>.01f) draw_text(names[i],px+24,y+56,font_face::regular,11,theme::accent_violet.modulate(reveal[i]),align_left|align_middle);
        dl->PopClipRect();
        if(!fresh && hot && input.m_left_clicked) {vgui::bridge::applyBindMode(module,modes[i]);}
        px+=cell;
    }
    dl->PopClipRect();
}
}
