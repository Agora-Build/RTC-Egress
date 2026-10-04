const fs = require('node:fs');
const path = require('node:path');

function parseReview(raw, format) {
  let review = raw;
  if (format === 'claude') {
    let data;
    try {
      data = JSON.parse(raw);
    } catch {
      throw new Error('Could not parse Claude execution output as JSON.');
    }
    const items = (Array.isArray(data) ? data : [data]).filter((item) => item && typeof item === 'object');
    const result = [...items].reverse().find((item) => item.type === 'result');
    if (result) {
      if (result.is_error || (result.subtype && result.subtype !== 'success')) {
        throw new Error('Claude review execution failed.');
      }
      review = result.result;
    } else {
      // Older action output may contain assistant messages without a final result.
      const assistant = [...items].reverse().find((item) => item.role === 'assistant' || item.type === 'assistant');
      const content = assistant && (assistant.content ?? assistant.message?.content);
      review = typeof content === 'string' ? content : Array.isArray(content)
        ? content.filter((item) => item.type === 'text').map((item) => item.text || '').join('')
        : '';
    }
  } else if (format !== 'markdown') {
    throw new Error(`Unsupported review format: ${format}`);
  }
  if (typeof review !== 'string' || !review.trim()) {
    throw new Error('Review output was empty; nothing posted.');
  }
  return review.trim();
}

function readReview(directory, format) {
  const files = fs.readdirSync(directory, { withFileTypes: true }).filter((entry) => entry.isFile());
  if (files.length !== 1) {
    throw new Error('Expected exactly one review output artifact file.');
  }
  return parseReview(fs.readFileSync(path.join(directory, files[0].name), 'utf8'), format);
}

async function postReview({ github, context }, { reviewer, headSha, review }) {
  const text = parseReview(review, 'markdown');
  const body = `## \u{1F916} ${reviewer} Code Review\n\nReviewed commit: \`${headSha}\`\n\n${text}`;
  if (body.length > 65536) {
    throw new Error('Review output is too long for a GitHub comment.');
  }
  await github.rest.issues.createComment({
    ...context.repo,
    issue_number: context.payload.pull_request.number,
    body,
  });
}

module.exports = { parseReview, readReview, postReview };
