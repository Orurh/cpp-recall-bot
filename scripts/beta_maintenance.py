#!/usr/bin/env python3
"""Private host checks and verified backups. No dependency or credential on command line."""

import argparse
import datetime as dt
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import shutil
import socket
import ssl
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import uuid


class Failure(Exception):
    """Only fixed, non-sensitive messages may be passed here."""


def command(args, label, *, output=None, timeout=15):
    try:
        result = subprocess.run(args, stdout=output or subprocess.PIPE,
                                stderr=subprocess.PIPE, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise Failure(label) from error
    if result.returncode:
        raise Failure(label)
    return result.stdout


def read_json(path):
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        return {}


def save_json(path, value, mode=0o600):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    with os.fdopen(fd, 'w') as stream:
        json.dump(value, stream, ensure_ascii=False)
    os.chmod(temporary, mode)
    temporary.replace(path)


def stamp(epoch=None):
    return dt.datetime.fromtimestamp(time.time() if epoch is None else epoch, dt.timezone.utc).isoformat()


def certificate_expiration(hostname, connect_host='127.0.0.1'):
    """Verify the public name even when its proxy is on a separate host."""
    context = ssl.create_default_context()
    with socket.create_connection((connect_host, 443), timeout=4) as connection:
        with context.wrap_socket(connection, server_hostname=hostname) as tls:
            return ssl.cert_time_to_seconds(tls.getpeercert()['notAfter'])


def init_data(token, owner):
    fields = {'auth_date': str(int(time.time())), 'user': json.dumps({'id': owner}, separators=(',', ':'))}
    check = '\n'.join(f'{key}={value}' for key, value in sorted(fields.items()))
    secret = hmac.new(b'WebAppData', token.encode(), hashlib.sha256).digest()
    fields['hash'] = hmac.new(secret, check.encode(), hashlib.sha256).hexdigest()
    return urllib.parse.urlencode(fields)


def alert_decision(state, issues, now):
    """Three bad checks, hourly reminders, one recovery; caller persists delivered only."""
    state['consecutive_failures'] = state.get('consecutive_failures', 0) + 1 if issues else 0
    if issues and state['consecutive_failures'] >= 3 and now - state.get('last_alert', 0) >= 3600:
        return 'problem'
    if not issues and state.get('alert_open'):
        return 'recovery'
    return None


def http_issues(previous, current):
    if previous.get('boot') != current['boot']:
        return []
    issues = []
    count = current['requests'] - previous['requests']
    errors = current['errors'] - previous['errors']
    if errors >= 3 and errors / max(count, 1) >= 0.1:
        issues.append('Повышенная доля ошибок HTTP (не менее 10%)')
    if current['rejected'] - previous.get('rejected', current['rejected']) >= 3:
        issues.append('Перегрузка очереди HTTP: отклонены соединения')
    return issues


class Monitor:
    def __init__(self, args):
        self.args = args
        self.state_path = args.state_dir / 'private.json'
        self.public_path = args.state_dir / 'public' / 'status.json'
        self.state = read_json(self.state_path)
        self.bot = f'{args.compose_project}-bot-1'
        self.postgres = f'{args.compose_project}-postgres-1'
        self.env = {}

    def container(self, name):
        raw = command(['docker', 'inspect', name], 'Не удалось проверить контейнер')
        return json.loads(raw)[0]

    def credentials(self):
        if not self.env:
            data = self.container(self.bot)
            self.env = dict(item.split('=', 1) for item in data['Config']['Env'] if '=' in item)
        return self.env

    def local_request(self, path, body=None):
        headers = {}
        if body is not None:
            env = self.credentials()
            owner = int(env.get('MINI_APP_FEEDBACK_ADMIN_ID', '0'))
            if owner <= 0:
                raise Failure('Не настроен владелец статистики')
            headers = {'Authorization': 'tma ' + init_data(env['TELEGRAM_BOT_TOKEN'], owner),
                       'Content-Type': 'application/json'}
        request = urllib.request.Request(f'http://127.0.0.1:{self.args.port}{path}',
                                         data=None if body is None else json.dumps(body).encode(), headers=headers)
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=8) as response:
                return json.load(response)
        except Exception as error:
            raise Failure('HTTP-сервис не готов или не отвечает') from error

    def send(self, message):
        env = self.credentials()
        owner = int(env.get('MINI_APP_FEEDBACK_ADMIN_ID', '0'))
        if owner <= 0:
            raise Failure('Не настроен адресат оповещений')
        proxies = {}
        proxy = env.get('HTTPS_PROXY') or env.get('https_proxy')
        if proxy:
            parsed = urllib.parse.urlsplit(proxy)
            if parsed.scheme != 'http' or not parsed.hostname or parsed.username:
                raise Failure('Неподдерживаемая настройка прокси оповещений')
            host = parsed.hostname
            # Docker DNS is not available on the host; inspect the existing proxy, never restart it.
            try:
                socket.getaddrinfo(host, parsed.port or 80)
            except socket.gaierror:
                networks = self.container(host)['NetworkSettings']['Networks']
                host = next((item['IPAddress'] for item in networks.values() if item.get('IPAddress')), '')
                if not host:
                    raise Failure('Прокси оповещений недоступен')
            proxies['https'] = f'http://{host}:{parsed.port or 80}'
        payload = json.dumps({'chat_id': owner, 'text': message}).encode()
        request = urllib.request.Request('https://api.telegram.org/bot' + env['TELEGRAM_BOT_TOKEN'] + '/sendMessage',
                                         data=payload, headers={'Content-Type': 'application/json'})
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler(proxies)).open(request, timeout=12) as response:
                if not json.load(response).get('ok'):
                    raise Failure('Оповещение не доставлено')
        except Exception as error:
            # urllib exceptions may contain the token in the URL: never log their text.
            raise Failure('Оповещение не доставлено') from error

    def publish(self, issues):
        usage = shutil.disk_usage(self.args.backup_dir.parent)
        save_json(self.public_path, {'checked_at': stamp(), 'backup_verified_at': self.state.get('backup_verified_at'),
                  'backup_bytes': self.state.get('backup_bytes'), 'disk_free_bytes': usage.free, 'issues': issues}, 0o644)
        save_json(self.state_path, self.state)

    def check(self):
        issues = []
        try:
            container = self.container(self.bot)
            if not container['State']['Running']:
                issues.append('Контейнер приложения остановлен')
            restarts = container.get('RestartCount', 0)
            if restarts > self.state.get('restarts', restarts):
                issues.append('Контейнер приложения перезапустился')
            self.state['restarts'] = restarts
            if not self.local_request('/health/live').get('alive') or not self.local_request('/health/ready').get('ready'):
                issues.append('Приложение не готово')
            metrics = self.local_request('/api/v1/admin/statistics', {'days': 1})['technical']
            now = time.time()
            if metrics['uptime_seconds'] > 120 and now - metrics['telegram_last_success'] > 120:
                issues.append('Нет успешного опроса Telegram более двух минут')
            previous = self.state.get('http', {})
            current = {'boot': metrics['started_at'], 'requests': metrics['http_requests'],
                       'errors': metrics['http_server_errors'], 'rejected': metrics.get('rejected_connections', 0)}
            issues.extend(http_issues(previous, current))
            self.state['http'] = current
            if metrics['pool_available'] == 0:
                issues.append('Все соединения PostgreSQL заняты')
        except Failure as error:
            issues.append(str(error))
        usage = shutil.disk_usage(self.args.backup_dir.parent)
        if usage.free < max(1073741824, usage.total * 0.10):
            issues.append('Мало свободного места: менее 10% или 1 ГиБ')
        if time.time() - self.state.get('backup_verified_epoch', 0) > 36 * 3600:
            issues.append('Нет проверенной резервной копии за последние 36 часов')
        if self.state.get('backup_failed'):
            issues.append('Последнее резервное копирование или проверка восстановления завершились ошибкой')
        try:
            hostname = urllib.parse.urlsplit(self.credentials().get('MINI_APP_URL', '')).hostname
            if hostname:
                expires = certificate_expiration(hostname, getattr(self.args, 'tls_connect_host', '127.0.0.1'))
                if expires - time.time() < 14 * 86400:
                    issues.append('До окончания TLS-сертификата осталось менее 14 дней')
        except Exception:
            issues.append('Не удалось проверить HTTPS и TLS-сертификат')
        decision = alert_decision(self.state, issues, time.time())
        if decision:
            try:
                self.send('C++ Recall: ' + ('сервис восстановлен.' if decision == 'recovery' else '\n' + '\n'.join(issues)))
                self.state.update(last_alert=time.time(), alert_open=decision == 'problem')
            except Failure:
                print('Оповещение не доставлено; повтор при следующей проверке', file=sys.stderr)
        self.publish(issues)
        print('Проверка: ' + ('есть замечания' if issues else 'всё в порядке'))

    def backup(self):
        target = self.args.backup_dir
        target.mkdir(parents=True, exist_ok=True, mode=0o700)
        os.chmod(target, 0o700)
        config = self.container(self.postgres)
        env = dict(item.split('=', 1) for item in config['Config']['Env'] if '=' in item)
        user, database = env['POSTGRES_USER'], env['POSTGRES_DB']
        if not all(re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]{0,62}', item) for item in (user, database)):
            raise Failure('Неподдерживаемое имя пользователя или базы')
        filename = 'database-' + dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '.dump'
        temporary = target / (filename + '.partial')
        result = target / filename
        test_database = 'cpp_recall_restore_check_' + uuid.uuid4().hex
        created = False
        try:
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'wb') as stream:
                command(['docker', 'exec', self.postgres, 'pg_dump', '-U', user, '-d', database, '-Fc'],
                        'Ошибка создания резервной копии', output=stream, timeout=600)
            temporary.replace(result)
            command(['docker', 'exec', self.postgres, 'createdb', '-U', user, test_database],
                    'Не удалось создать изолированную базу для проверки')
            created = True
            with result.open('rb') as stream:
                process = subprocess.run(['docker', 'exec', '-i', self.postgres, 'pg_restore', '-U', user,
                                          '-d', test_database, '--exit-on-error', '--no-owner'],
                                         stdin=stream, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=600)
            if process.returncode:
                raise Failure('Не удалось восстановить проверочную базу')
            sql = "SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version = '012_learning_statistics.sql') AND (SELECT count(*) >= 0 FROM attempts);"
            verified = command(['docker', 'exec', self.postgres, 'psql', '-U', user, '-d', test_database,
                                '-At', '-v', 'ON_ERROR_STOP=1', '-c', sql], 'Ошибка проверки восстановленной схемы')
            if verified.strip() != b't':
                raise Failure('Восстановленная схема не прошла проверку')
            self.state.update(backup_verified_at=stamp(), backup_verified_epoch=time.time(),
                              backup_bytes=result.stat().st_size, backup_failed=False)
            # Only files created by this script, never manual backups or unrelated directories.
            for old in target.iterdir():
                if re.fullmatch(r'database-\d{8}T\d{12}Z\.dump', old.name) and old.is_file():
                    if old.stat().st_mtime < time.time() - 14 * 86400:
                        old.unlink()
        except Exception as error:
            self.state['backup_failed'] = True
            if isinstance(error, Failure):
                raise
            raise Failure('Ошибка резервного копирования или восстановления') from error
        finally:
            try:
                if created:
                    command(['docker', 'exec', self.postgres, 'dropdb', '-U', user, test_database],
                            'Не удалось удалить только проверочную базу', timeout=60)
            except Failure:
                self.state['backup_failed'] = True
                raise
            finally:
                save_json(self.state_path, self.state)
        print('Резервная копия создана и восстановлена в изолированную проверочную базу')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('check', 'backup', 'test-alert'))
    parser.add_argument('--compose-project', default='cpp-recall-bot')
    parser.add_argument('--port', type=int, default=8081)
    parser.add_argument('--tls-connect-host', default='127.0.0.1',
                        help='TLS proxy address; certificate is verified against MINI_APP_URL')
    parser.add_argument('--state-dir', type=Path, default=Path('/var/lib/cpp-recall-monitoring'))
    parser.add_argument('--backup-dir', type=Path, default=Path('/var/lib/cpp-recall/backups'))
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,62}', args.compose_project) or not 1 <= args.port <= 65535:
        parser.error('Invalid project or port')
    args.state_dir.mkdir(parents=True, exist_ok=True, mode=0o755)
    (args.state_dir / 'public').mkdir(exist_ok=True, mode=0o755)
    os.chmod(args.state_dir, 0o755)
    os.chmod(args.state_dir / 'public', 0o755)
    args.backup_dir.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    monitor = Monitor(args)
    if args.action == 'test-alert':
        monitor.send('C++ Recall: мониторинг включён. Это проверочное оповещение владельцу. Ежедневная копия проверяется восстановлением в отдельную базу; статистика доступна во вкладке «Статистика».')
        print('Проверочное оповещение доставлено')
    elif args.action == 'backup':
        monitor.backup()
    else:
        monitor.check()


if __name__ == '__main__':
    try:
        main()
    except Exception as error:
        # Never print exception details from Docker, urllib or environment contents.
        print(str(error) if isinstance(error, Failure) else 'Не удалось выполнить обслуживание C++ Recall', file=sys.stderr)
        sys.exit(1)
