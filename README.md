<p align="center">
  <img src="assets/attemory_logo.png" alt="Attemory" width="320"><br><b>Cut agent token usage with high-recall memory retrieval.</b>
</p>

Attemory is a semantic retrieval engine for long memory, documents, and
codebases. It turns large corpora into model-readable memory and retrieves
relevant evidence by letting a local model attend over that memory, rather than
relying only on keyword matching or embedding similarity.

For AI agents, this means large repositories and long histories can be indexed
once, then searched before the expensive model starts its own exploration.
Instead of spending tokens on broad grep/read loops, repeated file inspection,
and exploratory subagents, the agent gets compact evidence to inspect first.

On [SWE-QA](benchmarks/sweqa.md), adding one Attemory semantic-search hint
before Claude Code reduced model tokens by **43.8%** while keeping answer
quality essentially tied: **83.17 vs 83.39** under a GPT-5.4 judge across
**15 repositories and 720 questions**.

## Agent Token Savings

The SWE-QA comparison keeps the downstream agent the same and changes only the
initial context:

```text
Baseline: Claude Code + read-only tools + Task subagents + DeepSeek v4
Attemory: Claude Code + read-only tools + Task subagents + DeepSeek v4
          + one pre-run Attemory semantic-search hint
```

Attemory only gives it likely files and line ranges before the
agent loop starts.

| System | Judge score | Model tokens | Main-agent tokens | Subagent tokens | Tool calls | Cost estimate |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | 83.39 | 285.39M | 122.60M | 162.80M | 26,997 | $453.47 |
| Attemory hint | 83.17 | 160.39M | 86.60M | 73.79M | 17,340 | $296.68 |
| Change | -0.23 | **-43.8%** | **-29.4%** | **-54.7%** | **-35.8%** | **-34.6%** |

