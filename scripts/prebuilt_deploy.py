#!/usr/bin/env python3
"""Trusted VM controller: green main -> pinned GHCR image, never a local build.

Uses the existing durable deployment/backup/rollback state machine. Host files and
this controller are installed explicitly, never updated from an incoming image.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re
import runpy
import shutil
import tempfile
import time
import urllib.parse

from auto_deploy import Deployer, Failure, Host, IMAGE, MAINTENANCE, REPOSITORY, SHA, run

REGISTRY = 'ghcr.io/orurh/cpp-recall'
IMAGE_ID = re.compile(r'sha256:[a-f0-9]{64}')
PINNED = re.compile(re.escape(REGISTRY) + r'@sha256:[a-f0-9]{64}')


def reviewed_digest(record, sha):
    labels = record.get('Config', {}).get('Labels') or {}
    if (not SHA.fullmatch(sha) or record.get('Architecture') != 'amd64'
            or labels.get('org.opencontainers.image.revision') != sha
            or labels.get('org.opencontainers.image.source') != f'https://github.com/{REPOSITORY}'):
        raise Failure('Образ не соответствует проверенному коммиту и репозиторию')
    digests = {value for value in record.get('RepoDigests', []) if PINNED.fullmatch(value)}
    if len(digests) != 1:
        raise Failure('Не удалось однозначно закрепить digest образа')
    return digests.pop()


def persist_image(path, image):
    """Change only the release reference, atomically, without logging dotenv."""
    if not (IMAGE_ID.fullmatch(image) or PINNED.fullmatch(image)) or path.is_symlink():
        raise Failure('Недопустимая ссылка или путь конфигурации образа')
    lines = path.read_text().splitlines()
    indices = [i for i, line in enumerate(lines) if line.startswith('CPP_RECALL_IMAGE=')]
    if len(indices) != 1:
        raise Failure('Конфигурация должна содержать ровно одну ссылку на образ')
    lines[indices[0]] = 'CPP_RECALL_IMAGE=' + image
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, prefix='.image-', delete=False) as stream:
            temporary = Path(stream.name)
            os.fchmod(stream.fileno(), 0o600)
            stream.write('\n'.join(lines) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
        temporary = None
        descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


class PrebuiltHost(Host):
    def __init__(self, args):
        super().__init__(args)
        self.candidates = {}

    def build(self, sha, release):
        # The state machine's "build" stage is a download in this host adapter.
        if not SHA.fullmatch(sha):
            raise Failure('Некорректный коммит образа')
        usage = shutil.disk_usage(self.root)
        if usage.free < max(2 * 1024 ** 3, usage.total * .1):
            raise Failure('Недостаточно места для безопасного скачивания образа')
        reference = f'{REGISTRY}:{sha}'
        run(['docker', 'pull', '--platform', 'linux/amd64', reference], timeout=600)
        record = json.loads(run(['docker', 'image', 'inspect', reference]))[0]
        digest = reviewed_digest(record, sha)
        pinned = json.loads(run(['docker', 'image', 'inspect', digest]))[0]
        if pinned['Id'] != record['Id'] or reviewed_digest(pinned, sha) != digest:
            raise Failure('Закреплённый образ изменился при проверке')
        self.candidates[sha] = digest

    def maintenance(self):
        if self.monitor is None:
            scope = runpy.run_path(str(MAINTENANCE))
            args = argparse.Namespace(compose_project=self.args.compose_project, port=self.args.port,
                                      state_dir=self.args.monitor_state_dir, backup_dir=self.args.backup_dir,
                                      tls_connect_host=self.args.tls_connect_host)
            self.monitor = scope['Monitor'](args)
        return self.monitor

    def previous(self):
        container = f'{self.args.compose_project}-bot-1'
        image = run(['docker', 'inspect', '--format', '{{.Image}}', container]).decode().strip()
        if not IMAGE_ID.fullmatch(image):
            raise Failure('Не удалось сохранить прежний образ')
        return {'image': image, 'version': self.technical()['version']}

    def switch(self, image):
        prefix = f'{IMAGE}:release-'
        if image.startswith(prefix):
            sha = image[len(prefix):]
            image = self.candidates.get(sha, '')
        if not (IMAGE_ID.fullmatch(image) or PINNED.fullmatch(image)):
            raise Failure('Непроверенный или незакреплённый образ запрещён')
        run(['docker', 'image', 'inspect', image])  # Must already exist locally.
        persist_image(self.root / '.env', image)
        command = ['docker', 'compose', '--project-name', self.args.compose_project,
                   '--project-directory', str(self.root), '--env-file', str(self.root / '.env'),
                   '-f', str(self.root / 'compose.yaml'), '-f', str(self.root / 'host.compose.yaml'),
                   '--profile', 'release']
        run(command + ['up', '-d', '--no-deps', '--no-build', '--pull', 'never', 'bot'], timeout=120)

    def verify(self, version):
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            try:
                technical = self.technical()
                if technical['version'] == version and self.maintenance().local_request('/health/ready')['ready']:
                    url = urllib.parse.urlsplit(self.maintenance().credentials()['MINI_APP_URL'])
                    if url.scheme != 'https' or not url.hostname or url.username or url.password:
                        raise Failure('Не настроен публичный HTTPS')
                    run(['curl', '--fail', '--silent', '--show-error', '--max-time', '8', '--noproxy', '*',
                         f'https://{url.hostname}{url.path.rstrip("/")}/health/ready'], timeout=10)
                    return
            except Exception:
                time.sleep(2)
        raise Failure('Новая версия не прошла проверку готовности')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project-dir', type=Path, default=Path('/opt/cpp-recall-bot'))
    parser.add_argument('--state-dir', type=Path, default=Path('/var/lib/cpp-recall-deploy'))
    parser.add_argument('--compose-project', default='cpp-recall-germany')
    parser.add_argument('--port', type=int, default=8081)
    parser.add_argument('--monitor-state-dir', type=Path, default=Path('/var/lib/cpp-recall-monitoring'))
    parser.add_argument('--backup-dir', type=Path, default=Path('/var/lib/cpp-recall/backups'))
    parser.add_argument('--tls-connect-host', help='External TLS proxy address; defaults to MINI_APP_URL host')
    parser.add_argument('--retry', action='store_true')
    args = parser.parse_args()
    args.proxy_container = ''  # No dependency on the old host's other services proxy.
    if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,62}', args.compose_project) or not 1 <= args.port <= 65535:
        parser.error('Invalid project or port')
    os.umask(0o077)
    args.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (args.state_dir / 'deploy.lock').open('a') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print('Другой выпуск ещё выполняется')
            return
        result = Deployer(PrebuiltHost(args), args.state_dir).tick(args.retry)
        print('prebuilt_deploy:', result, flush=True)


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        print(str(error) if isinstance(error, Failure) else 'Выпуск готового образа не выполнен', flush=True)
        raise SystemExit(1)
