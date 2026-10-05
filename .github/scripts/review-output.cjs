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
    if (!result) {
      throw new Error('Claude review result is missing; execution may be incomplete.');
    }
    if (result.is_error || (result.subtype && result.subtype !== 'success')) {
      throw new Error('Claude review execution failed.');
    }
    review = result.result;
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

async function postReview({ github, context }, { reviewer, headSha, mergeSha, review }) {
  const text = parseReview(review, 'markdown');
  const body = `## \u{1F916} ${reviewer} Code Review\n\nPR head: \`${headSha}\`\nReviewed merge: \`${mergeSha}\`\n\n${text}`;
  if ([...body].length > 65536) {
    throw new Error('Review output is too long for a GitHub comment.');
  }
  await github.rest.issues.createComment({
    ...context.repo,
    issue_number: context.payload.pull_request.number,
    body,
  });
}

module.exports = { parseReview, readReview, postReview };

if (require.main === module) {
  const [format, input, output] = process.argv.slice(2);
  fs.writeFileSync(output, parseReview(fs.readFileSync(input, 'utf8'), format) + '\n');
}
