"""Read production source sections in fixtures without losing their relative include paths.

Only authored .inc and *_host.c sections are expanded; ordinary SDK/system headers
remain includes. No game-derived/generated headers are loaded by this helper.
"""
from pathlib import Path
import re


def read_source(path, _parents=()):
    path = Path(path).resolve()
    if path in _parents:
        raise ValueError(f'cyclic source-section include: {path}')
    def include(match):
        name = match.group(1)
        if 'generated' in Path(name).parts or not (name.endswith('.inc') or name.endswith('_host.c')):
            return match.group(0)
        return read_source(path.parent / name, (*_parents, path))
    return re.sub(r'^[ \t]*#include "([^"\n]+)"[^\n]*$', include,
                  path.read_text(encoding='utf-8'), flags=re.MULTILINE)
