#!/usr/bin/env python3
"""Pull only green main commits; never accept public jobs or inbound deploy commands.

Installed controller/Compose configuration are trusted host files, not auto-updated.
No token is needed for this public repository. Never log raw subprocess/HTTP errors.
"""
import argparse
import contextlib
import fcntl
import io
import ipaddress
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request

from check_release_files import validate

REPOSITORY = 'Orurh/cpp-recall-bot'
REMOTE = f'https://github.com/{REPOSITORY}.git'
IMAGE = 'cpp-recall-bot-bot'
CONTAINER = 'cpp-recall-bot-bot-1'
MAINTENANCE = Path('/usr/local/lib/cpp-recall/beta_maintenance.py')
SHA = re.compile(r'[a-f0-9]{40}')


class Failure(Exception):
    """Fixed safe messages only."""


def run(args, *, env=None, timeout=60):
    try:
        result = subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=timeout, check=False)
        if result.returncode:
            raise Failure('Команда выпуска завершилась ошибкой')
        return result.stdout
    except (OSError, subprocess.TimeoutExpired) as error:
        raise Failure('Команда выпуска недоступна или превысила срок') from error


def save(path, value):
    temporary = path.with_suffix('.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o600)
    temporary.replace(path)


def green_run(data, sha):
    runs = data.get('workflow_runs', [])
    if not runs:
        return False
    # The latest run for this SHA matters, not any older successful attempt.
    latest = max(runs, key=lambda row: (row.get('run_number', 0), row.get('run_attempt', 0)))
    return (latest.get('head_sha') == sha and latest.get('head_branch') == 'main'
            and latest.get('event') == 'push' and latest.get('path') == '.github/workflows/ci.yml'
            and latest.get('status') == 'completed' and latest.get('conclusion') == 'success'
            and latest.get('repository', {}).get('full_name') == REPOSITORY)


@contextlib.contextmanager
def monitor_lock():
    with open('/run/cpp-recall-monitor.lock', 'a') as stream:
        deadline = time.monotonic() + 60
        while True:
            try:
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() >= deadline:
                    raise Failure('Наблюдатель или резервное копирование ещё заняты')
                time.sleep(1)
        yield


class Host:
    def __init__(self, args):
        self.args = args
        self.root = args.project_dir.resolve()
        self.cache = args.state_dir / 'repo.git'
        self.env = {**os.environ, 'GIT_TERMINAL_PROMPT': '0'}
        self.monitor = None
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        if args.proxy_container:
            addresses = json.loads(run(['docker', 'inspect', '--format', '{{json .NetworkSettings.Networks}}',
                                        args.proxy_container]))
            address = next((v['IPAddress'] for v in addresses.values() if v.get('IPAddress')), '')
            try:
                ipaddress.IPv4Address(address)
            except ValueError as error:
                raise Failure('Не удалось определить адрес существующего прокси') from error
            proxy = f'http://{address}:1080'
            self.env.update(HTTPS_PROXY=proxy, https_proxy=proxy, HTTP_PROXY=proxy, http_proxy=proxy)
            self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({'https': proxy}))

    def head(self):
        rows = run(['git', 'ls-remote', REMOTE, 'refs/heads/main'], env=self.env, timeout=30).decode().split()
        if len(rows) != 2 or not SHA.fullmatch(rows[0]) or rows[1] != 'refs/heads/main':
            raise Failure('Не удалось однозначно определить main')
        return rows[0]

    def green(self, sha):
        query = urllib.parse.urlencode({'branch': 'main', 'event': 'push', 'head_sha': sha, 'per_page': 10})
        request = urllib.request.Request(
            f'https://api.github.com/repos/{REPOSITORY}/actions/workflows/ci.yml/runs?{query}',
            headers={'Accept': 'application/vnd.github+json', 'User-Agent': 'cpp-recall-auto-deploy',
                     'X-GitHub-Api-Version': '2022-11-28'})
        try:
            with self.opener.open(request, timeout=20) as response:
                return green_run(json.load(response), sha)
        except Exception as error:
            raise Failure('Не удалось подтвердить успешный CI; выпуск отложен') from error

    def prepare(self, sha):
        releases = self.args.state_dir / 'releases'
        releases.mkdir(exist_ok=True)
        destination = releases / sha
        if destination.exists():
            if (destination / '.release-sha').read_text() != sha:
                raise Failure('Кэш исходников не прошёл проверку')
            return destination
        if not self.cache.exists():
            run(['git', 'init', '--bare', str(self.cache)])
        run(['git', '-C', str(self.cache), 'fetch', '--quiet', '--depth=1', REMOTE, 'refs/heads/main'],
            env=self.env, timeout=120)
        if run(['git', '-C', str(self.cache), 'rev-parse', 'FETCH_HEAD']).decode().strip() != sha:
            raise Failure('Main изменился при загрузке; выпуск отложен')
        paths = run(['git', '-C', str(self.cache), 'ls-tree', '-r', '--name-only', '-z', sha]).decode().split('\0')
        validate(path for path in paths if path)
        archive = run(['git', '-C', str(self.cache), 'archive', sha])
        temporary = Path(tempfile.mkdtemp(prefix=sha + '-', dir=releases))
        with tarfile.open(fileobj=io.BytesIO(archive)) as tree:
            tree.extractall(temporary, filter='data')
        (temporary / '.release-sha').write_text(sha)
        temporary.rename(destination)
        return destination

    def build(self, sha, release):
        usage = shutil.disk_usage(self.args.state_dir)
        if usage.free < max(2 * 1024 ** 3, usage.total * .1):
            raise Failure('Недостаточно места для безопасной сборки')
        # Use the host's working Compose/BuildKit path, not Docker's legacy builder.
        config = self.args.state_dir / 'build-compose.json'
        save(config, {'services': {'bot': {
            'image': f'{IMAGE}:release-{sha}',
            'build': {'context': str(release), 'args': {'APP_VERSION': sha}},
        }}})
        run(['docker', 'compose', '--project-name', 'cpp-recall-build', '--file', str(config),
             'build', 'bot'], env={**os.environ, 'DOCKER_BUILDKIT': '1'}, timeout=900)

    def maintenance(self):
        if self.monitor is None:
            scope = runpy.run_path(str(MAINTENANCE))
            args = argparse.Namespace(compose_project='cpp-recall-bot', port=self.args.port,
                                      state_dir=Path('/var/lib/cpp-recall-monitoring'),
                                      backup_dir=Path('/var/lib/cpp-recall/backups'))
            self.monitor = scope['Monitor'](args)
        return self.monitor

    def technical(self):
        monitor = self.maintenance()
        monitor.env = {}  # Container/config may have changed since the last probe.
        return monitor.local_request('/api/v1/admin/statistics', {'days': 1})['technical']

    def previous(self):
        image = run(['docker', 'inspect', '--format', '{{.Image}}', CONTAINER]).decode().strip()
        if not re.fullmatch(r'sha256:[a-f0-9]{64}', image):
            raise Failure('Не удалось сохранить прежний образ')
        return {'image': image, 'version': self.technical()['version']}

    def backup(self):
        self.maintenance().backup()  # Verified by restoring to its own temporary DB, never production.

    def switch(self, image):
        run(['docker', 'image', 'tag', image, f'{IMAGE}:latest'])
        command = ['docker', 'compose', '--project-name', 'cpp-recall-bot', '--project-directory', str(self.root)]
        for name in ('compose.yaml', 'deploy/outbound-proxy.compose.yaml', 'deploy/public.compose.yaml',
                     'deploy/monitoring.compose.yaml'):
            command.extend(['-f', str(self.root / name)])
        # Never recreate PostgreSQL, Caddy, other services, networks or volumes.
        run(command + ['up', '-d', '--no-deps', '--no-build', 'bot'], timeout=120)

    def verify(self, version):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                technical = self.technical()
                if technical['version'] == version and self.maintenance().local_request('/health/ready')['ready']:
                    url = urllib.parse.urlsplit(self.maintenance().credentials()['MINI_APP_URL'])
                    if url.scheme != 'https' or not url.hostname:
                        raise Failure('Не настроен публичный HTTPS')
                    run(['curl', '--fail', '--silent', '--show-error', '--max-time', '8', '--noproxy', '*',
                         '--resolve', f'{url.hostname}:443:127.0.0.1',
                         f'https://{url.hostname}{url.path.rstrip("/")}/health/ready'], timeout=10)
                    return
            except Exception:
                time.sleep(2)
        raise Failure('Новая версия не прошла проверку готовности')

    def install_monitor(self, source):
        temporary = MAINTENANCE.with_suffix('.new')
        shutil.copyfile(source, temporary)
        os.chmod(temporary, 0o644)
        temporary.replace(MAINTENANCE)
        self.monitor = None

    def notify(self, message):
        try:
            self.maintenance().send('C++ Recall · выпуск: ' + message)
        except Exception:
            print('Оповещение о выпуске не доставлено', flush=True)


