import assert from 'node:assert/strict'
import http from 'node:http'
import test from 'node:test'
import { SentenceSplitter, PcmResampler, readLines, wavStreamHeader, streamChat, streamSpeech } from './stream.mjs'
import { resamplePcm16, pcm16ToWav } from './audio.mjs'

function signal(samples) {
  const buf = Buffer.alloc(samples * 2)
  for (let i = 0; i < samples; i++)
    buf.writeInt16LE(Math.round(20000 * Math.sin(i / 7) + 3000 * Math.sin(i / 2.3)), i * 2)
  return buf
}

test('splitter cuts on the roadmap terminators and keeps the partial tail', () => {
  const s = new SentenceSplitter()
  assert.deepEqual(s.push('你好呀。今天'), ['你好呀。'])
  assert.deepEqual(s.push('天气不错！要不要'), ['今天天气不错！'])
  assert.deepEqual(s.push('出去走走？'), ['要不要出去走走？'])
  assert.deepEqual(s.push('分号；也切'), ['分号；'])
  assert.deepEqual(s.flush(), ['也切'])
  assert.deepEqual(s.flush(), [])
})

test('splitter absorbs terminator runs and closing quotes', () => {
  const s = new SentenceSplitter()
  assert.deepEqual(s.push('真的吗？！太好了。”下一句'), ['真的吗？！', '太好了。”'])
  assert.deepEqual(s.flush(), ['下一句'])
})

test('splitter soft-cuts a long comma clause so first audio is not delayed', () => {
  const s = new SentenceSplitter()
  const clause = '这是一个非常长的从句而且没有任何句末标点只有逗号，'
  const out = s.push(clause + '后面还有内容。')
  assert.ok(out.length >= 2, `expected a soft cut, got ${JSON.stringify(out)}`)
  assert.ok(out[0].endsWith('，'))
})

test('splitter ignores empty and whitespace-only deltas', () => {
  const s = new SentenceSplitter()
  assert.deepEqual(s.push(''), [])
  assert.deepEqual(s.push('   '), [])
  assert.deepEqual(s.flush(), [])
})

test('incremental resampler matches whole-buffer resamplePcm16 exactly', () => {
  const src = signal(4800)   // 200 ms at 24 kHz
  const expected = resamplePcm16(src, 24000, 16000)
  for (const size of [1, 2, 6, 64, 999, 4800]) {
    const r = new PcmResampler(24000, 16000)
    const pieces = []
    for (let offset = 0; offset < src.length; offset += size)
      pieces.push(r.push(src.subarray(offset, Math.min(src.length, offset + size))))
    pieces.push(r.flush())
    const got = Buffer.concat(pieces)
    assert.equal(got.length, expected.length, `chunk size ${size}: length mismatch`)
    assert.deepEqual(got, expected, `chunk size ${size}: sample mismatch`)
  }
})

test('resampler handles odd sample counts and rate bypass', () => {
  for (const n of [1, 2, 3, 5, 17]) {
    const src = signal(n)
    const r = new PcmResampler(24000, 16000)
    const got = Buffer.concat([r.push(src), r.flush()])
    assert.deepEqual(got, resamplePcm16(src, 24000, 16000), `n=${n}`)
  }
  const same = signal(100)
  const bypass = new PcmResampler(16000, 16000)
  assert.deepEqual(Buffer.concat([bypass.push(same), bypass.flush()]), same)
})

test('resampler carries a split sample and rejects a truncated stream', () => {
  assert.throws(() => new PcmResampler(0, 16000))
  const src = signal(64)
  const r = new PcmResampler(24000, 16000)
  const pieces = []
  // Feed one byte at a time: every sample straddles two network chunks.
  for (let i = 0; i < src.length; i++) pieces.push(r.push(src.subarray(i, i + 1)))
  pieces.push(r.flush())
  assert.deepEqual(Buffer.concat(pieces), resamplePcm16(src, 24000, 16000))
  const truncated = new PcmResampler(24000, 16000)
  truncated.push(Buffer.alloc(3))
  assert.throws(() => truncated.flush(), /ended mid-sample/)
})

test('wavStreamHeader finds the payload and tolerates a streaming size field', () => {
  const wav = pcm16ToWav(signal(100), 24000)
  const header = wavStreamHeader(wav)
  assert.equal(header.sampleRate, 24000)
  assert.equal(header.headerBytes, 44)
  assert.deepEqual(wav.subarray(header.headerBytes), signal(100))
  assert.equal(wavStreamHeader(wav.subarray(0, 20)), null)
  const streaming = Buffer.from(wav)
  streaming.writeUInt32LE(0xFFFFFFFF, 40)
  assert.equal(wavStreamHeader(streaming).headerBytes, 44)
})

