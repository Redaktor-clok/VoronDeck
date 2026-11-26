"""Existing non-RU/EN gaps in the imported public tree; new gaps still fail."""
import json
from pathlib import Path

_DATA = json.loads((Path(__file__).resolve().parents[1] / "fixtures" /
                    "translation_coverage_baseline.json").read_text(encoding="utf-8"))
assert set(_DATA["locales"]) == {"de", "es", "fr", "it", "ja", "pt", "zh"}


def existing_gap(locale: str, text: str) -> bool:
    return text in _DATA["locales"].get(locale, ())
