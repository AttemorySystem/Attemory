# Attemory Benchmarks

We use four public benchmarks to validate Attemory:
[LongMemEval](https://github.com/xiaowu0162/LongMemEval),
[LoCoMo](https://github.com/snap-research/locomo), and
[Semble](https://github.com/MinishLab/semble), plus
[SWE-QA-Bench](https://github.com/peng-weihan/SWE-QA-Bench) as an
agent-facing code QA workload.

Together, they test retrieval accuracy, scalability, and universal retrieval
across different data types:

1. Long-term conversational memory retrieval at million-token scale.
2. End-to-end long-conversation question answering.
3. Codebase retrieval for software agents.
4. End-to-end repository question answering with Claude Code.

Attemory achieves SOTA-class results on the retrieval and memory benchmarks
without benchmark-specific tuning. The same retrieval engine reads raw
memories with attention, without summaries as the stored representation, query
rewriting, agent-generated synthetic memories, graph schemas, or separate
keyword/vector retrieval stacks.

## Results

| Benchmark | Task | Context scale | Main result |
|---|---|---:|---:|
| LongMemEval-S-cleaned | Retrieval-only long-term memory | about 40 sessions / 115k tokens | **98.72% session R_any@5**, **92.77% session R_all@5**, **98.94% message R_all@50** |
| LongMemEval-M-cleaned | Retrieval-only long-term memory | about 500 sessions / 1.5M tokens / 4.9k messages | **94.89% session R_any@5**, **83.62% session R_all@5**, **92.55% message R_all@50** |
| LoCoMo | End-to-end memory QA | 10 long conversations / 1,540 QA items | **94.52% accuracy** |
| Semble | Code retrieval | 63 repos / 19 languages / largest repo about 5M tokens | **0.9055 file-level NDCG@10** |
| SWE-QA | End-to-end code QA agent | 15 repos / 720 questions | **43.8% fewer model tokens** with near-tied GPT-5.4 judge score, **83.17 vs 83.39** |

LongMemEval is reported at two levels. Session-level retrieval asks whether the
system finds the correct historical sessions. Message-level retrieval is
stricter and more useful for agents: it asks whether the system finds the exact
user or assistant messages that contain the evidence. On LongMemEval-M,
Attemory searches about **500 sessions**, about **4,894 messages**, and roughly
**1.5M tokens** per query, yet retrieves all labeled evidence messages within
the top 50 messages for **92.55%** of answerable queries.

Result artifacts:

| Benchmark | Final results |
|---|---|
| LongMemEval | [`results/LongMemEval/`](results/LongMemEval/) |
| LoCoMo | [`results/LoCoMo/`](results/LoCoMo/) |
| Semble | [`results/semble/`](results/semble/) |
| SWE-QA | [`results/SWEQA/`](results/SWEQA/) |

## Local Reproduction

All four benchmarks can be prepared and run from this repository. LongMemEval
and Semble validate local retrieval directly and do not require cloud services
or API keys. LoCoMo uses the EverOS end-to-end answer-and-judge harness, and
SWE-QA uses Claude Code plus an LLM judge; those LLM endpoints can be local or
hosted. Hardware acceleration is recommended for running Attemory quickly on the
larger benchmarks.

```bash
cd benchmarks
./prepare_bench.sh longmemeval
./prepare_bench.sh locomo
./prepare_bench.sh semble
./prepare_bench.sh sweqa
```

You can also prepare everything at once:

```bash
cd benchmarks
./prepare_bench.sh all
```

Start an Attemory server separately before running a benchmark. LoCoMo and
SWE-QA also need OpenAI-compatible or Claude-Code-compatible LLM endpoints for
their answer/judge stages; those endpoints may point to local model servers or
hosted APIs.

Detailed run instructions:

- LongMemEval retrieval: [`LongMemEval.md`](LongMemEval.md)
- LoCoMo with EverOS/EverCore: [`LoCoMo.md`](LoCoMo.md)
- Semble code retrieval: [`semble.md`](semble.md)
- SWE-QA with Claude Code: [`sweqa.md`](sweqa.md)

## Benchmark Integrity

Attemory uses simple adapters for different benchmark formats, but the
retrieval engine is not specialized to a single benchmark.

Across these runs, Attemory does not use:

- answer labels or gold evidence ids during retrieval
- benchmark-specific query-type routing, including LongMemEval `question_type`
- query rewriting into multiple paraphrases
- memory summarization as the stored representation
- synthetic or agent-generated memory facts
- hand-written scoring boosts for names, dates, phrases, or topics
- any graph database, vector database, or BM25/vector hybrid stack
- any cloud retrieval services

The adapters preserve the benchmark's original data as closely as possible. They
add only lightweight context structure: timestamp headers for conversations,
file headers for code, and fixed retrieval instructions. The benchmark question
itself is used directly as the search query, including short or vague queries in
the benchmark that may be only one or two words.

## Scalability 

Attemory is not only a high-accuracy retriever on small contexts. The final
benchmarks exercise million-token retrieval in both conversation memory and
codebase retrieval:

| Setting | Scale | Result |
|---|---|---:|
| LongMemEval-M-cleaned, session retrieval | about 500 sessions / about 1.5M tokens per query | **94.89% session R_any@5**, **83.62% session R_all@5** |
| LongMemEval-M-cleaned, message retrieval | about 4,894 user+assistant messages per query | **90.21% message R_all@30**, **92.55% message R_all@50** |
| Semble largest repo, `zig` | about 5M indexed tokens, 14,816 code memories | **0.9565 file-level NDCG@10** |

This is the core scalable-retrieval result: the same raw-memory retrieval engine
works across 1.5M-token conversational histories and multi-million-token
codebases, without switching to a benchmark-specific retrieval stack.

For downstream agents, scale matters only if the retrieved context is compact.
LongMemEval-M shows that Attemory can reduce a 1.5M-token conversation history
to the exact evidence messages: it retrieves every labeled evidence message
within the top 50 messages for **92.55%** of answerable queries. Semble shows
the same property for code: Attemory indexes multi-million-token repositories,
including `zig` at about 5M tokens, and still returns a high-quality file
ranking. Together, these results show that million-token memories are workable
for downstream agents.

## LongMemEval-S/M-Cleaned

[LongMemEval](https://github.com/xiaowu0162/LongMemEval) evaluates long-term
memory over user-assistant histories. The cleaned benchmark has two
retrieval settings: S, with roughly 115k-token histories, and M, the harder
long-history setting with about 500 sessions and roughly 1.5M tokens per
question in the original benchmark description. We run the cleaned files and
follow the retrieval evaluation convention of excluding the 30 abstention
questions that have no ground-truth evidence location, leaving 470 answerable
retrieval queries.

Attemory is evaluated through the official LongMemEval retrieval harness with an
added `attemory` adapter. The adapter stores raw user and assistant turns as
retrievable memories and inserts session-date headers as no-id context memories.
Search returns message ids; session metrics are computed by mapping those
messages back to their parent sessions and de-duplicating the session ranking.

### Metrics

For a top-k retrieved list:

- `recall_any@k`: at least one gold evidence item appears in the top k.
- `recall_all@k`: every gold evidence item appears in the top k.
- `ndcg_any@k`: ranking quality with binary relevance over the top k.

We report two levels:

- `session`: LongMemEval session-level evidence, using `answer_session_ids`.
- `message`: all-role user+assistant message retrieval, using labeled evidence
  turns. This is more agent-relevant than coarse session retrieval, but it is not
  the official user-only LongMemEval turn mode.

### Main Results

We start the Attemory server with large tier for LongMemEval.

| Dataset | Session R_any@5 | Session R_all@5 | Session NDCG@5 | Message R_all@30 | Message R_all@50 |
|---|---:|---:|---:|---:|---:|
| **LongMemEval-S-cleaned** | **98.72** | **92.77** | **93.90** | **96.81** | **98.94** |
| **LongMemEval-M-cleaned** | **94.89** | **83.62** | **87.76** | **90.21** | **92.55** |

S is close to the retrieval ceiling. M is the scale test: Attemory still finds
all gold evidence sessions in the top 5 for **83.62%** of answerable queries
from about 500 historical sessions.

The message-level numbers are the more precise agent-facing result. In
M-cleaned, each query has about **4,894** historical user+assistant messages on
average. Attemory retrieves all labeled evidence messages within the top 30
messages for **90.21%** of answerable queries, and within the top 50 messages
for **92.55%**. Top-50 individual messages are a compact evidence set compared
with passing whole sessions, and they contain much less irrelevant context.

These runs use the same raw-ingest and raw-query pipeline. We have not started
LongMemEval-specific tuning such as prompt search, query rewriting,
question-type routing, or per-case rules, so the numbers should be read as a
clean baseline rather than a saturated benchmark-specific result.

Detailed M-cleaned metrics:

| Level | R_any@1 | R_any@3 | R_any@5 | R_any@10 | R_any@30 | R_any@50 |
|---|---:|---:|---:|---:|---:|---:|
| Session | 86.38 | 93.19 | **94.89** | 96.38 | 99.36 | 99.36 |
| Message, user+assistant | 72.98 | 87.02 | 90.64 | 93.62 | 97.23 | 97.87 |

| Level | R_all@1 | R_all@3 | R_all@5 | R_all@10 | R_all@30 | R_all@50 |
|---|---:|---:|---:|---:|---:|---:|
| Session | 29.57 | 77.45 | **83.62** | 89.57 | 96.60 | 97.66 |
| Message, user+assistant | 24.68 | 60.85 | 71.91 | 80.64 | **90.21** | **92.55** |

| Level | NDCG_any@5 | NDCG_any@10 | NDCG_any@50 |
|---|---:|---:|---:|
| Session | 87.76 | 89.16 | 90.46 |
| Message, user+assistant | 76.08 | 78.55 | 80.64 |

Detailed info: [`LongMemEval.md`](LongMemEval.md)

## LoCoMo

[LoCoMo](https://github.com/snap-research/locomo) evaluates long-horizon
conversational memory through question answering. It is not a retrieval-only
benchmark: the system stores the conversation, searches memory, passes selected
context to an answer model, and then an LLM judge compares the answer to the
gold answer.

We use the EverOS evaluation harness:

```text
add -> search -> answer -> evaluate
```

Configuration:

- questions: LoCoMo category 1-4, 1540 questions
- excluded: category 5 adversarial questions
- answer model: `openai/gpt-4.1-mini`
- judge model: `gpt-4o-mini`
- judge runs: 3
- metric: mean LLM-judge QA accuracy

### Main Result

| System | Harness | Accuracy | Notes |
|---|---|---:|---|
| **Attemory** | EverOS/EverCore | **94.52%** | 1455/1540 correct, 3-run mean |

Other EverOS/LoCoMo method results are here:
[EverOS hits SOTA performance on LoCoMo](https://evermind.ai/blogs/everos-hits-sota-performance-on-locomo).

### Category Breakdown

LoCoMo category ids in the data are:

| Category | Type | Questions | Attemory |
|---|---|---:|---:|
| 1 | Multi-hop | 282 | **94.33% +/- 0.58pp** |
| 2 | Temporal | 321 | **92.63% +/- 0.39pp** |
| 3 | Open-domain | 96 | **80.90% +/- 0.49pp** |
| 4 | Single-hop | 841 | **96.87% +/- 0.15pp** |
| Overall | Weighted | 1540 | **94.52% +/- 0.03pp** |

Detailed method: [`LoCoMo.md`](LoCoMo.md)

## Semble

[Semble](https://github.com/MinishLab/semble) is a code search benchmark over
about 1250 queries, 63 repositories, and 19 programming languages. The published
Semble benchmark reports query-level NDCG@10 and lists CodeRankEmbed Hybrid at
**0.862** and Semble's own hybrid stack at **0.854**.

We start the Attemory server with medium tier for semble benchmark.

Attemory uses a deliberately simple code-memory setup:

- recursively scan source files
- split files into roughly 30-line chunks, preferring blank-line boundaries
- add no-id file headers for path context
- store raw code chunks as retrievable memories
- search with the benchmark query and a fixed code-navigation query context
- map chunk hits back to files, fuse chunk ranks into file ranks, and compute
  file-level NDCG@10

It does not use AST parsing, tree-sitter chunking, symbol indexes, code-specific
embeddings, language-specific chunkers, or a BM25/vector hybrid retriever.

### Main Result

Semble's published baseline table is available in the
[official Semble benchmark README](https://github.com/MinishLab/semble/blob/main/benchmarks/README.md).

| Method | Metric | Score |
|---|---|---:|
| **Attemory** | file-level NDCG@10 | **0.9055** |
| CodeRankEmbed Hybrid | NDCG@10 | 0.862 |
| Semble hybrid | NDCG@10 | 0.854 |
| CodeRankEmbed | NDCG@10 | 0.765 |
| BM25 | NDCG@10 | 0.673 |
| ripgrep | NDCG@10 | 0.126 |

Full Attemory run:

The largest repository in this run is `zig`, about 5M indexed tokens in our
measurement. Attemory indexes it as 14,816 code memories and reaches **0.9565
file-level NDCG@10** on that repository.

Attemory retrieves at chunk level: each code chunk is indexed and ranked as an
individual memory. Since Semble annotations are overwhelmingly file-level labels
rather than precise line-span labels, we evaluate at file level by mapping
retrieved chunks back to their source files. When multiple chunks from the same
file are retrieved, their chunk ranks are fused into a single file score,
producing the final file ranking used for NDCG@10. This preserves chunk-level
retrieval evidence while matching the dominant granularity of the benchmark
labels: in the annotation set we inspected, 1457 of 1491 targets are path-only
and only 34 include line spans.

Detailed method: [`semble.md`](semble.md)

## SWE-QA

[SWE-QA-Bench](https://github.com/peng-weihan/SWE-QA-Bench) evaluates
repository question answering. We run Claude Code on 15 pinned repositories and
compare the normal read-only agent against the same agent with an Attemory
semantic code-search hint injected before the agent loop starts.

This benchmark is agent-facing rather than retrieval-only. The final metric is
LLM-judged answer quality, and the main systems metric is Claude Code model
token usage.

The compared systems are:

```text
Baseline run:
  Claude Code + read-only tools + DeepSeek v4 model + Task subagents

Attemory run:
  Claude Code + read-only tools + DeepSeek v4 model + Task subagents
  + one pre-run Attemory semantic retrieval hint
```

The downstream agent is unchanged. It can ignore the hint, inspect the
repository normally with `Read`, `Grep`, `Glob`, `Bash`, and `Task`, and launch
subagents. Attemory is not exposed as an interactive tool during the run; it
only supplies a compact file/range hint before Claude Code starts.

The Attemory setup:

- indexes each pinned repo commit as raw code chunks
- adds no-id file headers for path context
- searches with the clean SWE-QA question
- reranks multi-segment search results with `oneshot_search`
- fuses chunk hits into a compact file/range hint
- lets Claude Code answer with normal read-only repository tools

### Main Result

| System | GPT-5.4 judge score | Model tokens | Turns | Tool calls |
|---|---:|---:|---:|---:|
| baseline Claude Code | **83.39** | 285.39M | 10491 | 26997 |
| Claude Code + Attemory hint | 83.17 | **160.39M** | **8765** | **17340** |

Attemory reduces model tokens by **43.8%** with a near-tied judge score
(-0.23 average score over 720 paired samples). The reduction is visible in both
main-agent and subagent tokens in the detailed SWE-QA report.

Detailed method and reproduction commands: [`sweqa.md`](sweqa.md)
