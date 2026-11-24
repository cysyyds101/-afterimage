/* Regenerate firmware/main/font_cn_18.c: a lightweight LVGL subset font (18 px,
 * 4 bpp, SimHei) covering every character that appears in on-screen string
 * literals of main.c / voice_client.c, plus printable ASCII. Run from anywhere:
 *   node tools/gen_cn_font.mjs
 * Requires npx (lv_font_conv is fetched on first use) and C:/Windows/Fonts/simhei.ttf.
 */
import { readFileSync } from 'node:fs'
import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const sources = ['firmware/main/main.c', 'firmware/main/voice_client.c']
const chars = new Set()
for (let c = 0x20; c <= 0x7e; c++) chars.add(String.fromCharCode(c))
for (const rel of sources) {
  const text = readFileSync(path.join(root, rel), 'utf8')
  for (const m of text.matchAll(/"(?:[^"\\]|\\.)*"/g))
    for (const ch of m[0])
      if (ch.codePointAt(0) > 0x7e) chars.add(ch)
}
const symbols = [...chars].join('')
const out = path.join(root, 'firmware/main/font_cn_18.c')
const npxCli = path.join(path.dirname(process.execPath), 'node_modules', 'npm', 'bin', 'npx-cli.js')
const result = spawnSync(process.execPath, [
  npxCli, '-y', 'lv_font_conv',
  '--font', 'C:/Windows/Fonts/simhei.ttf',
  '--size', '18', '--bpp', '4', '--format', 'lvgl',
  '--lv-include', 'lvgl.h',
  '--lv-font-name', 'lv_font_cn_18',
  '--symbols', symbols,
  '-o', out,
], { stdio: 'inherit', cwd: root })
if (result.status !== 0) process.exit(result.status ?? 1)
console.log(`font_cn_18.c generated: ${chars.size} glyphs (${[...chars].filter(c => c.codePointAt(0) > 0x7e).length} CJK/symbols + ASCII)`)