"""Verify actual encoded deliverables and firmware media constraints."""
from pathlib import Path
from PIL import Image, ImageChops, ImageStat, JpegImagePlugin
import json, subprocess, hashlib
root=Path(__file__).resolve().parents[1]
manifest=json.loads((root/'assets'/'manifest.json').read_text(encoding='utf-8'))
report={'checks':[],'actions':[],'hardware_tested':False}
master=Image.open(root/'assets'/'character.png')
assert master.mode=='RGBA' and master.getchannel('A').getextrema()==(0,255)
report['checks'].append('Master is RGBA with actual transparent and opaque pixels')
for action in manifest['actions']:
    name=action['name']; frames=[]
    for i in range(8):
        file=root/'firmware'/'spiffs'/f'{name}_{i:02d}.jpg'
        im=Image.open(file); im.load()
        assert im.size==(400,480) and im.mode=='RGB'
        assert JpegImagePlugin.get_sampling(im)==2, file
        assert not im.info.get('progressive')
        assert file.stat().st_size < 128*1024
        # The panel-safe margins must remain black after JPEG encoding.
        for pixel in [(0,0),(399,0),(0,479),(399,479)]:
            assert max(im.getpixel(pixel))<=4
        frames.append(im)
    diffs=[sum(ImageStat.Stat(ImageChops.difference(frames[i],frames[(i+1)%8])).mean)/3 for i in range(8)]
    assert max(diffs)>1, 'No actual motion'
    videos=[]
    for suffix in ('mp4','avi'):
        path=root/'videos'/f'{name}.{suffix}'
        probe=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0',
                          '-show_entries','stream=codec_name,width,height,pix_fmt,r_frame_rate,nb_frames,duration',
                          '-of','json',str(path)],text=True))['streams'][0]
        assert (probe['width'],probe['height'])==(410,502)
        assert probe['codec_name']==('h264' if suffix=='mp4' else 'mjpeg')
        assert probe['r_frame_rate']=='20/1'
        if suffix=='avi': assert abs(float(probe['duration'])-action['duration_ms']/1000)<0.001
        # Decode the whole stream, not only the container header.
        subprocess.run(['ffmpeg','-v','error','-i',str(path),'-f','null','-'],check=True)
        videos.append({'file':path.name,**probe})
    report['actions'].append({'name':name,'frame_step_mean_rgb':[round(d,2) for d in diffs],
                              'videos':videos})
report['checks'] += ['32 baseline JPEG frames are 400x480 YUV420, each <128 KiB',
                    'All eight MP4/AVI files fully decode at 410x502, 20 fps',
                    'AVI loop durations match firmware timing',
                    'Motion exists in every action, all four corner pixels remain black']
report['frame_data_bytes']=sum(f.stat().st_size for f in (root/'firmware'/'spiffs').glob('*.jpg'))
report['checks'].append('All frame assets fit in 8 MiB SPIFFS partition')
assert report['frame_data_bytes'] < 8*1024*1024*0.7
(root/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps({'checks':report['checks'],'frame_data_bytes':report['frame_data_bytes']},indent=2))
