import crypto from 'node:crypto'
import http from 'node:http'
import fs from 'node:fs'
import { WebSocketServer, WebSocket } from 'ws'
import { pcm16ToWav } from './audio.mjs'
import { streamChat, streamSpeech } from './stream.mjs'
import { TOOLS, executeTool, fetchWeather, formatWeather } from './tools.mjs'
import { renderWeatherCard, latestOverlayPath } from './overlay.mjs'
import { initMemory, recentMessages, recallFor, recordTurn, noteTurnComplete } from './memory.mjs'

const env = process.env
const port = Number(env.PORT || 8780)
const deviceToken = env.DEVICE_TOKEN || ''
const maxAudioBytes = 16000 * 2 * 30
const startedAt = Date.now()
// Tool calling stays on by default (the detected local model advertises it).
// CHAT_TOOLS=0 disables it outright, and a per-turn fallback retries without
// tools if the model rejects them, so pure chat always answers.
const toolsEnabled = env.CHAT_TOOLS !== '0'

/** Live connection registry for /health and platform status reports.
 * The device token is held in memory only, for platform calls; never logged. */
const devices = new Map()

function endpoint(base, path) {
  return `${String(base || '').replace(/\/$/, '')}${path}`
}

function sameSecret(a, b) {
  const left = Buffer.from(String(a))
  const right = Buffer.from(String(b))
  return left.length === right.length && left.length > 0 && crypto.timingSafeEqual(left, right)
}

function sendJson(ws, value) {
  if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(value))
}

function state(ws, name, message) {
  const device = devices.get(ws)
  const changed = device && device.state !== name
  if (device) device.state = name
  sendJson(ws, { type: 'state', state: name, message })
  if (changed) reportPlatform(device, name)
}

/** Fire-and-forget live status for the platform studio console. Failures are
 * logged without secrets and never block or fail the voice turn. */
function reportPlatform(device, stateName, extra = {}) {
  if (!env.PLATFORM_API_URL || !device?.token) return
  const headers = {
    Authorization: `Bearer ${device.token}`,
    'X-Qiban-Bridge-Key': env.PLATFORM_BRIDGE_KEY || '',
    'Content-Type': 'application/json',
  }
  if (env.PLATFORM_SITE_TOKEN) headers['OAI-Sites-Authorization'] = `Bearer ${env.PLATFORM_SITE_TOKEN}`
  void fetch(endpoint(env.PLATFORM_API_URL, '/api/v1/voice/status'), {
    method: 'POST', headers, body: JSON.stringify({ state: stateName, ...extra }),
    signal: AbortSignal.timeout(8000),
  }).then(response => {
    if (!response.ok) console.warn(`Platform status report failed: state=${stateName} HTTP ${response.status}`)
    else console.log(`Platform status reported: state=${stateName} HTTP ${response.status}`)
  }).catch(error => console.warn(`Platform status report error: state=${stateName}`, error instanceof Error ? error.message : error))
}

async function checkedFetch(url, options, service) {
  const response = await fetch(url, { ...options, signal: AbortSignal.timeout(120000) })
  if (!response.ok) {
    const body = (await response.text()).slice(0, 500)
    throw new Error(`${service} returned ${response.status}: ${body}`)
  }
  return response
}

async function transcribe(pcm, sampleRate) {
  const form = new FormData()
  form.set('model', env.STT_MODEL || 'whisper-1')
  form.set('file', new Blob([pcm16ToWav(pcm, sampleRate)], { type: 'audio/wav' }), 'turn.wav')
  const response = await checkedFetch(endpoint(env.STT_BASE_URL, '/audio/transcriptions'), {
    method: 'POST', headers: { Authorization: `Bearer ${env.STT_API_KEY}` }, body: form,
  }, 'Speech-to-text service')
  const result = await response.json()
  if (!result.text?.trim()) throw new Error('Speech-to-text service returned no text')
  return result.text.trim()
}

