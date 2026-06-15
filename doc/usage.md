---
title: Attemory Usage
---

# Attemory Usage

Attemory is a local attention-native memory retrieval service. The Python module
is the primary user-facing interface: it starts the local server, talks to the
HTTP API, manages sessions, and exposes memory search to applications,
command-line workflows, and agents.

This guide describes the public Python-facing usage surface:

- `attemory-server`: starts the local Attemory server.
- `attemory.AttemoryClient`: Python API for applications.
- `attemory`: command-line client.

## Basic Concepts

Attemory stores memory in sessions and uses attention over KV state to retrieve
the memories most relevant to a query.

| Concept | Meaning |
| --- | --- |
| Session | A named memory space, usually one user, project, repository, or agent memory. |
| Memory | One text item you add to a session. A memory can carry an optional client-defined `id`; Attemory stores and returns it but does not enforce uniqueness. Memory text is limited to 4 MB, and `id` is limited to 4096 bytes. |
| Segment | A group of memories that fits within the active model context. Large sessions are split into segments internally; current Attemory tiers use Qwen3.5 models with a 262,144-token context window. |
| Index | The step that builds searchable KV state for a session or segment. Search is fast only after indexing. |
| Search | Retrieval over an indexed session. If the session has multiple segments, it returns per-segment ranked memories. |
| One-shot search | Retrieval over a supplied memory list without creating or saving a persistent session. It is useful when the candidate set is temporary. |
| Save session | Persist an indexed session and its reusable cache state to disk so it can be restored later. |
| Restore session | Load a previously saved session back into the running server before searching or updating it. |

A typical persistent workflow is:

1. Create a session.
2. Add memories.
3. Index the session.
4. Search with a query and optional `query_context`.
5. Optionally save the session for later reuse.

If a saved session is not currently loaded, restore it before searching or
updating it.

Indexing may be expensive because it builds KV state through model prefill.
Search is fast after indexing because the reusable KV cache is already built.

## Installation

Install Attemory on macOS Apple Silicon:

```bash
uv pip install attemory
```

The default macOS install includes the Metal-capable runtime. It supports both
`--backend metal` and `--backend cpu`.

Install Attemory on Linux CPU:

```bash
uv pip install "attemory[cpu]"
```

For Linux NVIDIA GPUs:

```bash
uv pip install "attemory[cuda]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu126/
```

The same extras work with `pip`:

```bash
pip install attemory
pip install "attemory[cpu]"
pip install "attemory[cuda]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu126/
```

Specific CUDA runtime packages are available when you need to match a GPU or
driver fleet explicitly:

```bash
pip install "attemory[cuda-cu121]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu121/

pip install "attemory[cuda-cu124]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu124/

pip install "attemory[cuda-cu126]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu126/

pip install "attemory[cuda-cu129]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu129/
```

Use:

| Extra | Platform |
| --- | --- |
| `cpu` | Portable Linux CPU runtime. |
| `cuda` | Recommended Linux NVIDIA GPU runtime. Currently resolves to `cuda-cu126`. |
| `cuda-cu121`, `cuda-cu124`, `cuda-cu126`, `cuda-cu129` | Specific Linux NVIDIA GPU runtimes for CUDA 12.1, 12.4, 12.6, and 12.9. Use one when you need to match a driver/runtime requirement explicitly. |

On Linux, a bare `attemory` install only installs the Python package and does
not install a native runtime. Choose `cpu` or a CUDA extra explicitly.

Most Linux GPU users should install `attemory[cuda]`, which currently resolves
to `cuda-cu126`, together with
`--extra-index-url https://attemorysystem.github.io/Attemory/whl/cu126/`. Use
`cuda-cu129` for Blackwell GPUs such as RTX 50 series; it includes native
`sm_120` kernels and targets newer Linux systems with glibc 2.28 or later.
Choose `cuda-cu124` or `cuda-cu121` only when your NVIDIA driver is too old for
CUDA 12.6.

## Starting The Server

Start a local server with a model tier:

```bash
attemory-server --small --backend gpu --port 9006
```

Built-in model tiers:

| Tier | Model | Recommended starting point |
| --- | --- | --- |
| `--tiny` | Qwen3.5-0.8B-Q8_0 | CPU or very small local tests; no VRAM required on CPU. |
| `--small` | Qwen3.5-2B-Q8_0 | Linux GPU with roughly 5 GB or more VRAM. |
| `--medium` | Qwen3.5-4B-Q8_0 | Linux GPU with roughly 8 GB or more VRAM. |
| `--large` | Qwen3.5-9B-Q8_0 | Linux GPU with roughly 12 GB or more VRAM. |

