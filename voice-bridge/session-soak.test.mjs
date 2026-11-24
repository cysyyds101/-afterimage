import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const memDir = fs.mkdtempSync(path.join(os.tmpdir(), 'qiban-mem-'))
import http from 'node:http'
import { spawn } from 'node:child_process'
import test from 'node:test'
import { WebSocket } from 'ws'
import { pcm16ToWav } from './audio.mjs'

/* Phase 1.2 evidence. The board keeps a bounded PSRAM playback queue
 * (SPEAKER_QUEUE_SLOTS = 192 frames of 1280 B, ~245 KB / ~7.7 s of 16 kHz PCM).
 * That sizing is only safe if the bridge never dumps audio much faster than
 * real time, so this test measures the delivery rate instead of assuming it.
 * It also drives several turns over one session and a reconnect, which is the
 * "weak network recovery, no leak, no crash" acceptance item. */

const SENTENCES = [
  '第一句话。', '第二句话！', '第三句话？', '第四句话；', '第五句话。',
  '第六句话！', '第七句话？', '第八句话；', '第九句话。', '第十句话！',
]
const SAMPLES_PER_SENTENCE = 12000      // 0.5 s at 24 kHz
const REALTIME_BYTES_PER_S = 32000      // 16 kHz mono 16-bit
const MAX_RATE_RATIO = 1.35             // paced send is ~1.14x; allow jitter

function toneFor(index, samples) {
  const value = 800 + index * 400
  const buf = Buffer.alloc(samples * 2)
  for (let i = 0; i < samples; i++) buf.writeInt16LE(value, i * 2)
  return buf
}

const EXPECTED = Buffer.concat(SENTENCES.map((_, i) => {
  // Mirror the bridge's resampling by constructing the 16 kHz tone directly:
  // a constant amplitude is invariant under linear interpolation, so the
  // expected bytes are exact regardless of chunk boundaries.
  const samples = Math.round(SAMPLES_PER_SENTENCE * 16000 / 24000)
  return toneFor(i, samples)
}))

async function startProvider(aborted) {
  const server = http.createServer((req, res) => {
    let body = ''
    req.on('data', c => { body += c })
    res.on('close', () => { if (!res.writableFinished) aborted.push(req.url) })
    req.on('end', async () => {
      if (req.url.endsWith('/audio/transcriptions')) {
        res.writeHead(200, { 'content-type': 'application/json' })
        res.end(JSON.stringify({ text: '你好呀' }))
        return
      }
      if (req.url.endsWith('/chat/completions')) {
        res.writeHead(200, { 'content-type': 'text/event-stream' })
        for (const s of SENTENCES) {
          for (let i = 0; i < s.length; i += 2) {
            if (res.writableEnded) return
            res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: s.slice(i, i + 2) } }] })}\n\n`)
            await new Promise(r => setTimeout(r, 8))
          }
        }
        res.write('data: [DONE]\n\n')
        res.end()
        return
      }
      if (req.url.endsWith('/audio/speech')) {
        const index = SENTENCES.indexOf(JSON.parse(body).input)
        assert.ok(index >= 0, 'unexpected TTS text')
        const pcm = toneFor(index, SAMPLES_PER_SENTENCE)
        res.writeHead(200, { 'content-type': 'audio/wav' })
        await new Promise(r => setTimeout(r, 60))
        const payload = pcm16ToWav(pcm, 24000)
        for (let offset = 0; offset < payload.length; offset += 2048) {
          if (res.writableEnded) return
          res.write(payload.subarray(offset, offset + 2048))
          await new Promise(r => setTimeout(r, 2))
        }
        res.end()
        return
      }
      res.writeHead(404); res.end('not found')
    })
  })
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  return {
    base: `http://127.0.0.1:${server.address().port}`,
    close: () => new Promise(resolve => { server.closeAllConnections?.(); server.close(resolve) }),
  }
}

async function startBridge(provider, port) {
  const child = spawn(process.execPath, ['server.mjs'], {
    cwd: import.meta.dirname,
    env: {
      ...process.env,
      MEMORY_DIR: memDir,
      PORT: String(port),
      DEVICE_TOKEN: 'soak-test-token',
      MOCK_MODE: '', PLATFORM_API_URL: '',
      STT_BASE_URL: provider.base, STT_API_KEY: 'test', STT_MODEL: 'test',
      TTS_BASE_URL: provider.base, TTS_API_KEY: 'test', TTS_MODEL: 'test',
      TTS_RESPONSE_FORMAT: 'wav', TTS_PCM_SAMPLE_RATE: '24000',
      CHAT_BASE_URL: provider.base, CHAT_API_KEY: 'test', CHAT_MODEL: 'test',
      SYSTEM_PROMPT: 'test',
    },
    stdio: ['ignore', 'ignore', 'pipe'],
  })
  let stderr = ''
  child.stderr.on('data', d => { stderr += d })
  for (let attempt = 0; attempt < 80; attempt++) {
    try { const r = await fetch(`http://127.0.0.1:${port}/health`); if (r.ok) break } catch {}
    await new Promise(r => setTimeout(r, 50))
  }
  return { child, stderr: () => stderr }
}