class Deployer:
    def __init__(self, host, state_dir):
        self.host = host
        self.state_dir = state_dir
        self.path = state_dir / 'state.json'
        self.state = json.loads(self.path.read_text()) if self.path.exists() else {}

    def persist(self, **changes):
        self.state.update(changes)
        save(self.path, self.state)

    def rollback(self):
        previous = self.state['previous']
        self.host.switch(previous['image'])
        self.host.install_monitor(self.state_dir / 'previous-maintenance.py')
        self.host.verify(previous['version'])
        self.persist(phase='idle')

    def tick(self, retry=False):
        # A killed/rebooted controller must resolve its journal before considering another commit.
        if self.state.get('phase') in ('switching', 'rollback_failed'):
            with monitor_lock():
                try:
                    self.rollback()
                except Exception:
                    self.persist(phase='rollback_failed')
                    raise Failure('Нужна ручная проверка: автоматический откат не завершён')
            self.host.notify('Незавершённое обновление отменено, прежняя версия восстановлена.')
        sha = self.host.head()
        if self.state.get('deployed_sha') == sha:
            return 'already_deployed'
        if self.state.get('failed_sha') == sha and not retry:
            if self.state.get('attempts', 0) >= 3:
                return 'failed_waiting_for_new_commit_or_retry'
            if time.time() < self.state.get('retry_after', 0):
                return 'waiting_before_retry'
        if self.state.get('checked_sha') == sha and time.time() < self.state.get('next_check_at', 0) and not retry:
            return 'waiting_for_ci'
        self.persist(checked_sha=sha, next_check_at=time.time() + 180)
        if not self.host.green(sha):
            return 'waiting_for_ci'
        attempts = self.state.get('attempts', 0) if self.state.get('attempt_sha') == sha else 0
        self.persist(attempt_sha=sha, attempts=1 if retry else attempts + 1)
        try:
            self.persist(stage='prepare')
            release = self.host.prepare(sha)
            self.persist(stage='build')
            self.host.build(sha, release)
            if self.host.head() != sha:
                return 'main_changed_before_switch'
            with monitor_lock():
                previous = self.host.previous()
                self.persist(stage='backup')
                self.host.backup()
                shutil.copyfile(MAINTENANCE, self.state_dir / 'previous-maintenance.py')
                # The image ID is retained independently of the mutable :latest tag.
                run(['docker', 'image', 'tag', previous['image'], f'{IMAGE}:previous'])
                self.persist(phase='switching', candidate_sha=sha, previous=previous, failed_sha=sha)
                try:
                    self.persist(stage='switch')
                    self.host.switch(f'{IMAGE}:release-{sha}')
                    self.persist(stage='verify')
                    self.host.verify(sha)
                    self.persist(stage='monitor')
                    self.host.install_monitor(release / 'scripts/beta_maintenance.py')
                    self.persist(phase='idle', stage='completed', deployed_sha=sha, failed_sha=None, deployed_at=time.time())
                except Exception:
                    try:
                        self.rollback()
                    except Exception:
                        self.persist(phase='rollback_failed')
                        raise Failure('Нужна ручная проверка: новая версия и откат не прошли проверку')
                    raise Failure('Новая версия отклонена; прежний образ и наблюдатель восстановлены')
        except Exception as error:
            self.persist(failed_sha=sha, retry_after=time.time() + 300)
            message = str(error) if isinstance(error, Failure) else 'Ошибка подготовки выпуска'
            self.host.notify(f'{sha[:7]} · {self.state.get("stage", "prepare")}: {message}. '
                             'Автоповтор через 5 минут, не более 3 попыток. '
                             'Ручной повтор: sudo python3 /usr/local/lib/cpp-recall/auto_deploy.py --retry.')
            raise Failure(message) from error
        self.host.notify(f'{sha[:7]} обновлён автоматически; резервная копия и HTTPS проверены.')
        return 'deployed'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-dir', type=Path, default=Path('/opt/cpp-recall-bot'))
    parser.add_argument('--state-dir', type=Path, default=Path('/var/lib/cpp-recall-deploy'))
    parser.add_argument('--port', type=int, default=8081)
    parser.add_argument('--proxy-container', default='outbound-proxy')
    parser.add_argument('--retry', action='store_true')
    args = parser.parse_args()
    os.umask(0o077)
    args.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (args.state_dir / 'deploy.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('Другой выпуск ещё выполняется')
            return
        result = Deployer(Host(args), args.state_dir).tick(args.retry)
        print('auto_deploy:', result, flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # Never expose URLs with credentials, subprocess output, SQL, .env or initData.
        print(str(error) if isinstance(error, Failure) else 'Автодеплой не выполнен; рабочая версия не подтверждена',
              file=sys.stderr)
        sys.exit(1)
