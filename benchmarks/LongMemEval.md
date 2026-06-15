# Attemory on LongMemEval

This note describes the retrieval method implemented in
`LongMemEval/src/retrieval/attemory_adapter.py`.

The goal is to evaluate Attemory on the retrieval portion of LongMemEval. Each
LongMemEval sample contains one query and a haystack of dated conversation
sessions. The adapter builds one temporary Attemory session per sample, retrieves
dialogue turns, maps those turns back to LongMemEval corpus ids, and reports both
session-level and turn-level recall metrics.

## How to run

Prepare the pinned LongMemEval checkout and apply the Attemory adapter patch:

```bash
cd benchmarks
./prepare_bench.sh longmemeval
```

Create a Python 3.12 environment. Python 3.12 is recommended because
LongMemEval's lite requirements pin `numpy==1.26.3`, which does not provide
wheels for newer Python versions.

```bash
cd LongMemEval
uv venv --python 3.12
source .venv/bin/activate
uv pip install -r requirements-lite.txt "httpx<0.28"
```

Start an Attemory server with large tier separately, then run retrieval from LongMemEval's
retrieval directory:

```bash
cd src/retrieval
export ATTEMORY_HOST=127.0.0.1
export ATTEMORY_PORT=9006

# run s split
bash run_retrieval.sh ../../data/longmemeval_s_cleaned.json attemory session

# or run m split
bash run_retrieval.sh ../../data/longmemeval_m_cleaned.json attemory session
```

The output is written under:

```text
benchmarks/LongMemEval/retrieval_logs/attemory/session/
```

The final result files are under the directory:

```text
benchmarks/LongMemEval/retrieval_logs/
```

Useful environment variables:

```text
ATTEMORY_HOST     default: 127.0.0.1
ATTEMORY_PORT     default: 9006
ATTEMORY_TIMEOUT  default: 3600
ATTEMORY_WORKERS  default: 1
```

## 1. Supported Granularity

The adapter only supports LongMemEval `session` granularity.

This does not mean every Attemory memory is a whole session. The memory unit is
still a dialogue turn. The `session` granularity means the official result is
primarily interpreted as session retrieval: retrieved turns are mapped back to
their parent session ids and session recall is computed from that ranking.

The adapter also reports a turn-level metric as additional diagnostics. That
turn metric is not the same as LongMemEval's built-in `turn` granularity.
Attemory's method is closer to a strict turn-level retriever: every dialogue
turn is retrievable, including both user turns and assistant turns. Even when
the command-line granularity is `session`, Attemory first retrieves turns and
then maps those turns back to their parent sessions.

Official LongMemEval `turn` mode only indexes user turns and drops assistant
turns. Attemory therefore does not use that path. It runs the official harness
in `session` granularity, but internally retrieves complete dialogue turns and
reports the main metric after mapping the turn ranking back to sessions.

## 2. Skip Rules

The cleaned LongMemEval files contain 500 questions, including 30 abstention
questions whose `question_id` contains `_abs`. The official retrieval script
intentionally excludes these 30 `_abs` questions from retrieval metrics.
Attemory follows the same rule, so the reported retrieval result is averaged
over the remaining 470 questions.

## 3. Corpus Construction

For each LongMemEval sample, the adapter builds a linear corpus while preserving
the original session structure.

Input fields used:

```text
entry["haystack_session_ids"]
entry["haystack_sessions"]
entry["haystack_dates"]
entry["question"]
entry["question_date"]
entry["answer_session_ids"]
```

For each haystack session, the adapter appends:

```text
session date header
<user>: ...
<assistant>: ...
<user>: ...
...
```

The session header text is:

```text
**The following conversations take place at {timestamp}
```

Each dialogue turn is formatted as:

```text
<{role}>: {content}
```

Both user and assistant turns are included. Each turn is one retrievable memory.
The session header is context only.

## 4. Corpus Ids

The adapter uses corpus indices as Attemory memory ids.

For a dialogue turn:

```text
Attemory memory id = str(corpus_index)
```

That makes result mapping direct:

```text
SearchResult.id -> int id -> corpus[idx]
```

LongMemEval corpus ids are built separately for metrics:

```text
{session_id}_{turn_id + 1}
```

If the session id contains `"answer"` but a turn does not have
`has_answer=True`, the adapter replaces `"answer"` with `"noans"` in that corpus
id. This prevents non-answer turns from answer sessions from being counted as
correct turn-level evidence.

