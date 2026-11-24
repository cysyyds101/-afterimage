import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const memDir = fs.mkdtempSync(path.join(os.tmpdir(), 'qiban-mem-'))
import { spawn } from 'node:child_process'
import http from 'node:http'
import test from 'node:test'
import { WebSocket } from 'ws'
import { pcm16ToWav } from './audio.mjs'

const DEVICE_TOKEN = 'qb_dev_platform-test-token'
const BRIDGE_KEY = 'test-bridge-key'
const CHAT_KEY = 'platform-chat-key-never-log'

/** Mock platform: voice/session hands out a config that changes on every call,
 * so a per-turn refresh is observable as a model/persona change mid-connection. */
async function startPlatform(provider, { failAfterFirst = false } = {}) {
  const sessionCalls = []
  const statusReports = []
  const server = http.createServer((request, response) => {
    let body = ''
    request.on('data', c => { body += c })
    request.on('end', () => {
      const auth = request.headers.authorization || ''
      const bridgeKey = request.headers['x-qiban-bridge-key'] || ''
      if (request.url === '/api/v1/voice/session' && request.method === 'POST') {
        sessionCalls.push({ auth, bridgeKey })
        // The startup stackStatus probe resolves with the static DEVICE_TOKEN;
        // only device-token calls count towards the per-turn refresh contract.
        const deviceCalls = sessionCalls.filter(c => c.auth === `Bearer ${DEVICE_TOKEN}`).length
        if (failAfterFirst && deviceCalls > 1) {
          response.writeHead(503, { 'content-type': 'application/json' })
          response.end(JSON.stringify({ error: 'platform temporarily unavailable' }))
          return
        }
        const n = deviceCalls
        response.writeHead(200, { 'content-type': 'application/json' })
        response.end(JSON.stringify({
          baseUrl: provider.base, apiKey: CHAT_KEY,
          model: `platform-model-${n}`, systemPrompt: `persona-${n}`,
        }))
        return
      }
      if (request.url === '/api/v1/voice/status' && request.method === 'POST') {
        statusReports.push({ auth, bridgeKey, ...JSON.parse(body) })
        response.writeHead(200, { 'content-type': 'application/json' })
        response.end(JSON.stringify({ ok: true }))
        return
      }
      response.writeHead(404); response.end('not found')
    })
  })
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  return {
    base: `http://127.0.0.1:${server.address().port}`,
    sessionCalls, statusReports,
    close: () => new Promise(resolve => { server.closeAllConnections?.(); server.close(resolve) }),
  }
}

/** Mock OpenAI-compatible provider recording what each chat request carried. */
async function startProvider() {
  const chatLog = []
  const server = http.createServer((request, response) => {
    let body = ''
    request.on('data', c => { body += c })
    request.on('end', async () => {
      if (request.url.endsWith('/audio/transcriptions')) {
        response.writeHead(200, { 'content-type': 'application/json' })
        response.end(JSON.stringify({ text: '你好呀' }))
        return
      }
      if (request.url.endsWith('/chat/completions')) {
        const payload = JSON.parse(body)
        chatLog.push({
          model: payload.model,
          system: payload.messages.find(m => m.role === 'system')?.content,
          auth: request.headers.authorization || '',
        })
        response.writeHead(200, { 'content-type': 'text/event-stream' })
        for (const piece of ['你好', '呀。']) {
          response.write(`data: ${JSON.stringify({ choices: [{ delta: { content: piece } }] })}\n\n`)
          await new Promise(r => setTimeout(r, 10))
        }
        response.write('data: [DONE]\n\n')
        response.end()
        return
      }
      if (request.url.endsWith('/audio/speech')) {
        const pcm = Buffer.alloc(2400)
        for (let i = 0; i < 1200; i++) pcm.writeInt16LE(1500, i * 2)
        response.writeHead(200, { 'content-type': 'audio/wav' })
        response.end(pcm16ToWav(pcm, 24000))
        return
      }
      response.writeHead(404); response.end('not found')
    })
  })
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  const base = `http://127.0.0.1:${server.address().port}`
  return {
    base, chatLog,
    close: () => new Promise(resolve => { server.closeAllConnections?.(); server.close(resolve) }),
  }
}

async function startBridge(provider, platform, port) {
  const child = spawn(process.execPath, ['server.mjs'], {
    cwd: import.meta.dirname,
    env: {
      ...process.env,
      MEMORY_DIR: memDir,
      PORT: String(port),
      MOCK_MODE: '',
      DEVICE_TOKEN: 'unused-static-token',
      PLATFORM_API_URL: platform.base,
      PLATFORM_BRIDGE_KEY: BRIDGE_KEY,
      PLATFORM_SITE_TOKEN: '',
      STT_BASE_URL: provider.base, STT_API_KEY: 'test', STT_MODEL: 'test',
      TTS_BASE_URL: provider.base, TTS_API_KEY: 'test', TTS_MODEL: 'test',
      TTS_RESPONSE_FORMAT: 'wav', TTS_PCM_SAMPLE_RATE: '24000',
    },
    stdio: ['ignore', 'ignore', 'pipe'],
  })
  let stderr = ''
  child.stderr.on('data', d => { stderr += d })
  for (let attempt = 0; attempt < 80; attempt++) {
    try { if ((await fetch(`http://127.0.0.1:${port}/health`)).ok) break } catch {}
    await new Promise(r => setTimeout(r, 50))
  }
  return { child, stderr: () => stderr }
}

