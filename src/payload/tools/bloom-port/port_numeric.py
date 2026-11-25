"""Translate the supplied pure raster/geometry kernels. No Java runtime is used.

The only scheduling adapter is Span's offline image bake: its row workers are
run sequentially, producing the same pixels. Unsupported constructs fail builds.
"""
from pathlib import Path
import re, shutil
ROOT=Path(__file__).resolve().parents[2]
ORIGINAL=ROOT.parent.parent/'.fix-build/bloom-complete/Bloomclient-buildable/src/main/java/net/caffeinemc/mods/lithium/lattice/bundle/pipeline/shard'
DEST=ROOT/'src/modules/visuals/bloom/native'
SAVED=Path(__file__).with_name('original')
DEST.mkdir(exist_ok=True);SAVED.mkdir(exist_ok=True)
NAMES={'PaletteArrayCache':'palette','FacetArrayCache':'facet','BatchArrayCache':'branch','SpanArrayCache':'span','StrataArrayCache':'strata','RibbonArrayCache':'ribbon','SectorArrayCache':'sector','SliceArrayCache':'slice'}

def replace_body(text,name,replacement):
    m=re.search(r'(?:public|private|protected)?\s*static [\w\[\]]+ '+name+r'\([^)]*\)\s*\{',text)
    if not m:raise ValueError(name)
    start=m.end();pos=start;depth=1
    while depth:
        if text[pos]=='{':depth+=1
        elif text[pos]=='}':depth-=1
        pos+=1
    return text[:start]+replacement+text[pos-1:]

def unsigned_shifts(text):
    while '>>>' in text:
        pos=text.index('>>>');end=pos
        while text[end-1].isspace():end-=1
        start=end-1;depth=0
        while start>=0:
            ch=text[start]
            if ch in '])':depth+=1
            elif ch in '[(':depth-=1
            if depth<0:break
            if depth==0 and not(ch.isalnum()or ch in '_.[]()'):break
            start-=1
        start+=1
        m=re.match(r'>>>\s*(\d+)',text[pos:]);bits=int(m[1]);lhs=text[start:end]
        typ='std::uint64_t' if bits>=32 or re.search(r'\blong '+re.escape(lhs)+r'\b',text)else'std::uint32_t'
        text=text[:start]+f'(static_cast<{typ}>({lhs}) >> {bits})'+text[pos+m.end():]
    return text

def convert(text):
    text=unsigned_shifts(text)
    # Java evaluates an assignment's indexed LHS before the incrementing RHS.
    text=re.sub(r'(\w+)\[(\w+)\] = \2\+\+',r'(\1[\2]=\2,++\2)',text)
    text=re.sub(r'\b(?:public|private|protected)\s+static\s+(?:final\s+)?','',text)
    text=re.sub(r'\b(?:static|volatile|final|synchronized)\s+','',text)
    text=text.replace('boolean','bool').replace('null','nullptr')
    for typ,native in [('bool','bool'),('float','float'),('double','double'),('int','int'),('byte','std::int8_t'),('short','std::int16_t'),('long','std::uint64_t'),('String','std::string')]:
        text=re.sub(r'new '+typ+r'\[\]\[\]\s*\{','Array<Array<'+native+'>>{',text)
        text=re.sub(r'new '+typ+r'\[([^\]]+)\]\[([^\]]+)\]',r'matrix<'+native+r'>(\1,\2)',text)
        text=re.sub(r'new '+typ+r'\[([^\]]+)\]\[\]',r'Array<Array<'+native+r'>>(\1)',text)
        text=re.sub(r'new '+typ+r'\[\]\s*\{','Array<'+native+'> {',text)
        # Nested indexes in dimensions need balanced brackets.
        pattern=re.compile(r'new '+typ+r'\[')
        while (m:=pattern.search(text)):
            pos=m.end();start=pos;depth=1
            while depth:
                if text[pos]=='[':depth+=1
                elif text[pos]==']':depth-=1
                pos+=1
            text=text[:m.start()]+'Array<'+native+'>('+text[start:pos-1]+')'+text[pos:]
        text=text.replace(typ+'[][]','Array<Array<'+native+'>>').replace(typ+'[]','Array<'+native+'>')
    text=re.sub(r'\blong\b','std::uint64_t',text)
    text=re.sub(r'\bbyte\b','int',text)
    text=re.sub(r'\bString\b','std::string',text)
    text=re.sub(r'\bObject (var\d+) = (var\d+\[[^\]]+\]\+\+);',r'int \1 = \2;',text)
    text=re.sub(r'new Random\(([^)]*)\)',r'Random(\1)',text)
    text=text.replace('Runtime.getRuntime().availableProcessors() - Runtime.getRuntime().availableProcessors()','0')
    text=text.replace('Integer.bitCount','bitCount')
    text=text.replace('new Object().hashCode()','0').replace('Thread.currentThread().getName().length()','0')
    text=text.replace('System.arraycopy','arraycopy').replace('System.nanoTime()','0')
    text=text.replace('.length()', '.size()').replace('.length','.size()').replace('.charAt(','.at(')
    for cls in ['Math','MathHelper','Arrays','Float']:
        text=text.replace(cls+'.',cls+'::')
    for java,native in NAMES.items():text=text.replace(java+'.',native+'::')
    text=text.replace('" + "','" "')
    text=re.sub(r'catch \((?:RuntimeException|Throwable) \w+\)\s*\{\s*throw \w+;\s*\}','catch (...) { throw; }',text)
    text=re.sub(r'catch \(Throwable \w+\)', 'catch (...)',text)
    return text

