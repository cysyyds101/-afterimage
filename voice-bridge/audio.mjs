export function pcm16ToWav(pcm, sampleRate = 16000, channels = 1) {
  const data = Buffer.from(pcm)
  const out = Buffer.alloc(44 + data.length)
  out.write('RIFF', 0)
  out.writeUInt32LE(36 + data.length, 4)
  out.write('WAVEfmt ', 8)
  out.writeUInt32LE(16, 16)
  out.writeUInt16LE(1, 20)
  out.writeUInt16LE(channels, 22)
  out.writeUInt32LE(sampleRate, 24)
  out.writeUInt32LE(sampleRate * channels * 2, 28)
  out.writeUInt16LE(channels * 2, 32)
  out.writeUInt16LE(16, 34)
  out.write('data', 36)
  out.writeUInt32LE(data.length, 40)
  data.copy(out, 44)
  return out
}

export function wavToPcm16(wav) {
  const input = Buffer.from(wav)
  if (input.length < 44 || input.toString('ascii', 0, 4) !== 'RIFF' || input.toString('ascii', 8, 12) !== 'WAVE')
    throw new Error('TTS did not return a valid WAV file')
  let offset = 12
  let format
  let pcm
  while (offset + 8 <= input.length) {
    const id = input.toString('ascii', offset, offset + 4)
    const size = input.readUInt32LE(offset + 4)
    const start = offset + 8
    if (start + size > input.length) throw new Error('Truncated WAV chunk')
    if (id === 'fmt ') {
      format = {
        encoding: input.readUInt16LE(start),
        channels: input.readUInt16LE(start + 2),
        sampleRate: input.readUInt32LE(start + 4),
        bits: input.readUInt16LE(start + 14),
      }
    } else if (id === 'data') pcm = input.subarray(start, start + size)
    offset = start + size + (size & 1)
  }
  if (!format || !pcm || format.encoding !== 1 || format.bits !== 16 || format.channels !== 1)
    throw new Error('Only mono 16-bit PCM WAV is supported')
  return { pcm: Buffer.from(pcm), sampleRate: format.sampleRate }
}

export function resamplePcm16(input, sourceRate, targetRate = 16000) {
  const source = Buffer.from(input)
  if (sourceRate === targetRate) return source
  if (!sourceRate || !targetRate || source.length % 2) throw new Error('Invalid PCM or sample rate')
  const sourceSamples = source.length / 2
  const targetSamples = Math.max(1, Math.round(sourceSamples * targetRate / sourceRate))
  const out = Buffer.alloc(targetSamples * 2)
  for (let i = 0; i < targetSamples; i++) {
    const position = i * sourceRate / targetRate
    const left = Math.min(sourceSamples - 1, Math.floor(position))
    const right = Math.min(sourceSamples - 1, left + 1)
    const mix = position - left
    const value = Math.round(source.readInt16LE(left * 2) * (1 - mix) + source.readInt16LE(right * 2) * mix)
    out.writeInt16LE(Math.max(-32768, Math.min(32767, value)), i * 2)
  }
  return out
}