async function connect(port) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/voice`)
  const states = []
  let audioBytes = 0
  let turnResolve = null
  let turnReject = null
  let sawAudio = false
  const ready = new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('hello timed out')), 10000)
    ws.on('open', () => ws.send(JSON.stringify({ type: 'hello', token: DEVICE_TOKEN, sampleRate: 16000 })))
    ws.on('message', (data, binary) => {
      if (binary) { audioBytes += data.length; sawAudio = true; return }
      const message = JSON.parse(data.toString())
      if (message.type !== 'state') return
      states.push(message.state)
      if (message.state === 'ready' && states.length === 1) { clearTimeout(timer); resolve() }
      if (message.state === 'idle' && sawAudio && turnResolve) { turnResolve() }
      if (message.state === 'error' && turnReject) turnReject(new Error(`bridge error: ${message.message}`))
    })
    ws.on('error', reject)
  })
  await ready
  return {
    ws, states,
    get audioBytes() { return audioBytes },
    async turn() {
      sawAudio = false
      audioBytes = 0
      const input = Buffer.alloc(6400)
      for (let i = 0; i < input.length / 2; i++) input.writeInt16LE((i * 37) % 30000, i * 2)
      const done = new Promise((resolve, reject) => {
        turnResolve = resolve
        turnReject = reject
        setTimeout(() => reject(new Error('turn timed out')), 20000).unref?.()
      })
      ws.send(JSON.stringify({ type: 'start' }))
      ws.send(input)
      ws.send(JSON.stringify({ type: 'audio_end' }))
      await done
      turnResolve = turnReject = null
    },
  }
}

test('platform config is re-resolved every turn and live status reaches the platform', { timeout: 60000 }, async () => {
  const provider = await startProvider()
  const platform = await startPlatform(provider)
  let bridge
  try {
    bridge = await startBridge(provider, platform, 18785)
    const session = await connect(18785)
    const deviceCalls = () => platform.sessionCalls.filter(c => c.auth === `Bearer ${DEVICE_TOKEN}`)
    assert.equal(deviceCalls().length, 1, 'hello must resolve the platform profile once')
    assert.equal(deviceCalls()[0].auth, `Bearer ${DEVICE_TOKEN}`)
    assert.equal(deviceCalls()[0].bridgeKey, BRIDGE_KEY)

    await session.turn()
    assert.equal(provider.chatLog.length, 1)
    assert.equal(provider.chatLog[0].model, 'platform-model-2', 'turn 1 must use the freshly resolved model')
    assert.equal(provider.chatLog[0].system, 'persona-2')
    assert.equal(provider.chatLog[0].auth, `Bearer ${CHAT_KEY}`)
    assert.ok(session.audioBytes > 0)

    await session.turn()
    assert.equal(provider.chatLog.length, 2)
    assert.equal(provider.chatLog[1].model, 'platform-model-3', 'turn 2 must pick up the platform change without reconnecting')
    assert.equal(provider.chatLog[1].system, 'persona-3')
    assert.equal(deviceCalls().length, 3, 'one resolve at hello plus one per turn')

    const health = await (await fetch('http://127.0.0.1:18785/health')).json()
    assert.equal(health.authentication, 'platform')
    assert.equal(health.devices, 1)
    assert.ok(!('admin' in health), 'the merged console removed /admin from the bridge')
    assert.equal((await fetch('http://127.0.0.1:18785/admin')).status, 404)

    session.ws.close()
    await new Promise(r => setTimeout(r, 600))

    const reports = platform.statusReports
    for (const report of reports) {
      assert.equal(report.auth, `Bearer ${DEVICE_TOKEN}`)
      assert.equal(report.bridgeKey, BRIDGE_KEY)
    }
    const reportedStates = reports.map(r => r.state)
    for (const expected of ['ready', 'listening', 'thinking', 'speaking', 'idle', 'offline'])
      assert.ok(reportedStates.includes(expected), `missing status report: ${expected} (got ${reportedStates})`)
    const summary = reports.find(r => r.state === 'idle' && r.turns === 2)
    assert.ok(summary, 'turn summary must carry the turn count')
    assert.equal(summary.model, 'platform-model-3')
    assert.ok(Number.isFinite(summary.firstAudioMs))

    const logs = bridge.stderr()
    assert.ok(!logs.includes(CHAT_KEY), 'chat key leaked into bridge logs')
    assert.ok(!logs.includes(DEVICE_TOKEN), 'device token leaked into bridge logs')
    assert.ok(!logs.includes(BRIDGE_KEY), 'bridge key leaked into bridge logs')
  } finally {
    bridge?.child.kill()
    await platform.close()
    await provider.close()
  }
})

test('a failed platform refresh keeps the last good config instead of dropping the turn', { timeout: 60000 }, async () => {
  const provider = await startProvider()
  const platform = await startPlatform(provider, { failAfterFirst: true })
  let bridge
  try {
    bridge = await startBridge(provider, platform, 18786)
    const session = await connect(18786)
    await session.turn()
    assert.equal(provider.chatLog.length, 1)
    assert.equal(provider.chatLog[0].model, 'platform-model-1', 'turn must fall back to the hello-time config')
    assert.ok(session.audioBytes > 0)
    assert.ok(bridge.stderr().includes('Platform config refresh failed'))
    assert.ok(!bridge.stderr().includes(CHAT_KEY))
    session.ws.close()
  } finally {
    bridge?.child.kill()
    await platform.close()
    await provider.close()
  }
})