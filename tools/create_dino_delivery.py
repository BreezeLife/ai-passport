#!/usr/bin/env python3
"""Package one verified Dino Passport 40 build, source snapshot and host previews.

All writes stay under build/delivery. Nothing is flashed, committed or uploaded.
Run only after the complete 40-species assets and real-LVGL previews are ready.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / 'build'
DELIVERY_ROOT = BUILD / 'delivery'
DEFAULT_NAME = 'DinoPassport40-2026-10-02'
FULL_BIN = 'FoloToy-AI-Passport-full.bin'
APP_BIN = 'FoloToy-AI-Passport.bin'
AUDIO_DIR = ROOT / 'assets/audio/dinobook40'
ANIMATION_DIR = ROOT / 'assets/animations/dinosaurs40'
EXCLUDED_DIRS = {'.git', 'node_modules', 'build', 'managed_components', '.agents',
                 '.project-pulse', '__pycache__'}
EXCLUDED_ROOT_FILES = {'sdkconfig', 'sdkconfig.old', 'STATUS.md', 'STATUS.zh_CN.md'}
HEX_HASH = re.compile(r'[0-9a-f]{64}')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_path(path, root=ROOT, must_exist=True):
    """Check every component without following user-created symlinks."""
    path = Path(path)
    if not path.is_absolute():
        path = root / path
    require('..' not in path.parts, 'Traversal in path: ' + str(path))
    require(path.is_relative_to(root), 'Path outside permitted directory: ' + str(path))
    current = root
    require(not current.is_symlink(), 'Repository root must not be a symlink')
    for part in path.relative_to(root).parts:
        current = current / part
        if current.exists() or current.is_symlink():
            require(not current.is_symlink(), 'Symlink is not a package input: ' + str(current))
        elif must_exist:
            raise ValueError('Missing package input: ' + str(current))
    return path


def regular(path):
    path = safe_path(path)
    require(stat.S_ISREG(path.lstat().st_mode), 'Expected regular file: ' + str(path))
    return path


def read_file(path):
    return regular(path).read_bytes()


def read_json(path):
    return json.loads(read_file(path).decode('utf-8'))


def relative_file(directory, name):
    require(isinstance(name, str) and name and '\\' not in name and '\x00' not in name,
            'Invalid relative file name')
    require(not any(ord(character) < 32 for character in name), 'Control character in file name')
    relative = PurePosixPath(name)
    require(not relative.is_absolute() and '..' not in relative.parts,
            'Invalid relative file name: ' + name)
    path = safe_path(directory / relative)
    require(path.is_relative_to(directory), 'Input escapes its directory: ' + name)
    return regular(path)


def check_hash(path, expected):
    require(isinstance(expected, str) and HEX_HASH.fullmatch(expected), 'Invalid SHA-256 field')
    data = read_file(path)
    require(digest(data) == expected, 'Hash mismatch: ' + str(path.relative_to(ROOT)))
    return data


def source_inventory():
    result = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'],
                            cwd=ROOT, check=True, capture_output=True)
    paths = []
    for raw in sorted(set(result.stdout.split(b'\x00')) - {b''}):
        name = os.fsdecode(raw)
        relative = PurePosixPath(name)
        require(not relative.is_absolute() and '..' not in relative.parts and
                not any(ord(character) < 32 for character in name), 'Unsafe source inventory name')
        # Skip installed skill mirrors before inspecting any symlink target.
        if (any(part in EXCLUDED_DIRS for part in relative.parts) or
                name in EXCLUDED_ROOT_FILES or name.endswith(('.pyc', '.pyo')) or
                name.startswith(('.claude/skills/', '.codex/skills/'))):
            continue
        path = regular(ROOT / relative)
        paths.append((name, path))
    require(paths, 'Empty Git source inventory')
    names = {name for name, _ in paths}
    required = {'AGENTS.md', 'PROJECT.md', 'MEMORY.md', 'TASKS.md', 'WORKLOG.md',
                'assets/data/dinosaurs.json',
                'assets/audio/dinobook40/manifest.json', 'assets/fonts/SourceHanSansSC-Normal.otf',
                'assets/fonts/SourceHanSansSC-OFL.txt', 'tools/create_dino_delivery.py',
                'tools/generate_dino_animation.py', 'tools/generate_dino_audio.py',
                'main/dino_animation_assets.c', 'main/dino_audio_assets.c'}
    require(required <= names, 'Missing required source files: ' + ', '.join(sorted(required - names)))
    require(any(name.startswith('tests/') for name in names) and
            any(name.startswith('docs/') for name in names), 'Tests/docs missing from source inventory')
    return paths


def check_assets(archive_manifest, archive):
    catalog = read_json(ROOT / 'assets/data/dinosaurs.json')
    require(len(catalog) == 40 and len({item['id'] for item in catalog}) == 40,
            'Exactly 40 unique dinosaurs are required')
    ids = [item['id'] for item in catalog]
    require(all(len(item['facts']) == 2 and len(item['spoken_facts']) == 2 and
                len(item['options']) == 2 and item['correct'] == index % 2
                for index, item in enumerate(catalog)), 'Incomplete facts/questions or answer order')
    audio = read_json(AUDIO_DIR / 'manifest.json')
    require(audio.get('schema_version') == 2 and len(audio.get('clips', [])) == 202 and
            [clip['id'] for clip in audio['clips']] == list(range(202)), 'Complete 202-clip audio required')
    require((audio['pcm']['sample_rate'], audio['pcm']['channels'], audio['pcm']['bits'],
             audio['source']['voice'], audio['source']['rate']) == (8000, 1, 16, 'Tingting', 155),
            'Unexpected narration format or voice provenance')
    require(audio['catalog_sha256'] == digest(read_file(ROOT / 'assets/data/dinosaurs.json')),
            'Audio catalog identity is stale')
    blob = check_hash(relative_file(AUDIO_DIR, audio['blob']['file']), audio['blob']['sha256'])
    require(len(blob) == audio['blob']['bytes'] and len(blob) <= 3_700_000 and
            len(blob) >= 64 and blob[:8] == b'DINOA40\x00' and
            struct.unpack_from('<I', blob, 8)[0] == len(blob) and
            blob[12:44] == hashlib.sha256(blob[64:]).digest() and blob[44:64] == bytes(20),
            'Invalid audio blob header/size/payload identity')
    require(blob[:64].hex() == audio['blob']['header_hex'], 'Audio header manifest mismatch')
    app = read_file(archive / APP_BIN)
    require(blob[:64] in app, 'Archived application does not contain this audio identity header')
    descriptors = b''.join(struct.pack('<III', clip['offset'], clip['bytes'], clip['samples'])
                           for clip in audio['clips'])
    require(descriptors in app, 'Archived application audio descriptors differ from this source')
    audio_images = [name for name, offset in archive_manifest['image_offsets'].items()
                    if offset == 0x35a000]
    require(len(archive_manifest['image_offsets']) == 4 and
            set(archive_manifest['image_offsets'].values()) == {0, 0x8000, 0x10000, 0x35a000},
            'Expected only bootloader/table/application/audio; no identity or Recovery payload')
    require(len(audio_images) == 1, 'Archived external audio must be at 0x35a000')
    require(read_file(archive / audio_images[0]) == blob, 'Archived audio differs from source audio')
    for clip in audio['clips']:
        offset, size, samples = clip['offset'], clip['bytes'], clip['samples']
        require(offset >= 64 and size > 0 and samples > 0 and size == (samples + 1) // 2 and
                offset + size <= len(blob), 'Invalid audio descriptor range')
        require(digest(blob[offset:offset + size]) == clip['adpcm_sha256'], 'Audio clip hash mismatch')
        check_hash(relative_file(AUDIO_DIR, clip['wav']), clip['wav_sha256'])
    check_hash(ROOT / 'main/dino_audio_assets.c', audio['generated_c_sha256'])
    check_hash(ROOT / 'main/dino_audio_assets.h', audio['generated_h_sha256'])
    animation = read_json(ANIMATION_DIR / 'metadata.json')
    require(animation.get('complete') is True and animation.get('species_count') == 40 and
            animation.get('frames') == 8 and len(animation.get('species', [])) == 40 and
            animation.get('width') == 144 and animation.get('height') == 88 and
            animation.get('frame_bytes') == 6336 and animation.get('frame_interval_ms') == 125 and
            animation.get('missing_originals') == [], 'Complete native 40/320 animation metadata required')
    require([item['id'] for item in animation['species']] == ids, 'Animation/catalog order mismatch')
    require(animation['source_plan_sha256'] == digest(read_file(ANIMATION_DIR / 'generation-plan.json')),
            'Animation generation plan is stale')
    check_hash(ROOT / 'main/dino_animation_assets.c', animation['generated_c_sha256'])
    for index, item in enumerate(animation['species']):
        require(item['index'] == index and item['unique_frames'] == 8 and len(item['frames']) == 8 and
                len({frame['rgb565_sha256'] for frame in item['frames']}) == 8,
                'Every species must have eight distinct final frames')
        check_hash(relative_file(ANIMATION_DIR, item['source']['path']), item['source']['sha256'])
        regular(ANIMATION_DIR / 'prompts' / (item['id'] + '.txt'))
        for key, length in [('packed', 50_688), ('palette', 32)]:
            data = check_hash(relative_file(ANIMATION_DIR, item[key]['path']), item[key]['sha256'])
            require(len(data) == length and data in app,
                    'Archived application lacks current animation ' + key + ': ' + item['id'])
        for frame in item['frames']:
            check_hash(relative_file(ANIMATION_DIR, frame['preview']), frame['preview_sha256'])
        check_hash(relative_file(ANIMATION_DIR, item['gif']['path']), item['gif']['sha256'])
    return catalog, audio, animation


def check_host_preview(directory, catalog):
    manifest = read_json(directory / 'dino-ui-host-preview.json')
    require(manifest.get('format_version') == 1 and manifest.get('host_tests') == 'PASS' and
            manifest.get('device_photographs') is False and manifest.get('engine') == 'LVGL 9.5.0' and
            manifest.get('pool_bytes') == 49152,
            'Passing actual-LVGL computer-preview manifest required')
    require(manifest.get('assets_metadata_sha256') == digest(read_file(ANIMATION_DIR / 'metadata.json')),
            'Host preview uses a different animation metadata snapshot')
    species = manifest.get('species', [])
    require(len(species) == 40 and [item['id'] for item in species] == [item['id'] for item in catalog],
            'Host preview must contain the same ordered 40 species')
    require(manifest.get('gif_durations_ms') == [120, 130] * 4, 'Host GIF timing contract differs')
    require(manifest.get('source_sha256'), 'Host source hashes are required')
    for name, expected in manifest['source_sha256'].items():
        check_hash(relative_file(ROOT, name), expected)
    files = {'dino-ui-host-preview.json': directory / 'dino-ui-host-preview.json'}
    for role in ('report', 'contact_sheet', 'motion_overview'):
        item = manifest[role]
        path = relative_file(directory, item['path'])
        check_hash(path, item['sha256']); files[item['path']] = path
    for index, item in enumerate(species):
        require(item['index'] == index and len(item['frames']) == 8 and len(item['frame_sha256']) == 8,
                'Host species frame count/order mismatch')
        for name, expected in [(item['gif'], item['gif_sha256']),
                               (item['first_frame'], item['first_frame_sha256'])] + list(zip(item['frames'], item['frame_sha256'])):
            path = relative_file(directory, name)
            check_hash(path, expected); files[name] = path
    return manifest, files


def check_source_completeness(inventory, audio, animation):
    names = {name for name, _ in inventory}
    required = {
        'assets/animations/dinosaurs40/metadata.json',
        'assets/animations/dinosaurs40/generation-plan.json',
        'assets/fonts/characters.txt',
        'assets/fonts/dino_font_14.c', 'assets/fonts/dino_font_18.c',
        'assets/fonts/dino_font_24.c', 'sdkconfig.defaults', 'partitions.csv',
    }
    for clip in audio['clips']:
        required.add('assets/audio/dinobook40/' + clip['wav'])
    for item in animation['species']:
        prefix = 'assets/animations/dinosaurs40/'
        required.update(prefix + item[key]['path'] for key in ('source', 'packed', 'palette', 'gif'))
        required.add(prefix + 'prompts/' + item['id'] + '.txt')
        required.update(prefix + frame['preview'] for frame in item['frames'])
    require(required <= names, 'Source inventory excludes required assets: ' + ', '.join(sorted(required - names)))


def source_zip(path, inventory):
    hashes = {}
    with zipfile.ZipFile(path, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, source in inventory:
            data = read_file(source)
            hashes[name] = {'sha256': digest(data), 'bytes': len(data)}
            info = zipfile.ZipInfo('DinoPassport40-source/' + name, (2026, 10, 2, 0, 0, 0))
            info.create_system = 3
            info.external_attr = (stat.S_IMODE(source.stat().st_mode) | stat.S_IFREG) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(path) as archive:
        require(archive.testzip() is None and len(archive.infolist()) == len(hashes), 'Source ZIP CRC/count failed')
        for info in archive.infolist():
            name = info.filename.removeprefix('DinoPassport40-source/')
            require(digest(archive.read(info)) == hashes[name]['sha256'], 'Source ZIP file hash mismatch')
    return hashes


def gallery_html(catalog, host):
    records = []
    for entry, preview in zip(catalog, host['species']):
        records.append(dict(entry, gif=preview['gif'], first_frame=preview['first_frame']))
    data = json.dumps(records, ensure_ascii=False).replace('<', '\\u003c')
    return '''<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>小小恐龙护照 · 电脑预览</title><style>
:root{color-scheme:light;--paper:#f7f4e9;--ink:#24463b;--soft:#d9e1d4}*{box-sizing:border-box}
body{margin:0;background:var(--paper);color:var(--ink);font:16px/1.65 system-ui,sans-serif}
main{max-width:1100px;margin:auto;padding:24px}h1{font-size:28px;margin:0}p{margin:8px 0 20px}
.viewer{display:grid;grid-template-columns:260px 1fr;gap:28px;align-items:start;padding:22px;border:1px solid var(--soft);border-radius:18px}
#screen{width:240px;height:320px;object-fit:contain;background:var(--paper);border:1px solid var(--soft);border-radius:12px}
button,select{font:inherit;color:inherit;border:1px solid #a5b9a7;border-radius:10px;background:white;padding:8px 14px;cursor:pointer}
button:hover,button:focus-visible{background:#e7eedf}#choices{display:flex;gap:10px;flex-wrap:wrap}#feedback{min-height:2em}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(120px,1fr));gap:12px;margin-top:24px}.tile{padding:8px}.tile img{width:90px;height:120px;object-fit:contain}.tile span{display:block;font-size:14px}
.meta{color:#557265;font-size:14px}.controls{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}#facts p{margin:8px 0}
@media(max-width:650px){.viewer{grid-template-columns:1fr}main{padding:16px}}
</style><main><h1>小小恐龙护照</h1>
<p class="meta">40 种 · 电脑预览 · AI 行走插画示意<br>屏幕图像来自真实 LVGL 宿主渲染。这里不是设备实拍、实测设备帧率或经过科学验证的步态；语音读音与扬声器仍待实机验收。</p>
<section class="viewer"><div><img id="screen" alt="恐龙护照电脑渲染画面"><div class="controls"><button id="previous">上一种</button><button id="next">下一种</button></div><div class="controls"><button id="mode">显示静态首帧</button></div></div>
<div><select id="species" aria-label="选择恐龙"></select><h2 id="name"></h2><div class="meta" id="details"></div><h3>一起观察</h3><div id="facts"></div><h3 id="question"></h3><div id="choices"></div><p id="feedback" aria-live="polite"></p><p class="meta">网页点击用于查看知识与观察题；设备按键规则请阅读交付 README。</p></div></section><div class="grid" id="gallery"></div></main>
<script id="data" type="application/json">''' + data + '''</script><script>
const records=JSON.parse(document.getElementById('data').textContent);let current=0,moving=true;
const byId=id=>document.getElementById(id);function show(index){current=(index+records.length)%records.length;const d=records[current];byId('species').value=current;byId('name').textContent=d.name;byId('details').textContent=d.latin+' · '+d.period+' · '+d.diet+' · '+d.trait;byId('screen').src=moving?d.gif:d.first_frame;byId('mode').textContent=moving?'显示静态首帧':'显示动作 GIF';byId('facts').replaceChildren(...d.facts.map(f=>{const p=document.createElement('p');p.textContent=f;return p}));byId('question').textContent=d.question;byId('feedback').textContent='';byId('choices').replaceChildren(...d.options.map((choice,i)=>{const b=document.createElement('button');b.textContent=choice;b.onclick=()=>byId('feedback').textContent=i===d.correct?'发现啦！一起继续观察。':d.hint+' 再试一次吧。';return b}));}
records.forEach((d,i)=>{const option=document.createElement('option');option.value=i;option.textContent=(i+1)+' · '+d.name;byId('species').append(option);const b=document.createElement('button');b.className='tile';const image=document.createElement('img');image.src=d.first_frame;image.alt=d.name;image.loading='lazy';const label=document.createElement('span');label.textContent=d.name;b.append(image,label);b.onclick=()=>show(i);byId('gallery').append(b)});
byId('species').onchange=e=>show(Number(e.target.value));byId('previous').onclick=()=>show(current-1);byId('next').onclick=()=>show(current+1);byId('mode').onclick=()=>{moving=!moving;show(current)};show(0);
</script></html>
'''


def readmes(full_hash, elf_hash):
    english = f'''English · [简体中文](README.zh_CN.md)

# Dino Passport 40 delivery

This package contains the verified merged firmware, its unchanged component/debug
archive, a Git-inventory source snapshot, file hashes, and offline computer
previews. Open `preview/index.html` locally to browse all 40 names, facts,
questions, and real-LVGL GIFs. These are AI-generated walking illustrations and
computer renders, not device footage or measured device frame rate.

Merged image: `DinoPassport40-full-0x0.bin`, SHA-256 `{full_hash}`.
Matching application ELF SHA-256: `{elf_hash}`.
The complete archived image paths, offsets, debug files, and manifest remain in
`firmware/{full_hash}/`. Source files are in `DinoPassport40-source.zip`;
`source-files.json` and `SHA256SUMS` record the packaged bytes.
The Git source snapshot excludes Apple system-voice recordings. This local
narrated firmware package contains restricted development voice output; keep
it for personal development and do not publicly share it.

The merged image is intended for offset `0x0`; writing its FF-filled gaps can
erase existing NVS and device identity. Preserving identity/progress requires
compatible segmented writes using the exact archived images and `flash_args`,
with audio at `0x35a000`. Both methods require separate authorization for the
specific device and data impact. The package never flashes automatically,
provides no identity/Recovery payload, and does not authorize full-chip erasure.
A blank device needs separately provisioned compatible identity and Recovery.

Motion controls: enter from camp; UP/DOWN wrap all 40 species, reset frame zero,
and preserve pause; OK pauses/resumes; long-OK restarts/plays; long-UP returns to
camp. Facts, two-choice questions, narration, and five footprint pages remain.
The 202 ordinary Tingting development clips use 8 kHz/155 narration. The 64-byte
audio-header comparison binds resource identity; it does not verify all payload
bytes at runtime. Offline package verification checks full resource hashes.

Build: PASS (verified archive and matching ELF). Host tests: PASS (supplied
actual-LVGL report and checked complete assets). Device tests: NOT RUN.
Unverified: physical display, gait, frame timing, controls, pronunciation,
speaker level, memory stability, stored data, and Recovery. No flash, commit,
push, or publication is performed by this tool.
'''
    chinese = f'''[English](README.md) · 简体中文

# 恐龙护照 40 种交付

包内包含已核验合并固件、保持原路径的分段 / 调试归档、根据 Git 文件清单
取得的源码快照、哈希，以及离线电脑预览。本地打开 `preview/index.html`
即可浏览 40 种名字、知识、观察题和真实 LVGL GIF。这些是 AI 行走插画与
电脑渲染，不是设备实拍或实测设备帧率。

合并镜像：`DinoPassport40-full-0x0.bin`，SHA-256 `{full_hash}`。
匹配应用 ELF SHA-256：`{elf_hash}`。
完整镜像路径、地址、调试文件及 manifest 保留在 `firmware/{full_hash}/`。
源码见 `DinoPassport40-source.zip`；`source-files.json` 与 `SHA256SUMS` 记录
打包字节身份。
Git 源码快照排除 Apple 系统语音录音。本地带语音固件包包含受限开发语音，
仅供个人开发，请勿公开分享。

合并镜像从 `0x0` 写入，其 FF 填充间隙可能擦除已有 NVS 和设备身份。
保留身份 / 进度需采用兼容的分段写入，使用精确归档镜像与 `flash_args`，
音频位于 `0x35a000`。两种方式都需针对具体设备及数据影响另行授权。
本包不会自动刷机，不提供身份 / Recovery 载荷，也不授权全片擦除。
空白设备需另行配置兼容身份与 Recovery。

动作从营地进入：上下循环 40 种，切换归零且保留暂停；确定暂停 / 继续；
长按确定重播并播放；长按上键返回。保留知识、双选题、讲解和五页足迹。
202 段普通 Tingting 开发讲解使用 8 kHz、原语速 155。64 字节音频头比较只
绑定资源身份，不在运行时校验完整载荷；离线包验证检查完整资源哈希。

Build: PASS（归档与匹配 ELF 已核验）。Host tests: PASS（真实 LVGL 报告及
完整资源核查）。Device tests: NOT RUN。
Unverified: 实际显示、步态、帧时序、按键、读音、音量、内存稳定性、存档
及 Recovery。本工具不执行刷机、提交、推送或发布。
'''
    return english, chinese


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def create_delivery(archive, preview, name):
    archive = archive if archive.is_absolute() else ROOT / archive
    archive = safe_path(archive, BUILD)
    require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{1,79}', name), 'Unsafe delivery name')
    # The authoritative verifier must succeed before packaging reads or writes.
    subprocess.run([sys.executable, str(ROOT / 'tools/archive_firmware.py'), 'verify', str(archive)],
                   cwd=ROOT, check=True)
    preview = preview if preview.is_absolute() else ROOT / preview
    preview = safe_path(preview, BUILD)
    firmware = read_json(archive / 'manifest.json')
    require(firmware.get('schema_version') == 2, 'External-resource archive schema 2 is required')
    catalog, audio, animation = check_assets(firmware, archive)
    host, preview_files = check_host_preview(preview, catalog)
    host_hash = digest(read_file(preview / 'dino-ui-host-preview.json'))
    inventory = source_inventory()
    check_source_completeness(inventory, audio, animation)
    safe_path(DELIVERY_ROOT, must_exist=False).mkdir(parents=True, exist_ok=True)
    destination = safe_path(DELIVERY_ROOT / name, must_exist=False)
    zip_path = safe_path(DELIVERY_ROOT / (name + '.zip'), must_exist=False)
    hash_path = safe_path(DELIVERY_ROOT / (name + '.zip.sha256'), must_exist=False)
    require(not destination.exists() and not zip_path.exists() and not hash_path.exists(),
            'Delivery already exists; preserve it and choose --name')
    with tempfile.TemporaryDirectory(prefix='.' + name + '-', dir=DELIVERY_ROOT) as scratch:
        stage = Path(scratch) / name; stage.mkdir()
        target_archive = stage / 'firmware' / archive.name
        for relative in list(firmware['files']) + ['manifest.json']:
            source = relative_file(archive, relative)
            target = target_archive / relative; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        shutil.copyfile(archive / FULL_BIN, stage / 'DinoPassport40-full-0x0.bin')
        # Recheck the copied exact component archive before publication.
        subprocess.run([sys.executable, str(ROOT / 'tools/archive_firmware.py'), 'verify', str(target_archive)],
                       cwd=ROOT, check=True)
        source_path = stage / 'DinoPassport40-source.zip'
        hashes = source_zip(source_path, inventory)
        source_hash = digest(source_path.read_bytes())
        write_json(stage / 'source-files.json', {'inventory_command': 'git ls-files --cached --others --exclude-standard -z',
                                               'files': hashes, 'file_count': len(hashes),
                                               'source_zip_sha256': source_hash})
        (stage / 'DinoPassport40-source.zip.sha256').write_text(source_hash + '  DinoPassport40-source.zip\n')
        preview_target = stage / 'preview'; preview_target.mkdir()
        for relative, source in preview_files.items():
            target = preview_target / relative; target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        (preview_target / 'index.html').write_text(gallery_html(catalog, host), encoding='utf-8')
        english, chinese = readmes(firmware['full_bin_sha256'], firmware['app_elf_sha256'])
        (stage / 'README.md').write_text(english, encoding='utf-8')
        (stage / 'README.zh_CN.md').write_text(chinese, encoding='utf-8')
        write_json(stage / 'delivery-manifest.json', {
            'schema_version': 1, 'name': name, 'firmware_full_sha256': firmware['full_bin_sha256'],
            'application_elf_sha256': firmware['app_elf_sha256'], 'source_zip_sha256': source_hash,
            'source_file_count': len(hashes), 'species': 40, 'facts': 80, 'questions': 40,
            'audio_clips': 202, 'audio_blob_bytes': audio['blob']['bytes'],
            'audio_blob_sha256': audio['blob']['sha256'], 'animation_frames': 320,
            'host_preview_manifest_sha256': host_hash,
            'device_tests': 'NOT RUN', 'device_photographs': False,
            'runtime_audio_payload_integrity': 'identity-header comparison only',
            'source_snapshot': 'current Git inventory, including uncommitted authorized project changes',
        })
        sums = []
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                sums.append(digest(path.read_bytes()) + '  ' + path.relative_to(stage).as_posix())
        (stage / 'SHA256SUMS').write_text('\n'.join(sums) + '\n', encoding='utf-8')
        # Source/host files must not change during copying.
        for relative, source in inventory:
            require(digest(read_file(source)) == hashes[relative]['sha256'], 'Source changed during packaging: ' + relative)
        check_host_preview(preview, catalog)
        require(digest(read_file(preview / 'dino-ui-host-preview.json')) == host_hash,
                'Host preview changed during packaging')
        for relative, source in preview_files.items():
            require(read_file(source) == (preview_target / relative).read_bytes(),
                    'Host preview file changed during packaging: ' + relative)
        check_assets(firmware, archive)
        temporary_zip = Path(scratch) / (name + '.zip')
        with zipfile.ZipFile(temporary_zip, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as package:
            for path in sorted(stage.rglob('*')):
                if path.is_file():
                    package.write(path, name + '/' + path.relative_to(stage).as_posix())
        with zipfile.ZipFile(temporary_zip) as package:
            require(package.testzip() is None, 'Delivery ZIP CRC verification failed')
            for line in sums:
                expected, relative = line.split('  ', 1)
                require(digest(package.read(name + '/' + relative)) == expected, 'Delivery ZIP hash mismatch: ' + relative)
        package_hash = digest(temporary_zip.read_bytes())
        stage.rename(destination)
        temporary_zip.rename(zip_path)
        with hash_path.open('x', encoding='utf-8') as output:
            output.write(package_hash + '  ' + zip_path.name + '\n')
    print('Delivery directory: ' + str(destination))
    print('Delivery ZIP: ' + str(zip_path))
    print('SHA-256: ' + package_hash)
    print('40 species / 80 facts / 40 questions / 202 clips / 320 frames. Device tests NOT RUN.')
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path, help='exact content-addressed archive under build/firmware')
    parser.add_argument('--preview-dir', type=Path, default=BUILD / 'preview')
    parser.add_argument('--name', default=DEFAULT_NAME, help='new delivery directory name; existing packages are preserved')
    args = parser.parse_args()
    try:
        create_delivery(args.archive, args.preview_dir, args.name)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
