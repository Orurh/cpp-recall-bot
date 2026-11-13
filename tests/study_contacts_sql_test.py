"""Execute the study export with real PostgreSQL and connection-local fixtures."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
from urllib.parse import unquote, urlparse
import transfer_pilot_report_test as legacy

ROOT = Path(__file__).resolve().parents[1]
QUERY = ROOT / 'scripts/export_study_contacts.sql'
FIXTURE = r"""
SET search_path = pg_temp;
SET timezone = 'Europe/Moscow';
CREATE TEMP TABLE sessions(id BIGINT, user_id BIGINT, mode TEXT);
CREATE TEMP TABLE attempts(id BIGINT, user_id BIGINT, session_id BIGINT, question_id TEXT,
    content_revision TEXT, correct BOOLEAN, attempt_number INTEGER, assisted BOOLEAN,
    answered_at TIMESTAMPTZ, answer TEXT);
CREATE TEMP TABLE learning_events(event_key TEXT, event_name TEXT, user_id BIGINT,
    session_id BIGINT, question_id TEXT, content_revision TEXT, assisted BOOLEAN,
    occurred_at TIMESTAMPTZ);
CREATE TEMP TABLE question_content_revisions(question_id TEXT, content_revision TEXT,
    content_snapshot JSONB);
CREATE TEMP TABLE questions(question_id TEXT, content_revision TEXT, content_snapshot JSONB);
CREATE TEMP TABLE analytics_meta(enabled_at TIMESTAMPTZ);
CREATE TEMP TABLE study_contact_tracking(started_at TIMESTAMPTZ);
CREATE TEMP TABLE study_contacts(id BIGINT, user_id BIGINT, session_id BIGINT,
    question_id TEXT, question_content_revision TEXT, attempt_number INTEGER, kind TEXT,
    reference_id TEXT, reference_content_revision TEXT, offered_at TIMESTAMPTZ);
CREATE TEMP TABLE users(user_id BIGINT, profile TEXT);
CREATE TEMP TABLE system_design_attempts(id BIGINT, user_id BIGINT, session_id BIGINT,
    question_id TEXT, answer TEXT);
INSERT INTO analytics_meta VALUES('2025-11-01T00:00:00Z');
INSERT INTO study_contact_tracking VALUES('2025-12-01T00:00:00Z');
INSERT INTO sessions VALUES(1,11,'miniapp:recommended'),(2,11,'practice'),(0,11,'practice'),(3,0,'practice');
INSERT INTO users VALUES(11,'PRIVATE_PROFILE');
INSERT INTO system_design_attempts VALUES(777,11,1,'SD_PRIVATE','PRIVATE_ANSWER');
INSERT INTO question_content_revisions VALUES
 ('q1',repeat('a',64),'{{"skill_id":"containers.vector","question_family_id":"vector.size","evidence_type":"recognition","content":"PRIVATE_CONTENT"}}');
INSERT INTO questions VALUES
 ('q2',repeat('b',64),'{{"skill_id":"WRONG_CURRENT_BANK","question_family_id":"WRONG_CURRENT_BANK","evidence_type":"application"}}');
INSERT INTO attempts VALUES
 (1,11,1,'q1',repeat('a',64),true,1,false,'2026-01-01T01:00:00.123456Z','PRIVATE_ANSWER'),
 (2,11,2,'q2',repeat('b',64),false,1,false,'2026-01-09T01:00:00Z','PRIVATE_ANSWER');
INSERT INTO learning_events SELECT 'answer:'||id,'answer',user_id,session_id,question_id,
    content_revision,assisted,answered_at FROM attempts;
INSERT INTO learning_events VALUES
 ('reference:1:q1','reference_opened',11,1,'q1',repeat('a',64),true,'2026-01-01T00:30:00Z'),
 ('theory:1:q1','theory_opened',11,1,'q1',repeat('a',64),true,'2026-01-01T00:40:00Z');