/** Ordered, paced audio pipeline.
 *
 * Sentences are synthesized as soon as the LLM finishes them, but their audio
 * is emitted strictly in sentence order. The next sentence's TTS request is
 * already in flight while the current one plays, so there is no request
 * round-trip gap between sentences.
 *
 * `startedAt` is the moment transcription completed; the first emitted frame
 * against that clock is the Phase 1 acceptance metric.
 */
function createAudioPipeline(ws, { startedAt, signal, chunkSize = 1280, paceMs = 35, maxInFlight = 3 }) {
  const segments = []
  let llmDone = false
  let wake = null
  let inFlight = 0
  const waiting = []
  let firstAudioMs = null
  let frames = 0
  let bytes = 0
  let carry = Buffer.alloc(0)
  let failure = null

  const notify = () => { const w = wake; wake = null; if (w) w() }
  const closed = () => ws.readyState !== WebSocket.OPEN

  async function acquire() {
    if (inFlight < maxInFlight) { inFlight++; return }
    await new Promise(resolve => waiting.push(resolve))
    inFlight++
  }
  function release() {
    inFlight--
    const next = waiting.shift()
    if (next) next()
  }

  function startSegment(text) {
    const segment = { chunks: [], done: false, error: null }
    segments.push(segment)
    notify()
    void (async () => {
      try {
        await acquire()
        if (closed() || signal?.aborted) return
        await streamSpeech({
          url: endpoint(env.TTS_BASE_URL, '/audio/speech'),
          apiKey: env.TTS_API_KEY,
          model: env.TTS_MODEL,
          voice: env.TTS_VOICE || 'alloy',
          text,
          format: env.TTS_RESPONSE_FORMAT || 'wav',
          pcmSampleRate: env.TTS_PCM_SAMPLE_RATE,
          signal,
          onPcm: pcm => { segment.chunks.push(pcm); notify() },
        })
      } catch (error) {
        segment.error = error
      } finally {
        release()
        segment.done = true
        notify()
      }
    })()
    return segment
  }

  async function sendPiece(piece) {
    while (ws.bufferedAmount > chunkSize * 8 && !closed())
      await new Promise(resolve => setTimeout(resolve, 10))
    if (closed()) return
    if (firstAudioMs === null) {
      firstAudioMs = Date.now() - startedAt
      state(ws, 'speaking', '正在回答…')
    }
    ws.send(piece, { binary: true })
    frames++
    bytes += piece.length
    await new Promise(resolve => setTimeout(resolve, paceMs))
  }

  async function emit(buf) {
    carry = carry.length ? Buffer.concat([carry, buf]) : buf
    while (carry.length >= chunkSize && !closed()) {
      const piece = carry.subarray(0, chunkSize)
      carry = Buffer.from(carry.subarray(chunkSize))
      await sendPiece(piece)
    }
  }

  const pump = (async () => {
    let index = 0
    for (;;) {
      if (closed() || signal?.aborted || failure) break
      if (index >= segments.length) {
        if (llmDone) break
        // Register the waiter and re-check synchronously: a producer cannot
        // run between these two statements, so no wakeup can be lost.
        await new Promise(resolve => {
          wake = resolve
          if (segments.length > index || llmDone || closed()) { wake = null; resolve() }
        })
        continue
      }
      const segment = segments[index]
      while (segment.chunks.length && !closed() && !failure)
        await emit(segment.chunks.shift())
      if (segment.error) { failure = segment.error; break }
      if (segment.done) { index++; continue }
      await new Promise(resolve => {
        wake = resolve
        if (segment.chunks.length || segment.done || segment.error || closed()) { wake = null; resolve() }
      })
    }
    if (carry.length && !closed() && !failure) await sendPiece(carry)
    carry = Buffer.alloc(0)
  })()

  return {
    addSentence: text => { if (!closed() && !failure) startSegment(text) },
    finish() { llmDone = true; notify() },
    get firstAudioMs() { return firstAudioMs },
    get stats() { return { frames, bytes, sentences: segments.length } },
    async done() {
      llmDone = true
      notify()
      await pump
      if (failure) throw failure
      if (!closed()) state(ws, 'idle', '按住 BOOT 说话')
    },
  }
}

