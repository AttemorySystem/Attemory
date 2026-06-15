from __future__ import annotations

"""Weekly diary retrieval example.

Run an Attemory server first, for example:

    attemory-server --small --backend gpu --port 9006

Then run:

    python examples/weekly_diary.py
"""

import argparse
from pathlib import Path

from attemory import AttemoryClient, AttemoryHTTPError, MemoryInput


SYSTEM_PROMPT = (
    "Read the following weekly diary entries carefully and answer the query at the end."
)

def load_diary(path: Path) -> list[tuple[int, MemoryInput]]:
    memories: list[tuple[int, MemoryInput]] = []
    current_day: str | None = None
    line_number = 0

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        line_number += 1
        if line.startswith("[") and line.endswith("]"):
            current_day = line[1:-1]
            memories.append((line_number, MemoryInput(text=line)))
            continue
        if current_day is None:
            raise ValueError(f"diary line appears before a day header: {line}")
        memories.append(
            (
                line_number,
                MemoryInput(
                    id=str(line_number),
                    text=line,
                ),
            )
        )

    return memories


def print_results(
    title: str,
    query: str,
    query_context: str | None,
    label: str,
    results,
) -> None:
    print(f"\n== {title} ==")
    print(f"query: {query}")
    if query_context is not None:
        print(f"query_context: {query_context}")
    print(f"--- Results ---")
    for result in results:
        if result.id is None:
            continue
        preview = result.text.replace("\n", " ")
        marker = " ✓" if result.id == label else ""
        print(f"rank={result.rank} line={result.id} text={preview}{marker}")


def print_diary(memories: list[tuple[int, MemoryInput]]) -> None:
    print("== Weekly diary ==")
    for line_number, memory in memories:
        print(f"{line_number}: {memory.text}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Attemory weekly diary example")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9006)
    parser.add_argument("--session-id", default="weekly-diary")
    parser.add_argument("--top-k", type=int, default=3)
    args = parser.parse_args()

    diary_path = Path(__file__).with_name("weekly_diary.txt")
    diary = load_diary(diary_path)
    print_diary(diary)

    client = AttemoryClient(host=args.host, port=args.port, session_id=args.session_id)
    try:
        client.delete_session()
    except AttemoryHTTPError:
        pass
    client.create_session()
    client.add_system(SYSTEM_PROMPT)

    for _, memory in diary:
        client.add_memory(memory)

    client.index_session()

    direct_query = "Who did I have dinner with at Japanese restaurant?"
    direct_label = "20"
    direct_results = client.search(direct_query, top_k=args.top_k)
    print_results("Direct fact", direct_query, None, direct_label, direct_results)

    evening_query = "Who did I meet on Thursday?"
    evening_label = "27"
    evening_context = "The user is asking about social activities at evening."
    evening_results = client.search(
        evening_query,
        query_context=evening_context,
        top_k=args.top_k,
    )
    print_results(
        "Query context: evening social activity",
        evening_query,
        evening_context,
        evening_label,
        evening_results,
    )

    travel_query = "Who did I meet on Thursday?"
    travel_label = "25"
    travel_context = "The user is asking about family activities at noon."
    travel_results = client.search(
        travel_query,
        query_context=travel_context,
        top_k=args.top_k,
    )
    print_results(
        "Query context: family communication",
        travel_query,
        travel_context,
        travel_label,
        travel_results,
    )

    reasoning_query = "Assume today is Thursday, who did I have dinner with yesterday?"
    reasoning_label = "20"
    reasoning_context = (
        "The user's query: {query}\n"
        "Resolve relative date into target date before ranking diary entries. "
    ).format(query=reasoning_query)
    reasoning_results = client.search(
        reasoning_query,
        query_context=reasoning_context,
        top_k=args.top_k,
    )
    print_results(
        "Temporal reasoning",
        reasoning_query,
        reasoning_context,
        reasoning_label,
        reasoning_results,
    )

if __name__ == "__main__":
    main()
