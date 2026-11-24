import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const memDir = fs.mkdtempSync(path.join(os.tmpdir(), 'qiban-mem-'))
import http from 'node:http'
import { spawn } from 'node:child_process'
import test from 'node:test'
import { WebSocket } from 'ws'
import { resamplePcm16, pcm16ToWav } from './audio.mjs'

/* A local mock provider. No real endpoints, no keys, no network: the Phase 1
 * latency target is measured against a deterministic stand-in so the result is
 * reproducible on any machine. */

const SENTENCES = ['你好呀。', '今天天气真不错！', '要不要一起出去走走？', '我可以陪你聊天。']

function toneFor(index, samples) {
  // Constant amplitude per sentence so ordering survives resampling and can be
  // asserted byte-for-byte on the received stream.
  const value = 1000 + index * 500
  const buf = Buffer.alloc(samples * 2)
  for (let i = 0; i < samples; i++) buf.writeInt16LE(value, i * 2)
  return buf
}

async function startProvider({ format, chatDelayMs = 25, ttsFirstByteMs = 120, aborted }) {
  const server = http.createServer((req, res) => {
    let body = ''
    req.on('data', c => { body += c })
    // Detect a premature disconnect on the *response*: req 'close' fires as
    // soon as the request body is read, which for a small POST is long before
    // the streamed response finishes and would flag every turn as aborted.
    res.on('close', () => { if (!res.writableFinished) aborted.push(req.url) })
    req.on('end', async () => {
      if (req.url.endsWith('/audio/transcriptions')) {
        res.writeHead(200, { 'content-type': 'application/json' })
        res.end(JSON.stringify({ text: '你好呀' }))
        return
      }
      if (req.url.endsWith('/chat/completions')) {
        res.writeHead(200, { 'content-type': 'text/event-stream' })
        // Split each sentence into small deltas so the splitter is exercised.
        const deltas = []
        for (const s of SENTENCES) for (let i = 0; i < s.length; i += 2) deltas.push(s.slice(i, i + 2))
        for (const delta of deltas) {
          if (res.writableEnded) return
          res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: delta } }] })}\n\n`)
          await new Promise(r => setTimeout(r, chatDelayMs))
        }
        res.write('data: [DONE]\n\n')
        res.end()
        return
      }
      if (req.url.endsWith('/audio/speech')) {
        const text = JSON.parse(body).input
        const index = SENTENCES.indexOf(text)
        assert.ok(index >= 0, `unexpected TTS text: ${text}`)
        const pcm = toneFor(index, 12000)   // 0.5 s at 24 kHz
        const payload = format === 'pcm' ? pcm : pcm16ToWav(pcm, 24000)
        res.writeHead(200, { 'content-type': format === 'pcm' ? 'application/octet-stream' : 'audio/wav' })
        await new Promise(r => setTimeout(r, ttsFirstByteMs))
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
  const base = `http://127.0.0.1:${server.address().port}`
  return {
    base,
    close: () => new Promise(resolve => {
      // undici keeps sockets alive; without this, close() waits forever.
      server.closeAllConnections?.()
      server.close(resolve)
    }),
  }
}