These are practical starting points, not hard limits. Peak memory depends on
backend, context length, `--kv-type`, active segment size, and how much KV is
resident in memory.

When you start with `--tiny`, `--small`, `--medium`, or `--large`, the Python
wrapper resolves the tier to a Hugging Face model, downloads it if needed, and
passes the local model path to the native server.

Model downloads use the standard Hugging Face Hub cache:

| Variable | Purpose |
| --- | --- |
| `HF_HUB_CACHE` | Hugging Face hub cache root. |
| `HF_HOME` | Hugging Face home directory; hub cache is read from `$HF_HOME/hub`. |
| `HF_TOKEN` or `HUGGING_FACE_HUB_TOKEN` | Token for private Hugging Face downloads. |
| `HF_HUB_OFFLINE=1` | Disable network access and require cached model files. |
| `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY` | Proxy settings used by model downloads; lowercase variants are also honored. |
| `NO_PROXY` | Hosts that should bypass the proxy; lowercase variant is also honored. |

Use Hugging Face tooling to pre-download, move, or share model files. When a
tier model is already present in the Hugging Face cache, `attemory-server`
reuses it instead of downloading again.

On macOS Metal:

```bash
attemory-server --small --backend metal --port 9006
```

On CPU:

```bash
attemory-server --small --backend cpu --port 9006
```

By default, Attemory stores KV as `q4_0`. Use `--kv-type` when you need to
override the KV tensor type:

```bash
attemory-server --small --backend gpu --kv-type q8_0 --port 9006
```

Set cache and default search result limits when needed:

```bash
attemory-server \
  --small \
  --backend gpu \
  --kv-type q4_0 \
  --cache-dir ~/.cache/attemory/cache \
  --search-top-k 20
```

Approximate KV cache footprint with `--kv-type q4_0`:

| Tier | Approximate KV cache size |
| --- | --- |
| `tiny` | 2 KB per indexed token. |
| `small` | 2 KB per indexed token. |
| `medium` | 7 KB per indexed token. |
| `large` | 7 KB per indexed token. |

Disk cache can grow quickly for large sessions. Clear or rotate `--cache-dir`
when old indexed sessions are no longer needed.

The HTTP server intentionally uses one worker thread. Long-running indexing or
search requests block other API calls until they finish; run separate server
processes when workloads need isolation.

## Server Options

`attemory-server` accepts these common options:

| Option | Meaning |
| --- | --- |
| `--tiny`, `--small`, `--medium`, `--large` | built-in model tier. |
| `--backend gpu`, `--backend cpu`, `--backend metal` | Select runtime backend. Linux uses `gpu` or `cpu`; macOS uses `metal` or `cpu`. |
| `--host HOST` | Bind address. Default is `127.0.0.1`. |
| `--port PORT` or `-p PORT` | HTTP port. Default is `9006`. |
| `--cache-dir DIR` | KV cache directory. |
| `--model PATH` or `-m PATH` | Use an already downloaded local GGUF model file instead of resolving and downloading a built-in tier model. Only Q8_0 GGUF models are currently tested. |
| `--kv-type TYPE` | Override both KV tensor types. Default is `q4_0`. |
| `--search-top-k N` | Default number of results returned from each segment. |
| `--search-candidate-top-k N` | Used inside attention search ranking. |
| `--http-log`, `--no-http-log` | Enable or disable HTTP request logs. Disabled by default. |
| `--run-log`, `--no-run-log` | Enable or disable runtime logs, including core runtime logs. Disabled by default. |

On multi-GPU Linux hosts, expose one GPU to the process with CUDA:

```bash
CUDA_VISIBLE_DEVICES=1 attemory-server --small --backend gpu
```

Attemory is currently optimized for a single active request on one GPU. Setting
multiple visible GPU ids is not useful for the current runtime.

## Python Client

Import the public API:

```python
from attemory import AttemoryClient, MemoryInput
```

Create a client:

```python
client = AttemoryClient(
    host="127.0.0.1",
    port=9006,
    session_id="demo",
    timeout=3600.0,
)
```

You can bind a new client to another session:

```python
project = client.with_session("project-alpha")
```

Most session methods use the client's default `session_id`, but every method
also accepts `session_id=...` explicitly.

Session ids must be non-empty and may contain only letters, digits, `.`, `_`,
and `-`. The reserved path components `.` and `..` are rejected.

## Persistent Session Workflow

A persistent session stores memories, builds searchable KV state, and can be
saved to disk for later use and search.

