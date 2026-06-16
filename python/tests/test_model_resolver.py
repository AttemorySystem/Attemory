from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest

from attemory import model_resolver


def test_prepare_server_args_appends_downloaded_tier_model(monkeypatch: Any) -> None:
    monkeypatch.setattr(
        model_resolver,
        "_resolve_or_fetch_model",
        lambda tier: Path(f"/models/{tier}.gguf"),
    )

    assert model_resolver.prepare_server_args(["--small", "--backend", "cpu"]) == [
        "--small",
        "--backend",
        "cpu",
        "--model",
        "/models/small.gguf",
    ]


def test_prepare_server_args_respects_explicit_model(monkeypatch: Any) -> None:
    def fail_resolve(_: str) -> Path:
        raise AssertionError("model resolver should not be called")

    monkeypatch.setattr(model_resolver, "_resolve_or_fetch_model", fail_resolve)

    assert model_resolver.prepare_server_args(["--small", "--model", "/tmp/model.gguf"]) == [
        "--small",
        "--model",
        "/tmp/model.gguf",
    ]


def test_prepare_server_args_leaves_help_and_metadata_commands_unchanged(
    monkeypatch: Any,
) -> None:
    def fail_resolve(_: str) -> Path:
        raise AssertionError("model resolver should not be called")

    monkeypatch.setattr(model_resolver, "_resolve_or_fetch_model", fail_resolve)

    assert model_resolver.prepare_server_args(["--help"]) == ["--help"]
    assert model_resolver.prepare_server_args(["--model-info", "small"]) == [
        "--model-info",
        "small",
    ]


def test_hf_cache_dir_honors_environment_precedence(
    monkeypatch: Any,
    tmp_path: Path,
) -> None:
    monkeypatch.setenv("HF_HOME", str(tmp_path / "hf-home"))
    monkeypatch.setenv("HUGGINGFACE_HUB_CACHE", str(tmp_path / "hf-hub-cache"))
    monkeypatch.setenv("HF_HUB_CACHE", str(tmp_path / "explicit-cache"))

    assert model_resolver._hf_cache_dir() == tmp_path / "explicit-cache"

    monkeypatch.delenv("HF_HUB_CACHE")
    assert model_resolver._hf_cache_dir() == tmp_path / "hf-hub-cache"

    monkeypatch.delenv("HUGGINGFACE_HUB_CACHE")
    assert model_resolver._hf_cache_dir() == tmp_path / "hf-home" / "hub"


def test_relative_filename_rejects_unsafe_paths() -> None:
    with pytest.raises(model_resolver.ModelResolverError, match="unsafe"):
        model_resolver._relative_filename("../model.gguf")

    with pytest.raises(model_resolver.ModelResolverError, match="unsafe"):
        model_resolver._relative_filename("/absolute/model.gguf")

    assert model_resolver._relative_filename("nested/model.gguf") == Path("nested/model.gguf")