async function streamAudio(ws, pcm) {
  state(ws, 'speaking', '正在回答…')
  const chunkSize = 1280
  for (let offset = 0; offset < pcm.length && ws.readyState === WebSocket.OPEN; offset += chunkSize) {
    while (ws.bufferedAmount > chunkSize * 8) await new Promise(resolve => setTimeout(resolve, 10))
    ws.send(pcm.subarray(offset, Math.min(pcm.length, offset + chunkSize)), { binary: true })
    await new Promise(resolve => setTimeout(resolve, 35))
  }
  state(ws, 'idle', '按住 BOOT 说话')
}

function validatePlatformSpeechConfig() {
  const required = ['STT_BASE_URL', 'STT_API_KEY', 'STT_MODEL', 'TTS_BASE_URL', 'TTS_API_KEY', 'TTS_MODEL']
  return required.filter(name => !env[name])
}

function validateChatConfig(value) {
  if (!value || typeof value !== 'object') throw new Error('Chat model is not configured for this device owner')
  const chat = {
    baseUrl: String(value.baseUrl || '').replace(/\/$/, ''),
    apiKey: String(value.apiKey || ''),
    model: String(value.model || ''),
    systemPrompt: String(value.systemPrompt || env.SYSTEM_PROMPT || '请用简短自然的中文回答。'),
  }
  let url
  try { url = new URL(chat.baseUrl) } catch { throw new Error('Chat API URL is invalid') }
  if (!['https:', 'http:'].includes(url.protocol) || !chat.apiKey || !chat.model)
    throw new Error('Chat model is not fully configured for this device owner')
  return chat
}

function echoChat() {
  return { baseUrl: 'http://127.0.0.1', apiKey: 'echo-test', model: 'echo-test', systemPrompt: 'echo-test' }
}

async function resolveDevice(token) {
  if (env.PLATFORM_API_URL) {
    if (!env.PLATFORM_BRIDGE_KEY) throw new Error('Voice bridge is missing PLATFORM_BRIDGE_KEY')
    const headers = {
      Authorization: `Bearer ${token}`,
      'X-Qiban-Bridge-Key': env.PLATFORM_BRIDGE_KEY,
      'Content-Type': 'application/json',
    }
    if (env.PLATFORM_SITE_TOKEN) headers['OAI-Sites-Authorization'] = `Bearer ${env.PLATFORM_SITE_TOKEN}`
    const response = await fetch(endpoint(env.PLATFORM_API_URL, '/api/v1/voice/session'), {
      method: 'POST', headers, body: '{}', signal: AbortSignal.timeout(120000),
    })
    // Echo link tests run before the owner saves chat settings; the platform
    // still authenticates the device token and the bridge key.
    if (response.status === 409 && env.MOCK_MODE === 'echo') return echoChat()
    if (!response.ok) {
      const body = (await response.text()).slice(0, 500)
      throw new Error(`Qiban platform returned ${response.status}: ${body}`)
    }
    return validateChatConfig(await response.json())
  }
  if (!sameSecret(token, deviceToken)) throw new Error('Invalid device token')
  if (env.MOCK_MODE === 'echo') return echoChat()
  return validateChatConfig({
    baseUrl: env.CHAT_BASE_URL,
    apiKey: env.CHAT_API_KEY,
    model: env.CHAT_MODEL,
    systemPrompt: env.SYSTEM_PROMPT,
  })
}

/** Print the live ASR / LLM / TTS stack at startup so a BOOT-key test needs no
 * other terminal. Retries while SenseVoice is still loading. */
