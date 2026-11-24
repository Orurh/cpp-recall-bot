#!/usr/bin/env python3
"""Compile skill-owned JSON into static C++ catalog data, never runtime files.

Skill/source semantics and exact question revisions are also checked by the real
C++ catalog validators. This script does not mint or refresh reviewed revisions.
"""
import argparse
import json
from pathlib import Path
import re


SECTIONS = ('theory', 'references', 'theory_bindings', 'reference_bindings')
TEXT_FIELDS = ('id', 'title', 'principle', 'mechanism', 'example', 'common_mistake')
ARTICLE_FIELDS = set(TEXT_FIELDS) | {'catalog_order', 'sources', 'source_kind'}
ID = re.compile(r'[a-z][a-z0-9_]*(?:\.[a-z0-9_]+)*\Z')
SHA = re.compile(r'[0-9a-f]{64}\Z')
DESIGN_SOURCES = {
    'https://blog.cleancoder.com/uncle-bob/2014/05/08/SingleReponsibilityPrinciple.html',
    'https://blog.cleancoder.com/uncle-bob/2014/05/12/TheOpenClosedPrinciple.html',
    'https://blog.cleancoder.com/uncle-bob/2020/10/18/Solid-Relevance.html',
    'https://www.cs.cmu.edu/Groups/venari/subtype-toplas.html',
}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_json(path):
    def invalid_constant(value):
        raise ValueError('invalid JSON constant: ' + value)
    try:
        return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique_object,
                          parse_constant=invalid_constant)
    except (OSError, UnicodeError, ValueError) as error:
        raise ValueError(f'{path}: {error}') from error


def fields(value, expected, where):
    if type(value) is not dict or set(value) != expected:
        raise ValueError(f'{where}: expected exactly {sorted(expected)}')


def text(value, where):
    if type(value) is not str or not value or '\0' in value:
        raise ValueError(f'{where}: expected a nonempty string without NUL')
    try:
        value.encode('utf-8')
    except UnicodeError as error:
        raise ValueError(f'{where}: invalid Unicode') from error
    return value


def identifier(value, where):
    if not ID.fullmatch(text(value, where)):
        raise ValueError(f'{where}: invalid identifier')
    return value


def strings(value, where):
    if type(value) is not list or not value:
        raise ValueError(f'{where}: expected a nonempty string array')
    return [text(item, where) for item in value]


def article(value, theory, where):
    fields(value, ARTICLE_FIELDS | ({'primary'} if theory else set()), where)
    for field in TEXT_FIELDS:
        text(value[field], where + '.' + field)
    identifier(value['id'], where + '.id')
    if theory and type(value['primary']) is not bool:
        raise ValueError(where + ': primary must be boolean')
    kind = value['source_kind']
    if kind not in ('cpp_standard', 'design_principle'):
        raise ValueError(where + ': unknown source_kind')
    for source in strings(value['sources'], where + '.sources'):
        valid = source in DESIGN_SOURCES if kind == 'design_principle' else (
            source.startswith('https://timsong-cpp.github.io/cppwp/n4950/') or
            (theory and source.startswith('https://eel.is/c++draft/')))
        if not valid:
            raise ValueError(where + ': source does not match its subject')


def load_materials(root, skills=None, questions=None):
    """Load strict authoring data; optional maps come from caller-owned metadata.

    C++ SkillCatalog remains authoritative in production: generated owners are
    checked by ValidateReferenceCatalog rather than parsing C++ source here.
    """
    root = Path(root)
    paths = sorted(root.glob('topics/*/skills/*/materials*.json'))
    if not paths or set(root.rglob('materials*.json')) != set(paths):
        raise ValueError('materials must be at topics/<topic>/skills/<skill>/materials*.json')
    data = {section: [] for section in SECTIONS}
    data['owners'] = []
    for path in paths:
        owner = read_json(path)
        fields(owner, {'schema_version', 'skill_id', *SECTIONS}, str(path))
        if type(owner['schema_version']) is not int or owner['schema_version'] != 1:
            raise ValueError(f'{path}: unsupported schema version')
        skill = identifier(owner['skill_id'], str(path))
        topic = path.relative_to(root).parts[1]
        identifier(topic, str(path))
        if skill != path.parent.name or (skills is not None and skills.get(skill) != topic):
            raise ValueError(f'{path}: unknown or misplaced skill')
        if (skill, topic) not in data['owners']:
            if any(item[0] == skill for item in data['owners']):
                raise ValueError(f'{path}: skill appears under different topics')
            data['owners'].append((skill, topic))
        for section in SECTIONS:
            if type(owner[section]) is not list:
                raise ValueError(f'{path}: {section} must be an array')
            for index, item in enumerate(owner[section]):
                where = f'{path}:{section}[{index}]'
                if section in ('theory', 'references'):
                    article(item, section == 'theory', where)
                elif section == 'theory_bindings':
                    fields(item, {'catalog_order', 'question_family_id', 'theory_id'}, where)
                    identifier(item['question_family_id'], where)
                    identifier(item['theory_id'], where)
                else:
                    fields(item, {'catalog_order', 'question_id', 'content_revision', 'reference_ids'}, where)
                    identifier(item['question_id'], where)
                    if not SHA.fullmatch(text(item['content_revision'], where)):
                        raise ValueError(where + ': invalid reviewed card SHA-256')
                    ids = strings(item['reference_ids'], where)
                    if len(ids) != len(set(ids)):
                        raise ValueError(where + ': duplicate reference ids')
                if type(item['catalog_order']) is not int or item['catalog_order'] < 0:
                    raise ValueError(where + ': order must be a nonnegative integer')
                data[section].append(dict(item, skill_id=skill))
    for section in SECTIONS:
        data[section].sort(key=lambda item: item['catalog_order'])
        if [item['catalog_order'] for item in data[section]] != list(range(len(data[section]))):
            raise ValueError(section + ': duplicate or noncontiguous global order')
    theory = {}
    references = set()
    primary_skills = set()
    for item in data['theory']:
        if item['id'] in theory or (item['primary'] and item['skill_id'] in primary_skills):
            raise ValueError('duplicate theory id or primary skill')
        theory[item['id']] = item['skill_id']
        if item['primary']:
            primary_skills.add(item['skill_id'])
    for item in data['references']:
        if item['id'] in references:
            raise ValueError('duplicate reference id')
        references.add(item['id'])
    families = set()
    known_families = {(card['skill_id'], card.get('question_family_id'))
                      for card in questions.values()} if questions is not None else None
    for item in data['theory_bindings']:
        pair = (item['skill_id'], item['question_family_id'])
        if pair in families or theory.get(item['theory_id']) != item['skill_id']:
            raise ValueError('duplicate family binding or unknown/cross-skill theory')
        if known_families is not None and pair not in known_families:
            raise ValueError('theory binding references an unknown or cross-skill question family')
        families.add(pair)
    cards = set()
    for item in data['reference_bindings']:
        if item['question_id'] in cards or not set(item['reference_ids']) <= references:
            raise ValueError('duplicate card binding or unknown reference')
        cards.add(item['question_id'])
        if questions is not None:
            card = questions.get(item['question_id'])
            if card is None or card['skill_id'] != item['skill_id']:
                raise ValueError('unknown card or binding owned by another skill')
            if 'content_revision' in card and card['content_revision'] != item['content_revision']:
                raise ValueError('stale reviewed card binding')
    return data