Headers also get corpus ids, but if the session id contains `"answer"` the
header id is rewritten to `"noans"` as well. Headers are never added to Attemory
with a memory id, so they are not returned as valid retrieved turns.

## 5. Attemory Session Per Question

LongMemEval has one question per sample. The adapter therefore creates a fresh
temporary Attemory session for each sample:

```text
lme-{question_id}-{granularity}
```

The id is sanitized so it only contains alphanumeric characters plus `.`, `_`,
and `-`.

The session is deleted before use if it already exists, and it is deleted again
in a `finally` block after retrieval. The adapter does not persist LongMemEval
sessions across questions.

## 6. Index Layout

For each temporary session, the adapter calls:

```text
create_session()
add_system(ATTEMORY_SYSTEM_PROMPT)
```

The system prompt is:

```text
Read the following conversations carefully and answer the query at the end.
```

Then it adds memories in original session order.

For each LongMemEval session:

1. If remaining tokens are below `5000`, call `next_segment()` before starting
   the next LongMemEval session.
2. Add the session date header without an id.
3. Add each dialogue turn in that session with id `str(corpus_index)`.

The resulting Attemory context looks like:

```text
system prompt

**The following conversations take place at 2023/...
<user>: ...
<assistant>: ...

**The following conversations take place at 2023/...
<user>: ...
<assistant>: ...
...
```

Headers have no ids. Dialogue turns have ids. This means headers can provide date
context to Attemory, but only turns participate in the final ranking.

After all memories are added, the adapter calls:

```text
index_session()
```

It does not call `save_session()` because each session has one query only.

## 7. Query and Query Context

The query text passed to Attemory is:

```text
Query: {question}
```

The query context is:

```text
**Read above selected conversation carefully and answer the user's query: {question}. query's date/time: {question_date}

Find prior user-stated evidence that should guide a personalized answer to the query. Focus on facts, preferences, constraints, named entities, places, dates, times, events, activities, possessions, services, products, plans, outcomes, past experiences, relationships, and life-stage context. For advice or recommendation queries, prefer conversations that reveal the user's relevant context, not only conversations that already contain advice. Do not require exact keyword overlap: a relevant conversation may reveal evidence in the same personal domain, such as current items, intended upgrades, old friends, school/work history, or meaningful memories. Use assistant messages only as supporting evidence when they clarify user facts. For recent/latest/last questions, prefer the newest direct evidence before the current date/time, which is {question_date}
```

The query context is designed for LongMemEval's conversational memory setting.
It tells the attemory to retrieve evidence needed to answer a personal question,
not just literal keyword matches.

## 8. Initial Search

The adapter calls Attemory search as:

```text
session_client.search(query, query_context=query_context)
```

The adapter reads each result:

```text
result.id
result.segment_id
```

Valid results are those whose id can be parsed as an integer corpus index.
Headers have no id, so they are ignored.

The first valid occurrence of each corpus index is kept. Duplicate ids are
deduplicated while preserving result order.

The adapter also records the set of returned `segment_id`s. If all returned
results come from one segment, the initial search order is used directly. If
multiple segments appear, the adapter runs an additional one-shot rerank.

## 9. One-Shot Search for Multi-Segment Results

If the initial search returns valid ids from multiple Attemory segments, the
adapter calls `oneshot_search` on the selected turns only.

The one-shot system prompt is:

```text
Read the following selected conversations carefully and find the most relavent conversations for the user's query.
```

Before the selected conversations, the adapter inserts a pre-memory without id:

```text
The user's query: {question}. query's date/time: {question_date}.

Find conversations with direct evidence for the query. Focus on user-stated facts, named entities, category inference, and usage evidence: people, places, services, products, dates, times, events, activities, preferences, and outcomes. Use assistant messages only as supporting evidence when they clarify user facts. For recent/latest/last questions, prefer the newest direct evidence before the current date/time.

While reading, decide whether each conversation contains direct user evidence for the query, not just a related topic.
```

Then it rebuilds the selected turns under their original session headers.

The one-shot input layout is:

```text
pre-memory instruction

**The following conversations take place at date A
selected turn from date A
selected turn from date A

**The following conversations take place at date B
selected turn from date B
...
```

Important details:

1. Only turns selected by the previous search/rerank round are included.
2. Sessions with no selected turns are omitted.
3. Session order remains the original LongMemEval haystack order.
4. Turn order within each session remains chronological.
5. Session headers and the pre-memory have no ids.
6. Selected dialogue turns keep their original corpus-index ids.

