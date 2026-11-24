"""Focused regression tests for heuristic card and pre-answer help audit."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from scripts.audit_answer_quality import audit  # noqa: E402


class AnswerQualityAuditTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "content/questions/demo").mkdir(parents=True)
        (self.root / "content/cpp/topics/demo/skills/demo.skill").mkdir(parents=True)

    def build(self, options, correct="b", text="Что выведет программа?", article=None):
        q = {
            "id": "sample", "public_id": "CR-TEST", "topic": "demo",
            "level": 2, "evidence_type": "application", "text": text,
            "task": {"type": "single_choice", "correct_option_id": correct,
                     "options": [{"id": chr(97 + i), "text": item}
                                 for i, item in enumerate(options)]},
        }
        (self.root / "content/questions/demo/sample.json").write_text(
            json.dumps(q, ensure_ascii=False), encoding="utf-8")
        refs = [] if article is None else [{
            "id": "help.sample", "title": "Помощь", "principle": article,
            "mechanism": "", "example": "", "common_mistake": "",
        }]
        binds = [] if article is None else [
            {"question_id": "sample", "reference_ids": ["help.sample"]}]
        (self.root / "content/cpp/topics/demo/skills/demo.skill/materials.json").write_text(
            json.dumps({"references": refs, "reference_bindings": binds},
                       ensure_ascii=False), encoding="utf-8")
        return {entry["category"] for entry in audit(self.root)["findings"]}

    def test_neutral_choices(self):
        kinds = self.build(["MM", "CM", "CC", "Ошибка компиляции"])
        self.assertEqual(kinds, set())

    def test_correct_choice_has_unique_rationale(self):
        kinds = self.build([
            "Программа не компилируется",
            "Печатает 5: это происходит из-за вызова копирующего конструктора",
            "Печатает 7",
            "Неопределённое поведение",
        ])
        self.assertIn("asymmetric_rationale", kinds)

    def test_multiple_explained_choices(self):
        kinds = self.build([
            "5: оба слагаемых постоянны",
            "7: используется старое значение",
            "9: значение уже обновилось",
            "Код не компилируется: тип не совпадает",
        ])
        self.assertIn("rationales_in_choices", kinds)

    def test_before_answer_full_solution_is_flagged(self):
        full = "Перемещение заменяется копированием при потенциально бросающем конструкторе"
        kinds = self.build(["Ошибка компиляции", full, "Перемещение",
                            "Неопределённое поведение"], article=full)
        self.assertIn("help_repeats_answer", kinds)

    def test_no_content_changes(self):
        options = ["MM", "CM", "CC", "Ошибка компиляции"]
        path = self.root / "content/questions/demo/sample.json"
        self.build(options)
        before = path.read_bytes()
        audit(self.root)
        self.assertEqual(path.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
