"""Real export query against connection-local temporary tables only."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from urllib.parse import unquote, urlparse

ROOT = Path(__file__).resolve().parents[1]
QUERY = ROOT / 'scripts/export_retrieval.sql'
FIXTURE = """
CREATE TEMP TABLE sessions(id BIGINT, user_id BIGINT, mode TEXT);
CREATE TEMP TABLE attempts(id BIGINT, user_id BIGINT, session_id BIGINT, question_id TEXT,
    content_revision TEXT, correct BOOLEAN, attempt_number INTEGER, assisted BOOLEAN,
    answered_at TIMESTAMPTZ, answer TEXT);
CREATE TEMP TABLE learning_events(event_key TEXT, event_name TEXT, user_id BIGINT,
    session_id BIGINT, question_id TEXT, content_revision TEXT, assisted BOOLEAN,
    occurred_at TIMESTAMPTZ);
CREATE TEMP TABLE question_content_revisions(question_id TEXT, content_revision TEXT, content_snapshot JSONB);
CREATE TEMP TABLE analytics_meta(enabled_at TIMESTAMPTZ);
INSERT INTO analytics_meta VALUES(now() - INTERVAL '30 days');
INSERT INTO sessions VALUES(1,11,'miniapp:recommended'),(2,11,'miniapp:recommended');
INSERT INTO question_content_revisions VALUES
 ('q1',repeat('a',64),'{{"skill_id":"containers.vector","question_family_id":"vector.size","evidence_type":"recognition"}}'),
 ('q2',repeat('b',64),'{{"skill_id":"containers.vector","question_family_id":"vector.capacity","evidence_type":"application"}}');
INSERT INTO attempts VALUES
 (1,11,1,'q1',repeat('a',64),true,1,false,now()-INTERVAL '8 days','PRIVATE_ANSWER'),
 (2,11,2,'q1',repeat('a',64),false,1,false,now()-INTERVAL '1 day','PRIVATE_ANSWER');
INSERT INTO learning_events SELECT 'answer:'||id,'answer',user_id,session_id,question_id,
    content_revision,assisted,answered_at FROM attempts;
{extra}
\i {query}
SELECT 'after:' || count(*) FROM attempts;
"""


class RetrievalReportSqlTest(unittest.TestCase):
    def export(self, extra=''):
        url = urlparse(os.environ['TEST_DATABASE_URL'])
        self.assertIn(url.hostname, ('127.0.0.1', 'localhost'))
        env = dict(os.environ, PGHOST=url.hostname, PGPORT=str(url.port or 5432),
                   PGUSER=unquote(url.username or ''), PGPASSWORD=unquote(url.password or ''),
                   PGDATABASE=unquote(url.path.lstrip('/')))
        result = subprocess.run(['psql', '-X', '-qAt', '--no-password', '-v', 'ON_ERROR_STOP=1'],
            input=FIXTURE.format(extra=extra, query="'" + str(QUERY) + "'"),
            text=True, capture_output=True, env=env, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.strip().splitlines()
        self.assertEqual(lines[-1], 'after:2', 'export changed stored answers')
        document = json.loads(lines[-2])
        self.assertIs(document['read_only'], True)
        self.assertIs(document['complete'], True)
        report = subprocess.run([sys.executable, str(ROOT / 'scripts/report_retrieval.py')],
            input=json.dumps(document), text=True, capture_output=True, timeout=10)
        self.assertEqual(report.returncode, 0, report.stderr)
        return document, json.loads(report.stdout)

    def test_actual_sql_exports_delayed_first_failure_without_raw_answers(self):
        document, report = self.export()
        self.assertEqual(report['delayed']['total'], 1)
        self.assertEqual(report['delayed']['incorrect'], 1)
        self.assertNotIn('PRIVATE_ANSWER', json.dumps(document))
        self.assertNotIn('answer', document['events'][0].keys() - {'event'})

    def test_help_without_an_answer_is_exported_and_shortens_observed_pause(self):
        document, report = self.export("""INSERT INTO learning_events VALUES
          ('reference:2:q2','reference_opened',11,2,'q2',repeat('b',64),true,
           now()-INTERVAL '1 day 1 hour');""")
        self.assertEqual(report['help_events'], 1)
        self.assertEqual(report['delayed']['total'], 0)
        self.assertEqual(report['outcomes']['short_pause'], 1)

    def test_missing_answer_provenance_does_not_certify_legacy_false_help(self):
        _, report = self.export("DELETE FROM learning_events WHERE event_key='answer:2';")
        self.assertEqual(report['outcomes']['unknown_assistance'], 1)
        self.assertEqual(report['delayed']['total'], 0)

    def test_missing_archived_metadata_is_not_replaced_with_current_bank(self):
        _, report = self.export("DELETE FROM question_content_revisions;")
        self.assertEqual(report['unknown_metadata_events'], 2)
        self.assertEqual(report['delayed']['total'], 0)


if __name__ == '__main__':
    if not os.environ.get('TEST_DATABASE_URL'):
        print('retrieval_report_sql skipped: disposable TEST_DATABASE_URL is not set')
        sys.exit(77)
    unittest.main(verbosity=2)