function openSession(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/voice`)
  const session = { ws, states: [], audio: [], gaps: [], reply: null, firstAudioMs: null }
  let lastAudioAt = null
  let turnStartedAt = null
  let resolveTurn = null
  session.turnDone = null
  ws.on('open', () => ws.send(JSON.stringify({
    type: 'hello', token: 'soak-test-token', sampleRate: 16000, format: 'pcm_s16le',
  })))
  ws.on('message', (data, binary) => {
    const now = Date.now()
    if (binary) {
      if (session.firstAudioMs === null && turnStartedAt !== null)
        session.firstAudioMs = now - turnStartedAt
      if (lastAudioAt !== null) session.gaps.push(now - lastAudioAt)
      lastAudioAt = now
      session.audio.push(Buffer.from(data))
      return
    }
    const message = JSON.parse(data.toString())
    if (message.type === 'reply') session.reply = message.text
    if (message.type !== 'state') return
    session.states.push(message.state)
    if (message.state === 'listening') turnStartedAt = now
    if (message.state === 'idle' && resolveTurn) { const r = resolveTurn; resolveTurn = null; r() }
  })
  session.ready = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('session did not become ready')), 10000)
    const check = setInterval(() => {
      if (session.states.includes('ready')) { clearTimeout(timer); clearInterval(check); resolve() }
    }, 20)
    ws.on('error', e => { clearTimeout(timer); clearInterval(check); reject(e) })
  })
  session.runTurn = () => {
    session.audio = []; session.gaps = []; session.states = []; session.firstAudioMs = null
    lastAudioAt = null
    const input = Buffer.alloc(6400)
    for (let i = 0; i < input.length / 2; i++) input.writeInt16LE((i * 37) % 30000, i * 2)
    const done = new Promise((resolve, reject) => {
      resolveTurn = resolve
      setTimeout(() => reject(new Error('turn timed out')), 40000).unref?.()
    })
    ws.send(JSON.stringify({ type: 'start' }))
    ws.send(input)
    ws.send(JSON.stringify({ type: 'audio_end' }))
    return done
  }
  return session
}

test('multi-turn session stays ordered, paced and healthy across a reconnect', { timeout: 180000 }, async () => {
  const aborted = []
  const provider = await startProvider(aborted)
  const port = 18795
  const bridge = await startBridge(provider, port)
  const ratios = []
  try {
    const session = openSession(port)
    await session.ready
    for (let turn = 0; turn < 3; turn++) {
      await session.runTurn()
      const received = Buffer.concat(session.audio)
      assert.deepEqual(received, EXPECTED, `turn ${turn}: audio bytes/order mismatch`)
      assert.equal(session.reply, SENTENCES.join(''), `turn ${turn}: reply text`)
      const maxGap = Math.max(...session.gaps)
      assert.ok(maxGap < 200, `turn ${turn}: inter-frame gap ${maxGap} ms`)
      assert.ok(session.firstAudioMs < 1000, `turn ${turn}: first audio ${session.firstAudioMs} ms`)
      // Delivery rate versus real time: this is what bounds the board queue.
      const audioSeconds = received.length / REALTIME_BYTES_PER_S
      const wallSeconds = (session.gaps.reduce((a, b) => a + b, 0) + 35) / 1000
      const ratio = audioSeconds / wallSeconds
      ratios.push(ratio)
      assert.ok(ratio < MAX_RATE_RATIO, `turn ${turn}: delivery ${ratio.toFixed(2)}x real time`)
      console.log(`[soak] turn ${turn}: first audio ${session.firstAudioMs} ms, max gap ${maxGap} ms, ` +
        `${received.length} B at ${ratio.toFixed(2)}x real time`)
    }
    assert.equal(aborted.length, 0, `provider saw aborted requests: ${aborted.join(', ')}`)
    session.ws.close()
    await new Promise(r => setTimeout(r, 300))

    // The board's esp_websocket_client reconnects the transport and re-sends
    // hello; a fresh connection is the server-side equivalent.
    const again = openSession(port)
    await again.ready
    await again.runTurn()
    assert.deepEqual(Buffer.concat(again.audio), EXPECTED, 'post-reconnect audio mismatch')
    assert.ok(again.firstAudioMs < 1000, 'post-reconnect first audio too slow')
    console.log(`[soak] reconnect turn: first audio ${again.firstAudioMs} ms`)
    again.ws.close()
    await new Promise(r => setTimeout(r, 300))

    const health = await fetch(`http://127.0.0.1:${port}/health`)
    assert.ok(health.ok, 'bridge unhealthy after session soak')
    assert.ok(!bridge.stderr().includes('Voice turn failed'), bridge.stderr())
  } finally {
    bridge.child.kill()
    await provider.close()
  }
})