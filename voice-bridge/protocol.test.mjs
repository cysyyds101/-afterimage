import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const memDir = fs.mkdtempSync(path.join(os.tmpdir(), 'qiban-mem-'))
import { spawn } from 'node:child_process'
import test from 'node:test'
import { WebSocket } from 'ws'

test('authenticated echo turn streams state and PCM back', { timeout: 15000 }, async () => {
  const port = 18780
  const token = 'integration-test-token'
  const child = spawn(process.execPath, ['server.mjs'], {
    cwd: import.meta.dirname,
    env: { ...process.env, MEMORY_DIR: memDir, PORT: String(port), DEVICE_TOKEN: token, MOCK_MODE: 'echo' },
    stdio: ['ignore', 'ignore', 'pipe'],
  })
  try {
    for (let attempt = 0; attempt < 50; attempt++) {
      try {
        const response = await fetch(`http://127.0.0.1:${port}/health`)
        if (response.ok) break
      } catch {}
      await new Promise(resolve => setTimeout(resolve, 50))
    }
    const ws = new WebSocket(`ws://127.0.0.1:${port}/voice`)
    const input = Buffer.alloc(6400)
    for (let i = 0; i < input.length / 2; i++) input.writeInt16LE((i * 37) % 30000, i * 2)
    const states = []
    const audio = []
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('WebSocket test timed out')), 10000)
      ws.on('open', () => ws.send(JSON.stringify({ type: 'hello', token, sampleRate: 16000, format: 'pcm_s16le' })))
      ws.on('message', (data, binary) => {
        if (binary) { audio.push(Buffer.from(data)); return }
        const message = JSON.parse(data.toString())
        if (message.type !== 'state') return
        states.push(message.state)
        if (message.state === 'ready') {
          ws.send(JSON.stringify({ type: 'start' }))
          ws.send(input)
          ws.send(JSON.stringify({ type: 'audio_end' }))
        } else if (message.state === 'idle') {
          clearTimeout(timer)
          ws.close()
          resolve()
        }
      })
      ws.on('error', reject)
    })
    assert.deepEqual(states, ['ready', 'listening', 'thinking', 'speaking', 'idle'])
    assert.deepEqual(Buffer.concat(audio), input)
  } finally {
    child.kill()
  }
})
