"""Translate the provided static numeric engine; JVM access is supplied by C++ snapshots.

Kept reproducible so fixes to the supplied source can be reviewed against generated
code. This deliberately rejects unsupported Java constructs instead of guessing.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(__file__).with_name("DriftPassCache.java")
OUT = ROOT / "src/modules/visuals/bloom/WorldParticlesEngine.cpp"

def convert(text):
    pointers = set(re.findall(r'\b(?:MinecraftClient|ClientWorld|Camera|Entity|LivingEntity)\s+(var\d+)', text))
    pointers.update(re.findall(r'instanceof LivingEntity\s+(var\d+)', text))
    text = re.sub(r'if \(([^\n]+) instanceof LivingEntity (var\d+) &&', r'if (auto* \2 = \1; \2 &&', text)
    text = re.sub(r'\b(?:public|private|protected)\s+static\s+(?:final\s+)?', '', text)
    text = text.replace('boolean', 'bool').replace('null', 'nullptr')
    for typ in ['float','double','int','bool']:
        text = re.sub(r'new '+typ+r'\[\]\s*\{', 'Array<'+typ+'> {', text)
        text = re.sub(r'new '+typ+r'\[([^\]]+)\]', r'Array<'+typ+r'>(\1)', text)
        text = re.sub(r'\b'+typ+r'\[\]', 'Array<'+typ+'>', text)
    text = text.replace('new Random()', 'Random()')
    text = text.replace('.length', '.size()')
    for cls in ['RegionPassCache','Math','MathHelper','Float','MinecraftClient','Heightmap.Type','StatusEffects','CameraSubmersionType']:
        text = text.replace(cls+'.', cls.replace('.', '::')+'::')
    text = text.replace('MinecraftClient::getInstance().', 'MinecraftClient::getInstance()->')
    text = text.replace('->player.', '->player->').replace('.player.', '.player->').replace('->world.', '->world->').replace('.world.', '.world->')
    for name in pointers:
        text = re.sub(r'\b'+name+r'\.', name+'->', text)
    text = re.sub(r'\bMinecraftClient\s+(var\d+)', r'MinecraftClient* \1', text)
    text = re.sub(r'\bCamera\s+(var\d+)', r'Camera* \1', text)
    text = re.sub(r'catch \(RuntimeException \w+\)\s*\{\s*throw \w+;\s*\}', 'catch (...) { throw; }', text)
    return text

source = SOURCE.read_text(encoding="utf-8-sig")
source = re.sub(r'assert\b.*?;', '', source, flags=re.S)
body = source[source.index('{', source.index('public final class'))+1:source.rfind('}')]
body = re.sub(r'private DriftPassCache\(\)\s*\{\s*\}', '', body)
methods = list(re.finditer(r'(?:public|private) static ([\w\[\]]+) (\w+)\(([^)]*)\)\s*\{', body))
prototypes = [convert(m.group(0)[:-1].strip())+';' for m in methods]
result = ('// Generated from the supplied WorldParticles engine. Runtime: C++ only.\n'
          '#include "BloomCompat.h"\nnamespace vanta::modules::bloom::world {\n'
          + '\n'.join(prototypes) + '\n' + convert(body) + '\nbool visible(){return am>0 || RegionPassCache::bh;}\nvoid seedForTest(std::uint64_t seed){al.setSeed(seed);}\n}\n')
if re.search(r'\b(?:new|instanceof|RuntimeException|assert)\b', result):
    raise SystemExit('Untranslated Java construct in WorldParticles')
OUT.write_text(result, encoding='utf-8')
print(OUT)
