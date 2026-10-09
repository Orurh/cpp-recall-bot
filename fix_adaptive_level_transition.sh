#!/usr/bin/env bash
set -euo pipefail

python3 - <<'PY'
from pathlib import Path

path = Path("src/app/quiz_session_service.cc")
text = path.read_text()

old = """int NextTargetLevel(
    const Question& question,
    AnswerResult result,
    std::size_t attempts) {
  if (result == AnswerResult::kCorrect && attempts == 1) {
    return std::min(question.level + 1, kMaxLevel);
  }

  if (result == AnswerResult::kCorrect && attempts == 2) {
    return question.level;
  }

  return std::max(question.level - 1, kMinLevel);
}
"""

new = """int NextTargetLevel(
    int current_level,
    const Question& question,
    AnswerResult result,
    std::size_t attempts) {
  ValidateTargetLevel(current_level);

  if (result == AnswerResult::kCorrect && attempts == 1) {
    if (question.level < current_level) {
      return current_level;
    }

    return std::min(current_level + 1, kMaxLevel);
  }

  if (result == AnswerResult::kCorrect && attempts == 2) {
    return current_level;
  }

  if (question.level > current_level) {
    return current_level;
  }

  return std::max(current_level - 1, kMinLevel);
}
"""

if old not in text:
    raise SystemExit("NextTargetLevel block not found; current file differs from expected state")

text = text.replace(old, new, 1)

old_call = """    next.target_level = NextTargetLevel(
        question,
        result,
        submit_result.attempts);
"""

new_call = """    next.target_level = NextTargetLevel(
        session.target_level,
        question,
        result,
        submit_result.attempts);
"""

if old_call not in text:
    raise SystemExit("NextTargetLevel call not found; current file differs from expected state")

text = text.replace(old_call, new_call, 1)
path.write_text(text)
PY

echo "Adaptive level transition fix applied."
echo "Run:"
echo "  make"
echo "  make content-check"
