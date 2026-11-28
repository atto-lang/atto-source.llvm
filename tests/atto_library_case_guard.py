"""Host-only baseline library corpus checks; not compiler or guest evidence."""
REQUIRED = frozenset(
    name + '.atto' for name in (
        'core_num', 'core_cmp', 'core_option_result', 'core_mem', 'alloc_arena',
        'std_io', 'std_path_posix', 'std_path_windows', 'aliases',
        'neg_unknown_core', 'neg_unknown_std', 'neg_traversal', 'neg_nested',
        'neg_private', 'neg_dotted_selector', 'neg_ores_selector',
    )
)


def validate_corpus(cases):
    missing = REQUIRED - {case.name for case in cases}
    if missing:
        raise ValueError('missing required library cases: ' + ', '.join(sorted(missing)))


def validate_case(name, meta):
    reject = bool(meta.get('reject'))
    if name in REQUIRED and reject != name.startswith('neg_'):
        raise ValueError('unexpected fixture polarity: ' + name)
    if not reject and 'exit' not in meta:
        raise ValueError('missing explicit expected exit: ' + name)


def rejection_valid(result, errors):
    return (result.returncode > 0 and bool(result.stderr.strip())
            and not result.stdout and all(e in result.stderr for e in errors))
