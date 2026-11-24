/* Streaming helpers for the voice bridge (Phase 1).
 *
 * Kept separate from server.mjs so the sentence splitter, the incremental
 * resampler and the SSE reader can be unit-tested without a network or secrets.
 */

/** Chinese sentence terminators the roadmap specifies, plus newline. */
const TERMINATORS = new Set(['。', '！', '？', '；', '!', '?', ';', '\n'])
/** A sentence is also cut at a comma once it grows past this many characters,
 * so a long clause cannot delay the first audible audio. */
const SOFT_CUT = '，,'
const SOFT_CUT_LENGTH = 24

export class SentenceSplitter {
  constructor() { this.pending = '' }

  /** Feed a delta; returns the sentences that became complete. */
  push(text) {
    if (!text) return []
    this.pending += text
    const out = []
    let start = 0
    for (let i = 0; i < this.pending.length; i++) {
      const ch = this.pending[i]
      const hard = TERMINATORS.has(ch)
      const soft = SOFT_CUT.includes(ch) && i - start + 1 >= SOFT_CUT_LENGTH
      if (!hard && !soft) continue
      // Absorb a run of terminators ("？！") and closing quotes into one cut.
      let end = i + 1
      while (end < this.pending.length &&
             (TERMINATORS.has(this.pending[end]) || '”"’\'）)】'.includes(this.pending[end]))) end++
      const sentence = this.pending.slice(start, end).trim()
      if (sentence) out.push(sentence)
      i = end - 1
      start = end
    }
    this.pending = this.pending.slice(start)
    return out
  }

  /** End of stream: whatever is left is a sentence too. */
  flush() {
    const rest = this.pending.trim()
    this.pending = ''
    return rest ? [rest] : []
  }
}

/** Incremental 16-bit linear resampler.
 *
 * Carries the fractional position and the unconsumed tail across chunks so a
 * streamed body produces the same samples as one whole-buffer resamplePcm16()
 * call over the concatenated input.
 */
export class PcmResampler {
  constructor(sourceRate, targetRate = 16000) {
    if (!sourceRate || !targetRate) throw new Error('Invalid sample rate')
    this.sourceRate = sourceRate
    this.targetRate = targetRate
    this.step = sourceRate / targetRate
    this.tail = Buffer.alloc(0)   // source samples from floor(pos) onward
    this.pos = 0                  // fractional read position inside tail
    this.received = 0             // total source samples seen
    this.emitted = 0              // total target samples produced
    this.bypass = sourceRate === targetRate
    this.byteCarry = Buffer.alloc(0)   // network chunks can split a 16-bit sample
  }

  push(chunk) {
    let input = Buffer.from(chunk)
    if (this.byteCarry.length) {
      input = Buffer.concat([this.byteCarry, input])
      this.byteCarry = Buffer.alloc(0)
    }
    if (input.length % 2) {
      this.byteCarry = Buffer.from(input.subarray(input.length - 1))
      input = input.subarray(0, input.length - 1)
    }
    if (!input.length) return Buffer.alloc(0)
    if (this.bypass) { this.received += input.length / 2; this.emitted += input.length / 2; return input }
    this.received += input.length / 2
    this.tail = this.tail.length ? Buffer.concat([this.tail, input]) : input
    return this._drain(false)
  }

  flush() {
    if (this.byteCarry.length) throw new Error('TTS stream ended mid-sample')
    if (this.bypass) return Buffer.alloc(0)
    return this._drain(true)
  }

  _drain(final) {
    const available = this.tail.length / 2
    const target = final
      ? Math.max(1, Math.round(this.received * this.targetRate / this.sourceRate))
      : Infinity
    // Upper bound on outputs for this pass, so one buffer is allocated instead
    // of one per sample (TTS chunks carry hundreds of samples at a time).
    const capacity = final
      ? Math.max(0, target - this.emitted)
      : Math.max(0, Math.ceil((available - 1 - this.pos) / this.step) + 1)
    const out = Buffer.alloc(capacity * 2)
    let written = 0
    while (this.emitted < target && written < capacity) {
      const left = Math.floor(this.pos)
      if (left + 1 >= available && !final) break
      if (left >= available) break
      const right = Math.min(available - 1, left + 1)
      const mix = this.pos - left
      const a = this.tail.readInt16LE(left * 2)
      const b = this.tail.readInt16LE(right * 2)
      const value = Math.round(a * (1 - mix) + b * mix)
      out.writeInt16LE(Math.max(-32768, Math.min(32767, value)), written * 2)
      written++
      this.emitted++
      this.pos += this.step
    }
    const drop = Math.floor(this.pos)
    if (drop > 0) {
      this.tail = Buffer.from(this.tail.subarray(drop * 2))
      this.pos -= drop
    }
    return written === capacity ? out : Buffer.from(out.subarray(0, written * 2))
  }
}

