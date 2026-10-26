#!/usr/bin/env python3
"""Verify all card-review decisions against the current question JSON files.

Run from the repository root: python3 scripts/validate_content_reviews.py
This checks integrity, not the semantic correctness of C++ answers.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys


def canonical_digest(question: dict) -> str:
    canonical = json.dumps(question, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    content = root / "content"
    qroot = content / "questions"
    rroot = root / "docs" / "reviews" / "content"
    if not qroot.is_dir() or not rroot.is_dir():
        print("ERROR: expected content/questions and docs/reviews/content", file=sys.stderr)
        return 2
    by_id = {}
    errors = []
    for path in sorted(qroot.rglob("*.json")):
        try:
            question = json.loads(path.read_text(encoding="utf-8"))
            qid = question["id"]
            if qid in by_id:
                errors.append(f"duplicate question ID {qid}")
            by_id[qid] = (question, path)
        except (ValueError, KeyError) as exc:
            errors.append(f"{path}: {exc}")
    seen = {}
    reviewed = 0
    for path in sorted(rroot.glob("*.json")):
        try:
            registry = json.loads(path.read_text(encoding="utf-8"))
            records = registry["reviews"]
        except (ValueError, KeyError) as exc:
            errors.append(f"{path}: {exc}")
            continue
        for record in records:
            qid = record.get("question_id")
            if not isinstance(qid, str):
                errors.append(f"{path}: missing question_id")
                continue
            if qid in seen:
                errors.append(f"{qid}: repeated across {seen[qid]} and {path}")
                continue
            seen[qid] = path
            if qid not in by_id:
                errors.append(f"{path}: unknown question {qid}")
                continue
            q, qpath = by_id[qid]
            if record.get("content_revision") != canonical_digest(q):
                errors.append(f"{qid}: review revision mismatches {qpath}")
            for key in ("public_id", "skill_id", "question_family_id", "standard"):
                if record.get(key) != q.get(key):
                    errors.append(f"{qid}: mismatched {key}")
            if not q.get("source"):
                errors.append(f"{qid}: missing source")
            if record.get("decision") == "accepted":
                reviewed += 1
    registry_path = content / "question_registry.json"
    if registry_path.is_file():
        try:
            registry = json.loads(registry_path.read_text(encoding="utf-8"))
            for qid, (q, path) in by_id.items():
                if registry.get(qid) != q.get("public_id"):
                    errors.append(f"{qid}: public_id mismatches registry")
        except ValueError as exc:
            errors.append(f"{registry_path}: {exc}")
    print(f"Questions: {len(by_id)}, reviews with accepted decision: {reviewed}; unreviewed: {len(by_id) - len(seen)}")
    if errors:
        for issue in errors:
            print("ERROR:", issue, file=sys.stderr)
        return 1
    print("PASS: metadata and SHA-256 revisions match their reviewed content")
    print("NOTE: technical integrity does not imply independent editorial approval")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
