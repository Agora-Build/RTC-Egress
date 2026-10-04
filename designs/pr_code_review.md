# Automated PR Code Review

RTC-Egress runs Codex and Claude reviews when a pull request is opened,
updated, reopened, or marked ready for review. Each reviewer posts an ordinary
PR comment containing findings or an explicit statement that it found no
actionable issues, together with the reviewed head commit SHA. These comments
are separate from GitHub approval reviews and production build/test checks.

## Configuration

Configure these repository or organization secrets in GitHub Actions:

| Secret | Purpose |
| --- | --- |
| `OPENAI_API_KEY` | Credential for the OpenAI-compatible review provider |
| `OPENAI_BASE_URL` | HTTPS provider root or `/v1` base URL; trailing slashes are accepted |
| `ANTHROPIC_API_KEY` | Credential for the Anthropic-compatible review provider |
| `ANTHROPIC_BASE_URL` | Base URL consumed directly by Claude Code |

The workflow normalizes the Codex URL to end in `/v1/responses`, preserving any
provider path prefix and avoiding duplicate `/v1` segments. The action receives
it through `responses-api-endpoint` and uses `gpt-5.6-sol`, matching Vox. A provider
credential sent to the default OpenAI endpoint can fail authentication even
when the credential works with its intended provider. Missing configuration
fails before either model starts, with the missing secret's name.

To configure the URL interactively without putting it in shell history:

```sh
gh secret set OPENAI_BASE_URL --repo Agora-Build/RTC-Egress
```

Do not put credential values in documentation, review artifacts, or memory.

## Review And Publication

1. The model job checks out the PR merge ref with full history and no persisted
   Git credentials. The prompt asks the reviewer to inspect the actual diff
   against the event's base SHA. The prompt includes a literal SHA so Claude's
   permission checker can validate the diff command, and treats the changed-file
   summary as untrusted context.
2. The model job has `contents: read` and `pull-requests: read`. It receives its
   provider configuration and produces an artifact; it cannot post a review.
3. A separate job downloads the artifact, reads the publisher from the triggering
   PR head, validates the result, and posts the comment. It has PR write access
   and receives no provider credentials. The publisher treats model output as
   text and never executes it.

An authorization job checks the author's actual repository permission through
GitHub's API. Provider-backed reviews run only for same-repository PRs whose
author has `admin`, `maintain`, or `write` access. Fork PRs and other authors are skipped.
An author-access 404 also skips the review; other API failures fail authorization.
The publisher also runs code from that trusted PR, so this author restriction
applies to both model access and publication. The standalone workflow tests
run without provider credentials, including on fork PRs.

Codex stores its final Markdown in `codex-review-output`. Claude extracts the
completed result from the action's execution JSON and uploads only the final
Markdown in `claude-review-output`. Artifacts expire after seven days. The full
Claude execution transcript is not uploaded, and assistant messages without a
completed result fail extraction instead of being posted as a partial review.

A missing artifact, malformed JSON, failed Claude result, empty review, or
GitHub publication error fails the workflow. Empty output cannot silently
produce a successful review check. Oversized comments fail instead of being
silently truncated. Concurrent runs for an updated PR cancel older runs, and
the comment's head SHA identifies which version was reviewed.

## Action Version And Timeouts

Codex is pinned to `openai/codex-action` commit
`52fe01ec70a42f454c9d2ebd47598f9fd6893d56` (v1.11), matching Vox's mitigation
for hangs in the v1.12 launch path. Reassess this pin after the upstream fix
for [#150](https://github.com/openai/codex-action/issues/150) and
[#169](https://github.com/openai/codex-action/issues/169) is released and
verified with actual reviews. Codex has a ten-minute job timeout, Claude has
fifteen minutes, and each publication job has five minutes.

The configuration follows Vox's
[Codex workflow](https://github.com/Agora-Build/Vox/blob/main/.github/workflows/codex-code-review.yml)
and [Claude workflow](https://github.com/Agora-Build/Vox/blob/main/.github/workflows/claude-code-review.yml).
The separation between model execution and posting is also illustrated in
[OpenAI's GitHub Action documentation](https://developers.openai.com/codex/github-action/).

## Verification And Troubleshooting

Run the behavioral tests and workflow linter:

```sh
node --test .github/scripts/*.test.cjs
go run github.com/rhysd/actionlint/cmd/actionlint@v1.7.7 -shellcheck= \
  .github/workflows/codex-code-review.yml \
  .github/workflows/claude-code-review.yml \
  .github/workflows/review-workflow-tests.yml
```

`Review Workflow Tests` runs these checks whenever `.github/` changes. Tests
cover real Claude output shapes, errors and missing results, literal code in
review text, artifact validation, comment construction, and GitHub failures.
They also execute the actual workflow authorization scripts with GitHub API
mocks, run the provider configuration step with fixture URLs, and check the
literal-SHA diff prompt.

For a live PR, check that both `review` and `post` jobs succeed and that both
comments name the current PR head SHA. An artifact or a successful model step
alone does not prove that the review was published. For authentication errors,
check the provider URL, credential, and model together. For publication errors,
check the `post` job and the review artifact without exposing credentials.
