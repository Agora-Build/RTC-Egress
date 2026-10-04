const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const { test } = require('node:test');

const root = path.resolve(__dirname, '../..');
const AsyncFunction = Object.getPrototypeOf(async function () {}).constructor;

function stepBlock(workflow, step, field) {
  const lines = workflow.split('\n');
  const start = lines.findIndex((line) => line.trim() === `- name: ${step}`);
  assert.ok(start >= 0, `Missing step: ${step}`);
  const fieldIndex = lines.findIndex((line, index) => index > start && line.trim() === `${field}: |`);
  assert.ok(fieldIndex >= 0, `Missing ${field} block for ${step}`);
  const indent = lines[fieldIndex].match(/^ */)[0].length + 2;
  const block = [];
  for (const line of lines.slice(fieldIndex + 1)) {
    if (line.trim() && !line.startsWith(' '.repeat(indent))) break;
    block.push(line.slice(indent));
  }
  return block.join('\n');
}

for (const name of ['codex', 'claude']) {
  const workflow = fs.readFileSync(path.join(root, `.github/workflows/${name}-code-review.yml`), 'utf8');
  const authorize = new AsyncFunction('github', 'context', 'core', stepBlock(workflow, 'Check PR author access', 'script'));

  async function checkAccess({ permission = 'write', fork = false, error } = {}) {
    const outputs = {};
    let calls = 0;
    const context = {
      repo: { owner: 'Agora-Build', repo: 'RTC-Egress' },
      payload: { pull_request: { user: { login: 'review-author' }, head: { repo: { id: fork ? 2 : 1 } }, base: { repo: { id: 1 } } } },
    };
    const github = { rest: { repos: { getCollaboratorPermissionLevel: async (input) => {
      calls++;
      assert.deepEqual(input, { ...context.repo, username: 'review-author' });
      if (error) throw error;
      return { data: { permission } };
    } } } };
    const core = { setOutput: (key, value) => { outputs[key] = value; }, info: () => {} };
    await authorize(github, context, core);
    return { trusted: outputs.trusted, calls };
  }

  for (const permission of ['admin', 'maintain', 'write', 'read', 'triage', 'none']) {
    test(`${name} author with ${permission} permission is gated correctly`, async () => {
      assert.deepEqual(await checkAccess({ permission }), {
        trusted: String(['admin', 'maintain', 'write'].includes(permission)), calls: 1,
      });
    });
  }

  test(`${name} fork PR never requests provider authorization`, async () => {
    assert.deepEqual(await checkAccess({ fork: true }), { trusted: 'false', calls: 0 });
  });

  test(`${name} non-collaborator or bot returning 404 is skipped`, async () => {
    assert.deepEqual(await checkAccess({ error: { status: 404 } }), { trusted: 'false', calls: 1 });
  });

  test(`${name} GitHub permission API failure stops authorization`, async () => {
    await assert.rejects(checkAccess({ error: new Error('GitHub unavailable') }), /GitHub unavailable/);
  });

  test(`${name} diff prompt uses a literal event SHA, without a shell variable`, () => {
    const prompt = stepBlock(workflow, `Run ${name === 'codex' ? 'Codex' : 'Claude'} Code Review`, 'prompt');
    assert.ok(prompt.includes('git diff ${{ github.event.pull_request.base.sha }}...HEAD'));
    assert.ok(!prompt.includes('$PR_BASE_SHA'));
  });
}

const codex = fs.readFileSync(path.join(root, '.github/workflows/codex-code-review.yml'), 'utf8');
const configure = stepBlock(codex, 'Check provider configuration', 'run');
for (const base of ['https://provider.example/openai', 'https://provider.example/openai/v1/']) {
  test(`Codex workflow configuration normalizes ${base}`, (t) => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'review-config-'));
    t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
    const output = path.join(directory, 'output');
    const result = spawnSync('bash', ['-e', '-c', configure], {
      cwd: root, encoding: 'utf8',
      env: { ...process.env, OPENAI_API_KEY: 'test-review-key', OPENAI_BASE_URL: base, GITHUB_OUTPUT: output },
    });
    assert.equal(result.status, 0, result.stderr);
    assert.equal(fs.readFileSync(output, 'utf8'), 'responses-endpoint=https://provider.example/openai/v1/responses\n');
    assert.ok(codex.includes('responses-api-endpoint: ${{ steps.provider.outputs.responses-endpoint }}'));
  });
}

test('Codex workflow fails visibly if the provider URL is missing', () => {
  const result = spawnSync('bash', ['-e', '-c', configure], {
    cwd: root, encoding: 'utf8', env: { ...process.env, OPENAI_API_KEY: 'test-review-key', OPENAI_BASE_URL: '' },
  });
  assert.equal(result.status, 1);
  assert.match(result.stdout, /Missing OPENAI_BASE_URL secret/);
});
