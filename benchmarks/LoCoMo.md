# Attemory on LoCoMo

LoCoMo's public evaluation path is mainly question answering over dialogue
history. It does not provide a standalone retrieval-only benchmark script
comparable to LongMemEval's retrieval evaluation. For this benchmark, we use
the [EverOS](https://github.com/EverMind-AI/EverOS) end-to-end LoCoMo harness:

```text
add -> search -> answer -> evaluate
```

Attemory is used as the retrieval system. The current configuration uses
`openai/gpt-4.1-mini` as the answer model and `gpt-4o-mini` as the judge model.
The reported score is final answer accuracy after LLM judging, not direct
retrieval recall.

## Flow

The benchmark runs as follows:

1. Load LoCoMo conversations and QA pairs through EverOS.
2. Add each LoCoMo conversation into one Attemory session.
3. Search Attemory with the raw LoCoMo question.
4. Convert returned dialogue-turn ids back into selected conversation context.
5. Ask the answer model to answer from that selected context.
6. Use the LoCoMo judge prompt to compare the generated answer with the gold
   answer.

The Attemory adapter is configured as:

```text
system: attemory
```

The server is started externally. Benchmark script only connects to it through the
Attemory Python HTTP client.

## Indexed Context

Attemory indexes dialogue turns, while session headers provide timestamp
context. The indexed context has this structure:

```text
system prompt

The following conversation takes place at <timestamp>.
Speaker A: ...
Speaker B: ...

The following conversation takes place at <timestamp>.
Speaker A: ...
Speaker B: ...
...
```

The system prompt is:

```text
Read the following conversation and answer the query at the end.
```

Each dialogue turn is added as a retrievable Attemory memory with its LoCoMo
dialogue id. Session headers are added as context memories without ids, so they
can help the model understand time but are not treated as valid retrieved
dialogue turns.

Image captions produced by the LoCoMo loader are included as normal text in the
dialogue content.

## Search

The search query is the raw LoCoMo question:

```text
{question}
```

The adapter filters out returned items without ids, because those are context
headers rather than dialogue turns. The remaining results are the retrieved
dialogue turns.

## Answer Context

The answer model is not given raw relevance-ranked snippets directly. Instead,
the adapter maps the returned dialogue ids back into the original LoCoMo
conversation and rebuilds a selected conversation template:

```text
The following selected conversation takes place at <timestamp>.
Speaker A: selected turn
Speaker B: selected turn

The following selected conversation takes place at <timestamp>.
Speaker A: selected turn
...
```

This keeps the evidence compact while preserving the session timestamp and the
original dialogue order inside each session. That matters for temporal and
multi-turn questions, where the answer model needs to reason over when a turn
happened and how nearby selected turns relate to each other.

EverOS's Attemory answer prompt reads this selected context and the question:

```text
{context}

Question: {question}
```

## Evaluation

LoCoMo is evaluated as answer accuracy. In the current EverOS config:

```text
evaluation:
  type: llm_judge
  num_runs: 3
  filter_category: [5]
```

For each question, the answer model generates an answer from Attemory's selected
context. The judge model compares the generated answer with the gold answer.
The judge is run three times, and the final score is the mean accuracy across
those runs.

## How to run

Prepare the pinned EverOS checkout and apply the Attemory adapter patch:

```bash
cd benchmarks
./prepare_bench.sh locomo
```

Create the EverOS environment:

```bash
cd EverOS/methods/EverCore
uv venv --python 3.12
source .venv/bin/activate
uv sync --group evaluation
```

Start an Attemory server separately, then configure the local retrieval endpoint:

```bash
export ATTEMORY_HOST=127.0.0.1
export ATTEMORY_PORT=9006
```

LoCoMo is an end-to-end QA benchmark, so the answer and judge stages need an
OpenAI-compatible LLM endpoint:

```bash
export LLM_API_KEY=<your-api-key>
export LLM_BASE_URL=https://openrouter.ai/api/v1
```

`LLM_BASE_URL` can point to any compatible local or remote endpoint. The
Attemory retrieval system itself connects only to the local Attemory server.

Run the benchmark commands from:

```text
benchmarks/EverOS/methods/EverCore
```

Smoke test:

```bash
uv run python -m evaluation.cli --dataset locomo --system attemory --smoke
```

Full run:

```bash
uv run python -m evaluation.cli --dataset locomo --system attemory
```

Run selected stages:

```bash
uv run python -m evaluation.cli --dataset locomo --system attemory \
    --stages search answer evaluate
```

Use a named output directory:

```bash
uv run python -m evaluation.cli --dataset locomo --system attemory \
    --run-name experiment1
```

Results are written under:

```text
evaluation/results/locomo-attemory/
```

The most useful files are:

```text
search_results.json   retrieved dialogue turns and selected context
answer_results.json   generated answers and answer context
eval_results.json     judge results and aggregate metrics
report.txt            summary report
```
