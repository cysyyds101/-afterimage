from pathlib import Path
from PIL import Image
from io import BytesIO
import json, base64, argparse

root = Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('fragment',type=Path)
args=parser.parse_args()
manifest=json.loads((root/'assets'/'manifest.json').read_text(encoding='utf-8'))
data=[]
for action in manifest['actions']:
    frames=[]
    for index in range(8):
        im=Image.open(root/'assets'/'frames'/action['name']/f'{index:02d}.png').convert('RGB')
        panel=Image.new('RGB',(410,502),'black'); panel.paste(im,(5,11))
        buf=BytesIO(); panel.resize((246,301),Image.Resampling.LANCZOS).save(buf,format='JPEG',quality=78,optimize=True,subsampling=2)
        frames.append('data:image/jpeg;base64,'+base64.b64encode(buf.getvalue()).decode())
    data.append({'label':action['label'],'ms':action['frame_ms'],'frames':frames})
text=args.fragment.read_text(encoding='utf-8')
if '__ASSETS__' not in text: raise RuntimeError('Template already populated; restore template to regenerate.')
text=text.replace('__ASSETS__',json.dumps(data,ensure_ascii=False,separators=(',',':')))
args.fragment.write_text(text,encoding='utf-8')
assert args.fragment.stat().st_size < 1000000
assert '\\"' not in text and '__ASSETS__' not in text
print('Inline preview bytes:', args.fragment.stat().st_size)
