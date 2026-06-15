# Attemory on Semble

This note describes the retrieval method used by
`benchmarks/semble/benchmarks/baselines/attemory.py`.

The goal of the script is to evaluate Attemory as a code retrieval system on the
Semble benchmark. Semble asks a natural-language or symbol-like query and
expects a ranked list of source files. Attemory retrieves code chunks, and the
script fuses chunk hits back into a file ranking for NDCG@10.

## How to run

Prepare the pinned Semble checkout and apply the Attemory benchmark adapter:

```bash
cd benchmarks
./prepare_bench.sh semble
```

Create the benchmark environment:

```bash
cd semble
uv venv --python 3.12
source .venv/bin/activate
uv pip install -e ".[benchmark]"
```

Start an Attemory server separately, then run a small smoke test:

```bash
export ATTEMORY_ROOT="$(realpath ../..)"
export ATTEMORY_HOST=127.0.0.1
export ATTEMORY_PORT=9006

python -m benchmarks.baselines.attemory \
    --repo click \
    --search-topk 20 \
    --output output/attemory-click-smoke.json
```

Run the full benchmark:

```bash
python -m benchmarks.baselines.attemory \
    --search-topk 20 \
    --output output/tune-query-context-v6-full.json
```

The first run builds or restores Attemory sessions for each repo. Existing
indexes are reused by default. To force a rebuild:

```bash
python -m benchmarks.baselines.attemory \
    --rebuild-index \
    --search-topk 20 \
    --output output/tune-query-context-v6-full.json
```

Useful options:

```text
--repo <name>          run one repo
--search-topk 20       chunk hits kept before file-level fusion
--top-k 10             files used for NDCG@10
--index-root index_map saved Attemory session sidecars
--output <file>        JSON result file; existing entries are reused for resume
```

## 1. Code Files

For each benchmark repo, the script recursively scans source files under the
repo checkout.

Skipped directories:

```text
.git, .hg, .mypy_cache, .pytest_cache, .ruff_cache, .svn, .tox,
.venv, __pycache__, build, dist, node_modules, venv
```

Included files are selected by source-code suffixes, including:

```text
.c, .cc, .cpp, .h, .hpp, .cs, .go, .java, .js, .jsx, .ts, .tsx,
.py, .pyi, .rb, .rs, .scala, .kt, .php, .swift, .lua, .zig,
.hs, .ex, .exs, .sh, .bash, .vue, .svelte, .html, .css, .scss
```

`CMakeLists.txt` is also included explicitly.

Files are sorted by their repo-relative path before chunking and indexing. This
stable order is important because Attemory memory ids are assigned sequentially
and are later used to rebuild the same file/chunk layout for reranking.

## 2. Chunking

Each source file is split into code chunks of roughly 30 lines.

The chunker is intentionally simple and language-independent:

1. Start at the first line of the file.
2. Pick a preferred end line at `start + 30 - 1`.
3. Search for a blank line within 10 lines before or after the preferred end.
4. If blank lines are found, split at the blank line closest to the preferred
   end.
5. If no blank line is found, split exactly at the preferred end.
6. Continue from the next line.

Empty chunks are ignored. Code comments remain part of the chunk text; the chunk
contains exactly the source text from the file line range.

Each chunk gets:

```text
id:         sequential string id, starting from "0"
file:       repo-relative source path
start_line: 1-based start line
end_line:   1-based end line
text:       raw source text for that range
```

The memory text stored in Attemory is only the raw chunk text. File path context
is added separately through file header memories.

## 3. Attemory Index Layout

Each repo maps to one Attemory session:

```text
attemory-semble-{repo}
```

When building a fresh index, the script:

1. Deletes the existing session with that id, if any.
2. Creates a new Attemory session.
3. Adds this system prompt:

```text
Read the following code carefully and find the most relevant code to the query.
```

4. Adds memories in file order.

For every file, the script first adds a file header memory without an id:

```text
// the following code come from path/to/file
```

Then it adds that file's chunks as memories with ids:

```text
id="0", text=<raw code chunk>
id="1", text=<raw code chunk>
...
```

The no-id file header gives Attemory path context, but it is not counted as a
retrievable code chunk. Any search result without an id is filtered out.

To avoid splitting a file across Attemory segments, when a new file begins the
script checks the remaining token budget from the previous `add_memory` result.
If fewer than 5000 tokens remain, it manually calls `next_segment()` before
adding the next file header.

After all memories are added, the script calls:

```text
index_session()
save_session()
```

The saved sidecar files are:

```text
index.json   repo/session metadata
chunks.jsonl chunk id -> file path and line range
```

By default, later runs reuse the sidecar files and call `restore_session()` on
the saved Attemory session.

## 4. Query

For each Semble query, the script sends:

```text
Query: {query}
```

as the search query.

It also sends the following fixed query context:

