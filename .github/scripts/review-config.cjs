const fs = require('node:fs');

function responsesEndpoint(baseUrl) {
  let url;
  try {
    url = new URL(baseUrl.trim());
  } catch {
    throw new Error('OPENAI_BASE_URL must be a valid HTTPS provider base URL.');
  }
  if (url.protocol !== 'https:' || url.username || url.password || url.search || url.hash) {
    throw new Error('OPENAI_BASE_URL must be HTTPS without credentials, query parameters, or fragments.');
  }
  const basePath = url.pathname.replace(/\/+$/, '');
  if (basePath.endsWith('/responses')) {
    throw new Error('OPENAI_BASE_URL must be a base URL, not a Responses endpoint.');
  }
  url.pathname = basePath + (basePath.endsWith('/v1') ? '/responses' : '/v1/responses');
  return url.href;
}

module.exports = { responsesEndpoint };

if (require.main === module) {
  fs.appendFileSync(process.env.GITHUB_OUTPUT,
    `responses-endpoint=${responsesEndpoint(process.env.OPENAI_BASE_URL || '')}\n`);
}
