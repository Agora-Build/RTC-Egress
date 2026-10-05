const assert = require('node:assert/strict');
const { test } = require('node:test');
const { responsesEndpoint } = require('./review-config.cjs');

for (const [base, expected] of [
  ['https://provider.example', 'https://provider.example/v1/responses'],
  ['https://provider.example/', 'https://provider.example/v1/responses'],
  ['https://provider.example/v1', 'https://provider.example/v1/responses'],
  ['https://provider.example/v1/', 'https://provider.example/v1/responses'],
  ['https://provider.example/openai', 'https://provider.example/openai/v1/responses'],
  ['https://provider.example/openai/v1/', 'https://provider.example/openai/v1/responses'],
  ['  https://provider.example/openai/  ', 'https://provider.example/openai/v1/responses'],
]) {
  test(`provider base ${base} produces the correct Responses endpoint`, () => {
    assert.equal(responsesEndpoint(base), expected);
  });
}

test('missing or invalid provider URL fails without revealing its value', () => {
  for (const base of ['', 'invalid', 'http://provider.example', 'https://user:password@provider.example',
    'https://provider.example?token=private', 'https://provider.example/#fragment',
    'https://provider.example/v1/responses']) {
    assert.throws(() => responsesEndpoint(base), (error) => {
      assert.match(error.message, /OPENAI_BASE_URL/);
      assert.ok(!error.message.includes('password') && !error.message.includes('private'));
      return true;
    });
  }
});
