"""Compile emitted C++23 operations independently of the scalar model (offline only)."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

samples = json.loads(subprocess.check_output([sys.argv[1], '--emit'], text=True))
source = ['#include <vector>', '#include <iostream>', 'int main() {']
expected = []
for sample in samples:
    source.append('{')
    for line in [sample['initialization'], *sample['operations']]:
        source.extend([line, "std::cout << v.size() << ' ';"])
    source.extend(['std::cout << "\\n";', '}'])
    expected.append(' '.join(map(str, sample['expected'])))
source.append('}')
with tempfile.TemporaryDirectory(prefix='cpp-recall-gen01-') as directory:
    cpp = Path(directory) / 'examples.cc'
    # Test artifact only, not repository or user data.
    cpp.write_text('\n'.join(source), encoding='utf-8')
    binary = Path(directory) / 'examples'
    subprocess.run([sys.argv[2], '-std=c++23', '-D_GLIBCXX_ASSERTIONS', '-O0', str(cpp), '-o', str(binary)],
                   check=True, timeout=90)
    actual = subprocess.check_output([str(binary)], text=True, timeout=10).splitlines()
assert [line.strip() for line in actual] == expected
print(f'PASS: {len(samples)} emitted C++23 programs; every intermediate size matches the model')
