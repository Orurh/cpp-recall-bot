#!/usr/bin/env python3
"""Reject known local dumps, secrets and build artifacts in a release tree."""
from pathlib import PurePosixPath
import re
import subprocess
import sys


PRIVATE_FILES = {
    'AGENTS.md', 'PROJECT_INSTRUCTIONS.md', 'PROJECT_TEAM_COMPLETE.md', 'dashboard.md',
    'docs/AUTO_DEPLOY.md', 'docs/BETA_MONITORING.md', 'docs/LEARNING_ROADMAP.md',
    'docs/CONTENT_COVERAGE.md', 'docs/PILOT_LEARNING_CYCLE.md', 'docs/SHARED_CHALLENGES.md',
    'deploy/prebuilt/README.md', 'deploy/prebuilt/publish-image.yml.example',
}
PRIVATE_PREFIXES = ('.private/', 'agents/', 'product/', 'deploy/systemd/', 'deploy/prebuilt/systemd/')


def forbidden(path):
    parts = PurePosixPath(path).parts
    return (path in PRIVATE_FILES or path.startswith(PRIVATE_PREFIXES)
            or bool(re.fullmatch(r'docs/(pilot/[^/]+|reviews/20[^/]+)\.md', path))
            or bool(re.fullmatch(r'deploy/[^/]+-host\.compose\.yaml', path))
            or bool(re.fullmatch(r'[^/]+_patch\.sh', path)) or any(
        part in {'.git', '.codex', '.agents', '.aws', '__pycache__', '.pytest_cache',
                 'build', 'all-info.txt', 'all_json.txt'}
        or part.startswith('build-')
        or (part.startswith('.env') and part != '.env.example')
        or re.fullmatch(r'all[-_]code.*\.txt', part, re.IGNORECASE)
        or part.endswith(('.bundle', '.dump', '.partial', '.pyc', '.sqlite', '.sqlite3', '.pem', '.key'))
        for part in parts
    ))


def validate(paths):
    bad = [path for path in paths if forbidden(path)]
    if bad:
        # Only paths, never contents: a rejected file can contain credentials.
        raise ValueError('Release contains forbidden files: ' + ', '.join(bad))


def validate_history(revisions):
    if not revisions:
        return
    if any(not re.fullmatch(r'[a-f0-9]{40}|[a-f0-9]{64}', revision) for revision in revisions):
        raise ValueError('Invalid revision for history check')
    data = subprocess.check_output(['git', 'log', '--format=', '--name-only', '-z', *revisions, '--'])
    validate(path.strip() for path in data.decode().split('\0') if path.strip())


if __name__ == '__main__':
    try:
        if sys.argv[1:] == ['--pre-push']:
            revisions = []
            for line in sys.stdin:
                fields = line.split()
                if len(fields) != 4:
                    raise ValueError('Invalid push description')
                if set(fields[1]) != {'0'}:
                    revisions.append(fields[1])
            validate_history(revisions)
            print('Public history policy: OK')
        else:
            paths = subprocess.check_output(['git', 'ls-files', '-z']).decode().split('\0')
            validate(path for path in paths if path)
            print('Release file policy: OK (tracked files only)')
    except (ValueError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
