from pathlib import Path
import re
from transpile import convert, ROOT

s=Path(__file__).with_name('HaloPassCache.java').read_text(encoding='utf-8-sig')
s=re.sub(r'assert\b.*?;', '',s,flags=re.S)
s=s[s.index('{',s.index('public final class'))+1:s.rfind('}')]
s=re.sub(r'private HaloPassCache\(\)\s*\{\s*\}', '',s)
def replace_body(text,name,replacement):
    m=re.search(r'(?:public|private) static [\w\[\]]+ '+name+r'\([^)]*\)\s*\{',text);a=m.end();depth=1;b=a
    while depth:
        if text[b]=='{':depth+=1
        elif text[b]=='}':depth-=1
        b+=1
    return text[:a]+replacement+text[b-1:]
# Remove the decompiler's state-machine obfuscation from the reset routine.
s=replace_body(s,'p','''for(int k=0;k<6;++k){bx[k]=null;by[k]=bz[k]=ca[k]=false;cb[k]=cc[k]=cl[k]=cm[k]=cn[k]=0;}
for(int k=0;k<22;++k){cr[k]=null;cs[k]=false;ct[k]=cu[k]=0;}for(int k=0;k<16;++k)dd[k]=null;
for(int k=0;k<7;++k)dx[k]=0;dv=du=dt=0;dk=dj=di=dh=dg=df=false;eo=eq=-1;ep=0;''')
where=s.rfind('static {');initial=s[where+8:s.rfind('}')];s=s[:where]
methods=list(re.finditer(r'(?:public|private) static ([\w\[\]]+) (\w+)\(([^)]*)\)\s*\{',s))
def halo(text):
    pointervars=set(re.findall(r'\b(?:PlayerEntity|AbstractClientPlayerEntity|BindingBridge|List)\s+(var\d+)',text))
    text=text.replace('new float[][]', 'Array<Array<float>>').replace('float[][]','Array<Array<float>>')
    text=convert(text)
    for t in ['LivingEntity','PlayerEntity','byte']:
        native='std::int8_t' if t=='byte'else t
        text=re.sub(r'new '+t+r'\[([^\]]+)\]',r'Array<'+native+r'>(\1)',text)
        text=re.sub(r'\b'+t+r'\[\]',r'Array<'+native+'>',text)
    text=re.sub(r'\bbyte\b','std::int8_t',text)
    text=re.sub(r'if \(([^\n]+) instanceof LivingEntity (var\d+)\)',r'if(auto* \2 = \1; \2)',text)
    text=text.replace('Runtime.getRuntime().availableProcessors() - Runtime.getRuntime().availableProcessors()','0')
    for cls in ['EquipmentSlot','EntityPose','ConfigBridge']:
        text=text.replace(cls+'.',cls+'::')
    text=text.replace('ConfigBridge::c.e()','ConfigBridge::c->e()')
    for var in pointervars:text=re.sub(r'\b'+var+r'\.',var+'->',text)
    text=text.replace('List var3','List* var3').replace('BindingBridge var18','BindingBridge* var18')
    text=text.replace('int var25 = 625341585','std::uint32_t var25 = 625341585')
    text=re.sub(r'(\w+) >>> (\d+)',r'(static_cast<std::uint32_t>(\1) >> \2)',text)
    return text
out='// Generated from the supplied target visual engine. Runtime: C++ only.\n#include "BloomCompat.h"\nnamespace dt::modules::bloom::halo {\n'
out+='\n'.join(halo(m.group(0)[:-1].strip())+';'for m in methods)+'\n'+halo(s)
out+='\nconst bool initialized=[](){'+halo(initial)+'return true;}();\nbool visible(){return dt>0||RegionPassCache::ah;}\n}\n'
(ROOT/'src/modules/visuals/bloom/TargetEngine.cpp').write_text(out,encoding='utf-8')
