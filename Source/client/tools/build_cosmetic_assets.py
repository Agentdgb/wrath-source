"""Embed 20 original GodWeer cosmetics and mesh previews into native C++ data."""
import io
import json
import re
import sys
import zipfile
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
source, bridge = map(Path, sys.argv[1:3])
header = (root / "src/modules/render/swordreplace/SwordReplace.h").read_text()
catalogue = re.findall(r'"([^"]+)"', header.split("models[] = {")[1].split("};")[0])
assert len(catalogue) == 20 and len(set(catalogue)) == 20
prefix = "GodWeer/src/main/resources/"
files = {}
external = set()

def path_for(identifier, kind, ext):
    namespace, sep, path = identifier.partition(":")
    if not sep:
        namespace, path = "minecraft", namespace
    return f"assets/{namespace}/{kind}/{path}{ext}"

def matrix(axis, degrees):
    a = np.radians(degrees); c, s = np.cos(a), np.sin(a)
    if axis == "x": return np.array([[1,0,0],[0,c,-s],[0,s,c]])
    if axis == "y": return np.array([[c,0,s],[0,1,0],[-s,0,c]])
    return np.array([[c,-s,0],[s,c,0],[0,0,1]])

def preview(model, texture_files):
    textures = model["textures"]
    images = {}
    for key, name in textures.items():
        visited = set()
        while name.startswith("#"):
            assert name not in visited; visited.add(name); name = textures[name[1:]]
        images[key] = np.array(Image.open(io.BytesIO(texture_files[path_for(name,"textures",".png")])).convert("RGBA"))
    # Orthographic product view of the actual textured cuboids. No invented art.
    view = matrix("z", -24) @ matrix("y", -18) @ matrix("x", 8)
    quads = []
    for element in model.get("elements", []):
        x0,y0,z0 = element["from"]; x1,y1,z1 = element["to"]
        corners = {
            "north": [[x1,y0,z0],[x0,y0,z0],[x0,y1,z0],[x1,y1,z0]],
            "south": [[x0,y0,z1],[x1,y0,z1],[x1,y1,z1],[x0,y1,z1]],
            "west": [[x0,y0,z0],[x0,y0,z1],[x0,y1,z1],[x0,y1,z0]],
            "east": [[x1,y0,z1],[x1,y0,z0],[x1,y1,z0],[x1,y1,z1]],
            "up": [[x0,y1,z1],[x1,y1,z1],[x1,y1,z0],[x0,y1,z0]],
            "down": [[x0,y0,z0],[x1,y0,z0],[x1,y0,z1],[x0,y0,z1]],
        }
        for direction, face in element.get("faces", {}).items():
            points = np.array(corners[direction], dtype=float)
            rotation = element.get("rotation")
            if rotation:
                origin = np.array(rotation["origin"])
                if rotation.get("rescale"):
                    scale = 1 / np.cos(np.radians(rotation["angle"]))
                    axes = [a != rotation["axis"] for a in "xyz"]
                    points = origin + (points-origin)*np.where(axes,scale,1)
                points = (points-origin) @ matrix(rotation["axis"], rotation["angle"]).T + origin
            points = (points-8) @ view.T
            u0,v0,u1,v1 = face.get("uv", [0,0,16,16])
            uv = np.array([[u0,v1],[u1,v1],[u1,v0],[u0,v0]]) / 16
            uv = np.roll(uv, int(face.get("rotation", 0)/90), axis=0)
            tex = images[face["texture"].lstrip("#")]
            quads.append((points,uv,tex))
    assert quads, "model has no geometry"
    all_points = np.concatenate([q[0] for q in quads])
    low,high = all_points[:,:2].min(axis=0),all_points[:,:2].max(axis=0)
    center = (low+high)/2; scale = 420/max(high-low)
    canvas = np.zeros((512,512,4), dtype=np.uint8)
    depth = np.full((512,512), -np.inf)
    for pts,uv,tex in quads:
        xy = (pts[:,:2]-center)*scale
        xy[:,0] += 256; xy[:,1] = 256-xy[:,1]
        normal = np.cross(pts[1]-pts[0],pts[2]-pts[0]); n = np.linalg.norm(normal)
        shade = .72 + .28*abs(normal[2]/n) if n else 1
        for triangle in ((0,1,2),(0,2,3)):
            p=xy[list(triangle)]; t=uv[list(triangle)]; z=pts[list(triangle),2]
            lo=np.maximum(np.floor(p.min(axis=0)).astype(int),0)
            hi=np.minimum(np.ceil(p.max(axis=0)).astype(int),511)
            if (hi<lo).any(): continue
            x,y=np.meshgrid(np.arange(lo[0],hi[0]+1)+.5,np.arange(lo[1],hi[1]+1)+.5)
            denom=(p[1,1]-p[2,1])*(p[0,0]-p[2,0])+(p[2,0]-p[1,0])*(p[0,1]-p[2,1])
            if abs(denom)<1e-8: continue
            a=((p[1,1]-p[2,1])*(x-p[2,0])+(p[2,0]-p[1,0])*(y-p[2,1]))/denom
            b=((p[2,1]-p[0,1])*(x-p[2,0])+(p[0,0]-p[2,0])*(y-p[2,1]))/denom
            c=1-a-b; inside=(a>=-1e-5)&(b>=-1e-5)&(c>=-1e-5)
            tu=a*t[0,0]+b*t[1,0]+c*t[2,0]; tv=a*t[0,1]+b*t[1,1]+c*t[2,1]
            color=tex[np.clip((tv*tex.shape[0]).astype(int),0,tex.shape[0]-1),np.clip((tu*tex.shape[1]).astype(int),0,tex.shape[1]-1)].copy()
            color[:,:,:3]=(color[:,:,:3]*shade).astype(np.uint8)
            zz=a*z[0]+b*z[1]+c*z[2]
            region=depth[lo[1]:hi[1]+1,lo[0]:hi[0]+1]
            mask=inside&(color[:,:,3]>5)&(zz>=region)
            region[mask]=zz[mask]
            canvas[lo[1]:hi[1]+1,lo[0]:hi[0]+1][mask]=color[mask]
    image=Image.fromarray(canvas)
    assert np.count_nonzero(canvas[:,:,3]) > 200, "empty cosmetic preview"
    return image

