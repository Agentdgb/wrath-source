from pathlib import Path
import re
from transpile import convert, ROOT

path=Path(__file__).with_name('SceneLayerCache.java')
s=path.read_text(encoding='utf-8-sig')
s=re.sub(r'assert\b.*?;', '', s, flags=re.S)
s=s[s.index('{',s.index('public final class'))+1:s.rfind('}')]
s=re.sub(r'private SceneLayerCache\(\)\s*\{\s*\}', '', s)
s=re.sub(r'public static final Identifier ART[^;]+;', 'public static final int ART=0;',s)
methods=list(re.finditer(r'(?:public|private) static ([\w\[\]]+) (\w+)\(([^)]*)\)\s*\{',s))
def scene(text):
    text=convert(text)
    text=re.sub(r'\blong\b','std::int64_t',text)
    for cls in ['SyncController','BatchArrayCache','RenderPipelines']:
        text=text.replace(cls+'.',cls+'::')
    text=re.sub(r'(var\d+)\.(getMatrices|drawTexture|pushMatrix|popMatrix|translate|rotate|scale)\(',r'\1->\2(',text)
    text=re.sub(r'try\s*\{([^{}]*)\}\s*finally\s*\{([^{}]*)\}',r'{auto cleanup=finally([&]{\2});\1}',text)
    text=text.replace('bg[var8] % (float) (Math::PI * 2)','std::fmod(bg[var8],static_cast<float>(Math::PI*2))')
    return text
out='// Generated from the supplied GUI petal simulation. Runtime: C++ only.\n#include "PetalCompat.h"\nnamespace vanta::modules::bloom::petals {\n'
out+='\n'.join(scene(m.group(0)[:-1].strip())+';'for m in methods)+'\n'+scene(s)
out+='\nvoid reset(){v=false;av=0;aa=0;ao=false;}\n}\n'
(ROOT/'src/modules/visuals/bloom/PetalEngine.cpp').write_text(out,encoding='utf-8')
