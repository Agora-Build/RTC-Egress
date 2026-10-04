const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { test } = require('node:test');
const { parseReview, readReview, postReview } = require('./review-output.cjs');

test('Codex markdown preserves code and untrusted text literally', () => {
  const review = 'Found a bug in `sink.cpp`: ${process.env.TOKEN} and $(echo test).';
  assert.equal(parseReview(`\n${review}\n`, 'markdown'), review);
});

test('Claude final result wins over intermediate assistant messages', () => {
  const output = [
    { type: 'assistant', message: { content: [{ type: 'text', text: 'Still reviewing.' }] } },
    { type: 'result', subtype: 'success', is_error: false, result: 'No actionable findings.' },
  ];
  assert.equal(parseReview(JSON.stringify(output), 'claude'), 'No actionable findings.');
});

test('Claude accepts a single result object', () => {
  assert.equal(parseReview(JSON.stringify({ type: 'result', result: 'Review complete.' }), 'claude'), 'Review complete.');
});

test('Claude rejects intermediate assistant text if no result exists', () => {
  const output = [
    { role: 'assistant', content: 'Earlier analysis.' },
    { type: 'assistant', message: { content: [
      { type: 'thinking', thinking: 'Private analysis.' },
      { type: 'text', text: 'A real ' },
      { type: 'tool_use', name: 'Read' },
      { type: 'text', text: 'finding.' },
    ] } },
  ];
  assert.throws(() => parseReview(JSON.stringify(output), 'claude'), /result is missing/);
});

test('Claude rejects an assistant string without a completed result', () => {
  assert.throws(() => parseReview(JSON.stringify({ role: 'assistant', content: 'No issues.' }), 'claude'), /result is missing/);
});

test('failed Claude execution never publishes intermediate text', () => {
  for (const result of [
    { type: 'result', is_error: true, result: 'Authentication failed.' },
    { type: 'result', subtype: 'error_max_turns', result: 'Incomplete review.' },
  ]) {
    assert.throws(() => parseReview(JSON.stringify([
      { role: 'assistant', content: 'Looks good so far.' }, result,
    ]), 'claude'), /execution failed/);
  }
});

test('empty final Claude result does not fall back to an incomplete review', () => {
  assert.throws(() => parseReview(JSON.stringify([
    { role: 'assistant', content: 'Still working.' },
    { type: 'result', subtype: 'success', result: '  ' },
  ]), 'claude'), /empty/);
});

test('missing review text fails for both reviewers', () => {
  for (const [raw, format] of [
    [' \n', 'markdown'], ['[]', 'claude'], ['null', 'claude'],
    [JSON.stringify({ type: 'assistant', message: { content: [{ type: 'tool_use' }] } }), 'claude'],
  ]) {
    assert.throws(() => parseReview(raw, format), /empty|result is missing/);
  }
});

test('malformed Claude output fails without exposing raw output', () => {
  assert.throws(() => parseReview('PRIVATE_OUTPUT invalid json', 'claude'), {
    message: 'Could not parse Claude execution output as JSON.',
  });
});

test('unknown review formats fail', () => {
  assert.throws(() => parseReview('Review.', 'unknown'), /Unsupported review format/);
});

test('artifact reader requires exactly one regular output file', (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'review-output-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  assert.throws(() => readReview(directory, 'markdown'), /exactly one/);
  fs.mkdirSync(path.join(directory, 'ignored-directory'));
  fs.writeFileSync(path.join(directory, 'review.md'), 'No actionable findings.');
  assert.equal(readReview(directory, 'markdown'), 'No actionable findings.');
  fs.writeFileSync(path.join(directory, 'extra.md'), 'Ambiguous output.');
  assert.throws(() => readReview(directory, 'markdown'), /exactly one/);
});

test('missing output artifact fails', () => {
  assert.throws(() => readReview(path.join(os.tmpdir(), 'missing-review-' + process.pid), 'markdown'), /ENOENT/);
});

test('publisher posts the exact review and records both head and reviewed merge commits', async () => {
  let comment;
  const github = { rest: { issues: { createComment: async (input) => { comment = input; } } } };
  const context = { repo: { owner: 'Agora-Build', repo: 'RTC-Egress' }, payload: { pull_request: { number: 6 } } };
  await postReview({ github, context }, { reviewer: 'Codex', headSha: 'abc123', mergeSha: 'def456', review: 'No actionable findings.' });
  assert.deepEqual(comment, {
    owner: 'Agora-Build', repo: 'RTC-Egress', issue_number: 6,
    body: '## \u{1F916} Codex Code Review\n\nPR head: `abc123`\nReviewed merge: `def456`\n\nNo actionable findings.',
  });
});

test('publisher rejects empty or oversized output before calling GitHub', async () => {
  const github = { rest: { issues: { createComment: async () => assert.fail('Must not post') } } };
  const context = { repo: {}, payload: { pull_request: { number: 6 } } };
  for (const review of ['', '  ', 'x'.repeat(65536)]) {
    await assert.rejects(postReview({ github, context }, { reviewer: 'Claude', headSha: 'abc123', review }), /empty|too long/);
  }
});

test('publisher counts Unicode characters rather than UTF-16 code units', async () => {
  let body;
  const github = { rest: { issues: { createComment: async (comment) => { body = comment.body; } } } };
  const context = { repo: {}, payload: { pull_request: { number: 6 } } };
  await postReview({ github, context }, { reviewer: 'Claude', headSha: 'abc123', review: '\u{1F916}'.repeat(60000) });
  assert.ok([...body].length < 65536);
});

test('GitHub publication failures fail the posting job', async () => {
  const github = { rest: { issues: { createComment: async () => { throw new Error('GitHub rejected comment'); } } } };
  const context = { repo: {}, payload: { pull_request: { number: 6 } } };
  await assert.rejects(postReview({ github, context }, { reviewer: 'Claude', headSha: 'abc123', review: 'Review.' }), /GitHub rejected/);
});

test('Claude extraction uploads only the completed review text', (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'review-extract-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const input = path.join(directory, 'execution.json');
  const output = path.join(directory, 'review.md');
  fs.writeFileSync(input, JSON.stringify([
    { type: 'user', message: { content: 'Private tool output.' } },
    { type: 'assistant', message: { content: 'Intermediate analysis.' } },
    { type: 'result', subtype: 'success', result: 'No actionable findings.' },
  ]));
  const result = spawnSync(process.execPath, [path.join(__dirname, 'review-output.cjs'), 'claude', input, output], { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  assert.equal(fs.readFileSync(output, 'utf8'), 'No actionable findings.\n');
});

test('incomplete Claude execution fails extraction without creating an artifact', (t) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'review-extract-'));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  const input = path.join(directory, 'execution.json');
  const output = path.join(directory, 'review.md');
  fs.writeFileSync(input, JSON.stringify([{ type: 'assistant', content: 'Still reviewing.' }]));
  const result = spawnSync(process.execPath, [path.join(__dirname, 'review-output.cjs'), 'claude', input, output], { encoding: 'utf8' });
  assert.notEqual(result.status, 0);
  assert.ok(!fs.existsSync(output));
});
