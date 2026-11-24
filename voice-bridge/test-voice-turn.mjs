// End-to-end voice turn without the board: synthesize a Chinese utterance with
// the local edge-tts shim, feed it through the bridge's device WebSocket, and
// report transcript / reply / audio stats. Token comes from DEVICE_TOKEN env.
import { WebSocket } from 'ws'
import { wavStreamHeader, PcmResampler } from './stream.mjs'

const bridge = process.env.BRIDGE_URL || 'ws://127.0.0.1:8780/voice'
const speech = process.env.SPEECH_URL || 'http://127.0.0.1:8782/v1/audio/speech'
const token = process.env.DEVICE_TOKEN || ''
const question = process.argv[2] || '你好，请用一句话介绍一下你自己。'
if (!token) { console.error('DEVICE_TOKEN env is required'); process.exit(1) }

async function synth(text) {
  const res = await fetch(speech, {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ model: 'edge-tts', voice: 'zh-CN-XiaoxiaoNeural', input: text, response_format: 'wav' }),
  })
  if (!res.ok) throw new Error(`TTS ${res.status}`)
  const wav = Buffer.from(await res.arrayBuffer())
  const header = wavStreamHeader(wav)
  if (!header) throw new Error('bad wav')
  const pcm = new PcmResampler(header.sampleRate, 16000)
  const out = Buffer.concat([pcm.push(wav.subarray(header.headerBytes)), pcm.flush()])
  return out
}

function run(pcm) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(bridge, { headers: { 'x-test': 'turn' } })
    const started = Date.now()
    let transcript = null, reply = null, audioBytes = 0, audioFrames = 0, firstAudioMs = null
    const timer = setTimeout(() => { ws.close(); reject(new Error('turn timeout 90s')) }, 90000)
    const fail = error => { clearTimeout(timer); try { ws.close() } catch {} reject(error) }
    ws.on('open', () => ws.send(JSON.stringify({ type: 'hello', token, sampleRate: 16000 })))
    ws.on('error', fail)
    ws.on('message', (data, binary) => {
      if (binary) {
        audioBytes += data.length; audioFrames++
        if (firstAudioMs === null) firstAudioMs = Date.now() - started
        return
      }
      let message
      try { message = JSON.parse(data.toString()) } catch { return }
      if (message.type === 'ready' || message.state === 'ready') {
        ws.send(JSON.stringify({ type: 'start' }))
        for (let i = 0; i < pcm.length; i += 1280)
          ws.send(pcm.subarray(i, Math.min(pcm.length, i + 1280)), { binary: true })
        ws.send(JSON.stringify({ type: 'audio_end' }))
      }
      if (message.type === 'transcript') transcript = message.text
      if (message.type === 'reply') reply = message.text
      if (message.state === 'idle' && reply) {
        clearTimeout(timer)
        setTimeout(() => { ws.close(); resolve({ transcript, reply, audioBytes, audioFrames, firstAudioMs }) }, 300)
      }
      if (message.state === 'error') return fail(new Error(message.message || 'bridge error'))
    })
  })
}

const pcm = await synth(question)
console.log(`# synthesized ${question} -> ${pcm.length} bytes PCM16@16k`)
const result = await run(pcm)
console.log(JSON.stringify(result, null, 2))
if (!result.transcript || !result.reply) { console.error('MISSING transcript or reply'); process.exit(2) }
console.log('TURN OK')