```text
Read the selected code snippets carefully and answer the user's query: {question}. Treat the query as code navigation. Rank snippets by how directly they define, declare, implement, export, or explain the requested symbol, API, behavior, or feature. Prefer exact identifier matches and concrete implementation or definition code over snippets that only mention, call, wrap, test, or document it. Prefer production source over tests, examples, documentation, and generated files unless the query explicitly asks for them.
```

The important design point is that Semble queries are often code-navigation
queries. Some are long natural-language descriptions, while others are short
symbols or feature phrases. The query context tells the model to rank concrete
source evidence above tests, wrappers, mentions, docs, or examples.

## 5. Search

The first retrieval call is:

```text
session_client.search(
    "Query: {query}",
    query_context=<query context>,
    top_k=<search_topk>,
)
```

`search_topk` is the number of chunk hits kept before file-level fusion. In the
current benchmark runs we use:

```text
--search-topk 20
```

Search results can include file headers, because headers are memories too.
Headers have no id, so they are dropped by:

```text
keep result only if result.id is a known chunk id
```

The remaining chunk ids form the initial chunk ranking.

## 6. Multi-Segment Rerank with `oneshot_search`

If search results come from more than one Attemory segment, the script performs
an additional global rerank with `oneshot_search`.

This is necessary because a normal search can return top results per segment.
`oneshot_search` receives only the returned chunks, rebuilt in the same
file-header template used during indexing.

The rerank prompt is:

```text
Rank the selected code snippets by how directly they answer the query. Prefer snippets that contain the requested source implementation or definition. Prefer production source over tests, examples, documentation, and generated files unless the query explicitly asks for them.
```

The rerank memory template is:

```text
// the following code come from file_a
returned chunk from file_a
returned chunk from file_a

// the following code come from file_b
returned chunk from file_b
...
```

Details:

1. Only chunks returned by the previous search/rerank are included.
2. Files with no returned chunks are omitted.
3. Chunks are restored in original repo index order.
4. A file header is inserted before the first returned chunk from each file.
5. File headers still have no id.
6. Chunk memories keep their original chunk ids.

The oneshot call uses:

```text
session_client.oneshot_search(
    "Query: {query}",
    memories=<rebuilt file-header/chunk template>,
    system=<rerank system prompt>,
    query_context=<query context>,
    top_k=min(len(memories), number_of_headers + search_topk),
)
```

The `number_of_headers + search_topk` top-k is intentional: header memories can
be returned but are filtered out before scoring, so the call asks for enough
items to still keep approximately `search_topk` chunk results after dropping
headers.

If `oneshot_search` still returns results from multiple segments, the script can
repeat this process up to 8 times. In practice this recursively narrows the
candidate set until it fits into one segment or the max depth is reached.

After rerank, valid reranked chunk ids are placed before the previous search
order. Any previous chunk ids not returned by rerank are appended after them, so
the script preserves fallback candidates if rerank returns fewer valid ids.

## 7. Chunk Hits to File Ranking

Semble benchmark evaluates files, not chunks. Attemory returns chunk ids, so the script
maps each chunk id back through `chunks.jsonl`:

```text
chunk id -> repo-relative file path, line range
```

The script keeps the first `search_topk` valid chunk hits, then computes a
file-level score.

Default file aggregation mode is `sum`.

For each ranked chunk hit at rank `r` starting from 1:

```text
score[file] += 1 / log2(r + 1)
```

This means:

1. A high-ranked chunk gives more score than a low-ranked chunk.
2. Multiple chunks from the same file can reinforce that file.
3. A file that appears repeatedly among good chunk hits can outrank a file with
   only one weaker hit.

The final file ranking is sorted by:

```text
(-score[file], first_rank[file], file_path)
```

Tie breakers:

1. Higher fused score first.
2. If scores tie, the file whose first chunk appeared earlier ranks first.
3. If still tied, lexicographic file path order is used for deterministic
   output.

The top `--top-k` files are returned for evaluation. The default Semble metric
uses `--top-k 10`, so the reported score is NDCG@10.

## 8. Evaluation

For each query, Semble provides one or more relevant files.

The script ranks retrieved files and computes NDCG@10:

1. For each relevant target file, find its 1-based rank in the returned file
   list.
2. Build a relevance vector of length 10.
3. Compute DCG with `1 / log2(rank + 1)` discounting.
4. Normalize by ideal DCG for the number of relevant files.

Per-repo NDCG@10 is the average over that repo's queries.

The final benchmark score is the average of per-repo NDCG@10 across all selected
repos.

## 9. Current Full-Run Result

With medium tier attemory server and the query context above and:

```text
python -m benchmarks.baselines.attemory --search-topk 20 --output output/attemory-semble.json
```

the full Semble run produced:

```text
Average NDCG@10: 0.9055
```
