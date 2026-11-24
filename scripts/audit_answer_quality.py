#!/usr/bin/env python3
"""Heuristic audit of wording, answer choices and pre-answer references.

This produces *candidates for editorial review*, not verdicts about C++ correctness.
Does not edit questions, historical reviews, help bindings, or learner progress.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
WORD = re.compile(r"[\\w]+", re.UNICODE)
OUTPUT = re.compile(r"^[0-9A-Za-z ]{1,24}[:：]\\s")
EXPLAIN = re.compile(r"(?:\\s[—–-]\\s|[:：]\\s|\\bпотому что\\b|\\bтак как\\b)", re.I)
STOP = {"который", "которая", "которые", "которое", "будет", "можно", "после",
        "когда", "через", "только", "также", "между", "чтобы", "всегда"}


def normalize(value: str) -> str:
    return " ".join(WORD.findall(value.casefold()))


def tokens(value: str) -> set[str]:
    return {word for word in WORD.findall(value.casefold())
            if len(word) >= 5 and word not in STOP}


def load_bank(root: Path) -> tuple[list[dict], dict[str, dict], dict[str, list[str]]]:
    questions = [json.loads(p.read_text(encoding="utf-8"))
                 for p in sorted((root / "content/questions").rglob("*.json"))]
    references: dict[str, dict] = {}
    bindings: dict[str, list[str]] = {}
    for path in sorted((root / "content/cpp/topics").rglob("materials*.json")):
        material = json.loads(path.read_text(encoding="utf-8"))
        for article in material["references"]:
            if article["id"] in references:
                raise ValueError("duplicate article id: " + article["id"])
            references[article["id"]] = article
        for binding in material["reference_bindings"]:
            qid = binding["question_id"]
            if qid in bindings:
                raise ValueError("duplicate question binding: " + qid)
            bindings[qid] = binding["reference_ids"]
    return questions, references, bindings


def audit(root: Path) -> dict:
    questions, references, bindings = load_bank(root)
    findings: list[dict] = []
    counts = Counter()
    for q in questions:
        task = q["task"]
        if task["type"] != "single_choice":
            continue
        counts["single_choice"] += 1
        options = task["options"]
        correct = next(o for o in options if o["id"] == task["correct_option_id"])
        wrong = [o for o in options if o["id"] != correct["id"]]
        name = q["id"]
        def flag(kind: str, detail: str) -> None:
            findings.append({"id": name, "public_id": q.get("public_id", ""),
                             "topic": q["topic"], "level": q["level"],
                             "evidence": q["evidence_type"],
                             "category": kind, "detail": detail})
            counts[kind] += 1

        clen = len(correct["text"])
        lengths = [len(o["text"]) for o in options]
        longest_wrong = max(len(o["text"]) for o in wrong)
        if clen >= 40 and clen - longest_wrong >= 20 and clen > 1.45 * longest_wrong:
            flag("strong_length_cue", f"lengths={lengths}; correct={clen}, max_wrong={longest_wrong}")
        rationale = [bool(EXPLAIN.search(o["text"])) for o in options]
        if rationale[options.index(correct)] and sum(rationale) <= 2:
            flag("asymmetric_rationale", "explanation in correct option, not uniformly in distractors")
        if sum(rationale) >= 3:
            flag("rationales_in_choices", f"{sum(rationale)}/{len(options)} options contain explanations")
        if OUTPUT.match(correct["text"]) and sum(bool(OUTPUT.match(o["text"])) for o in options) < 3:
            flag("result_with_rationale", "correct choice mixes output and rationale; other formats differ")
        norm_answer = normalize(correct["text"])
        if len(norm_answer) >= 25 and norm_answer in normalize(q["text"]):
            flag("stem_repeats_answer", "question contains exact correct option text")
        # Only explicit before-answer reference articles count here, not post-answer
        # explanations or optional primary theory that may be shown in other flows.
        for ref_id in bindings.get(name, []):
            if ref_id not in references:
                flag("missing_reference", f"unknown article {ref_id}")
                continue
            article = references[ref_id]
            help_text = " ".join(str(article[field]) for field in (
                "title", "principle", "mechanism", "example", "common_mistake"))
            norm_help = normalize(help_text)
            if len(norm_answer) >= 25 and norm_answer in norm_help:
                flag("help_repeats_answer", f"pre-answer article {ref_id} repeats correct choice")
            answer_tokens = tokens(correct["text"])
            # Similarity is a REVIEW QUEUE, not a proof of leaked answer.
            if len(answer_tokens) >= 4:
                ratio = len(answer_tokens & tokens(help_text)) / len(answer_tokens)
                if ratio >= .75:
                    flag("help_high_overlap", f"{ref_id}: token overlap {ratio:.2f}")
    counts["questions"] = len(questions)
    counts["reference_articles"] = len(references)
    counts["reference_bindings"] = len(bindings)
    return {"counts": dict(counts), "findings": sorted(
        findings, key=lambda x: (x["category"], x["topic"], x["id"]))}


def markdown(report: dict) -> str:
    counts = report["counts"]
    out = ["# Контроль качества вопросов и справки (эвристический)",
           "",
           "Все находки — кандидаты для ручного ревью, не доказанные ошибки.",
           "Скрипт не проверяет C++-семантику, достоверность объяснений,",
           "баланс сложности, независимость семейств или учебный эффект.",
           "",
           f"Карточек: **{counts['questions']}**; single_choice: **{counts['single_choice']}**;",
           f"предответных статей: **{counts['reference_articles']}**;",
           f"привязок: **{counts['reference_bindings']}**.", "",
           "| Признак | Количество |", "|---|---:|"]
    for kind, count in sorted(counts.items()):
        if kind not in {"questions", "single_choice", "reference_articles", "reference_bindings"}:
            out.append(f"| `{kind}` | {count} |")
    out += ["", "## На проверку редактору", "",
            "| Карточка | Тема / уровень | Признак | Причина |",
            "|---|---|---|---|"]
    for item in report["findings"]:
        reason = item["detail"].replace("|", "\\|").replace("\\n", " ")
        out.append(f"| {item['public_id']} (`{item['id']}`) | "
                   f"{item['topic']} / L{item['level']} | {item['category']} | {reason} |")
    if not report["findings"]:
        out.append("| — | — | — | Сильные эвристические признаки не найдены |")
    out += ["", "## Как интерпретировать", "",
            "- `rationales_in_choices`: объяснения есть у большинства вариантов. "
            "Это может быть полезным обучающим форматом, но снижает чистоту проверки.",
            "- `asymmetric_rationale`: верный вариант выделяется объяснением; проверить в первую очередь.",
            "- `help_high_overlap`: совпадают слова; необходимо прочитать саму статью.",
            "- Наличие справки до ответа не означает утечку: открытие учитывается как помощь.",
            "- Исправление исторической карточки требует нового SHA и повторного ревью.",
            ""]
    return "\\n".join(out)


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--format", choices=["markdown", "json"], default="markdown")
    p.add_argument("--output", type=Path)
    args = p.parse_args()
    report = audit(ROOT)
    rendered = json.dumps(report, ensure_ascii=False, indent=2) if args.format == "json" else markdown(report)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\\n", encoding="utf-8")
    print(rendered if not args.output else f"Audit saved to {args.output}; findings: {len(report['findings'])}")


if __name__ == "__main__":
    main()