INSERT INTO study_contacts VALUES
 (1,11,1,'q1',repeat('a',64),1,'question_delivered',NULL,NULL,'2026-01-01T00:00:00Z'),
 (2,11,1,'q1',repeat('a',64),1,'question_delivered',NULL,NULL,'2026-01-01T00:00:01Z'),
 (3,11,1,'q1',repeat('a',64),1,'feedback_delivered',NULL,NULL,'2026-01-01T01:01:00Z'),
 (4,11,1,'q1',repeat('a',64),1,'reference_opened','vector.reference',repeat('c',64),'2026-01-01T03:02:00.654321+03'),
 (5,11,1,'q1',repeat('a',64),1,'theory_opened',NULL,NULL,'2026-01-01T01:03:00Z'),
 (6,11,1,'q1',repeat('a',64),1,'answer_revealed',NULL,NULL,'2026-01-01T01:04:00Z');
{extra}
SELECT 'before:' || (SELECT count(*) FROM attempts) || ':' ||
    (SELECT count(*) FROM study_contacts) || ':' || (SELECT count(*) FROM learning_events);
\i {query}
SELECT 'after:' || (SELECT count(*) FROM attempts) || ':' ||
    (SELECT count(*) FROM study_contacts) || ':' || (SELECT count(*) FROM learning_events);
"""


class StudyContactsSqlTest(unittest.TestCase):
    def test_real_export_reaches_contact_aware_cli_without_inflating_first_failure(self):
        document = self.export("""
DELETE FROM attempts; DELETE FROM study_contacts; DELETE FROM learning_events;
DELETE FROM question_content_revisions; DELETE FROM sessions;
UPDATE analytics_meta SET enabled_at='2026-11-01T00:00:00Z';
UPDATE study_contact_tracking SET started_at='2026-11-01T00:00:00Z';
INSERT INTO sessions VALUES(1,11,'miniapp'),(2,11,'miniapp'),(3,11,'miniapp'),(4,11,'miniapp');
INSERT INTO question_content_revisions VALUES
 ('ref',repeat('b',64),'{"skill_id":"references.fundamentals","question_family_id":"scope","evidence_type":"application"}'),
 ('oop',repeat('b',64),'{"skill_id":"oop.objects","question_family_id":"scope","evidence_type":"application"}'),
 ('intro',repeat('a',64),'{"skill_id":"raii.fundamentals","question_family_id":"scope","evidence_type":"application"}'),
 ('control',repeat('b',64),'{"skill_id":"raii.fundamentals","question_family_id":"scope","evidence_type":"application"}');
INSERT INTO attempts VALUES
 (1,11,1,'ref',repeat('b',64),true,1,false,'2026-11-01T12:00:00Z','PRIVATE'),
 (2,11,2,'oop',repeat('b',64),true,1,false,'2026-11-01T12:00:00Z','PRIVATE'),
 (3,11,3,'intro',repeat('a',64),true,1,false,'2026-11-02T12:00:00Z','PRIVATE'),
 (4,11,4,'control',repeat('b',64),false,1,false,'2026-11-05T13:00:00Z','PRIVATE');
INSERT INTO learning_events SELECT 'answer:'||id,'answer',user_id,session_id,question_id,
 content_revision,assisted,answered_at FROM attempts;
INSERT INTO study_contacts VALUES
 (1,11,4,'control',repeat('b',64),1,'question_delivered',NULL,NULL,'2026-11-05T12:00:00Z'),
 (2,11,4,'control',repeat('b',64),1,'question_delivered',NULL,NULL,'2026-11-05T12:30:00Z'),
 (3,11,4,'control',repeat('b',64),1,'answer_revealed',NULL,NULL,'2026-11-05T14:00:00Z');
