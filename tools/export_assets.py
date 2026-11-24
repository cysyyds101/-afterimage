"""Slice imagegen atlases and package the SAME keyframe timing for video/ESP32.

Only media preparation: grid slicing, uniform sizing, black compositing and encoding.
Character artwork and poses are from imagegen; no procedural face/limb replacement.
Requires Pillow, ffmpeg and ffprobe on PATH. Run from any directory.
"""
from pathlib import Path
from PIL import Image, ImageChops
import bisect
import json
import subprocess
import hashlib

ROOT = Path(__file__).resolve().parents[1]
ACTIONS = {
    'idle': {'label': '待机眨眼', 'ms': [500,250,350,100,100,100,300,300]},
    'wave': {'label': '挥手招呼', 'ms': [150]*8},
    'jump': {'label': '开心跳跃', 'ms': [150,150,100,100,200,100,150,250]},
    'sleep': {'label': '困倦点头', 'ms': [400]*8},
}
W,H,FPS = 400,480,20

def run(*args):
    subprocess.run([str(a) for a in args], check=True)

def encode(frames, dest, avi=False):
    command = ['ffmpeg','-hide_banner','-loglevel','error','-y',
               '-f','rawvideo','-pixel_format','rgb24','-video_size','410x502',
               '-framerate',str(FPS),'-i','pipe:0','-an']
    command += (['-c:v','mjpeg','-q:v','4','-pix_fmt','yuvj420p'] if avi else
                ['-c:v','libx264','-crf','18','-pix_fmt','yuv420p','-movflags','+faststart'])
    command += [str(dest)]
    proc = subprocess.Popen(command, stdin=subprocess.PIPE)
    try:
        for frame in frames: proc.stdin.write(frame.tobytes())
    finally: proc.stdin.close()
    if proc.wait(): raise RuntimeError(f'Encoding failed: {dest}')

def main():
    all_cells, union = {}, None
    for name in ACTIONS:
        sheet = Image.open(ROOT/'assets'/f'{name}_atlas.png').convert('RGBA')
        cells = []
        for idx in range(8):
            col,row = idx%4,idx//4
            box = (round(col*sheet.width/4),round(row*sheet.height/2),
                   round((col+1)*sheet.width/4),round((row+1)*sheet.height/2))
            rgba = sheet.crop(box).resize((512,512),Image.Resampling.LANCZOS)
            rgb = Image.new('RGB',rgba.size,'black'); rgb.paste(rgba,mask=rgba.getchannel('A'))
            bounds = rgb.convert('L').point(lambda p: 255 if p>18 else 0).getbbox()
            if bounds is None: raise ValueError(f'Empty cell {name}:{idx}')
            union = bounds if union is None else (min(union[0],bounds[0]),min(union[1],bounds[1]),max(union[2],bounds[2]),max(union[3],bounds[3]))
            cells.append(rgb)
        all_cells[name] = cells
    # A single crop and scale for ALL actions/frames preserves the movement.
    left,top,right,bottom = union
    scale = min(380/(right-left),440/(bottom-top))
    size = (round((right-left)*scale),round((bottom-top)*scale))
    dst = ((W-size[0])//2,(H-size[1])//2)
    manifest = {'screen':[410,502],'frame_size':[W,H], 'video_fps':FPS,
                'method':'imagegen eight-keyframe 2D animation, held frames, no optical flow',
                'actions':[]}
    spiffs = ROOT/'firmware'/'spiffs'; spiffs.mkdir(parents=True,exist_ok=True)
    previews,combined=[],[]
    for name,info in ACTIONS.items():
        outdir=ROOT/'assets'/'frames'/name; outdir.mkdir(parents=True,exist_ok=True)
        frame_images=[]
        for idx,cell in enumerate(all_cells[name]):
            frame=Image.new('RGB',(W,H),'black')
            frame.paste(cell.crop(union).resize(size,Image.Resampling.LANCZOS),dst)
            frame.save(outdir/f'{idx:02d}.png')
            frame.save(spiffs/f'{name}_{idx:02d}.jpg',quality=90,subsampling=2,optimize=True)
            panel=Image.new('RGB',(410,502),'black'); panel.paste(frame,(5,11))
            frame_images.append(panel)
        timeline=[]
        for frame,ms in zip(frame_images,info['ms']):
            assert ms%50==0
            timeline.extend([frame]*(ms//50))
        # Each standalone preview contains enough complete loops to play >=4 seconds.
        repetitions=max(2,(4000+sum(info['ms'])-1)//sum(info['ms']))
        repeated=timeline*repetitions
        encode(repeated,ROOT/'videos'/f'{name}.mp4')
        encode(timeline,ROOT/'videos'/f'{name}.avi',avi=True)
        gif_frames=[frame.resize((205,251),Image.Resampling.LANCZOS) for frame in frame_images]
        gif_frames[0].save(ROOT/'preview'/f'{name}.gif',save_all=True,append_images=gif_frames[1:],
                           duration=info['ms'],loop=0,disposal=2)
        combined.extend(repeated)
        previews.append(frame_images[4 if name in ('jump','sleep') else 2])
        manifest['actions'].append({'name':name,'label':info['label'],'keyframes':8,
                                    'duration_ms':sum(info['ms']),'frame_ms':info['ms'],
                                    'jpeg_bytes':sum((spiffs/f'{name}_{i:02d}.jpg').stat().st_size for i in range(8))})
    encode(combined,ROOT/'videos'/'all_actions.mp4')
    poster=Image.new('RGB',(410*4,502),'#000000')
    for idx,im in enumerate(previews): poster.paste(im,(410*idx,0))
    poster.save(ROOT/'preview'/'actions.jpg',quality=95)
    (ROOT/'assets'/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
    header='#pragma once\n#include <stdint.h>\n#define ANIM_WIDTH 400\n#define ANIM_HEIGHT 480\n#define ANIM_COUNT 4\n#define ANIM_FRAMES 8\n'
    header+='static const char *const anim_names[ANIM_COUNT] = {'+','.join(json.dumps(n) for n in ACTIONS)+'};\n'
    header+='static const uint16_t anim_durations[ANIM_COUNT][ANIM_FRAMES] = {\n'
    header+=''.join('    {'+','.join(map(str,a['ms']))+'},\n' for a in ACTIONS.values())+'};\n'
    (ROOT/'firmware'/'main'/'animation_manifest.h').write_text(header,encoding='utf-8')
    print(json.dumps(manifest,ensure_ascii=False,indent=2))

if __name__=='__main__': main()
