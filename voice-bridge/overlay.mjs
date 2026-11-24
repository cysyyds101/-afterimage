/* Dynamic display-overlay generation for the voice bridge.
 *
 * When a realtime tool (e.g. get_weather) produces structured data, render a
 * screen-sized card with Pillow (Chinese-capable) and expose it over LAN HTTP
 * at /overlay/latest.jpg so the board can pull and display it mid-conversation.
 * Rendering is fire-and-forget so it never adds latency to the voice reply.
 */
import { spawn } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const overlayDir = path.join(here, '.overlay')
const scriptPath = path.join(here, 'overlay', 'render_card.py')
const latestPath = path.join(overlayDir, 'latest.jpg')
fs.mkdirSync(overlayDir, { recursive: true })

const pythonBin = process.env.OVERLAY_PYTHON || 'D:/work/whale-maid/speech-service/.venv/Scripts/python.exe'
const baseUrl = (process.env.OVERLAY_BASE_URL || 'http://10.1.41.11:8780').replace(/\/$/, '')

/** Path of the most recent rendered card, or null if none exists yet. */
export function latestOverlayPath() {
  return fs.existsSync(latestPath) ? latestPath : null
}

/** Render a weather card from structured fetchWeather() data.
 * Resolves to the LAN download URL on success, null on failure. */
export function renderWeatherCard(data) {
  return new Promise(resolve => {
    const tmpPath = path.join(overlayDir, `render-${Date.now()}-${process.pid}.jpg`)
    let child
    try {
      child = spawn(pythonBin, [scriptPath, tmpPath], { stdio: ['pipe', 'ignore', 'pipe'] })
    } catch (error) {
      console.warn('Overlay renderer spawn failed:', error instanceof Error ? error.message : error)
      resolve(null)
      return
    }
    let stderr = ''
    child.stderr.on('data', chunk => { stderr += chunk.toString() })
    child.on('error', error => {
      console.warn('Overlay renderer error:', error instanceof Error ? error.message : error)
      resolve(null)
    })
    child.on('close', code => {
      if (code !== 0) {
        console.warn(`Overlay renderer exited ${code}: ${stderr.slice(0, 300)}`)
        try { fs.rmSync(tmpPath, { force: true }) } catch {}
        resolve(null)
        return
      }
      try {
        // Atomic swap so a concurrent board download never sees a partial file.
        fs.renameSync(tmpPath, latestPath)
        resolve(`${baseUrl}/overlay/latest.jpg`)
      } catch (error) {
        console.warn('Overlay publish failed:', error instanceof Error ? error.message : error)
        resolve(null)
      }
    })
    child.stdin.on('error', () => {})
    child.stdin.end(JSON.stringify(data))
  })
}