/** Read a fetch body line by line without assuming Node stream semantics.
 * Cancels the body on early exit so breaking at [DONE] releases the socket
 * instead of leaving it pinned until the turn's AbortController fires. */
export async function* readLines(body) {
  const reader = body.getReader()
  const decoder = new TextDecoder()
  let buffer = ''
  let finished = false
  try {
    for (;;) {
      const { value, done } = await reader.read()
      if (done) { finished = true; break }
      buffer += decoder.decode(value, { stream: true })
      let index
      while ((index = buffer.search(/\r?\n/)) >= 0) {
        const line = buffer.slice(0, index)
        buffer = buffer.slice(index + (buffer[index] === '\r' ? 2 : 1))
        yield line
      }
    }
    buffer += decoder.decode()
    if (buffer) yield buffer
  } finally {
    if (!finished) await reader.cancel().catch(() => {})
    reader.releaseLock()
  }
}

/** Stream a chat completion, emitting sentences as soon as they are complete.
 *
 * Falls back to a single JSON body when the provider ignores stream:true, so a
 * non-streaming endpoint still works (just without the latency win).
 * Returns the full reply text.
 */
export async function streamChat({ url, apiKey, model, messages, systemPrompt, onSentence, signal, temperature = 0.8, tools = null, executeTool = null, maxToolRounds = 4, extraBody = null }) {
  const convo = [...messages]
  const splitter = new SentenceSplitter()
  let full = ''
  const emit = text => { full += text; for (const s of splitter.push(text)) onSentence(s) }
  for (let round = 0; round <= maxToolRounds; round++) {
    // The final round drops tools so the model must answer instead of looping.
    const useTools = tools && tools.length && executeTool && round < maxToolRounds
    const body = { model, messages: convo, stream: true, temperature, ...(extraBody || {}) }
    if (useTools) body.tools = tools
    const response = await fetch(url, {
      method: 'POST',
      headers: { Authorization: `Bearer ${apiKey}`, 'Content-Type': 'application/json', Accept: 'text/event-stream' },
      body: JSON.stringify(body),
      signal,
    })
    if (!response.ok) {
      const text = (await response.text()).slice(0, 500)
      throw new Error(`Chat service returned ${response.status}: ${text}`)
    }
    let roundText = ''
    const roundEmit = text => { roundText += text; emit(text) }
    const toolCalls = []
    const contentType = response.headers.get('content-type') || ''
    if (!contentType.includes('text/event-stream')) {
      const result = await response.json()
      const msg = result.choices?.[0]?.message
      if (typeof msg?.content === 'string') roundEmit(msg.content)
      if (Array.isArray(msg?.tool_calls))
        for (const tc of msg.tool_calls) toolCalls.push({ id: tc.id, name: tc.function?.name || '', args: tc.function?.arguments || '{}' })
    } else {
      for await (const line of readLines(response.body)) {
        if (!line.startsWith('data:')) continue
        const payload = line.slice(5).trim()
        if (!payload || payload === '[DONE]') { if (payload === '[DONE]') break; continue }
        let event
        try { event = JSON.parse(payload) } catch { continue }
        const delta = event.choices?.[0]?.delta
        if (typeof delta?.content === 'string' && delta.content) roundEmit(delta.content)
        if (Array.isArray(delta?.tool_calls))
          for (const tc of delta.tool_calls) {
            const idx = tc.index ?? toolCalls.length
            if (!toolCalls[idx]) toolCalls[idx] = { id: tc.id || `call_${idx}`, name: '', args: '' }
            if (tc.id) toolCalls[idx].id = tc.id
            if (tc.function?.name) toolCalls[idx].name += tc.function.name
            if (tc.function?.arguments) toolCalls[idx].args += tc.function.arguments
          }
      }
    }
    const calls = toolCalls.filter(Boolean).filter(c => c.name)
    if (!calls.length) {
      for (const s of splitter.flush()) onSentence(s)
      if (!full.trim()) throw new Error('Chat service returned no reply')
      return full.trim()
    }
    convo.push({ role: 'assistant', content: roundText || null, tool_calls: calls.map(c => ({ id: c.id, type: 'function', function: { name: c.name, arguments: c.args } })) })
    for (const c of calls) {
      const result = await executeTool(c.name, c.args)
      convo.push({ role: 'tool', tool_call_id: c.id, content: String(result) })
    }
  }
  throw new Error('Chat service exceeded tool rounds')
}

