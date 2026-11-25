#include "core/profile/AppearanceData.h"
#include "vgui/dt/render/player_preview.hh"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <fstream>
#include "vgui/dt/views/player_list_layout.hh"
using namespace dt;
float alpha_text = 1.f;
void require(bool test,const char* message){if(!test)throw std::runtime_error(message);}
std::string encode(const std::string& value) {
    const char* alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result; unsigned bits=0; int count=0;
    for(unsigned char ch:value){bits=(bits<<8)|ch;count+=8;while(count>=6){count-=6;result+=alphabet[(bits>>count)&63];}}
    if(count) result+=alphabet[(bits<<(6-count))&63]; while(result.size()%4)result+='='; return result;
}
int main(int argc,char** argv){
    try {
        using namespace appearance;
        require(compactUuid("00000000-0000-0000-0000-000000000001")=="00000000000000000000000000000001","UUID normalization failed");
        require(compactUuid("../other").empty(),"Invalid UUID accepted");
        require(texturePath("https://example.org/texture/abc").empty(),"Untrusted texture host accepted");
        require(texturePath("https://textures.minecraft.net/texture/../../secret").empty(),"Invalid texture path accepted");
        const std::string hash(64,'a'), hash2(64,'b');
        nlohmann::json data={{"textures",{{"SKIN",{{"url","http://textures.minecraft.net/texture/"+hash},{"metadata",{{"model","slim"}}}}},{"CAPE",{{"url","https://textures.minecraft.net/texture/"+hash2}}}}}};
        auto result=parseTextures(encode(data.dump()));
        require(result.slim&&result.skin=="/texture/"+hash&&result.cape=="/texture/"+hash2,"Skin/cape metadata was lost");
        require(parseTextures("invalid!!!").skin.empty(),"Bad metadata accepted");
        require(parseTextures(encode("{\"textures\":42}")).skin.empty(),"Bad texture types accepted");
        data["textures"].erase("CAPE");
        require(parseTextures(encode(data.dump())).cape.empty(),"Profile without a cape invented one");
        const std::string capeId(32,'a'), secondId(32,'b');
        const auto capes=parseCapeHistory({{"CAPE",nlohmann::json::array({
            {{"image_hash",capeId},{"last_seen_at","2026-10-20T00:00:00Z"}},
            {{"image_hash",secondId},{"active",true}},{{"image_hash",capeId}},{{"image_hash","../invalid"}}})}});
        require(capes.size()==2&&capes.front().id==secondId&&capes.front().active,"Cape history ordering/deduplication failed");
        const auto names=parseNameHistory(nlohmann::json::array({{{"name","OldName"},{"changed_at",nullptr}},{{"name","NewName"},{"changed_at","2025-01-01"},{"accurate",true}},{{"name","OldName"}},{{"name","bad/name"}}}));
        require(names.size()==2&&names[0].changedAt.empty()&&names[1].accurate,"Name history parsing failed");
        require(parseNameHistory({{"error","private"}}).empty()&&parseCapeHistory(nullptr).empty(),"Unavailable history became fabricated records");
        auto size=resize_player_list(1100,700,1400,900);
        require(size.width==1100&&size.height==700,"Player List cannot be enlarged");
        size=resize_player_list(4000,3000,900,700);
        require(size.width==900&&size.height==700,"Player List escaped the framebuffer");
        size=resize_player_list(-1,-1,300,200);
        require(size.width==300&&size.height==200,"Small framebuffer produced invalid resize bounds");
        ImGui::CreateContext(); auto& io=ImGui::GetIO();io.DisplaySize={800,600};io.DeltaTime=1.f/60;
        unsigned char* pixels;int fw,fh;io.Fonts->GetTexDataAsRGBA32(&pixels,&fw,&fh);io.Fonts->SetTexID(1);
        Snapshot player;player.skin=2;player.cape=3;player.hasCape=true;
        nlohmann::json previews=nlohmann::json::array();
        for(bool legacy:{false,true})for(bool slim:{false,true})for(bool layers:{false,true})for(float angle:{-.35f,0.f,1.5f,3.49f}) {
            player.legacy=legacy;player.slim=slim;ImGui::NewFrame();auto* draw=ImGui::GetBackgroundDrawList();
            draw_player_preview(draw,player,0,0,180,180,angle,layers,true);
            require(draw->VtxBuffer.Size>24&&draw->VtxBuffer.Size<512,"Preview geometry is missing or unbounded");
            for(const auto& vertex:draw->VtxBuffer){
                require(std::isfinite(vertex.pos.x+vertex.pos.y)&&vertex.pos.x>=0&&vertex.pos.x<=180&&vertex.pos.y>=0&&vertex.pos.y<=180,"Preview escaped its panel");
                require(vertex.uv.x>=0&&vertex.uv.x<=1&&vertex.uv.y>=0&&vertex.uv.y<=1,"Preview UV outside skin");
            }
            if(argc>1 && !legacy && !slim && layers && (angle==-.35f || angle==3.49f)) {
                nlohmann::json frame={{"angle",angle},{"vertices",nlohmann::json::array()},{"indices",nlohmann::json::array()},{"commands",nlohmann::json::array()}};
                for(const auto& v:draw->VtxBuffer)frame["vertices"].push_back({v.pos.x,v.pos.y,v.uv.x,v.uv.y,v.col});
                for(auto index:draw->IdxBuffer)frame["indices"].push_back(index);
                for(const auto& command:draw->CmdBuffer)if(command.ElemCount)frame["commands"].push_back({command.IdxOffset,command.ElemCount,static_cast<unsigned long long>(command.GetTexID())});
                previews.push_back(std::move(frame));
            }
            ImGui::Render();
        }
        if(argc>1)std::ofstream(argv[1])<<previews.dump();
        ImGui::DestroyContext();
        std::cout<<"Appearance passed: UUID/texture validation, skin/cape data, missing cape, legacy/slim skins, layers and preview rotations.\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';if(ImGui::GetCurrentContext())ImGui::DestroyContext();return 1;}
}