The one-shot call is:

```text
session_client.oneshot_search(
    query,
    oneshot_memories,
    system=ATTEMORY_ONESHOT_SYSTEM_PROMPT,
    query_context=query_context,
    top_k=oneshot_top_k,
)
```

`oneshot_top_k` is computed as:

```text
min(len(oneshot_memories), number_of_no_id_memories + 50)
```

The `+ 50` matches the evaluation top-k. The no-id memories are counted because
pre-memory and headers may be returned by Attemory but are filtered out before
metrics. Asking for `number_of_no_id_memories + 50` keeps enough room for roughly
50 valid dialogue turns after filtering.

The one-shot rerank can run up to two rounds. If the one-shot result still spans
multiple segments, the adapter rebuilds a smaller one-shot input from that
round's selected ids and runs it again. If the result fits in one segment, it
stops early.

## 10. Merging Search and One-Shot Results

If one-shot rerank was used, the adapter makes one final turn-id order:

1. Add valid ids from the one-shot result in returned order.
2. Append valid ids from the original search result that were not already added.

This preserves one-shot as the stronger global rerank, while keeping original
search candidates as fallback.

If one-shot was not used, the initial search ids are the ranking.

## 11. Complete Ranking for LongMemEval

LongMemEval's evaluator expects a complete permutation over the corpus.
Attemory only returns selected memories.

The adapter therefore builds:

```text
final_rankings = retrieved ids first + all unseen corpus indices afterward
```

Only valid in-range corpus indices are accepted from Attemory results. Any corpus
item not retrieved is appended in original corpus order.

The returned value is:

```text
np.asarray(final_rankings, dtype=np.int64)
```

## 12. Turn Metrics

The adapter computes a diagnostic turn-level metric from the final ranking.

Correct turn ids are:

```text
corpus_ids that contain "answer"
```

Because non-answer turns inside answer sessions were rewritten to `"noans"`,
this set corresponds to labeled answer turns.

For each `k` in:

```text
1, 3, 5, 10, 30, 50
```

the adapter computes:

```text
recall_any@k
recall_all@k
ndcg_any@k
```

Turn recall is computed directly over the ranked corpus indices.

## 13. Session Metrics

Session metrics are computed from the same final turn ranking, but session ids
are de-duplicated first.

The procedure is:

1. Walk the final ranked corpus indices.
2. Map each index to `session_ids[idx]`.
3. Keep only the first occurrence of each session id.
4. Stop once `k` unique sessions have been collected.

Then compare that de-duplicated session ranking against:

```text
entry["answer_session_ids"]
```

For each `k` in:

```text
1, 3, 5, 10, 30, 50
```

the adapter computes:

```text
recall_any@k
recall_all@k
ndcg_any@k
```

This is the main metric for the current Attemory LongMemEval adapter.

## 14. Compatibility With Official LongMemEval Retrieval

The adapter is intentionally wired into the official
`src/retrieval/run_retrieval.py` harness, so result files and aggregate metric
names follow the official retrieval pipeline. The following pieces are aligned
with the official script:

1. The same input fields are used: question, question date, haystack sessions,
   haystack dates, haystack session ids, and answer session ids.
2. The same metric names are reported: `recall_any@k`, `recall_all@k`, and
   `ndcg_any@k` for `k = 1, 3, 5, 10, 30, 50`.
3. The 30 `_abs` abstention questions are excluded from averaged retrieval
   metrics, so results are reported over 470 questions, matching the official
   retrieval script.
4. `recall_any@k` and `recall_all@k` keep the official all-or-nothing meaning:
   `recall_any` is 1 if any target is retrieved in top `k`, and `recall_all` is
   1 only if every target is retrieved in top `k`.

There are also some method differences from the official built-in retrievers.

1. Official `session` mode indexes one item per session. The text is the
   concatenation of user messages in that session. Attemory indexes each
   dialogue turn as one memory and adds a no-id date header before each session.
   Both user and assistant turns are included.
2. Official `turn` mode indexes only user turns. Attemory's diagnostic turn
   metric indexes both user and assistant turns, so it is not directly
   comparable to an official `turn` run.
3. Official `session` mode ranks session corpus items directly. Attemory ranks
   turns first, then converts the turn ranking into a de-duplicated session
   ranking by taking the first occurrence of each parent session.