with zipfile.ZipFile(source) as archive:
    names = set(archive.namelist())
    selector = json.loads(archive.read(prefix+"assets/minecraft/items/diamond_sword.json"))["model"]
    choices = {case["when"]:case["model"]["model"] for case in selector["cases"]}
    model_paths = [choices[name] for name in catalogue]
    def collect(path):
        if path in files: return
        if prefix+path not in names:
            external.add(path); return
        files[path]=archive.read(prefix+path)
        if path.endswith(".json"):
            obj=json.loads(files[path])
            if "parent" in obj: collect(path_for(obj["parent"],"models",".json"))
            for texture in obj.get("textures",{}).values():
                if texture.startswith("#"): continue
                p=path_for(texture,"textures",".png"); collect(p)
                if prefix+p+".mcmeta" in names: collect(p+".mcmeta")
    for model_path in model_paths: collect(path_for(model_path,"models",".json"))
    assert external <= {"assets/minecraft/models/item/handheld.json","assets/minecraft/models/item/generated.json"}, external
    previews=[]
    preview_dir=root/"assets/swordreplace-previews"; preview_dir.mkdir(exist_ok=True)
    for i,(name,model_path) in enumerate(zip(catalogue,model_paths)):
        obj=json.loads(files[path_for(model_path,"models",".json")])
        image=preview(obj,files); previews.append(image)
        buffer=io.BytesIO(); image.save(buffer,format="PNG",optimize=True)
        files[f"preview/{i}.png"]=buffer.getvalue()
        image.save(preview_dir/f"{i}.png")
files["native/CosmeticBridge.class"]=bridge.read_bytes()
files["pack.mcmeta"]=json.dumps({"pack":{"pack_format":75,"min_format":46,"max_format":75,"description":"Sword cosmetics"}}).encode()
output=root/"src/modules/render/swordreplace/SwordReplaceAssets.cpp"
with output.open("w") as out:
    out.write('// Generated from original GodWeer assets by tools/build_cosmetic_assets.py.\n#include "SwordReplace.h"\nnamespace vanta::modules::swordreplace {\nnamespace {\n')
    ordered=sorted(files.items())
    for i,(path,data) in enumerate(ordered):
        out.write(f"const unsigned char data_{i}[] = {{\n")
        for offset in range(0,len(data),24): out.write(','.join(str(b) for b in data[offset:offset+24])+',\n')
        out.write('};\n')
    out.write('}\nconst Asset assets[] = {\n')
    for i,(path,data) in enumerate(ordered): out.write(f'{{"{path}",data_{i},{len(data)}}},\n')
    out.write('};\nconst unsigned assetCount = std::size(assets);\nconst char* const modelPaths[] = {\n')
    for path in model_paths: out.write(json.dumps(path)+',\n')
    out.write('};\nconst Asset* findAsset(std::string_view path) { for(const auto& a:assets) if(path==a.path) return &a; return nullptr; }\n}\n')
sheet=Image.new("RGB",(1000,800),(16,17,23)); draw=ImageDraw.Draw(sheet)
for i,image in enumerate(previews):
    x=(i%5)*200; y=(i//5)*200
    sheet.paste(image.resize((174,174)),(x+13,y),image.resize((174,174)))
    draw.text((x+8,y+177),catalogue[i],fill="white")
sheet.save(preview_dir/"contact-sheet.png")
print(f"Embedded {len(catalogue)} cosmetics, {len(files)} resources and 20 textured mesh previews; no external resource pack.")
