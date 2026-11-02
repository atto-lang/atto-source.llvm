"""Host-only C++ compile gate; NOT a guest source compiler test."""
import argparse
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cxx', required=True)
    parser.add_argument('--include', required=True)
    opts = parser.parse_args()
    prologue = '#include "oreslang/runtime/LogicalMutex.hpp"\n#include <functional>\n#include <string>\n#include <string_view>\n'
    scenarios = [
        ('void', '[](std::string& s) { s.append("!"); }', True),
        ('scalar', '[](std::string& s) { return s.size() > 0; }', True),
        ('reference', '[](std::string& s) -> std::string& { return s; }', False),
        ('pointer', '[](std::string& s) -> std::string* { return &s; }', False),
        ('reference_wrapper', '[](std::string& s) { return std::ref(s); }', False),
        ('string_view', '[](std::string& s) { return std::string_view(s); }', False),
        ('unverified_owned_copy', '[](std::string& s) { return std::string(s); }', False),
    ]
    with tempfile.TemporaryDirectory(prefix='critical-copyout-') as tmp:
        base = pathlib.Path(tmp)
        for name, callback, accepted in scenarios:
            source = base / f'{name}.cpp'
            source.write_text(prologue + 'int main() {\n  oreslang::runtime::LogicalMutex<std::string> gate("secret");\n'
                + '  auto ticket = gate.request();\n  gate.with_owner(ticket.token, ' + callback + ');\n}\n')
            result = subprocess.run([opts.cxx, '-std=c++17', '-Wall', '-Wextra', '-Wpedantic',
                '-Werror', '-fsyntax-only', '-I', opts.include, str(source)],
                capture_output=True, text=True, timeout=15, check=False)
            if accepted and result.returncode != 0:
                raise AssertionError(f'expected {name} accepted:\n{result.stderr}')
            if not accepted and (result.returncode == 0 or
                    'native logical mutex may return only void or scalar values' not in result.stderr):
                raise AssertionError(f'expected {name} rejected by copy-out contract:\n{result.stderr}')
            print(f'PASS native logical mutex {name} {"accepted" if accepted else "rejected"}')


if __name__ == '__main__':
    main()