""")
        result = legacy.TransferPilotReportTest.run_cli(self, document, legacy.PROTOCOL)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['first_recorded'], dict(total=1, correct=0, incorrect=1))
        self.assertEqual(report['control_encounters'], 1)
        self.assertNotIn('PRIVATE', result.stdout)

    def export(self, extra=''):
        url = urlparse(os.environ['TEST_DATABASE_URL'])
        self.assertIn(url.scheme, ('postgres', 'postgresql'))
        self.assertIn(url.hostname, ('127.0.0.1', 'localhost', '::1'),
                      'only a disposable loopback TEST_DATABASE_URL is allowed')
        env = {key: os.environ[key] for key in ('PATH', 'LANG', 'LC_ALL') if key in os.environ}
        env.update(PGHOST=url.hostname, PGPORT=str(url.port or 5432),
                   PGUSER=unquote(url.username or ''), PGPASSWORD=unquote(url.password or ''),
                   PGDATABASE=unquote(url.path.lstrip('/')), PGCONNECT_TIMEOUT='5')
        result = subprocess.run(['psql', '-X', '-qAt', '--no-password', '-v', 'ON_ERROR_STOP=1'],
            input=FIXTURE.format(extra=extra, query="'" + str(QUERY).replace("'", "''") + "'"),
            text=True, capture_output=True, env=env, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.strip().splitlines()
        self.assertEqual(len(lines), 3, result.stdout[:1000])
        self.assertEqual(lines[0].replace('before:', 'after:', 1), lines[2],
                         'export changed source rows')
        document = json.loads(lines[1])
        self.assertEqual(document['schema'], 'cpp-recall-study-input/v1')
        self.assertIs(document['read_only'], True)
        return document

    def test_exports_owned_answers_contacts_and_legacy_help_without_private_content(self):
        document = self.export()
        self.assertIs(document['complete'], True)
        self.assertEqual(document['tracking_since'], '2025-11-01T00:00:00.000000Z')
        self.assertEqual(document['contacts_since'], '2025-12-01T00:00:00.000000Z')
        self.assertRegex(document['observed_until'], r'^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{6}Z$')
        events = {event['id']: event for event in document['events']}
        self.assertEqual(set(events), {'answer:1', 'answer:2', 'contact:1', 'contact:2',
            'contact:3', 'contact:4', 'contact:5', 'contact:6',
            'legacy-help:reference:1:q1', 'legacy-help:theory:1:q1'})
        answer = events['answer:1']
        self.assertEqual((answer['user_id'], answer['session_id'], answer['direction']), (11, 1, 'cpp'))
        self.assertEqual((answer['skill_id'], answer['family_id'], answer['evidence_type']),
                         ('containers.vector', 'vector.size', 'recognition'))
        self.assertEqual(answer['occurred_at'], '2026-01-01T01:00:00.123456Z')
        self.assertEqual((answer['event'], answer['attempt_number'], answer['correct'],
                          answer['assisted'], answer['help_known']), ('answer', 1, True, False, True))
        contacts = [event for event in events.values() if event['event'] == 'contact']
        self.assertEqual({event['kind'] for event in contacts}, {'question_delivered',
            'feedback_delivered', 'reference_opened', 'theory_opened', 'answer_revealed'})
        self.assertNotEqual(events['contact:1']['occurred_at'], events['contact:2']['occurred_at'])
        self.assertEqual(events['contact:4']['occurred_at'], '2026-01-01T00:02:00.654321Z')
        self.assertEqual(events['contact:4']['reference_id'], 'vector.reference')
        self.assertEqual(events['contact:4']['reference_revision'], 'c' * 64)
        self.assertIsNone(events['contact:1']['reference_id'])
        self.assertIsNone(events['contact:1']['reference_revision'])
        for event in document['events']:
            self.assertGreater(event['user_id'], 0)
            self.assertGreater(event['session_id'], 0)
            self.assertNotIn('answer', event)
            if event['event'] == 'help':
                self.assertIsNone(event['attempt_number'])
                self.assertIs(event['assisted'], True)
        encoded = json.dumps(document)
        for private in ('PRIVATE_ANSWER', 'PRIVATE_CONTENT', 'PRIVATE_PROFILE', 'WRONG_CURRENT_BANK', 'SD_PRIVATE'):
            self.assertNotIn(private, encoded)
        self.assertEqual(document['events'], sorted(document['events'],
            key=lambda event: (event['occurred_at'], event['id'])))

    def test_metadata_uses_only_the_pinned_archive_and_preserves_unknown_revisions(self):
        document = self.export("""INSERT INTO study_contacts VALUES
            (7,11,1,'q1',NULL,1,'question_delivered',NULL,NULL,'2026-01-01T00:00:02Z');""")
        events = {event['id']: event for event in document['events']}
        self.assertEqual(events['answer:2']['revision'], 'b' * 64)
        self.assertIsNone(events['contact:7']['revision'])
        for event_id in ('answer:2', 'contact:7'):
            for field in ('skill_id', 'family_id', 'evidence_type'):
                self.assertIsNone(events[event_id][field])
        document = self.export('DELETE FROM question_content_revisions;')
        self.assertTrue(all(event['skill_id'] is None for event in document['events']))

    def test_invalid_ownership_nonpositive_ids_or_future_events_are_excluded(self):
        extra = """
        INSERT INTO sessions VALUES(4,99,'practice');
        INSERT INTO attempts SELECT id+100,99,session_id,question_id,content_revision,
            correct,attempt_number,assisted,answered_at,answer FROM attempts;
        INSERT INTO attempts SELECT id+200,user_id,0,question_id,content_revision,
            correct,attempt_number,assisted,answered_at,answer FROM attempts WHERE id < 10;
        INSERT INTO attempts SELECT id+300,0,3,question_id,content_revision,
            correct,attempt_number,assisted,answered_at,answer FROM attempts WHERE id < 10;
        INSERT INTO attempts SELECT id+400,user_id,session_id,question_id,content_revision,
            correct,attempt_number,assisted,now()+interval '1 day',answer FROM attempts WHERE id < 10;
        INSERT INTO study_contacts SELECT id+100,99,session_id,question_id,question_content_revision,
            attempt_number,kind,reference_id,reference_content_revision,offered_at FROM study_contacts;
        INSERT INTO study_contacts SELECT id+200,user_id,0,question_id,question_content_revision,
            attempt_number,kind,reference_id,reference_content_revision,offered_at FROM study_contacts WHERE id < 10;
        INSERT INTO study_contacts SELECT id+300,0,3,question_id,question_content_revision,
            attempt_number,kind,reference_id,reference_content_revision,offered_at FROM study_contacts WHERE id < 10;
        INSERT INTO study_contacts SELECT id+400,user_id,session_id,question_id,question_content_revision,
            attempt_number,kind,reference_id,reference_content_revision,now()+interval '1 day' FROM study_contacts WHERE id < 10;
        INSERT INTO learning_events SELECT 'bad-owner:'||event_key,event_name,99,session_id,
            question_id,content_revision,assisted,occurred_at FROM learning_events;
        INSERT INTO learning_events SELECT 'zero-session:'||event_key,event_name,user_id,0,
            question_id,content_revision,assisted,occurred_at FROM learning_events WHERE event_key LIKE 'reference:%';
        INSERT INTO learning_events SELECT 'zero-user:'||event_key,event_name,0,3,
            question_id,content_revision,assisted,occurred_at FROM learning_events WHERE event_key LIKE 'reference:%';
        INSERT INTO learning_events SELECT 'future:'||event_key,event_name,user_id,session_id,
            question_id,content_revision,assisted,now()+interval '1 day' FROM learning_events WHERE event_key LIKE 'reference:%';
        """
        self.assertEqual(self.export(extra)['events'], self.export()['events'])

    def test_help_known_requires_every_matching_answer_provenance_field(self):
        for mutation in ("DELETE FROM learning_events WHERE event_key='answer:1'",
                         "UPDATE learning_events SET event_name='other' WHERE event_key='answer:1'",
                         "UPDATE learning_events SET user_id=99 WHERE event_key='answer:1'",
                         "UPDATE learning_events SET session_id=2 WHERE event_key='answer:1'",
                         "UPDATE learning_events SET question_id='q2' WHERE event_key='answer:1'",
                         "UPDATE learning_events SET content_revision=repeat('b',64) WHERE event_key='answer:1'",
                         "UPDATE learning_events SET assisted=true WHERE event_key='answer:1'"):
            with self.subTest(mutation=mutation):
                events = {event['id']: event for event in self.export(mutation + ';')['events']}
                self.assertIs(events['answer:1']['help_known'], False)

    def test_missing_tracking_markers_remain_unknown(self):
        document = self.export('DELETE FROM analytics_meta; DELETE FROM study_contact_tracking;')
        self.assertIsNone(document['tracking_since'])
        self.assertIsNone(document['contacts_since'])

    def test_bounded_export_marks_overflow_incomplete_without_silently_truncating(self):
        for count, complete, exported in ((50000, True, 50000), (50001, False, 50001), (50002, False, 50001)):
            with self.subTest(count=count):
                document = self.export("""
                    DELETE FROM attempts; DELETE FROM learning_events; DELETE FROM study_contacts;
                    INSERT INTO study_contacts SELECT n,11,1,'q1',repeat('a',64),1,
                        'question_delivered',NULL,NULL,'2026-01-01T00:00:00Z'::timestamptz
                        FROM generate_series(1, %d) n;
                """ % count)
                self.assertIs(document['complete'], complete)
                self.assertEqual(len(document['events']), exported)


if __name__ == '__main__':
    if not os.environ.get('TEST_DATABASE_URL'):
        print('study_contacts_sql skipped: disposable TEST_DATABASE_URL is not set')
        sys.exit(77)
    unittest.main(verbosity=2)