def cpp_string(value):
    # Fixed-width octal prevents following digits from extending an escape and
    # preserves UTF-8 bytes regardless of the compiler's source character set.
    result = []
    for byte in value.encode('utf-8'):
        if byte in (34, 92):
            result.append('\\' + chr(byte))
        elif 32 <= byte < 127 and byte != 63:
            result.append(chr(byte))
        else:
            result.append(f'\\{byte:03o}')
    return '"' + ''.join(result) + '"'


def cpp_strings(values):
    return '{' + ', '.join(cpp_string(value) for value in values) + '}'


def generate(data, output):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    theory = ['// Generated from skill materials; edit JSON, not this file.',
              'const std::vector<TheoryBlock> kTheoryBlocks{']
    references = ['// Generated from skill materials; edit JSON, not this file.',
                  'const std::vector<ReferenceEntry> kEntries{']
    for section, lines in [('theory', theory), ('references', references)]:
        for item in data[section]:
            values = [cpp_string(item['id'])]
            if section == 'theory':
                values.append(cpp_string(item['skill_id']))
            values += [cpp_string(item[field]) for field in TEXT_FIELDS[1:]]
            values.append(cpp_strings(item['sources']))
            values.append('LearningSourceKind::' + ('kCppStandard' if item['source_kind'] == 'cpp_standard'
                                                    else 'kDesignPrinciple'))
            if section == 'theory':
                values.append('true' if item['primary'] else 'false')
            lines.append('    {' + ', '.join(values) + '},')
        lines.append('};')
    theory.append('constexpr std::array<TheoryBinding, ' + str(len(data['theory_bindings'])) + '> kTheoryBindings{{')
    for item in data['theory_bindings']:
        theory.append('    {' + ', '.join(cpp_string(item[field]) for field in
                      ('skill_id', 'question_family_id', 'theory_id')) + '},')
    theory.append('}};')
    references.append('const std::vector<ReferenceBinding> kBindings{')
    for item in data['reference_bindings']:
        references.append('    {' + cpp_string(item['question_id']) + ', ' + cpp_string(item['content_revision']) +
                          ', ' + cpp_strings(item['reference_ids']) + '},')
    references.append('};')
    references.append('constexpr std::pair<std::string_view, std::string_view> kMaterialOwners[]{')
    for skill, topic in data['owners']:
        references.append('    {' + cpp_string(skill) + ', ' + cpp_string(topic) + '},')
    references.append('};')
    for name, lines in [('theory_catalog_data.inc', theory), ('reference_catalog_data.inc', references)]:
        path = output / name
        content = '\n'.join(lines) + '\n'
        path.write_text(content, encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--content-root', type=Path, required=True)
    parser.add_argument('--questions', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        questions = {}
        for path in sorted(args.questions.rglob('*.json')):
            card = read_json(path)
            if type(card) is not dict or not {'id', 'skill_id'} <= set(card):
                raise ValueError(f'{path}: missing card identity')
            identifier(card['id'], str(path) + '.id')
            identifier(card['skill_id'], str(path) + '.skill_id')
            if card['id'] in questions:
                raise ValueError(f'{path}: duplicate question id')
            questions[card['id']] = card
        generate(load_materials(args.content_root, questions=questions), args.output_dir)
    except (ValueError, OSError) as error:
        parser.exit(1, f'learning content generation failed: {error}\n')


if __name__ == '__main__':
    main()
