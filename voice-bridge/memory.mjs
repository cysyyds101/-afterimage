/* Single-user lightweight long-term memory for the voice bridge.
 *
 * Three JSON files under data/memory/ survive restarts:
 *   recent_chat.json  last 8 turns (16 messages), seeded back into the session
 *   profile.json      <=250-char overall impression of the user
 *   episodes.json     <=50 fuzzy episode summaries {summary,tags,importance,timestamp}
 *
 * Recall is pure in-memory scoring (keyword overlap + time decay + importance),
 * so it adds no measurable latency before a reply. Consolidation runs ONLY
 * after TTS delivery, in the background (4 accumulated turns or 60 s idle),
 * and any malformed model output is skipped without touching the voice path.
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { streamChat } from './stream.mjs'

const MAX_RECENT_MESSAGES = 16
const MAX_PROFILE_CHARS = 250
const MAX_EPISODES = 50
const IDLE_CONSOLIDATE_MS = 60000
const BATCH_TURNS = 4
const DECAY_HALF_LIFE_DAYS = 14

const dir = process.env.MEMORY_DIR ||
  path.join(path.dirname(fileURLToPath(import.meta.url)), 'data', 'memory')
const files = {
  recent: path.join(dir, 'recent_chat.json'),
  profile: path.join(dir, 'profile.json'),
  episodes: path.join(dir, 'episodes.json'),
}

let recent = []
let profile = { text: '', updatedAt: 0 }
let episodes = []

function loadJson(file, fallback) {
  try { return JSON.parse(fs.readFileSync(file, 'utf8')) } catch { return fallback }
}

function saveJson(file, value) {
  const tmp = `${file}.tmp`
  fs.writeFileSync(tmp, JSON.stringify(value, null, 2), 'utf8')
  fs.renameSync(tmp, file)
}

const chars = text => Array.from(String(text))
const clampChars = (text, max) => chars(text).slice(0, max).join('')

export function initMemory() {
  fs.mkdirSync(dir, { recursive: true })
  const rawRecent = loadJson(files.recent, [])
  recent = Array.isArray(rawRecent)
    ? rawRecent.filter(m => m && (m.role === 'user' || m.role === 'assistant') && typeof m.content === 'string').slice(-MAX_RECENT_MESSAGES)
    : []
  const rawProfile = loadJson(files.profile, {})
  profile = {
    text: clampChars(typeof rawProfile?.text === 'string' ? rawProfile.text : '', MAX_PROFILE_CHARS),
    updatedAt: Number(rawProfile?.updatedAt) || 0,
  }
  const rawEpisodes = loadJson(files.episodes, [])
  episodes = Array.isArray(rawEpisodes) ? rawEpisodes.filter(validEpisode).slice(-MAX_EPISODES) : []
  console.log(`[Memory] 已恢复：短期对话 ${Math.floor(recent.length / 2)} 轮，人物印象 ${chars(profile.text).length} 字，经历 ${episodes.length} 条 (data/memory)`)
}

function validEpisode(e) {
  return e && typeof e.summary === 'string' && e.summary.trim() &&
    Array.isArray(e.tags) && Number.isFinite(Number(e.timestamp))
}

/** Copy of the persisted short-term chat, for seeding a new device session. */
export function recentMessages() {
  return recent.map(m => ({ role: m.role, content: m.content }))
}

/** CJK character bigrams + latin words; no segmenter or embedding needed. */
function terms(text) {
  const out = new Set()
  const lower = String(text).toLowerCase()
  for (const m of lower.matchAll(/[a-z0-9]{2,}/g)) out.add(m[0])
  for (const run of lower.match(/[\u4e00-\u9fff]+/g) || []) {
    if (run.length === 1) out.add(run)
    for (let i = 0; i + 2 <= run.length; i++) out.add(run.slice(i, i + 2))
  }
  return out
}