```python
from attemory import AttemoryClient, MemoryInput

client = AttemoryClient(port=9006, session_id="demo")

client.create_session()
client.add_system(
    "Read the following memories carefully and find the most relevant memory to the query."
)
client.add_memory(
    MemoryInput(
        id="user-pref-1",
        text="Alice prefers concise status updates with concrete next steps.",
    )
)
client.add_memory(
    MemoryInput(
        id="deploy-1",
        text="The deployment target is a single local service behind an internal proxy.",
    )
)

client.index_session()

# The memories are searchable after indexing. Save when the session should
# survive server restart.
client.save_session()

results = client.search("How should I write updates for Alice?")
for result in results:
    print(result.rank, result.id, result.text)

```

For a session with multiple segments, `search()` returns per-segment results.
Use the iterative context filtering pattern below when a large context needs
additional passes.

Core methods:

| Method | Purpose |
| --- | --- |
| `health()` | Check server health. |
| `list_sessions()` | Return all the sessions and their status. |
| `create_session(session_id=None)` | Create a session. |
| `delete_session(session_id=None)` | Delete a session. |
| `add_system(text, session_id=None)` | Add the system prompt used for memory reading. |
| `add_memory(memory, session_id=None, id=None)` | Add one memory string, mapping, or `MemoryInput`. |
| `next_segment(session_id=None)` | Seal the current segment and start a new one. |
| `index_session(session_id=None)` | Build searchable KV state for the session. |
| `save_session(session_id=None)` | Persist session metadata and KV cache state. |
| `restore_session(session_id=None)` | Load session metadata into the server. |
| `clear_cache(session_id=None)` | Clear resident KV cache for a loaded session. |
| `search(query, session_id=None, query_context=None, top_k=None)` | Return ranked memories. |
| `oneshot_search(...)` | Search a provided in-memory candidate set. |

### Session Status

`list_sessions()` returns `SessionStatus` objects:

| Field | Meaning |
| --- | --- |
| `session_id` | Session id. |
| `memory_count` | Number of memories in the session. |
| `segment_count` | Number of planned segments. |
| `total_tokens` | Sum of segment prefill tokens in the session. |
| `resident_segments` | Segments currently resident in memory. |
| `indexed_segments` | Segments with built KV search state. |
| `saved_segments` | Segments with persisted KV cache state. |
| `indexed` | True when all required segments are indexed. |
| `disk_cached` | True when disk cache is available. |
| `plan_ready` | True when the session segment plan is ready. |
| `facts_dirty` | True when session metadata changed after the last save. |

Example:

```python
for status in client.list_sessions():
    print(status.session_id, status.indexed, status.disk_cached)
```

## Search

`search()` is the primary retrieval API for applications. 

Only indexed sessions can be searched. After adding memories or otherwise
updating a session, call `index_session()` again before searching.

`search()` returns ranked `SearchResult` objects:

```python
results = client.search(
    "Find the memory about the deployment target.",
    top_k=20,
)

for result in results:
    print(result.rank, result.id, result.memory_idx, result.segment_id, result.text)
```

`top_k` is per segment. If a session has 5 segments and `top_k=20`, search may
return up to 100 results. Treat those results as the candidate context for the
next oneshot search pass when the context is still too large.

`SearchResult` fields:

| Field | Meaning |
| --- | --- |
| `rank` | One-based rank in the returned result list. |
| `text` | Retrieved memory text. |
| `id` | Optional client-provided memory id. |
| `memory_idx` | Internal memory index in the session. |
| `segment_id` | Segment that produced the result. |

## One-Shot Search

One-shot search ranks a candidate set without creating a persistent session.
It is commonly used after `search()` returns results from multiple segments and
the application needs one global ranking over the candidate memories.

```python
from attemory import AttemoryClient, MemoryInput

client = AttemoryClient(port=9006)

segment_results = client.search(
    "Which memory is most relevant to deployment?",
    session_id="demo",
    top_k=20,
)

candidate_memories = [
    MemoryInput(id=result.id, text=result.text)
    for result in segment_results
    if result.id is not None
]

reranked_results = client.oneshot_search(
    "Which memory is most relevant to deployment?",
    candidate_memories,
    top_k=10,
)
```

One-shot KV state is temporary and cannot be saved. If the supplied memories
exceed the model context, Attemory splits them into internal segments and
returns per-segment results. Treat those results the same way as persistent
search results: they are candidates for another context filtering pass.

## The Context Structure

Attemory retrieval can be treated as template-driven. It does not retrieve by
comparing a query to isolated strings; it retrieves by placing memories into a
context template, running attention over that context, and selecting the
memories that receive the strongest attention from the final query.

The template is part of the retrieval system. It decides where the system
prompt, memory text, memory-local metadata, `query_context`, and final query are
placed:

1. Use the system prompt to describe what kind of evidence should be retrieved.
2. Each memory can use stable fields and separators for memory-local context when useful, or store the full text directly.
3. Add request-time facts through `query_context`.
4. Search with a final query.

Only memory items, including any memory-local context embedded in them, are
retrieval candidates. The system prompt and `query_context` shape how memories
are read and scored, but they are not returned as `SearchResult` objects unless
you also add the same text as a normal memory.

```text
+--------------------------------------------------------------+
| # System prompt                                              |
| Read the conversations carefully and find relevant memory.   |
+--------------------------------------------------------------+
| # Memory candidates                                          |
|                                                              |
| user: ...                                                    |
| assistant: ...                                               |
|                                                              |
| user: ...                                                    |
| assistant: ...                                               |
+--------------------------------------------------------------+
| # Final query                                                |
| Query: What does ...                                         |
+--------------------------------------------------------------+
```

For conversations, timestamps and other memory-local context often matter. Put
that context inside the memory text, close to the content it describes, and keep
the format stable across retrieval passes:

```text
+--------------------------------------------------------------+
| # System prompt                                              |
| Read the conversations carefully and find relevant memory.   |
+--------------------------------------------------------------+
| # Memory candidates                                          |
| +----------------------------------------------------------+ |
| | # Memory-local context                                   | |
| | Conversation date: 2026-04-21                            | |
| +----------------------------------------------------------+ |
| | user: ...                                                | |
| | assistant: ...                                           | |
| +----------------------------------------------------------+ |
|                                                              |
| +----------------------------------------------------------+ |
| | # Memory-local context                                   | |
| | Conversation date: 2026-05-03                            | |
| +----------------------------------------------------------+ |
| | user: ...                                                | |
| | assistant: ...                                           | |
| +----------------------------------------------------------+ |
+--------------------------------------------------------------+
| # Final query                                                |
| Query: What does ...                                         |
+--------------------------------------------------------------+
```

When the query has long background information or request-time facts, use
`query_context` instead of appending everything to the query string. Attemory
places `query_context` between the memories and the query. It helps the model
interpret the question, but only the final query tokens are used for attention
ranking. `query_context` itself is not a candidate memory and is not returned.

```text
+--------------------------------------------------------------+
| # System prompt                                              |
| Read the conversations carefully and find relevant memory.   |
+--------------------------------------------------------------+
| # Memory candidates                                          |
|                                                              |
| Conversation date: 2026-04-21                                |
| user: ...                                                    |
| assistant: ...                                               |
+--------------------------------------------------------------+
| # Query context                                              |
| Focus on facts, people, places, dates, and user preferences. |
| Current time: {query_time}                                   |
+--------------------------------------------------------------+
| # Final query                                                |
| Query: What does ...                                         |
+--------------------------------------------------------------+
```

For complex retrieval tasks, it can help to repeat the question in
`query_context`, add retrieval guidance, and then keep the final query concise:

```text
+--------------------------------------------------------------+
| # System prompt                                              |
| Read the conversations carefully and find relevant memory.   |
+--------------------------------------------------------------+
| Memory candidates                                            |
|                                                              |
| Conversation date: 2026-04-21                                |
| user: ...                                                    |
| assistant: ...                                               |
+--------------------------------------------------------------+
| # Query context                                              |
| Read the conversations above and answer this question:       |
| {query}                                                      |
|                                                              |
| Focus on facts, people, places, dates, and user preferences. |
| Current time: {query_time}                                   |
+--------------------------------------------------------------+
| # Final query                                                |
| Query: {query}                                               |
+--------------------------------------------------------------+
```

This is the retrieval version of a repeat-prompt pattern: the model receives
the task framing before the final query, and the final query still anchors the
attention score.

### Iterative Context filtering

For very large contexts, retrieval is an iterative context refinement process:

1. Render raw memory blocks with a stable template.
2. Search the rendered context and keep the highest-ranked memories.
3. Render the selected memories again with the same template.
4. Search the smaller rendered context again.
5. Repeat until the remaining context fits within one model context and the
   results from one segment can be treated as a global ranking.

The template and filtering rule both matter: the template controls what the
model can recognize as useful evidence, and the filtering rule controls how
aggressively each pass narrows the context.