> `Cost estimate` is the `total_cost_usd` value emitted by Claude Code in the
> final `stream-json` result event. See [Claude Code documents](https://code.claude.com/docs/en/statusline)

The token drop comes from giving Claude Code a better starting point before it
begins repository exploration. The main agent still has normal read-only tools,
but it performs fewer broad search/read loops and launches fewer exploratory
subagent calls. See [the SWE-QA benchmark note](benchmarks/sweqa.md) for the
full per-repo breakdown, methodology, and reproduction commands.

## Retrieval Quality

Token savings only matter if recall stays high. Attemory reaches SOTA-class
results across long conversations, million-token memory, and multi-language
codebases. LongMemEval-M is especially important: its context is long enough
that few memory systems evaluate on it directly, while Attemory still retrieves
all labeled evidence messages in the top 50 for **92.55%** of answerable
queries.

| Benchmark | What it tests | Context size | Attemory result |
| --- | --- | ---: | --- |
| [LongMemEval-S](benchmarks/LongMemEval.md) | memory retrieval, the split most memory systems evaluate | about 40 sessions / 115k tokens | **98.72% session Recall_any@5**, **92.77% session Recall_all@5**, **98.94% message Recall_all@50** |
| [LongMemEval-M](benchmarks/LongMemEval.md) | Million-token memory retrieval, a scale few memory systems attempt | about 500 sessions / 1.5M tokens / 5k messages | **94.89% session Recall_any@5**, **83.62% session Recall_all@5**, **92.55% message Recall_all@50** |
| [LoCoMo](benchmarks/LoCoMo.md) | End-to-end long-conversation QA | 10 long conversations / 1,540 QA items | **94.52% accuracy** with GPT-4.1-mini as answer model and GPT-4o-mini as judge |
| [Semble](benchmarks/semble.md) | Code retrieval | 63 repos / 19 languages | **0.9055 file-level NDCG@10** |
| [SWE-QA](benchmarks/sweqa.md) | End-to-end code QA agent | 15 repos / 720 questions / largest repo index **9.47M tokens** | **43.8% fewer model tokens** with near-tied GPT-5.4 judge score, **83.17 vs 83.39** |

Together with SWE-QA, these results show the same engine working as memory
retrieval, code retrieval, and an agent-context reduction layer. Detailed
results and run instructions are in [`benchmarks/`](benchmarks/).

## How It Works

Attemory runs as a local retrieval service:

1. Index long memory, documents, or code into reusable KV state.
2. Search that memory with a local retrieval model instead of keyword or vector
   similarity alone.
3. Return compact evidence: memory ids, text snippets, or file and line ranges
   that a downstream agent can inspect first.

The retrieval path uses Qwen3.5 model tiers from `tiny` to `large`, with CUDA
GPU and Apple Metal acceleration available for local indexing and search. For
the context template, segment refinement, persistence, and API details, see
[`doc/usage.md`](doc/usage.md).

## Install

Attemory supports Linux and macOS. Hardware acceleration is available on NVIDIA
CUDA and Apple Metal.

```bash
uv pip install attemory           # macOS Apple Silicon, includes Metal runtime
uv pip install "attemory[cpu]"    # Linux CPU

# Linux CUDA
uv pip install "attemory[cuda]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu126/
```

The same install targets work with `pip`:

```bash
pip install attemory
pip install "attemory[cpu]"
pip install "attemory[cuda]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu126/
```

On macOS Apple Silicon, `attemory` automatically installs the Metal runtime. On
Linux, choose `cpu` or a CUDA extra explicitly. Use `cuda-cu126` by default:

```bash
pip install "attemory[cuda]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu126/
```

If you are using a Blackwell GPU such as RTX 50 series, use `cuda-cu129` with
the CUDA 12.9 wheel index:

```bash
pip install "attemory[cuda-cu129]" \
  --extra-index-url https://attemorysystem.github.io/Attemory/whl/cu129/
```

Use `cuda-cu124` or `cuda-cu121` only when your NVIDIA driver is too old for
CUDA 12.6.

Start a local server, then connect with the Python client:

```bash
attemory-server --small --backend gpu --port 9006
attemory-server --small --backend metal --port 9006
attemory-server --tiny --backend cpu --port 9006
```

## Quick Example

Create a session, add memory, index once, and retrieve compact evidence by id:

```bash
attemory-server --small --backend gpu --port 9006
```

```python
from attemory import AttemoryClient, MemoryInput

client = AttemoryClient(host="127.0.0.1", port=9006, session_id="weekly-diary")
client.create_session()

client.add_system(
    "Read the memory carefully and retrieve the evidence that answers the query."
)
client.add_memory(
    MemoryInput(
        id="diary-20",
        text="In the evening, I had dinner with Clara at a Japanese restaurant.",
    )
)

client.index_session()

results = client.search(
    "Who did I have dinner with at the Japanese restaurant?",
    top_k=3,
)

for result in results:
    print(result.id, result.text)
```

The same API can return chat memories, document snippets, or code chunks. See
[`examples/weekly_diary.py`](examples/weekly_diary.py) for a complete runnable
example and [`doc/usage.md`](doc/usage.md) for the full API guide.

## Build From Source

Developers building Attemory from source need a C++17 compiler, CMake 3.18 or
newer, and an attemory-core SDK.

Prebuilt `attemory-core-sdk` archives are published on the GitHub Releases
page. Download the SDK that matches your target runtime, then extract it to a
local directory:

```bash
mkdir -p 3rd/attemory-core-sdk
tar -xzf attemory-core-sdk.tar.gz -C 3rd/attemory-core-sdk --strip-components=1
```

Then pass the extracted SDK root to CMake with `ATMCORE_SDK`:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DATMCORE_SDK="$PWD/3rd/attemory-core-sdk"

cmake --build build --target attemory_server --parallel
```

Use the matching SDK archive for CUDA or macOS Metal builds, for example
`attemory-core-sdk-linux-cuda-cu126-...tar.gz` or
`attemory-core-sdk-macos-metal-...tar.gz`.

## Future Work

- [ ] MCP support for agent and tool integrations.
- [ ] Continued performance optimization for indexing, search, and native backends.
- [ ] Broader test coverage across APIs, packaging, persistence, and runtime
  variants.

## Acknowledgements

Attemory is built on the work of the [Qwen team](https://github.com/QwenLM) and
the [ggml/llama.cpp community](https://github.com/ggml-org/llama.cpp).

## Citation

If you use Attemory in research or benchmarks, please cite it as:

```bibtex
@software{attemory2026,
  title        = {Attemory: Attention-Native Memory Retrieval System},
  author       = {Lance Fang},
  year         = {2026},
  url          = {https://github.com/AttemorySystem/Attemory},
}
```

## License

Attemory is released under the MIT License. See [`LICENSE`](LICENSE).