async function stackStatus(attempt = 1) {
  const speechOrigin = env.STT_BASE_URL ? new URL(env.STT_BASE_URL).origin : ''
  let speech = null
  try {
    const response = await fetch(endpoint(speechOrigin, '/health'), { signal: AbortSignal.timeout(4000) })
    if (response.ok) speech = await response.json()
  } catch {}
  let llm = 'not configured'
  if (env.PLATFORM_API_URL && deviceToken) {
    try {
      const chat = await resolveDevice(deviceToken)
      llm = `${chat.model} @ ${chat.baseUrl}`
    } catch (error) {
      llm = `unavailable (${error instanceof Error ? error.message : String(error)})`
    }
  } else if (env.CHAT_MODEL) {
    llm = `${env.CHAT_MODEL} @ ${env.CHAT_BASE_URL}`
  }
  console.log('--- local voice stack ---')
  console.log(`ASR: ${env.STT_MODEL || 'n/a'} @ ${speechOrigin || 'n/a'} (ready=${speech?.asrReady ?? 'unknown'})`)
  console.log(`LLM: ${llm} (tools=${toolsEnabled ? 'on' : 'off'})`)
  console.log(`TTS: ${env.TTS_MODEL || 'n/a'} voice=${env.TTS_VOICE || 'n/a'} @ ${speechOrigin || 'n/a'}`)
  if (speech && speech.asrReady === false && attempt < 12)
    setTimeout(() => void stackStatus(attempt + 1), 5000)
  else if (speech && speech.asrReady === false)
    console.warn('ASR still not ready; check the speech-service window.')
}

async function processTurn(session, pcm) {
  const { ws, sampleRate } = session
  const controller = new AbortController()
  session.abort = controller
  try {
    state(ws, 'thinking', '正在识别…')
    if (env.MOCK_MODE === 'echo') {
      sendJson(ws, { type: 'transcript', text: '回声测试' })
      sendJson(ws, { type: 'reply', text: '这是刚才录到的声音。' })
      await streamAudio(ws, pcm)
      return
    }
    const missing = validatePlatformSpeechConfig()
    if (missing.length) throw new Error(`Voice bridge is missing: ${missing.join(', ')}`)
    if (!session.chat) throw new Error('Chat model is not configured for this device owner')
    // Re-resolve the platform profile every turn so a studio save takes effect
    // on the next turn without forcing the board to reconnect. On a refresh
    // failure, keep the last known-good config instead of dropping the turn.
    let chat = session.chat
    if (env.PLATFORM_API_URL && session.token) {
      try {
        chat = await resolveDevice(session.token)
        session.chat = chat
      } catch (error) {
        console.warn('Platform config refresh failed; using previous config:',
                     error instanceof Error ? error.message : error)
      }
    }
    const transcript = await transcribe(pcm, sampleRate)
    sendJson(ws, { type: 'transcript', text: transcript })
    // Phase 1 clock starts here: batch ASR latency is a separate, known floor.
    const startedAt = Date.now()
    session.messages.push({ role: 'user', content: transcript })
    session.messages = session.messages.slice(-16)
    state(ws, 'thinking', '角色正在思考…')
    const memory = recallFor(transcript)
    // Intercept get_weather to also render + push a screen overlay card. The
    // render is fire-and-forget so TTS latency is unchanged; the LLM still
    // receives the same plain-text weather summary as before.
    const execTool = async (name, argsJson) => {
      if (name === 'get_weather') {
        let args = {}
        try { args = argsJson ? JSON.parse(argsJson) : {} } catch {}
        const data = await fetchWeather(args.city)
        if (data.ok) {
          void renderWeatherCard(data).then(url => {
            if (url) {
              sendJson(ws, { type: 'display_overlay', url, duration_ms: 0 })
              console.log(`Weather overlay pushed: ${url} (persistent until next turn)`)
            }
          }).catch(error => console.warn('Weather overlay failed:', error instanceof Error ? error.message : error))
        }
        return formatWeather(data)
      }
      return executeTool(name, argsJson)
    }
    const pipeline = createAudioPipeline(ws, { startedAt, signal: controller.signal })
    let reply
    try {
      const chatRequest = {
        url: endpoint(chat.baseUrl, '/chat/completions'),
        apiKey: chat.apiKey,
        model: chat.model,
        messages: [{ role: 'system', content: chat.systemPrompt + memory.prompt }, ...session.messages],
        onSentence: sentence => pipeline.addSentence(sentence),
        signal: controller.signal,
        extraBody: env.LLM_REASONING_EFFORT ? { reasoning_effort: env.LLM_REASONING_EFFORT } : null,
      }
      try {
        reply = await streamChat({ ...chatRequest, tools: toolsEnabled ? TOOLS : null, executeTool: execTool })
      } catch (error) {
        // A model without function calling rejects the tools parameter outright
        // (HTTP 400 before any audio). Retry the turn as plain chat.
        const message = error instanceof Error ? error.message : String(error)
        if (!toolsEnabled || pipeline.stats.sentences || !/tool|function/i.test(message)) throw error
        console.warn('Chat model rejected tools; retrying without tools:', message)
        reply = await streamChat({ ...chatRequest, tools: null, executeTool: null })
      }
    } finally {
      pipeline.finish()
    }
    session.messages.push({ role: 'assistant', content: reply })
    recordTurn(transcript, reply)
    sendJson(ws, { type: 'reply', text: reply })
    await pipeline.done()
    noteTurnComplete(chat)
    const stats = pipeline.stats
    const device = devices.get(ws)
    if (device) {
      device.turns++
      device.lastFirstAudioMs = pipeline.firstAudioMs
      reportPlatform(device, 'idle', { turns: device.turns, model: chat.model, firstAudioMs: pipeline.firstAudioMs })
    }
    console.log(`Turn timing: transcript->first audio ${pipeline.firstAudioMs ?? 'n/a'} ms, ` +
                `sentences=${stats.sentences} frames=${stats.frames} bytes=${stats.bytes} model=${chat.model}`)
  } catch (error) {
    if (!controller.signal.aborted) {
      console.error('Voice turn failed:', error instanceof Error ? error.message : error)
      state(ws, 'error', error instanceof Error ? error.message : 'Voice turn failed')
    }
  } finally {
    controller.abort()
    session.abort = null
    session.busy = false
  }
}

