#!/usr/bin/env python3
"""Unprivileged forced-command launcher; ignore all SSH input except bounded protocol."""
import os
import re
import sys


def arguments(value):
    if value == 'dump':
        return ['dump']
    if re.fullmatch(r'receipt \d{10} \d{1,15} [a-f0-9]{64}', value):
        return value.split(' ')
    raise ValueError('command_not_allowed')


if __name__ == '__main__':
    try:
        args = arguments(os.environ.get('SSH_ORIGINAL_COMMAND', ''))
    except ValueError:
        print('command_not_allowed', file=sys.stderr)
        sys.exit(1)
    os.execv('/usr/bin/sudo', ['sudo', '-n', '/usr/bin/python3',
                             '/usr/local/lib/cpp-recall/export_database_backup.py', *args])