```text
+-------------------------------+
| Pass 1 context                |
| system prompt                 |
| raw memories                  |
| query_context                 |
| query                         |
+-------------------------------+
                |
                v
+-------------------------------+
| Search and keep top memories  |
+-------------------------------+
                |
                v
+-------------------------------+
| Pass 2 context                |
| system prompt                 |
| selected memories             |
| same query_context            |
| same query                    |
+-------------------------------+
                |
                v
+-------------------------------+
| Search and keep fewer items   |
+-------------------------------+
                |
                v
+-------------------------------+
| Final context                 |
| system prompt                 |
| selected memories(fit one seg)|
| same query_context            |
| same query                    |
+-------------------------------+
                |
                v
+-------------------------------+
| Final retrieved memories      |
+-------------------------------+
```

Memory-local context, such as session headers, code filename hints,
query-aware helper records, and other structured hints, can appear in returned
results when they are added through `add_memory()` or one-shot `memories`,
because they are part of the candidate memory list and participate in attention.
If helper memories are not meant to be shown to users, give them reserved ids
and filter them out in application code. The system prompt and `query_context`
also participate in prefill, but they are not memory items and are not returned
as `SearchResult` objects.

## Data Models

The Python package exposes dataclasses for structured results:

| Class | Purpose |
| --- | --- |
| `MemoryInput` | Input object for `add_memory()` and one-shot search. |
| `TokenUsage` | Token and segment usage returned by create/add operations. |
| `SessionStatus` | Loaded session state returned by `list_sessions()`. |
| `SearchResult` | Ranked memory returned by `search()` and one-shot search. |

All dataclasses can be converted to JSON-compatible values with:

```python
from attemory.models import to_jsonable

payload = to_jsonable(results)
```

## Errors

The public exception hierarchy is:

| Exception | Meaning |
| --- | --- |
| `AttemoryError` | Base class for Python client errors. |
| `AttemoryHTTPError` | Raised for non-2xx server responses. |
| `AttemoryResponseError` | Raised when a successful server response has an unexpected shape. |

Example:

```python
from attemory import AttemoryClient, AttemoryHTTPError

client = AttemoryClient(port=9006, session_id="demo")

try:
    client.search("query", top_k=20)
except AttemoryHTTPError as exc:
    print(exc.status_code, exc.error_code, exc.error_message)
```

## Command-Line Client

The `attemory` command wraps `AttemoryClient` and prints JSON envelopes. It is
kept mainly for debugging, smoke tests, and manual session inspection. For
applications and production integrations, prefer the Python API so errors,
timeouts, retries, and result objects stay explicit in your code.

Global options:

| Option | Environment fallback |
| --- | --- |
| `--host HOST` | `ATTEMORY_HOST`, default `127.0.0.1`. |
| `--port PORT` | `ATTEMORY_PORT`, default `9006`. |
| `--timeout SECONDS` | `ATTEMORY_TIMEOUT`, default `3600`. |
| `--session SESSION` | `ATTEMORY_SESSION`. |
| `--compact-json` | None. |

Examples:

```bash
attemory --session demo health
attemory --session demo create
attemory --session demo add-system --text "Read memories carefully."
attemory --session demo add-memory --id pref-1 --text "Alice likes concise updates."
attemory --session demo add-memory --id file-1 --file memory.txt
attemory --session demo index
attemory --session demo search --text "How should I update Alice?" --top-k 20
attemory --session demo save
```

If neither `--text` nor `--file` is passed to a text command, the CLI reads
UTF-8 text from standard input:

```bash
cat memory.txt | attemory --session demo add-memory --id long-note
```

CLI commands:

| Command | Purpose |
| --- | --- |
| `health` | Check server health. |
| `sessions` | List loaded sessions. |
| `create [session_id]` | Create a session. |
| `delete [session_id]` | Delete a loaded session. |
| `restore [session_id]` | Restore a persisted session. |
| `clear-cache [session_id]` | Clear resident KV cache. |
| `index [session_id]` | Build searchable KV state. |
| `save [session_id]` | Persist session and cache state. |
| `next-segment [session_id]` | Seal the current segment. |
| `add-system [session_id]` | Add a system prompt. |
| `add-memory [session_id]` | Add memory text with optional `--id`. |
| `search [session_id]` | Run the search command. |
| `oneshot-search` | Search a provided memory set without a session. |

One-shot search from the CLI:

```bash
attemory oneshot-search \
  --query "Which memory mentions deployment?" \
  --query-context "Current date: 2026-06-04" \
  --memory "The service is behind an internal proxy." \
  --memory "Alice likes concise updates." \
  --top-k 1
```

Using a JSON memory file:

```json
{
  "memories": [
    {"id": "m1", "text": "The service is behind an internal proxy."},
    {"id": "m2", "text": "Alice likes concise updates."}
  ]
}
```

```bash
attemory oneshot-search \
  --query "Which memory mentions deployment?" \
  --memories-json memories.json \
  --top-k 5
```
