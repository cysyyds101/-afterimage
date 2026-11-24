import assert from 'node:assert/strict'
import test from 'node:test'
import { pcm16ToWav, resamplePcm16, wavToPcm16 } from './audio.mjs'

test('PCM round-trips through WAV', () => {
  const pcm = Buffer.alloc(320)
  for (let i = 0; i < pcm.length / 2; i++) pcm.writeInt16LE((i * 101) % 32767, i * 2)
  const decoded = wavToPcm16(pcm16ToWav(pcm, 16000))
  assert.equal(decoded.sampleRate, 16000)
  assert.deepEqual(decoded.pcm, pcm)
})

test('24 kHz PCM resamples to 16 kHz with expected duration', () => {
  const pcm = Buffer.alloc(24000 * 2)
  const result = resamplePcm16(pcm, 24000, 16000)
  assert.equal(result.length, 16000 * 2)
})

test('invalid WAV is rejected', () => {
  assert.throws(() => wavToPcm16(Buffer.from('not a wave')), /valid WAV/)
})