/** Locate the payload of a WAV body so streaming can start before it ends.
 * Returns { sampleRate, headerBytes } or null if the header is not complete. */
export function wavStreamHeader(buffer) {
  if (buffer.length < 12) return null
  if (buffer.toString('ascii', 0, 4) !== 'RIFF' || buffer.toString('ascii', 8, 12) !== 'WAVE')
    throw new Error('TTS did not return a valid WAV file')
  let offset = 12
  let format = null
  while (offset + 8 <= buffer.length) {
    const id = buffer.toString('ascii', offset, offset + 4)
    const size = buffer.readUInt32LE(offset + 4)
    const start = offset + 8
    if (id === 'fmt ') {
      if (start + 16 > buffer.length) return null
      format = {
        encoding: buffer.readUInt16LE(start),
        channels: buffer.readUInt16LE(start + 2),
        sampleRate: buffer.readUInt32LE(start + 4),
        bits: buffer.readUInt16LE(start + 14),
      }
    } else if (id === 'data') {
      if (!format) throw new Error('WAV data chunk before fmt chunk')
      if (format.encoding !== 1 || format.bits !== 16 || format.channels !== 1)
        throw new Error('Only mono 16-bit PCM WAV is supported')
      // The declared size is ignored: streaming WAV often advertises 0xFFFFFFFF.
      return { sampleRate: format.sampleRate, headerBytes: start }
    }
    offset = start + size + (size & 1)
    if (size === 0xFFFFFFFF) return null
  }
  return null
}

/** Stream one sentence of TTS, delivering 16 kHz PCM as it arrives. */
export async function streamSpeech({ url, apiKey, model, voice, text, format, pcmSampleRate, onPcm, signal }) {
  const response = await fetch(url, {
    method: 'POST',
    headers: { Authorization: `Bearer ${apiKey}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({ model, voice, input: text, response_format: format, stream: true }),
    signal,
  })
  if (!response.ok) {
    const body = (await response.text()).slice(0, 500)
    throw new Error(`Text-to-speech service returned ${response.status}: ${body}`)
  }
  const reader = response.body.getReader()
  const rate = format === 'pcm' ? Number(pcmSampleRate || 24000) : null
  let resampler = rate ? new PcmResampler(rate, 16000) : null
  let pending = Buffer.alloc(0)
  let headerBytes = 0
  try {
    for (;;) {
      const { value, done } = await reader.read()
      if (done) break
      const chunk = Buffer.from(value)
      if (resampler) {
        const out = resampler.push(chunk)
        if (out.length) onPcm(out)
        continue
      }
      // WAV: wait for the header, then stream the data payload.
      pending = pending.length ? Buffer.concat([pending, chunk]) : chunk
      if (!headerBytes) {
        const header = wavStreamHeader(pending)
        if (!header) continue
        headerBytes = header.headerBytes
        resampler = new PcmResampler(header.sampleRate, 16000)
        const rest = pending.subarray(headerBytes)
        pending = Buffer.alloc(0)
        if (rest.length) { const out = resampler.push(rest); if (out.length) onPcm(out) }
        continue
      }
      const out = resampler.push(chunk)
      if (out.length) onPcm(out)
    }
    if (!resampler) throw new Error('TTS returned no audio payload')
    const out = resampler.flush()
    if (out.length) onPcm(out)
  } finally {
    reader.releaseLock()
  }
}