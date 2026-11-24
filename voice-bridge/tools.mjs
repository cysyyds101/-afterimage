/* Realtime internet tools for the voice bridge (Function Calling).
 * All three are key-free: DuckDuckGo HTML search, Open-Meteo weather, local clock.
 * Results are plain Chinese text sized for TTS, never raw HTML or secrets.
 */

const UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36'

export const TOOLS = [
  {
    type: 'function',
    function: {
      name: 'search_web',
      description: '实时搜索互联网，查询最新新闻、百科与事实信息。当用户询问时效性内容或你不确定的事实时调用。',
      parameters: { type: 'object', properties: { query: { type: 'string', description: '搜索关键词' } }, required: ['query'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_weather',
      description: '查询任意城市的实时天气与未来三天预报（温度、天气状况、湿度、风速）。',
      parameters: { type: 'object', properties: { city: { type: 'string', description: '城市名，如 北京 / Shanghai' } }, required: ['city'] },
    },
  },
  {
    type: 'function',
    function: {
      name: 'get_current_time',
      description: '返回当前精确日期、星期与时间（北京时间）。当用户询问现在几点/今天日期/星期几时调用。',
      parameters: { type: 'object', properties: {} },
    },
  },
]

function timeoutFetch(url, options = {}, ms = 15000) {
  return fetch(url, { ...options, signal: AbortSignal.timeout(ms), redirect: 'follow' })
}

function decodeEntities(text) {
  return text
    .replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"').replace(/&#x27;/g, "'").replace(/&#39;/g, "'")
    .replace(/&nbsp;/g, ' ').replace(/&ensp;/g, ' ').replace(/&emsp;/g, ' ')
    .replace(/&hellip;/g, '…').replace(/&mdash;/g, '—').replace(/&middot;/g, '·')
    .replace(/&#(\d+);/g, (_, n) => String.fromCodePoint(Number(n)))
}

function stripTags(html) {
  return decodeEntities(html.replace(/<[^>]*>/g, '')).replace(/\s+/g, ' ').trim()
}

export function getCurrentTime() {
  const now = new Date()
  const parts = new Intl.DateTimeFormat('zh-CN', {
    timeZone: 'Asia/Shanghai', year: 'numeric', month: 'numeric', day: 'numeric',
    weekday: 'long', hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false,
  }).formatToParts(now)
  const get = type => parts.find(p => p.type === type)?.value || ''
  return `现在是北京时间 ${get('year')}年${get('month')}月${get('day')}日 ${get('weekday')} ${get('hour')}:${get('minute')}:${get('second')}。`
}

const WMO = {
  0: '晴', 1: '大致晴朗', 2: '多云', 3: '阴天', 45: '雾', 48: '冻雾',
  51: '小毛毛雨', 53: '毛毛雨', 55: '大毛毛雨', 56: '冻毛毛雨', 57: '强冻毛毛雨',
  61: '小雨', 63: '中雨', 65: '大雨', 66: '冻雨', 67: '强冻雨',
  71: '小雪', 73: '中雪', 75: '大雪', 77: '雪粒',
  80: '小阵雨', 81: '阵雨', 82: '强阵雨', 85: '小阵雪', 86: '大阵雪',
  95: '雷阵雨', 96: '雷阵雨伴冰雹', 99: '强雷阵雨伴冰雹',
}
const wmo = code => WMO[code] ?? `天气代码${code}`

export async function fetchWeather(city) {
  const name = String(city || '').trim()
  if (!name) return { ok: false, message: '请告诉我要查询的城市名。' }
  const geoUrl = `https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(name)}&count=1&language=zh&format=json`
  const geoRes = await timeoutFetch(geoUrl, { headers: { 'User-Agent': UA } })
  if (!geoRes.ok) return { ok: false, message: `天气服务暂时不可用（HTTP ${geoRes.status}）。` }
  const geo = await geoRes.json()
  const place = geo.results?.[0]
  if (!place) return { ok: false, message: `没有找到城市「${name}」，请换一个更具体的城市名。` }
  const label = [place.name, place.admin1, place.country].filter(Boolean).join('，')
  const forecastUrl = `https://api.open-meteo.com/v1/forecast?latitude=${place.latitude}&longitude=${place.longitude}` +
    '&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m' +
    '&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=4'
  const res = await timeoutFetch(forecastUrl, { headers: { 'User-Agent': UA } })
  if (!res.ok) return { ok: false, message: `天气服务暂时不可用（HTTP ${res.status}）。` }
  const data = await res.json()
  const cur = data.current || {}
  const daily = data.daily || {}
  const days = (daily.time || []).slice(1, 4).map((d, i) => {
    const idx = i + 1
    return { date: d, code: daily.weather_code[idx], text: wmo(daily.weather_code[idx]),
             min: Math.round(daily.temperature_2m_min[idx]), max: Math.round(daily.temperature_2m_max[idx]) }
  })
  return {
    ok: true, city: place.name, label,
    temp: Math.round(cur.temperature_2m), feels: Math.round(cur.apparent_temperature),
    humidity: cur.relative_humidity_2m, wind: Math.round(cur.wind_speed_10m),
    code: cur.weather_code, text: wmo(cur.weather_code), daily: days,
  }
}

export function formatWeather(data) {
  if (!data || !data.ok) return data?.message || '天气查询失败。'
  let text = `${data.label}：当前${data.text}，气温 ${data.temp}°C（体感 ${data.feels}°C），湿度 ${data.humidity}%，风速 ${data.wind} km/h。`
  if (data.daily?.length) {
    const forecast = data.daily.map(d => `${d.date.slice(5).replace('-', '月')}日${d.text} ${d.min}~${d.max}°C`).join('；')
    text += `未来三天：${forecast}。`
  }
  return text
}

export async function getWeather(city) {
  return formatWeather(await fetchWeather(city))
}

function parseDuck(html, limit) {
  const out = []
  const linkRe = /<a[^>]*class="result__a"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g
  const snipRe = /<a[^>]*class="result__snippet"[^>]*>([\s\S]*?)<\/a>/g
  const links = []
  let m
  while ((m = linkRe.exec(html)) && links.length < limit) links.push({ href: m[1], title: stripTags(m[2]) })
  const snippets = []
  while ((m = snipRe.exec(html)) && snippets.length < limit) snippets.push(stripTags(m[1]))
  for (let i = 0; i < links.length; i++) {
    let href = links[i].href
    const uddg = href.match(/uddg=([^&]+)/)
    if (uddg) { try { href = decodeURIComponent(uddg[1]) } catch {} }
    let host = ''
    try { host = new URL(href).hostname } catch {}
    const snippet = (snippets[i] || '').slice(0, 180)
    out.push(`${i + 1}. ${links[i].title}${snippet ? ' — ' + snippet : ''}${host ? `（${host}）` : ''}`)
  }
  return out
}

function parseDuckLite(html, limit) {
  const rows = []
  const re = /<a[^>]*rel="nofollow"[^>]*href="([^"]+)"[^>]*>([\s\S]*?)<\/a>/g
  let m
  while ((m = re.exec(html)) && rows.length < limit) {
    const title = stripTags(m[2])
    if (!title) continue
    let host = ''
    try { host = new URL(m[1]).hostname } catch {}
    rows.push(`${rows.length + 1}. ${title}${host ? `（${host}）` : ''}`)
  }
  return rows
}

function parseBing(html, limit) {
  const heads = []
  const headRe = /<h2><a[^>]*href="([^"]+)"[^>]*>([\s\S]*?)<\/a><\/h2>/g
  let m
  while ((m = headRe.exec(html)) && heads.length < limit) heads.push({ href: m[1], title: stripTags(m[2]) })
  const snippets = []
  const snipRe = /<p class="b_lineclamp\d?"[^>]*>([\s\S]*?)<\/p>/g
  while ((m = snipRe.exec(html)) && snippets.length < limit) snippets.push(stripTags(m[1]))
  return heads.map((h, i) => {
    let host = ''
    try { host = new URL(h.href).hostname } catch {}
    const snippet = (snippets[i] || '').slice(0, 180)
    return `${i + 1}. ${h.title}${snippet ? ' — ' + snippet : ''}${host ? `（${host}）` : ''}`
  })
}

