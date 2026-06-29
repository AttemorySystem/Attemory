# Attemory on SWE-QA

This note reports a Claude Code + Attemory experiment on
[SWE-QA-Bench](https://github.com/peng-weihan/SWE-QA-Bench). The goal is to
test whether a semantic code-search hint can reduce end-to-end agent token
cost on repository question answering without quality loss.

## Conclusion

**Attemory reduces model tokens by 43.8% with a near-tied judge score(83.39 vs 83.17).**

The current result covers 15 SWE-QA repos and 720 paired samples.

The compared systems are:

```text
Baseline run: Claude Code + read-only repository tools + LLM(deepseek v4)
Attemory run: Claude Code + read-only repository tools + LLM(deepseek v4) + one pre-run attemory search hint
```

Judge config:

```text
Official SWE-QA five-dimension judge script with openai/gpt-5.4 as judge model.
```

Aggregate summary:

| System | Avg score | Total tokens | Main-agent tokens | Subagent tokens | Turns | Tool calls | CC cost estimate |
|---|---:|---:|---:|---:|---:|---:|---:|
| Baseline | 83.39 | 285.39M | 122.60M | 162.80M | 10491 | 26997 | $453.47 |
| Attemory | 83.17 | 160.39M | 86.60M | 73.79M | 8765 | 17340 | $296.68 |
| Change | -0.23 | -43.8% | -29.4% | -54.7% | -16.5% | -35.8% | -34.6% |

> **Cost note.** `CC cost estimate` is the `total_cost_usd` value emitted by
> Claude Code in the final `stream-json` result event, not a DeepSeek bill.
> [Claude Code documents this cost field](https://docs.anthropic.com/en/docs/claude-code/statusline)

Per-repo quality and token summary:

| repo | baseline score | Attemory score | score delta | baseline tokens | Attemory tokens | token delta |
|---|---:|---:|---:|---:|---:|---:|
| reflex | 82.02 | 82.02 | +0.00 | 35.86M | 15.09M | -57.9% |
| sklearn | 87.12 | 86.71 | -0.42 | 11.49M | 8.01M | -30.3% |
| flask | 84.58 | 85.58 | +1.00 | 9.90M | 6.57M | -33.7% |
| requests | 87.42 | 88.31 | +0.90 | 9.50M | 5.21M | -45.2% |
| streamlink | 77.90 | 77.65 | -0.25 | 15.43M | 8.00M | -48.2% |
| sympy | 78.75 | 79.73 | +0.98 | 20.81M | 15.40M | -26.0% |
| xarray | 86.88 | 86.23 | -0.65 | 17.37M | 11.64M | -33.0% |
| sqlfluff | 85.29 | 83.71 | -1.58 | 17.75M | 10.58M | -40.4% |
| astropy | 85.42 | 85.35 | -0.06 | 15.44M | 9.08M | -41.2% |
| conan | 81.69 | 79.31 | -2.38 | 22.08M | 10.00M | -54.7% |
| django | 85.10 | 85.08 | -0.02 | 20.75M | 11.76M | -43.3% |
| matplotlib | 87.17 | 87.10 | -0.06 | 13.90M | 9.52M | -31.5% |
| pylint | 76.50 | 75.12 | -1.38 | 34.77M | 16.19M | -53.4% |
| pytest | 85.40 | 84.67 | -0.73 | 15.31M | 8.82M | -42.4% |
| sphinx | 79.69 | 80.92 | +1.23 | 25.04M | 14.52M | -42.0% |

The quality result is a near tie under the judge, while the token reduction is
large and consistent across repos.

### Why Attemory Uses Fewer Tokens

The token reduction comes from moving repository localization out of the LLM's
trial-and-error loop. In the baseline, Claude Code must discover relevant files
through model-driven exploration: `Grep`, `Glob`, `Read`, and often `Task`
subagents that inspect code in their own contexts. Those exploratory turns
consume both main-agent tokens and subagent tokens.

In the Attemory run, semantic search is performed before Claude Code starts and
returns a high-recall list of likely files and line ranges. That hint narrows
the first exploration target for the main agent and also gives subagents more
focused evidence to verify. As a result, the agent performs fewer broad search
and read loops, launches fewer exploratory subagent calls, and carries less raw
repository evidence in the main context. This is why both main-agent and
subagent token usage drop in the aggregate table.

## Method

Attemory is used only as a code retrieval engine over the repositories in this experiment.
It is not used as conversational memory, and the
runner never stores Claude Code prompts, answers, tool outputs, summaries, or
trajectories into Attemory.

The SWE-QA Attemory setup does not use benchmark-specific per-question routing
or hand-written file boosts. It also keeps the same simple chunking and
retrieval pipeline used in the [Semble benchmark](semble.md): roughly 30-line code
chunks, a short generic indexing prompt, semantic search, optional
`oneshot_search` global reranking, and file-level fusion. The search prompt and
chunking policy were not tuned specifically for SWE-QA, leaving room for
benchmark-specific optimization.

Each Claude Code invocation uses:

```text
--output-format stream-json
--no-session-persistence
--permission-mode dontAsk
--tools Task,Bash,Read,Grep,Glob
--disallowed-tools Edit,Write,WebFetch,WebSearch
```

The runner also isolates `HOME` by default for each sample so Claude Code native
memory does not leak across samples.

### Overview

The experiment keeps Claude Code as the downstream agent and changes only the
context given to it before each run.

1. Claude Code is launched in read-only mode with repository tools only:
   `Task`, `Bash`, `Read`, `Grep`, and `Glob`.
2. Before an Attemory run, the pinned repository commit is indexed once
   offline. At run time, the clean SWE-QA question is sent to Attemory search.
3. Retrieved chunks are fused into a small ranked file/range list. This list is
   inserted into the prompt as a pre-run semantic hint.
4. Claude Code is then launched on the prepared prompt. It decides what to
   inspect and can use normal repository tools for any missing evidence. It is also
   free to launch `Task` subagents, which return compact evidence summaries to
   the main agent.

### Indexed Repos

Each SWE-QA repo maps to one Attemory session:

```text
attemory-sweqa-{repo}-{git_commit_prefix}
```

The indexing system prompt for attemory is:

```text
Read the following code carefully and find the most relevant code to the query.
```

For every source file, the runner adds one no-id file header:

```text
// the following code come from path/to/file
```

The indexer uses a simple source chunking policy rather than a
language-aware parser: source files are split into roughly 30-line blocks, with
splits moved to nearby blank lines when possible. The runner skips
generated/cache/build directories such as
`.git`, `.venv`, `__pycache__`, `build`, `dist`, `node_modules`,
`site-packages`, and `venv`.

The following table reports the indexed context size for each repository:

| repo | session | memories | segments | index tokens |
|---|---|---:|---:|---:|
| reflex | `attemory-sweqa-reflex-fe0f946dc0c2` | 3,776 | 4 | 824,772 |
| scikit-learn | `attemory-sweqa-scikit-learn-adb1ae76d798` | 15,643 | 21 | 4,502,891 |
| flask | `attemory-sweqa-flask-85c5d93cbd04` | 733 | 1 | 155,291 |
| requests | `attemory-sweqa-requests-46e939b5525d` | 415 | 1 | 103,694 |
| streamlink | `attemory-sweqa-streamlink-ab1f36560005` | 3,484 | 4 | 811,644 |
| sympy | `attemory-sweqa-sympy-3c817ed8ab55` | 26,208 | 41 | 9,473,651 |
| xarray | `attemory-sweqa-xarray-40119bf35c53` | 5,998 | 9 | 1,886,305 |
| sqlfluff | `attemory-sweqa-sqlfluff-db9801b7eb7f` | 5,197 | 6 | 1,196,290 |
| astropy | `attemory-sweqa-astropy-0a041d38fbd0` | 19,359 | 36 | 7,912,055 |
| conan | `attemory-sweqa-conan-52f43d98fe44` | 6,662 | 9 | 1,976,798 |
| django | `attemory-sweqa-django-14fc2e97036f` | 20,828 | 23 | 5,267,107 |
| matplotlib | `attemory-sweqa-matplotlib-a5e1f6086e92` | 13,230 | 18 | 4,186,770 |
| pylint | `attemory-sweqa-pylint-44740e5e5cdd` | 7,338 | 5 | 1,053,449 |
| pytest | `attemory-sweqa-pytest-5989efe3efec` | 3,464 | 4 | 882,198 |
| sphinx | `attemory-sweqa-sphinx-6c9e3209c4eb` | 7,161 | 9 | 1,932,045 |

### Query And Search

For each SWE-QA question, Attemory receives the clean benchmark question:

```text
Question: {question}
```

The query context is:

```text
Read the selected code snippets carefully and answer the user's repository question: {question}.
Treat the query as code navigation. Rank snippets by how directly they define, declare,
implement, export, configure, or explain the requested symbol, API, behavior, or feature.
Prefer exact identifier matches and concrete implementation code over snippets that only
mention, call, test, or document it.
```

The default search settings are:

```text
ATTEMORY_SEARCH_TOPK=8
ATTEMORY_CHUNK_TOPK=16
```

`ATTEMORY_CHUNK_TOPK` controls how many chunk hits are collected before file
fusion. `ATTEMORY_SEARCH_TOPK` controls how many fused files are inserted into
the Claude prompt.

### File Fusion

SWE-QA's downstream agent needs files and line ranges, not raw retrieval chunks.
The runner maps Attemory chunk ids through `chunks.jsonl`:

```text
chunk id -> repo-relative file path, start line, end line
```

It then computes a file score from chunk rank:

```text
score[file] += 1 / log2(chunk_rank + 1)
```

The final file order is sorted by:

```text
(-score[file], first_chunk_rank[file], file_path)
```

Adjacent chunk ranges from the same file are merged before they are shown to
Claude Code.

This chunking, query/search prompt, and file-fusion method is intentionally
borrowed from the [Semble benchmark](semble.md) setup without SWE-QA-specific
tuning. The reported numbers therefore measure a simple cross-benchmark
transfer setting, not a prompt or retrieval pipeline optimized for SWE-QA.

### Claude Code Prompt

Attemory is not exposed to Claude Code as a native tool in the reported run.
Instead, the runner performs search before launching Claude Code and injects a
compact hint:

```text
<semantic_search_results>
The following files and line ranges are semantic-search candidate evidence from the repository.
Inspect these locations first. Use them as a shortcut to relevant code, not as a complete answer scope.

...

1. path/to/file.py:10-39,80-110
2. another/file.py:1-30
</semantic_search_results>
```

The prompt instructs Claude Code to inspect those locations first, answer
directly when the evidence is sufficient, and run only narrow repository search
when a concrete symbol, file, caller, test, exception path, or anything is
missing.

For simplicity and comparability, this design keeps the search API outside the
agent's action space: both systems run the same Claude Code agent with the same
read-only tools, while Attemory only changes the initial context hint.

### Evaluation

The reported run uses the official SWE-QA five-dimension judge prompt from the
upstream repository:

```text
Benchmark construction/score/llm-as-a-judge.py
```

The patch adds a minimal OpenRouter-compatible client path to this official
script. It scores each answer on five dimensions:

```text
correctness
completeness
relevance
clarity
reasoning
```

The total score is out of 100.

## Reproduction

Prepare the pinned SWE-QA-Bench checkout and apply the Attemory runner:

```bash
cd benchmarks
./prepare_bench.sh sweqa
```

This downloads:

```text
SWE-QA-Bench
repo:   https://github.com/peng-weihan/SWE-QA-Bench.git
commit: d7bf283d65f4bbdc86ead92fc130eee4986355f0
```

The patch adds one directory inside the upstream checkout:

```text
SWE-QA-Bench/attemory/
```

which contains:

```text
attemory/scripts/           runner, index, judge, summary, and small helper modules
attemory/config/*.example   env templates without API keys
```

Only four scripts are intended to be invoked directly:
`run_sweqa.py`, `build_attemory_index.py`, `summarize_run.py`, and `judge.py`.
The remaining files under `scripts/` are implementation helpers used by those
entry points.

Create the SWE-QA repository checkouts at their pinned commits:

```bash
cd benchmarks/SWE-QA-Bench
./clone_repos.sh
```

The default patched runner uses:

```text
benchmarks/SWE-QA-Bench/Benchmark/*.jsonl
benchmarks/SWE-QA-Bench/datas/repos/<repo> if that directory exists,
otherwise benchmarks/SWE-QA-Bench/datas/<repo>
benchmarks/SWE-QA-Bench/.attemory-index/
```

The reported runs used this Claude Code version:

```text
claude --version
2.1.187 (Claude Code)
```

Create the Claude Code and Attemory environment file:

```bash
cd benchmarks/SWE-QA-Bench/attemory
cp config/deepseek.env.example config/deepseek.env
```

Use the Anthropic-compatible environment variables from
[DeepSeek's Claude Code integration guide](https://api-docs.deepseek.com/quick_start/agent_integrations/claude_code):

```bash
export ANTHROPIC_BASE_URL=https://api.deepseek.com/anthropic
export ANTHROPIC_AUTH_TOKEN=<your-deepseek-key>
export ANTHROPIC_MODEL="deepseek-v4-pro[1m]"
export ANTHROPIC_DEFAULT_OPUS_MODEL="deepseek-v4-pro[1m]"
export ANTHROPIC_DEFAULT_SONNET_MODEL="deepseek-v4-pro[1m]"
export ANTHROPIC_DEFAULT_HAIKU_MODEL=deepseek-v4-flash
export CLAUDE_CODE_SUBAGENT_MODEL=deepseek-v4-flash
export CLAUDE_CODE_EFFORT_LEVEL=max
```

Load the file:

```bash
source config/deepseek.env
```

Start an Attemory server separately, then point the runner at it:

```bash
export ATTEMORY_HOST=127.0.0.1
export ATTEMORY_PORT=9006
```

Build repo indexes before running the Attemory mode:

```bash
cd benchmarks/SWE-QA-Bench/attemory
python scripts/build_attemory_index.py --repo requests
```

Run one paired repo:

```bash
cd benchmarks/SWE-QA-Bench/attemory

python scripts/run_sweqa.py default requests --workers 1

python scripts/run_sweqa.py attemory requests --workers 1
```

Run all 15 SWE-QA repos:

```bash
cd benchmarks/SWE-QA-Bench/attemory

repos=(
  reflex scikit-learn flask requests streamlink sympy xarray sqlfluff
  astropy conan django matplotlib pylint pytest sphinx
)

for repo in "${repos[@]}"; do
  python scripts/run_sweqa.py default "${repo}" --workers 1

  python scripts/run_sweqa.py attemory "${repo}" --workers 1
done
```

By default, `run_sweqa.py` writes to:

```text
logs/{backend}_{repo}/
output/{backend}_{repo}.jsonl
```

Summarize runtime tokens and turns from Claude Code stream logs:

```bash
python scripts/summarize_run.py logs/default_requests
python scripts/summarize_run.py logs/attemory_requests
```

The script reads `claude_stream.jsonl`, reports `main_tokens`,
`subagent_tokens`, and `total_tokens`, and uses visible `Agent`/`Task` call ids
plus `parent_tool_use_id` to count subagent turns and tokens. `total_tokens` is
`main_tokens + subagent_tokens`. It prints a compact CSV-like table to stdout.
To also save the per-sample rows:

```bash
python scripts/summarize_run.py logs/attemory_requests \
  --csv output/attemory_requests_sample_metrics.csv
```

Each run directory contains one subdirectory per sample:

```text
logs/<run_name>/
  repo_root.txt
  samples.txt
  <sample_id>/
    sample.json
    prompt.txt
    claude_stream.jsonl
    claude_stderr.log
    exit_code
    attemory_search.jsonl      # Attemory mode only
    attemory_search.txt        # Attemory mode only
```

Configure the official judge API key. The wrapper defaults to OpenRouter
GPT-5.4 and sets the candidate/reference/output paths automatically:

```bash
export OPENROUTER_API_KEY=<openrouter-api-key>
```

Run the judge for one paired repo:

```bash
cd benchmarks/SWE-QA-Bench/attemory
python scripts/judge.py requests
```

This runs the official judge for both:

```text
output/default_requests.jsonl
output/attemory_requests.jsonl
```

and writes:

```text
output/default_requests_score.jsonl
output/attemory_requests_score.jsonl
```

Use `--workers` or `--model` to override the defaults:

```bash
python scripts/judge.py requests --workers 2 --model openai/gpt-5.4
```

Judge outputs are JSONL. Each row contains:

```text
question
score
```