/** Keyword overlap + exponential time decay + importance. Pure for tests. */
export function scoreEpisodes(list, query, now = Date.now()) {
  const queryTerms = terms(query)
  const lowerQuery = String(query).toLowerCase()
  return list.map(episode => {
    const tags = episode.tags.filter(t => typeof t === 'string')
    const epTerms = terms(`${episode.summary} ${tags.join(' ')}`)
    let overlap = 0
    for (const t of queryTerms) if (epTerms.has(t)) overlap++
    let tagHits = 0
    for (const tag of tags) if (tag.length >= 2 && lowerQuery.includes(tag.toLowerCase())) tagHits++
    const match = overlap + tagHits * 2
    const ageDays = Math.max(0, (now - Number(episode.timestamp)) / 86400000)
    const recency = Math.pow(0.5, ageDays / DECAY_HALF_LIFE_DAYS)
    const importance = Math.max(1, Math.min(5, Number(episode.importance) || 3))
    return { episode, match, score: match * 3 + importance * 0.6 + recency }
  })
}

/** Millisecond-level recall: profile + top 2~3 episodes as a prompt suffix. */
export function recallFor(query) {
  const scored = scoreEpisodes(episodes, query).sort((a, b) => b.score - a.score)
  const matched = scored.filter(s => s.match > 0).slice(0, 3)
  const picked = [...matched]
  for (const s of scored) {
    if (picked.length >= 2) break
    if (!picked.includes(s)) picked.push(s)
  }
  console.log(`[Memory] 已加载人物印象 + ${picked.length} 条相关经历`)
  let prompt = ''
  if (profile.text) prompt += `\n\n【对用户的长期印象（供参考，自然地体现，不要生硬复述）】\n${profile.text}`
  if (picked.length) {
    prompt += '\n\n【模糊经历回忆（可能相关的往事）】'
    for (const { episode } of picked) {
      const days = Math.floor((Date.now() - Number(episode.timestamp)) / 86400000)
      const when = days <= 0 ? '今天' : `${days}天前`
      prompt += `\n- ${when}：${episode.summary}`
    }
  }
  return { prompt, episodeCount: picked.length }
}

/** Persist one finished turn and queue it for background consolidation. */
export function recordTurn(userText, assistantText) {
  recent.push({ role: 'user', content: userText }, { role: 'assistant', content: assistantText })
  recent = recent.slice(-MAX_RECENT_MESSAGES)
  try { saveJson(files.recent, recent) } catch (error) {
    console.warn('[Memory] recent_chat 落盘失败:', error instanceof Error ? error.message : error)
  }
  pending.push({ user: userText, assistant: assistantText })
}

const GREETING = /^(你好|您好|哈喽|嗨|hi|hello|在吗|在不在|喂|测试|嗯+|哦+|好的?|谢谢|多谢|再见|拜拜)[呀啊哦哈呢吧~！!。？?，,\s.]*$/i

/** Tolerant parser for the consolidator output. Returns null on garbage. */
export function parseConsolidation(text) {
  if (typeof text !== 'string') return null
  const body = text.trim().replace(/^```(?:json)?/i, '').replace(/```\s*$/, '').trim()
  const start = body.indexOf('{')
  const end = body.lastIndexOf('}')
  if (start < 0 || end <= start) return null
  let data
  try { data = JSON.parse(body.slice(start, end + 1)) } catch { return null }
  if (!data || typeof data !== 'object') return null
  if (data.meaningful !== true) return { meaningful: false }
  if (typeof data.profile !== 'string' || !data.profile.trim()) return null
  const ep = data.episode
  if (!ep || typeof ep !== 'object' || typeof ep.summary !== 'string' || !ep.summary.trim()) return null
  const tags = (Array.isArray(ep.tags) ? ep.tags : [])
    .filter(t => typeof t === 'string' && t.trim())
    .map(t => clampChars(t.trim(), 10)).slice(0, 4)
  const importance = Math.max(1, Math.min(5, Math.round(Number(ep.importance) || 3)))
  return {
    meaningful: true,
    profile: clampChars(data.profile.trim(), MAX_PROFILE_CHARS),
    episode: { summary: clampChars(ep.summary.trim(), 30), tags, importance },
  }
}