header='#pragma once\n#include "../BloomCompat.h"\nnamespace dt::modules::bloom {\n'
for name,namespace in NAMES.items():
    path=SAVED/(name+'.java')
    if not path.exists():shutil.copyfile(ORIGINAL/(name+'.java'),path)
    s=path.read_text(encoding='utf-8-sig');s=re.sub(r'assert\b.*?;', '',s,flags=re.S).replace('synchronized ','')
    s=s[s.index('{',s.index('public final class'))+1:s.rfind('}')]
    s=re.sub(r'private '+name+r'\(\)\s*\{\s*\}', '',s)
    if namespace=='span':s=replace_body(s,'a','int[] pixels=new int[6340704];bc=false;b(pixels,0,1);return pixels;')
    if namespace=='branch':s=replace_body(s,'j','for(int k=1;k<var2;++k)if(var0[k]<var3||var0[k]>var5||var1[k]<var3||var1[k]>512-var3)return k;return var2;')
    initial=''
    if 'static {'in s:
        where=s.rfind('static {');initial=s[where+8:s.rfind('}')];s=s[:where]
    methods=list(re.finditer(r'(?:public|private)?\s*static ([\w\[\]]+) (\w+)\(([^)]*)\)\s*\{',s))
    declarations='\n'.join(convert(m.group(0)[:-1].strip())+';'for m in methods)
    fields=s[:methods[0].start()]
    # Preserve array initializer contents in the implementation; header only
    # declares the handles shared by geometry adapters.
    fielddecl=[];start=0;depth=0
    for i,ch in enumerate(fields):
        if ch=='{':depth+=1
        elif ch=='}':depth-=1
        elif ch==';'and depth==0:
            raw=convert(fields[start:i+1]).strip();start=i+1
            if not raw:continue
            raw=raw.split('=',1)[0].strip()+';';fielddecl.append('extern '+raw)
    header+='namespace '+namespace+' {\n'+'\n'.join(fielddecl)+'\n'+declarations+'\n}\n'
    text='// Ported from Bloom 2.2; numerical source preserved. Native C++ only.\n#include "NativeImages.h"\nnamespace dt::modules::bloom::'+namespace+' {\n'+convert(s)
    if initial:text+='\nconst bool initialized=[](){'+convert(initial)+'return true;}();\n'
    replacements={'(var64 - var60) % var78':'std::fmod(var64-var60,var78)', 'var1 % 6283.185307179586':'std::fmod(var1,6283.185307179586)', '((double)var12[var14 + 2] * var2 + (double)var12[var14 + 3]) % (Math::PI * 2)':'std::fmod((double)var12[var14+2]*var2+(double)var12[var14+3],Math::PI*2)', 'var1 * 1.8E-4 % 1.0':'std::fmod(var1*1.8E-4,1.0)', 'var1[var2 + 19] % 2.0':'std::fmod(var1[var2+19],2.0)'}
    for a,b in replacements.items():text=text.replace(a,b)
    text+='}\n'
    (DEST/(name+'.cpp')).write_text(text,encoding='utf-8')
    if re.search(r'\b(?:new|instanceof|Throwable|RuntimeException|Thread|System)\b',text):raise ValueError('Unsupported Java in '+name)
header+='}\n'
(DEST/'NativeImages.h').write_text(header,encoding='utf-8')