test('readLines handles LF, CRLF and lines split across chunks', async () => {
  const body = new ReadableStream({
    start(controller) {
      controller.enqueue(new TextEncoder().encode('data: a\r\ndata: b\n'))
      controller.enqueue(new TextEncoder().encode('data: c'))
      controller.enqueue(new TextEncoder().encode('d\n\n'))
      controller.close()
    },
  })
  const lines = []
  for await (const line of readLines(body)) lines.push(line)
  assert.deepEqual(lines, ['data: a', 'data: b', 'data: cd', ''])
})

/** Minimal mock provider so streaming can be tested without keys or network. */
async function mockProvider(handler) {
  const server = http.createServer(handler)
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
  return { url: `http://127.0.0.1:${server.address().port}`, close: () => new Promise(r => server.close(r)) }
}

test('streamChat emits sentences in order and returns the full reply', async () => {
  const deltas = ['你好', '呀。', '今天天气', '不错！', '要不要出去', '走走？']
  const provider = await mockProvider((req, res) => {
    res.writeHead(200, { 'content-type': 'text/event-stream' })
    let i = 0
    const timer = setInterval(() => {
      if (i >= deltas.length) {
        res.write('data: [DONE]\n\n')
        clearInterval(timer)
        res.end()
        return
      }
      res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: deltas[i++] } }] })}\n\n`)
    }, 5)
  })
  try {
    const sentences = []
    const text = await streamChat({
      url: `${provider.url}/chat/completions`, apiKey: 'test', model: 'test',
      messages: [{ role: 'user', content: 'hi' }], onSentence: s => sentences.push(s),
    })
    assert.deepEqual(sentences, ['你好呀。', '今天天气不错！', '要不要出去走走？'])
    assert.equal(text, deltas.join(''))
  } finally { await provider.close() }
})

test('streamChat falls back to a non-streaming JSON body', async () => {
  const provider = await mockProvider((req, res) => {
    res.writeHead(200, { 'content-type': 'application/json' })
    res.end(JSON.stringify({ choices: [{ message: { content: '第一句。第二句！' } }] }))
  })
  try {
    const sentences = []
    const text = await streamChat({
      url: `${provider.url}/chat/completions`, apiKey: 'test', model: 'test',
      messages: [], onSentence: s => sentences.push(s),
    })
    assert.deepEqual(sentences, ['第一句。', '第二句！'])
    assert.equal(text, '第一句。第二句！')
  } finally { await provider.close() }
})

test('streamChat surfaces provider errors and empty replies', async () => {
  const provider = await mockProvider((req, res) => { res.writeHead(500); res.end('boom') })
  try {
    await assert.rejects(() => streamChat({
      url: `${provider.url}/chat/completions`, apiKey: 'test', model: 'test',
      messages: [], onSentence: () => {},
    }), /Chat service returned 500/)
  } finally { await provider.close() }
})

test('streamSpeech resamples a streamed PCM body to 16 kHz', async () => {
  const src = signal(24000)   // 1 s at 24 kHz
  const provider = await mockProvider((req, res) => {
    res.writeHead(200, { 'content-type': 'application/octet-stream' })
    let offset = 0
    const timer = setInterval(() => {
      if (offset >= src.length) { clearInterval(timer); res.end(); return }
      res.write(src.subarray(offset, offset + 1000))
      offset += 1000
    }, 2)
  })
  try {
    const pieces = []
    await streamSpeech({
      url: `${provider.url}/audio/speech`, apiKey: 'test', model: 'test', voice: 'alloy',
      text: '你好', format: 'pcm', pcmSampleRate: 24000, onPcm: b => pieces.push(b),
    })
    assert.ok(pieces.length > 1, 'audio should arrive incrementally, not as one block')
    assert.deepEqual(Buffer.concat(pieces), resamplePcm16(src, 24000, 16000))
  } finally { await provider.close() }
})

test('streamSpeech handles a streamed WAV body', async () => {
  const src = signal(12000)
  const wav = pcm16ToWav(src, 24000)
  const provider = await mockProvider((req, res) => {
    res.writeHead(200, { 'content-type': 'audio/wav' })
    let offset = 0
    const timer = setInterval(() => {
      if (offset >= wav.length) { clearInterval(timer); res.end(); return }
      res.write(wav.subarray(offset, offset + 300))
      offset += 300
    }, 2)
  })
  try {
    const pieces = []
    await streamSpeech({
      url: `${provider.url}/audio/speech`, apiKey: 'test', model: 'test', voice: 'alloy',
      text: '你好', format: 'wav', onPcm: b => pieces.push(b),
    })
    assert.deepEqual(Buffer.concat(pieces), resamplePcm16(src, 24000, 16000))
  } finally { await provider.close() }
})