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
    # A later review can supersede a historical card only when it names the
    # exact predecessor revision. The original review remains immutable.
    review_files = sorted(rroot.glob("*.json"))
    declared_replacements = {}
    for path in review_files:
        try:
            registry = json.loads(path.read_text(encoding="utf-8"))
            for qid, previous in registry.get("replaces_card_revisions", {}).items():
                if (qid in declared_replacements or not isinstance(previous, str)
                        or len(previous) != 64 or any(ch not in "0123456789abcdef" for ch in previous)):
                    errors.append(f"{path}: invalid or duplicate predecessor for {qid}")
                else:
                    declared_replacements[qid] = (path, previous)
        except ValueError as exc:
            errors.append(f"{path}: {exc}")

    seen = {}  # question_id -> (review path, currently reviewed revision)
    reviewed = 0
    for path in review_files:
        try:
            registry = json.loads(path.read_text(encoding="utf-8"))
            records = registry["reviews"]
        except (ValueError, KeyError) as exc:
            errors.append(f"{path}: {exc}")
            continue
        replacements = registry.get("replaces_card_revisions", {})
        for record in records:
            qid = record.get("question_id")
            if not isinstance(qid, str):
                errors.append(f"{path}: missing question_id")
                continue
            if qid not in by_id:
                errors.append(f"{path}: unknown question {qid}")
                continue
            q, qpath = by_id[qid]
            reviewed_revision = record.get("content_revision")
            predecessor = seen.get(qid)
            if predecessor:
                permitted = (qid in replacements and replacements[qid] == predecessor[1]
                             and declared_replacements.get(qid) == (path, predecessor[1]))
                if not permitted:
                    errors.append(f"{qid}: repeated across {predecessor[0]} and {path}")
                    continue
            elif qid in replacements:
                errors.append(f"{qid}: replacement has no preceding reviewed revision in {path}")
                continue
            seen[qid] = (path, reviewed_revision)
            if reviewed_revision != canonical_digest(q):
                announced = declared_replacements.get(qid)
                if not (announced and announced[0] != path and reviewed_revision == announced[1]):
                    errors.append(f"{qid}: review revision mismatches {qpath}")
            for key in ("public_id", "skill_id", "question_family_id", "standard"):
                if record.get(key) != q.get(key):
                    errors.append(f"{qid}: mismatched {key}")
            if not q.get("source"):
                errors.append(f"{qid}: missing source")
            if record.get("decision") == "accepted" and predecessor is None:
                reviewed += 1

    for qid, (path, previous) in declared_replacements.items():
        final = seen.get(qid)
        if not final or final[0] != path or final[1] == previous:
            errors.append(f"{qid}: declared predecessor was not replaced in {path}")
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
