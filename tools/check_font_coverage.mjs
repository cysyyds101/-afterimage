import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const font = fs.readFileSync(path.join(root, 'firmware/main/font_cn_18.c'), 'utf8')
const m = font.match(/--symbols ([\s\S]*?) -o /)
if (!m) { console.error('Cannot find --symbols set in font header'); process.exit(1) }
const have = new Set(m[1])
const missing = new Set()
let total = 0
const litRe = new RegExp('"([^"' + String.fromCharCode(92) + String.fromCharCode(92) + ']|' + String.fromCharCode(92) + String.fromCharCode(92) + '.)*"', 'g')
for (const f of ['firmware/main/main.c', 'firmware/main/voice_client.c']) {
  const text = fs.readFileSync(path.join(root, f), 'utf8')
  for (const lit of text.matchAll(litRe))
    for (const ch of lit[0])
      if (ch.codePointAt(0) > 0x7e) { total++; if (!have.has(ch)) missing.add(ch) }
}
console.log('non-ASCII chars in literals:', total, '| font symbol set size:', have.size)
console.log(missing.size ? 'MISSING: ' + [...missing].join('') : 'COVERAGE OK: every on-screen character has a glyph')
process.exit(missing.size ? 1 : 0)