/** Drop the least important, then oldest, memories beyond the cap. */
function evictEpisodes() {
  while (episodes.length > MAX_EPISODES) {
    let worst = 0
    for (let i = 1; i < episodes.length; i++) {
      const a = episodes[i], b = episodes[worst]
      const ia = Number(a.importance) || 3, ib = Number(b.importance) || 3
      if (ia < ib || (ia === ib && Number(a.timestamp) < Number(b.timestamp))) worst = i
    }
    episodes.splice(worst, 1)
  }
}

let pending = []
let lastChat = null
let idleTimer = null
let running = false
let again = false

/** Called after a turn's TTS has been fully delivered — never before. */
export function noteTurnComplete(chat) {
  lastChat = chat
  if (idleTimer) clearTimeout(idleTimer)
  if (pending.length >= BATCH_TURNS) { void consolidate(); return }
  idleTimer = setTimeout(() => void consolidate(), IDLE_CONSOLIDATE_MS)
}

async function consolidate() {
  if (running) { again = true; return }
  if (!pending.length || !lastChat) return
  running = true
  try { await consolidateOnce() } catch (error) {
    console.warn('[Memory] 后台巩固失败，已跳过（不影响语音聊天）:', error instanceof Error ? error.message : error)
  } finally {
    running = false
    if (again) { again = false; void consolidate() }
  }
}

async function consolidateOnce() {
  const turns = pending
  pending = []
  if (turns.every(t => GREETING.test(t.user.trim()))) {
    console.log('[Memory] 本批均为寒暄，跳过长期记忆')
    return
  }
  const chat = lastChat
  const dialogue = turns.map(t => `用户：${t.user}\n助手：${t.assistant}`).join('\n')
  const system = '你是语音助手的记忆巩固器。判断对话是否含有值得长期记住的用户信息（称呼、性格习惯、喜好、最近在忙的事、重要事件）。' +
    '纯寒暄（你好/在吗/测试等）没有可记内容。只输出一个 JSON 对象，不要输出任何其它文字：\n' +
    '{"meaningful":true,"profile":"...","episode":{"summary":"...","tags":["...","..."],"importance":3}}\n' +
    '要求：profile 是对用户的整体印象，合并旧印象与新信息，冲突时以新信息为准，总字数不超过250字；' +
    'episode.summary 是不超过30字的一句话概括；tags 为2~4个关键词；importance 为1~5的整数。' +
    '若没有值得记的内容，只输出 {"meaningful":false}。'
  const reply = await streamChat({
    url: `${String(chat.baseUrl).replace(/\/$/, '')}/chat/completions`,
    apiKey: chat.apiKey,
    model: chat.model,
    messages: [
      { role: 'system', content: system },
      { role: 'user', content: `当前人物印象：${profile.text || '（空）'}\n\n本批对话：\n${dialogue}` },
    ],
    onSentence: () => {},
    temperature: 0.2,
    signal: AbortSignal.timeout(120000),
    extraBody: process.env.LLM_REASONING_EFFORT ? { reasoning_effort: process.env.LLM_REASONING_EFFORT } : null,
  })
  const result = parseConsolidation(reply)
  if (!result) {
    console.warn('[Memory] 巩固输出不是合法 JSON，已安全跳过')
    return
  }
  if (!result.meaningful) {
    console.log('[Memory] 模型判定本批无长期价值，跳过写入')
    return
  }
  profile = { text: result.profile, updatedAt: Date.now() }
  episodes.push({ ...result.episode, timestamp: Date.now() })
  evictEpisodes()
  try {
    saveJson(files.profile, profile)
    saveJson(files.episodes, episodes)
  } catch (error) {
    console.warn('[Memory] 落盘失败:', error instanceof Error ? error.message : error)
    return
  }
  console.log(`[Memory Updated] 人物印象 ${chars(profile.text).length} 字；新增经历「${result.episode.summary}」` +
    `(标签:${result.episode.tags.join('/')} 重要度:${result.episode.importance})；经历总数 ${episodes.length}`)
}