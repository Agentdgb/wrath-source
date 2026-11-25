from pathlib import Path
import re,shutil
from port_numeric import ROOT,DEST,SAVED,convert
BASE=ROOT.parent.parent/'.fix-build/bloom-complete/Bloomclient-buildable/src/main/java/net/caffeinemc/mods/lithium/lattice/bundle/pipeline'
for relative in ['shard/ShellArrayCache.java','vertex/resource/LerpZoneBuffer.java','vertex/resource/NodeZoneRenderer.java']:
    source=BASE/relative;target=SAVED/source.name
    if not target.exists():shutil.copyfile(source,target)
s=(SAVED/'ShellArrayCache.java').read_text(encoding='utf-8-sig');s=re.sub(r'assert\b.*?;', '',s,flags=re.S)
def method(text,name):
    m=re.search(r'(?:public |private )?static [\w.<>\[\]]+ '+name+r'\([^)]*\)\s*\{',text);start=m.start();pos=m.end();depth=1
    while depth:
        if text[pos]=='{':depth+=1
        elif text[pos]=='}':depth-=1
        pos+=1
    return text[start:pos]
names=['e','f','g','x','y','z','aa','ab','ac','ad','ae','af','ag','ah']
def sky(text):
    text=convert(text)
    for typ,native in [('VertexConsumerProvider.Immediate','Buffers*'),('VertexConsumer','Stream*'),('MatrixStack.Entry','Rotation')]:text=text.replace(typ,native)
    pointers=re.findall(r'(?:Stream\*|Buffers\*)\s+(var\d+)',text)
    for var in pointers:text=re.sub(r'\b'+var+r'\.',var+'->',text)
    text=text.replace('((float)var3 % 1.0F)','std::fmod((float)var3,1.0F)')
    return text
fields=s[s.index('private static final float[] dg'):s.index('private static int ec')]
initial=s[s.rfind('static {')+8:s.rfind('}')].rsplit('}',1)[0]
main=method(s,'d');main=main[main.index('float[] var6 ='):main.rfind('} catch')]
a=main.index('if (!cu');b=main.index('float var26 =',a)
main=main[:a]+'''float var25=input.underwater?0.0F:1.0F;ca+=(var25-ca)*(1.0F-(float)Math.exp(-5.0*input.dt));
'''+main[b:]
main=main.replace('FieldVolumeCache.ea','input.light')
a=main.index('float var70 =');b=main.index('float var45 =',a)
main=main[:a]+'''float var70=50.0F;float var43=(float)input.width;float var44=(float)input.height;if(var43<1||var44<1)return;
'''+main[b:]
main=main.replace('(float)((Integer)var2.options.getFov().getValue()).intValue()','input.fov')
a=main.index('j(var24.getDiagonalPlane()');b=main.index('float var55 =',a)
main=main[:a]+'''frame(input,(float)(var52*8.726646259971648E-4),var70,var46,var51);Rotation var54=rotation;
'''+main[b:]
a=main.index('if (cq == null)');b=main.index('if (cr &&',a)
main=main[:a]+'Buffers* var57=&buffers;\n'+main[b:]
main=main.replace('LerpZoneBuffer.aa(var52)', '(.35F*(.5F+.5F*(float)Math.sin(var52*.21)))').replace('var57.draw();','')
main=sky(main).replace('var57.', 'var57->')
main=main.replace('var1.', 'var1->')
out='''// Ported from ShellArrayCache: native vertex streams replace Java rendering objects.
#include "SkyModel.h"
#include "NativeImages.h"
namespace dt::modules::bloom::sky {
Array<float> FR(24);Rotation rotation;
bool cr=true,cs=true,ct=true,bc=true,av=true,ar=true,bk=true,am=false,ay=false,an=false;float ao=0,ca=1,cb=1,ax=1,aw=1;
constexpr int cl=0,cm=1,cn=2,co=3,aq=4,ba=5,bb=6,bj=7;
'''
out+='\n'.join(sky(method(s,n).split('{',1)[0])+';'for n in names)+'\n'+sky(fields)
out+='''
void frame(const Input&i,float angle,float radius,float pixels,float threshold){
 for(int k=0;k<3;++k){FR[k]=-i.right[k];FR[3+k]=i.up[k];FR[6+k]=i.forward[k];}
 float x=0,y=.81915206F,z=-.57357645F,c=std::cos(angle),s=std::sin(angle),v=1-c;
 rotation.m={c+x*x*v,y*x*v+z*s,z*x*v-y*s,x*y*v-z*s,c+y*y*v,z*y*v+x*s,x*z*v+y*s,y*z*v-x*s,c+z*z*v};
 for(int k=0;k<9;++k)FR[9+k]=rotation.m[k];FR[18]=radius;FR[19]=pixels;FR[20]=threshold;
}
void generate(State& state,const Input& input,Buffers& buffers){auto*var1=&state;float var5=input.dt;am=ay=an=false;ao=0;
'''+main+'}\n'
out+='\n'.join(sky(method(s,n))for n in names)
out+='\nconst bool initialized=[](){'+sky(initial)+'return true;}();\n'
out+='''void reset(){ca=1;dz=-1;dx[0]=dx[1]=-1;am=ay=an=false;ao=0;}
float moonCoverage(){return ao;}
float sunCoverage(){return an||ay?1.f:0.f;}
void starData(Array<float>positions,Array<int>colors){cz=positions;da=colors;}
}\n'''
# The star handles are state independent and used by the original d/y routines.
out=out.replace('Array<float> FR(24);Rotation rotation;', 'Array<float> FR(24),cz;Array<int> da;Rotation rotation;')
out=out.replace('var1->pw','var1->pw')
out=out.replace('>>>','UNSUPPORTED_SHIFT')
out=out.replace('var2 % (Math::PI * 2)','std::fmod(var2,Math::PI*2)')
out=out.replace('(0.012 * var8 + 0.1 * (double)var29) % 64.0','std::fmod(0.012*var8+.1*var29,64.0)')
(DEST/'SkyGeometry.cpp').write_text(out,encoding='utf-8')

profiles=(SAVED/'LerpZoneBuffer.java').read_text(encoding='utf-8-sig')
start=profiles.index('private static final float[][] cm');end=profiles.index('private final float[] cs',start)
data=profiles[start:end]
# Stop before instance state; only the preset constants belong in this unit.
data=data[:data.index('private final') ]if 'private final'in data else data
out='#include "SkyModel.h"\nnamespace dt::modules::bloom::sky {\n'+convert(data)
out+='''
const bool normalized=[](){for(auto row:cm)for(int k=0;k<16;++k)if(k!=9)row[k]/=255.f;return true;}();
void profile(Array<float>&out,int index,float density){index=std::clamp(index,0,12);for(int k=0;k<31;++k)out[k]=cm[index][k];float t=std::pow(std::clamp(density,0.f,1.f),.85f);out[17]=cn[index]+(co[index]-cn[index])*t;out[18]=cp[index]+(cq[index]-cp[index])*t;}
}\n'''
(DEST/'SkyProfiles.cpp').write_text(out,encoding='utf-8')