export async function searchWeb(query) {
  const q = String(query || '').trim()
  if (!q) return '请告诉我要搜索的关键词。'
  const enc = encodeURIComponent(q)
  // DuckDuckGo first (key-free, per spec); Bing is the fallback for networks
  // where DuckDuckGo/SearXNG are unreachable. All are key-free HTML endpoints.
  const sources = [
    { url: `https://html.duckduckgo.com/html/?q=${enc}`, parse: h => parseDuck(h, 5) },
    { url: `https://lite.duckduckgo.com/lite/?q=${enc}`, parse: h => parseDuckLite(h, 5) },
    { url: `https://www.bing.com/search?q=${enc}&setlang=zh-hans`, parse: h => parseBing(h, 5) },
  ]
  for (const { url, parse } of sources) {
    try {
      const res = await timeoutFetch(url, { headers: { 'User-Agent': UA, Accept: 'text/html' } }, 15000)
      if (!res.ok) continue
      const results = parse(await res.text())
      if (results.length) return `关于「${q}」的实时搜索结果：\n${results.join('\n')}`
    } catch {}
  }
  return `暂时无法联网搜索「${q}」（搜索服务不可达或被限流），请稍后再试或换个关键词。`
}

export async function executeTool(name, argsJson) {
  let args = {}
  try { args = argsJson ? JSON.parse(argsJson) : {} } catch {}
  try {
    if (name === 'get_current_time') return getCurrentTime()
    if (name === 'get_weather') return await getWeather(args.city)
    if (name === 'search_web') return await searchWeb(args.query)
    return `未知工具：${name}`
  } catch (error) {
    return `工具 ${name} 调用失败：${error instanceof Error ? error.message : '未知错误'}`
  }
}