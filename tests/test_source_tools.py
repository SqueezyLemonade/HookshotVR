"""Fixture source expansion follows authored sections, preserves headers and rejects cycles."""
from pathlib import Path
import runpy
import tempfile

read_source = runpy.run_path(str(Path(__file__).with_name('source_tools.py')))['read_source']


def main():
    with tempfile.TemporaryDirectory(prefix='source-sections-') as tmp:
        root = Path(tmp)
        (root / 'sections').mkdir()
        (root / 'main.cpp').write_text('#include "sdk.h"\n\n#include "sections/a.inc" // section\n#include "generated/private.inc"\n')
        (root / 'sections/a.inc').write_text('int before;\n#include "b_host.c"\nint after;\n')
        (root / 'sections/b_host.c').write_text('int nested;\n')
        result = read_source(root / 'main.cpp')
        assert '#include "sdk.h"\n\n' in result
        assert '#include "generated/private.inc"' in result
        assert result.index('int before;') < result.index('int nested;') < result.index('int after;')
        assert 'sections/a.inc' not in result
        (root / 'sections/b_host.c').write_text('#include "a.inc"\n')
        try:
            read_source(root / 'main.cpp')
        except ValueError as error:
            assert 'cyclic source-section include' in str(error)
        else:
            raise AssertionError('cyclic source sections were silently accepted')
        (root / 'sections/b_host.c').unlink()
        try:
            read_source(root / 'main.cpp')
        except FileNotFoundError:
            pass
        else:
            raise AssertionError('a missing source section was silently skipped')
    print('PASS: nested source sections, ordinary/generated headers, cycles and missing files')


if __name__ == '__main__':
    main()