const server = http.createServer((request, response) => {
  if (request.url === '/health') {
    response.writeHead(200, { 'content-type': 'application/json' })
    response.end(JSON.stringify({
      ok: true,
      uptimeSec: Math.round((Date.now() - startedAt) / 1000),
      mock: env.MOCK_MODE || null,
      authentication: env.PLATFORM_API_URL ? 'platform' : 'static',
      devices: devices.size,
      platformSpeech: {
        sttConfigured: !!(env.STT_BASE_URL && env.STT_API_KEY && env.STT_MODEL),
        ttsConfigured: !!(env.TTS_BASE_URL && env.TTS_API_KEY && env.TTS_MODEL),
      },
    }))
  } else if (request.url === '/overlay/latest.jpg' || request.url?.startsWith('/overlay/latest.jpg?')) {
    const filePath = latestOverlayPath()
    if (!filePath) { response.writeHead(404); response.end('No overlay yet'); return }
    response.writeHead(200, { 'content-type': 'image/jpeg', 'cache-control': 'no-store' })
    fs.createReadStream(filePath).pipe(response)
  } else {
    response.writeHead(404)
    response.end('Not found')
  }
})

const wss = new WebSocketServer({ server, path: '/voice', maxPayload: 64 * 1024 })
wss.on('connection', (ws, request) => {
  const session = { ws, authenticated: false, authenticating: false, recording: false, busy: false, sampleRate: 16000, chunks: [], bytes: 0, messages: [], chat: null, token: null, abort: null, heartbeat: null }
  const address = request.socket.remoteAddress || 'unknown'
  devices.set(ws, {
    id: crypto.randomUUID(),
    address,
    connectedAt: Date.now(),
    state: 'connecting',
    turns: 0,
    lastFirstAudioMs: null,
  })
  console.log(`Device WS connected from ${address}`)
  const authTimer = setTimeout(() => ws.close(4001, 'Authentication timeout'), 5000)
  ws.on('message', async (data, binary) => {
    if (binary) {
      if (!session.authenticated || !session.recording || session.busy) return
      const chunk = Buffer.from(data)
      if (session.bytes + chunk.length > maxAudioBytes) {
        session.recording = false
        state(ws, 'error', '录音超过 30 秒，请缩短后重试')
        return
      }
      session.chunks.push(chunk)
      session.bytes += chunk.length
      return
    }
    let message
    try { message = JSON.parse(data.toString()) } catch { return }
    if (!session.authenticated) {
      if (message.type !== 'hello' || session.authenticating || typeof message.token !== 'string') return
      session.authenticating = true
      try {
        session.chat = await resolveDevice(message.token)
        session.token = message.token
        const device = devices.get(ws)
        if (device) device.token = message.token
        clearTimeout(authTimer)
        session.authenticated = true
        session.messages = recentMessages()
        session.sampleRate = message.sampleRate === 16000 ? 16000 : 16000
        console.log(`Device authenticated: id=${device?.id} from ${device?.address}`)
        // Push standby status immediately so the studio card shows 待机 without
        // waiting for the first voice turn; voice_seen/last_seen refresh to now.
        // 'ready' is the post-auth standby state; studio renders ready/idle as 待机.
        state(ws, 'ready', '按住 BOOT 说话')
        // Keepalive + status refresh: Cloudflare quick tunnels drop idle WS
        // (~100s) and the studio card goes stale after 90s without a push.
        session.heartbeat = setInterval(() => {
          if (ws.readyState !== WebSocket.OPEN) return
          ws.ping()
          const d = devices.get(ws)
          if (d?.token) reportPlatform(d, d.state)
        }, 30000)
      } catch (error) {
        console.error('Device authentication failed:', error instanceof Error ? error.message : error)
        ws.close(4003, 'Invalid device token or chat configuration')
      }
      return
    }
    if (message.type === 'start' && !session.busy) {
      session.recording = true
      session.chunks = []
      session.bytes = 0
      state(ws, 'listening', '正在听…松开发送')
    } else if (message.type === 'audio_end' && session.recording && !session.busy) {
      session.recording = false
      if (session.bytes < 3200) {
        state(ws, 'error', '说话时间太短，请重试')
        return
      }
      session.busy = true
      void processTurn(session, Buffer.concat(session.chunks, session.bytes))
      session.chunks = []
    }
  })
  ws.on('error', error => {
    console.warn(`Device WS error: id=${devices.get(ws)?.id}`, error instanceof Error ? error.message : error)
  })
  ws.on('close', (code, reason) => {
    clearTimeout(authTimer)
    if (session.heartbeat) clearInterval(session.heartbeat)
    const device = devices.get(ws)
    console.log(`Device WS closed: id=${device?.id} code=${code} state=${device?.state}`)
    reportPlatform(device, 'offline')
    devices.delete(ws)
    // Cancel any in-flight LLM/TTS fetch so a dropped device cannot leak
    // sockets or keep synthesizing audio nobody will play.
    session.abort?.abort()
    session.recording = false
    session.chunks = []
    session.bytes = 0
  })
})

initMemory()

server.listen(port, '0.0.0.0', () => {
  console.log(`Qiban voice bridge listening on http://0.0.0.0:${port}`)
  console.log(env.MOCK_MODE === 'echo' ? 'Echo test mode is enabled.' : 'Provider mode is enabled.')
  console.log(env.PLATFORM_API_URL
    ? 'Chat settings and live device status are managed in the Qiban platform studio.'
    : 'Static single-device mode: chat settings come from .env.')
  void stackStatus()
})
