#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace dt::modules::spotify {
inline std::vector<std::uint8_t> ember_background(const std::uint8_t* rgba,int width,int height) {
    constexpr int side=128;
    using Color=std::array<float,3>;
    std::vector<Color> cover(side*side), scratch(side*side), warped(side*side);
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        const float sx=x*(width-1)/127.f, sy=y*(height-1)/127.f;
        const int ix=int(sx), iy=int(sy);
        for(int c=0;c<3;++c) {
            float value=0;
            for(int j=0;j<2;++j) for(int i=0;i<2;++i) {
                const auto* p=rgba+(size_t(std::min(iy+j,height-1))*width+std::min(ix+i,width-1))*4;
                value+=p[c]/255.f*(p[3]/255.f)*(i?sx-ix:1-sx+ix)*(j?sy-iy:1-sy+iy);
            }
            cover[y*side+x][c]=value;
        }
    }
    auto blur=[&](std::vector<Color>& pixels,int radius,float sigma) {
        std::vector<float> kernel(radius*2+1);
        float total=0;
        for(int k=-radius;k<=radius;++k) total+=kernel[k+radius]=std::exp(-float(k*k)/(2*sigma*sigma));
        for(auto& weight:kernel) weight/=total;
        for(int axis=0;axis<2;++axis) {
            for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
                Color color{};
                for(int k=-radius;k<=radius;++k) {
                    const int sx=std::clamp(x+(axis==0?k:0),0,side-1);
                    const int sy=std::clamp(y+(axis==1?k:0),0,side-1);
                    for(int c=0;c<3;++c) color[c]+=pixels[sy*side+sx][c]*kernel[k+radius];
                }
                scratch[y*side+x]=color;
            }
            pixels.swap(scratch);
        }
    };
    // Blur real cover pixels before bending them into broad, flowing shapes.
    // Keeping their spatial arrangement preserves accents that a palette loses.
    blur(cover,15,7.f);
    std::vector<float> brightness;
    brightness.reserve(cover.size());
    for(const auto& pixel:cover) brightness.push_back(std::max({pixel[0],pixel[1],pixel[2]}));
    const auto percentile=brightness.begin()+brightness.size()*9/10;
    std::nth_element(brightness.begin(),percentile,brightness.end());
    const float exposure=std::clamp(.58f/std::max(.16f,*percentile),1.f,2.8f);
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        const float u=x/127.f, v=y/127.f;
        // A broad rotating flow avoids the narrow mirrored streaks produced
        // by high-frequency waves, especially on monochrome sleeves.
        const float dx=u-.5f, dy=v-.5f;
        const float angle=.85f*std::exp(-(dx*dx+dy*dy)*2.f)-.3f;
        float su=.5f+.86f*(dx*std::cos(angle)-dy*std::sin(angle))+.17f*std::sin(v*4.4f+.4f);
        float sv=.5f+.86f*(dx*std::sin(angle)+dy*std::cos(angle))+.16f*std::sin(u*4.1f-1.f);
        auto mirror=[](float t) {t=std::fmod(std::abs(t),2.f);return t>1.f?2.f-t:t;};
        const float sx=mirror(su)*127.f, sy=mirror(sv)*127.f;
        const int ix=int(sx), iy=int(sy);
        Color color{};
        for(int j=0;j<2;++j) for(int i=0;i<2;++i) {
            const auto& p=cover[std::min(iy+j,127)*side+std::min(ix+i,127)];
            const float weight=(i?sx-ix:1-sx+ix)*(j?sy-iy:1-sy+iy);
            for(int c=0;c<3;++c) color[c]+=p[c]*weight;
        }
        const float luminance=color[0]*.2126f+color[1]*.7152f+color[2]*.0722f;
        const float glow=std::exp(-((u-.78f)*(u-.78f)/.055f+(v-.48f)*(v-.48f)/.12f));
        const float lower=std::exp(-((u-.36f)*(u-.36f)/.11f+(v-.88f)*(v-.88f)/.04f));
        const float light=.90f+.48f*glow+.22f*lower;
        for(int c=0;c<3;++c) {
            const float saturated=std::max(0.f,luminance+(color[c]-luminance)*1.3f);
            const float lifted=std::pow(std::min(1.f,saturated*exposure),.78f);
            warped[y*side+x][c]=std::clamp(.018f+lifted*light,0.f,.94f);
        }
    }
    blur(warped,18,7.f);
    std::vector<std::uint8_t> output(side*side*4,255);
    for(int i=0;i<side*side;++i) for(int c=0;c<3;++c) output[i*4+c]=static_cast<std::uint8_t>(std::clamp(warped[i][c],0.f,1.f)*255.f);
    return output;
}
// Small artwork-derived surfaces, generated only on cover changes off the render thread.
inline std::vector<std::uint8_t> art_background(const std::uint8_t* rgba,int width,int height,bool silk) {
    if(!rgba || width<=0 || height<=0) return {};
    if(!silk) return ember_background(rgba,width,height);
    // Quantized dominant colors preserve saturated artwork accents instead of
    // averaging every quadrant into the same grey/pink wash.
    struct Bin { std::array<float,3> rgb{}; int count=0; };
    std::array<Bin,512> bins{};
    for(int sy=0;sy<40;++sy) for(int sx=0;sx<40;++sx) {
        const int x=std::min(width-1,sx*width/40), y=std::min(height-1,sy*height/40);
        const auto* px=rgba+(static_cast<size_t>(y)*width+x)*4;
        if(px[3]<128) continue;
        auto& bin=bins[(px[0]/32)*64+(px[1]/32)*8+px[2]/32];
        ++bin.count;
        for(int c=0;c<3;++c) bin.rgb[c]+=px[c]/255.f;
    }
    for(auto& bin:bins) if(bin.count) for(float& c:bin.rgb) c/=bin.count;
    std::array<std::array<float,3>,4> palette{};
    for(int pick=0;pick<4;++pick) {
        float best=-1; int winner=-1;
        for(int i=0;i<512;++i) {
            const auto& bin=bins[i]; if(!bin.count) continue;
            const float hi=std::max({bin.rgb[0],bin.rgb[1],bin.rgb[2]});
            const float lo=std::min({bin.rgb[0],bin.rgb[1],bin.rgb[2]});
            const float saturation=(hi-lo)/std::max(.01f,hi);
            float separation=1.f;
            if(pick) {
                separation=3.f;
                for(int j=0;j<pick;++j) {
                    float d=0;
                    for(int c=0;c<3;++c) {float delta=bin.rgb[c]-palette[j][c]; d+=delta*delta;}
                    separation=std::min(separation,d);
                }
            }
            const float score=std::sqrt(float(bin.count))*(.12f+saturation)*(.15f+hi)*(pick?separation:1.f);
            if(score>best) {best=score; winner=i;}
        }
        if(winner>=0) palette[pick]=bins[winner].rgb;
    }
    // Keep the darkest extracted shade in the design for depth; do not raise
    // the brightness of every color to the same level.
    for(auto& color:palette) {
        const float hi=std::max({color[0],color[1],color[2],.01f});
        const float gain=std::min(1.f,.78f/hi);
        for(float& c:color) c=std::max(.02f,c*gain);
    }
    constexpr int side=128;
    std::vector<std::uint8_t> output(side*side*4);
    for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
        float u=x/127.f,v=y/127.f;
        u+=.10f*std::sin(v*4.f); v+=.08f*std::sin(u*5.f);
        const float centers[4][2]={{.12f,.12f},{.87f,.3f},{.25f,.86f},{.85f,.92f}};
        std::array<float,3> color{}; float sum=0;
        for(int i=0;i<4;++i) {
            float dx=u-centers[i][0],dy=v-centers[i][1];
            float weight=std::exp(-(dx*dx+dy*dy)*4.f);
            for(int c=0;c<3;++c) color[c]+=palette[i][c]*weight;
            sum+=weight;
        }
        auto out=output.data()+(y*side+x)*4;
        for(int c=0;c<3;++c) out[c]=static_cast<std::uint8_t>(std::clamp(color[c]/sum,0.f,1.f)*255);
        out[3]=255;
    }
    return output;
}
}
