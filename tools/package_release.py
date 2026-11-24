from pathlib import Path
import json, shutil, hashlib, zipfile
root=Path(__file__).resolve().parents[1]
build=root/'firmware'/'build'; release=root/'release'; release.mkdir(exist_ok=True)
for source,dest in [('bootloader/bootloader.bin','bootloader.bin'),
                    ('partition_table/partition-table.bin','partition-table.bin'),
                    ('whale_maid.bin','whale_maid.bin'),('storage.bin','storage.bin')]:
    shutil.copy2(build/source,release/dest)
flasher=json.loads((build/'flasher_args.json').read_text())
files={offset:Path(path).name for offset,path in flasher['flash_files'].items()}
(release/'offsets.json').write_text(json.dumps(files,indent=2),encoding='utf-8')
checksums={f.name:hashlib.sha256(f.read_bytes()).hexdigest() for f in release.glob('*.bin')}
(release/'sha256.json').write_text(json.dumps(checksums,indent=2),encoding='utf-8')
report=json.loads((root/'verification.json').read_text(encoding='utf-8'))
report['firmware_build']={'result':'passed','toolchain':'ESP-IDF 6.1',
                          'log':'firmware/build-final.log','hardware_flashed':bool(report.get('device_flash',{}).get('verified')),
                          'flash_files':files}
(root/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
# The distribution excludes generated build intermediates and downloaded SDK components.
include=['START_HERE.md','verification.json','assets','videos','release',
         'preview/actions.jpg','preview/idle.gif','preview/wave.gif','preview/jump.gif','preview/sleep.gif',
         'tools','firmware/main','firmware/spiffs','firmware/CMakeLists.txt',
         'firmware/partitions.csv','firmware/sdkconfig.defaults','firmware/sdkconfig',
         'firmware/dependencies.lock','firmware/build-final.log']
target=root/'whale-maid-project.zip'
with zipfile.ZipFile(target,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as archive:
    for item in include:
        path=root/item
        paths=sorted(path.rglob('*')) if path.is_dir() else [path]
        for file in paths:
            if file.is_file() and '__pycache__' not in file.parts:
                archive.write(file,Path('whale-maid')/file.relative_to(root))
print('Wrote',target,'bytes',target.stat().st_size)
print('Flash files',files)
