import test from 'node:test'
import assert from 'node:assert/strict'
import { scoreEpisodes, parseConsolidation } from './memory.mjs'

const now = Date.now()
const day = 86400000

test('keyword match outranks unrelated episodes', () => {
  const episodes = [
    { summary: '用户在准备嵌入式比赛', tags: ['比赛', '嵌入式'], importance: 3, timestamp: now - 20 * day },
    { summary: '用户喜欢喝美式咖啡', tags: ['咖啡', '喜好'], importance: 5, timestamp: now - day },
  ]
  const scored = scoreEpisodes(episodes, '我的嵌入式比赛准备得怎么样了', now).sort((a, b) => b.score - a.score)
  assert.equal(scored[0].episode.summary, '用户在准备嵌入式比赛')
  assert.ok(scored[0].match > 0)
  assert.equal(scored[1].match, 0)
})

test('time decay and importance break ties between equal matches', () => {
  const episodes = [
    { summary: '用户提到项目延期', tags: ['项目'], importance: 3, timestamp: now - 60 * day },
    { summary: '用户提到项目上线', tags: ['项目'], importance: 3, timestamp: now - day },
    { summary: '用户提到项目很关键', tags: ['项目'], importance: 5, timestamp: now - day },
  ]
  const scored = scoreEpisodes(episodes, '项目现在如何', now).sort((a, b) => b.score - a.score)
  assert.equal(scored[0].episode.summary, '用户提到项目很关键')
  assert.equal(scored[2].episode.summary, '用户提到项目延期')
})

test('parseConsolidation accepts clean JSON', () => {
  const result = parseConsolidation('{"meaningful":true,"profile":"用户叫小鲸，喜欢咖啡","episode":{"summary":"聊了比赛准备","tags":["比赛","嵌入式"],"importance":4}}')
  assert.equal(result.meaningful, true)
  assert.equal(result.profile, '用户叫小鲸，喜欢咖啡')
  assert.equal(result.episode.importance, 4)
})

test('parseConsolidation tolerates code fences and surrounding prose', () => {
  const result = parseConsolidation('好的，结果如下：\n```json\n{"meaningful":false}\n```')
  assert.deepEqual(result, { meaningful: false })
})

test('parseConsolidation clamps limits and bad importance', () => {
  const long = '字'.repeat(400)
  const result = parseConsolidation(JSON.stringify({
    meaningful: true, profile: long,
    episode: { summary: '句'.repeat(60), tags: ['a', 'b', 'c', 'd', 'e', 5], importance: 99 },
  }))
  assert.equal(Array.from(result.profile).length, 250)
  assert.equal(Array.from(result.episode.summary).length, 30)
  assert.deepEqual(result.episode.tags, ['a', 'b', 'c', 'd'])
  assert.equal(result.episode.importance, 5)
})

test('parseConsolidation rejects garbage safely', () => {
  for (const bad of [null, '', '不是JSON', '{broken', '{"meaningful":true}', '{"meaningful":true,"profile":"x"}', '{"meaningful":true,"profile":"x","episode":{"summary":""}}'])
    assert.equal(parseConsolidation(bad), null)
})