async function startBridge(provider, port, format) {
  const child = spawn(process.execPath, ['server.mjs'], {
    cwd: import.meta.dirname,
    env: {
      ...process.env,
      MEMORY_DIR: memDir,
      PORT: String(port),
      DEVICE_TOKEN: 'stream-test-token',
      MOCK_MODE: '',
      PLATFORM_API_URL: '',
      STT_BASE_URL: provider.base, STT_API_KEY: 'test', STT_MODEL: 'test',
      TTS_BASE_URL: provider.base, TTS_API_KEY: 'test', TTS_MODEL: 'test',
      TTS_RESPONSE_FORMAT: format, TTS_PCM_SAMPLE_RATE: '24000',
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

/** Drive one turn and collect timing, ordering and gap evidence. */
async function runTurn(port, { closeEarly = false } = {}) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/voice`)
  const input = Buffer.alloc(6400)
  for (let i = 0; i < input.length / 2; i++) input.writeInt16LE((i * 37) % 30000, i * 2)
  const states = []
  const audio = []
  const gaps = []
  let transcriptAt = null
  let firstAudioAt = null
  let lastAudioAt = null
  let reply = null
  let done
  const finished = new Promise((resolve, reject) => {
    done = resolve
    setTimeout(() => reject(new Error('turn timed out')), 25000).unref?.()
  })
  ws.on('open', () => ws.send(JSON.stringify({ type: 'hello', token: 'stream-test-token', sampleRate: 16000 })))
  ws.on('message', (data, binary) => {
    const now = Date.now()
    if (binary) {
      if (firstAudioAt === null) firstAudioAt = now
      if (lastAudioAt !== null) gaps.push(now - lastAudioAt)
      lastAudioAt = now
      audio.push(Buffer.from(data))
      if (closeEarly && audio.length === 3) { ws.close(); done() }
      return
    }
    const message = JSON.parse(data.toString())
    if (message.type === 'transcript') transcriptAt = now
    else if (message.type === 'reply') reply = message.text
    else if (message.type === 'state') {
      states.push(message.state)
      if (message.state === 'ready') {
        ws.send(JSON.stringify({ type: 'start' }))
        ws.send(input)
        ws.send(JSON.stringify({ type: 'audio_end' }))
      } else if (message.state === 'idle' && firstAudioAt !== null) {
        ws.close(); done()
      } else if (message.state === 'error') {
        ws.close(); done(new Error(`bridge reported error: ${message.message}`))
      }
    }
  })
  ws.on('error', () => {})
  await finished
  return { states, audio: Buffer.concat(audio), gaps, transcriptAt, firstAudioAt, reply }
}

for (const format of ['pcm', 'wav']) {
  test(`streaming turn (${format}) reaches first audio under 1 s and keeps sentence order`, { timeout: 40000 }, async () => {
    const aborted = []
    const provider = await startProvider({ format, aborted })
    const bridge = await startBridge(provider, format === 'pcm' ? 18791 : 18792, format)
    try {
      const turn = await runTurn(format === 'pcm' ? 18791 : 18792)
      assert.ok(turn.transcriptAt && turn.firstAudioAt, 'missing transcript or audio')
      const latency = turn.firstAudioAt - turn.transcriptAt
      assert.ok(latency < 1000, `transcript->first audio was ${latency} ms, target < 1000 ms`)
      assert.equal(turn.reply, SENTENCES.join(''))
      assert.ok(turn.states.includes('speaking'), `states: ${turn.states}`)
      assert.equal(turn.states.at(-1), 'idle')

      // Ordering: each sentence is a distinct constant amplitude, so the
      // received stream must visit those amplitudes in sentence order.
      const expected = SENTENCES.map((_, i) => resamplePcm16(toneFor(i, 12000), 24000, 16000))
      const expectedBytes = Buffer.concat(expected)
      assert.equal(turn.audio.length, expectedBytes.length, 'audio length mismatch')
      assert.deepEqual(turn.audio, expectedBytes, 'audio content/order mismatch')

      // No audible inter-sentence stall: pacing is 35 ms, allow scheduler slack.
      const maxGap = Math.max(...turn.gaps)
      assert.ok(maxGap < 200, `largest inter-frame gap was ${maxGap} ms`)
      assert.equal(aborted.length, 0, `provider saw aborted requests: ${aborted}`)
      console.log(`[${format}] transcript->first audio ${latency} ms, max gap ${maxGap} ms, bytes ${turn.audio.length}`)
    } finally {
      bridge.child.kill()
      await provider.close()
    }
  })
}

test('closing the socket mid-turn aborts provider requests and leaves the bridge healthy', { timeout: 40000 }, async () => {
  const aborted = []
  const provider = await startProvider({ format: 'pcm', aborted })
  const bridge = await startBridge(provider, 18793, 'pcm')
  try {
    await runTurn(18793, { closeEarly: true })
    await new Promise(r => setTimeout(r, 500))
    assert.ok(aborted.length > 0, 'expected in-flight provider requests to be aborted')
    // The bridge must still serve a fresh turn after the abrupt disconnect.
    const second = await runTurn(18793)
    assert.ok(second.audio.length > 0, 'bridge did not recover after disconnect')
    assert.equal(second.states.at(-1), 'idle')
    assert.ok(!/Voice turn failed/.test(bridge.stderr()), `unexpected error: ${bridge.stderr()}`)
  } finally {
    bridge.child.kill()
    await provider.close()
  }
})