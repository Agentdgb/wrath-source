from pathlib import Path
import re
from transpile import convert, ROOT

s=Path(__file__).with_name('EchoPassCache.java').read_text(encoding='utf-8-sig')
s=re.sub(r'assert\b.*?;', '',s,flags=re.S)
s=s[s.index('{',s.index('public final class'))+1:s.rfind('}')]
s=re.sub(r'private EchoPassCache\(\)\s*\{\s*\}', '',s)
def replace_body(text,name,replacement):
    m=re.search(r'(?:public|private) static [\w\[\]]+ '+name+r'\([^)]*\)\s*\{',text);a=m.end();depth=1;b=a
    while depth:
        if text[b]=='{':depth+=1
        elif text[b]=='}':depth-=1
        b+=1
    return text[:a]+replacement+text[b-1:]
s=replace_body(s,'b','''for(int k=0;k<9;++k){cj[k]=false;dk[k]=null;cp[k]=0;}for(int k=0;k<8;++k)gd[k]=false;
HIDE_N=dl=0;gf=false;ge=0;gg=gi=gj=1;fj=fk=fl=0;''')
# JVM collision/sound calls are supplied by native world callbacks. The original
# eight-slot eviction, style selection and initializer state are retained.
s=replace_body(s,'g','''if(var0==null||!on)return;
MinecraftClient client=MinecraftClient.getInstance();if(client.world==null)return;
int count=0,freeSlot=-1,oldest=-1;float age=-1;
for(int k=0;k<8;++k){if(!cj[k]){if(freeSlot<0)freeSlot=k;}else{++count;float v=Math.max(0.0F,cr[k])/bm[cn[k]];if(v>age){age=v;oldest=k;}}}
int slot=count<MathHelper.clamp(max,1,8)&&freeSlot>=0?freeSlot:oldest;if(slot<0)return;
int style=ae;if(style<0||style>5){style=ThreadLocalRandom.current().nextInt(6);if(style==dm)style=(style+1+ThreadLocalRandom.current().nextInt(5))%6;dm=style;}
float scale=(af>0?Math.min(af,4.0F):1.0F)*MathHelper.clamp(var0.getHeight()/1.8F,.6F,1.8F);
double floor=client.world.trace(var0.getX(),var0.getY()+.1,var0.getZ(),var0.getY()-6),ceiling=client.world.trace(var0.getX(),var0.getY()+var0.getHeight(),var0.getZ(),var0.getY()+Math.max(var0.getHeight()+.5,14.0*scale));
cj[slot]=true;cn[slot]=style;ck[slot]=var1;cr[slot]=-1;cs[slot]=var0.getX();ct[slot]=var0.getY();cu[slot]=var0.getZ();cv[slot]=Double.isFinite(floor)?floor+.015:var0.getY()+.015;cm[slot]=Double.isFinite(floor);cw[slot]=Double.isFinite(ceiling)?ceiling-.05:Double.POSITIVE_INFINITY;
cx[slot]=var0.getHeight();cy[slot]=var0.getWidth();cz[slot]=scale;co[slot]=var0.getId();dk[slot]=var0;da[slot]=var0.bodyYaw*(float)(Math.PI/180);dd[slot]=0;cl[slot]=var0.player;de[slot]=df[slot]=1;cp[slot]=0;dj[slot]=ThreadLocalRandom.current().nextLong();
dl=0;for(int k=0;k<8;++k)if(cj[k])++dl;
if(var1&&ac){gf=true;ge=0;gg=1;}if(var1&&ab)client.world.sound(style,var0.getX(),var0.getY()+.55*var0.getHeight(),var0.getZ());''')
s=replace_body(s,'h','''MinecraftClient client=MinecraftClient.getInstance();if(client.world==null)return;
for(int k=0;k<8;++k)if(cj[k])df[k]=client.world.visible(cs[k],ct[k]+.55*cx[k],cu[k])?1.0F:0.0F;''')
where=s.rfind('static {');initial=s[where+8:s.rfind('}')];s=s[:where]
methods=list(re.finditer(r'(?:public|private) static ([\w\[\]]+) (\w+)\(([^)]*)\)\s*\{',s))
def death(text):
    text=text.replace('new float[][]','Array<Array<float>>').replace('float[][]','Array<Array<float>>')
    text=convert(text)
    text=re.sub(r'->player\s*\.', '->player->',text)
    for typ,native in [('long','std::int64_t'),('LivingEntity','LivingEntity')]:
        text=re.sub(r'new '+typ+r'\[([^\]]+)\]',r'Array<'+native+r'>(\1)',text)
        text=re.sub(r'\b'+typ+r'\[\]',r'Array<'+native+'>',text)
    text=re.sub(r'\blong\b','std::int64_t',text)
    text=text.replace('std::int64_t var10 = var1.nextLong()', 'std::uint64_t var10 = static_cast<std::uint64_t>(var1.nextLong())').replace('(std::int64_t)var13 * -7046029254386353131L','static_cast<std::uint64_t>(var13) * static_cast<std::uint64_t>(-7046029254386353131LL)')
    text=re.sub(r'\bbyte\b','std::int8_t',text)
    text=re.sub(r'new Random\(([^)]*)\)',r'Random(\1)',text)
    text=text.replace('ThreadLocalRandom.current()', 'threadRandom()')
    text=text.replace('Runtime.getRuntime().availableProcessors() - Runtime.getRuntime().availableProcessors()','0')
    text=re.sub(r'->(next\w+|setSeed)\(',r'.\1(',text)
    text=text.replace('System.arraycopy','arraycopy').replace('System.nanoTime()','0')
    text=text.replace('Double.POSITIVE_INFINITY','std::numeric_limits<double>::infinity()').replace('Double.MAX_VALUE','std::numeric_limits<double>::max()').replace('Double.isFinite','std::isfinite')
    text=text.replace('Float::POSITIVE_INFINITY','std::numeric_limits<float>::infinity()').replace('Float::NaN','std::numeric_limits<float>::quiet_NaN()').replace('Float::isNaN','std::isnan')
    text=re.sub(r'\(\(Double\)([^\n;]*?)\.getValue\(\)\)\.floatValue\(\)',r'\1.getValue()',text)
    text=text.replace(' instanceof LivingEntity',' != nullptr').replace(' instanceof PlayerEntity','->player')
    text=text.replace('MinecraftClient client=', 'MinecraftClient* client=').replace('client.world','client->world').replace('client->world.', 'client->world->')
    text=re.sub(r'(\w+) >>> (\d+)',r'(static_cast<std::uint32_t>(\1) >> \2)',text)
    return text
out='// Generated from the supplied death visual engine. Runtime: C++ only.\n#include "BloomCompat.h"\n#include "DeathEffects.h"\nnamespace vanta::modules::bloom::death {\n'
out+='\n'.join(death(m.group(0)[:-1].strip())+';'for m in methods)+'\n'+death(s)
out+='\nconst bool initialized=[](){'+death(initial)+'return true;}();\nbool visible(){return dl>0||gf||HIDE_N>0;}\nint active(){return dl;}\nvoid configure(const DeathSettings&v){on=v.enabled;ae=v.mode;pal=v.theme?-3:v.color;max=v.max;af=v.scale;ag=v.amount;aa=v.hide;ab=v.sounds;ac=v.flash;}\n}\n'
(ROOT/'src/modules/visuals/bloom/DeathEngine.cpp').write_text(out,encoding='utf-8')
