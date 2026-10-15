#!/usr/bin/env python3
"""A -O / PYTHONOPTIMIZE run must enforce, not erase, native contract checks."""
import ast
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class ContractHarnessOptimizationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.harness = Path(__file__).with_name("contract.py")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ores-contract-opt-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixture = self.root / "fixture.ores"
        self.fixture.write_text("fixture", encoding="utf-8")

    def compiler(self, *, status=0, stdout="", stderr=""):
        path = self.root / "fake-compiler"
        path.write_text(
            "#!" + sys.executable + "\n"
            + "import sys\n"
            + f"sys.stdout.write({stdout!r})\n"
            + f"sys.stderr.write({stderr!r})\n"
            + f"sys.exit({status})\n",
            encoding="utf-8",
        )
        path.chmod(0o700)
        return path

    def linker(self, *, exit_code=0, link_status=0, compile_status=0,
               link_error="undefined reference to main", stub_link_status=0):
        path = self.root / "fake-clang"
        executable_contents = (
            "#!" + sys.executable + "\n"
            + "import sys\n"
            + f"sys.exit({exit_code})\n"
        )
        path.write_text(
            "#!" + sys.executable + "\n"
            + "from pathlib import Path\n"
            + "import sys\n"
            + "result = Path(sys.argv[sys.argv.index('-o') + 1])\n"
            + "if '-c' in sys.argv:\n"
            + f"    if {compile_status}:\n"
            + "        sys.stderr.write('malformed LLVM IR')\n"
            + f"        sys.exit({compile_status})\n"
            + "    result.write_bytes(b'fake object')\n"
            + "    sys.exit(0)\n"
            + "if any('entrypoint.o' in x for x in sys.argv):\n"
            + f"    if {stub_link_status}:\n"
            + "        sys.stderr.write('unresolved other_dependency')\n"
            + "        sys.exit(1)\n"
            + "    result.write_text('linked with main stub')\n"
            + "    sys.exit(0)\n"
            + f"if {link_status}:\n"
            + f"    sys.stderr.write({link_error!r})\n"
            + f"    sys.exit({link_status})\n"
            + f"result.write_text({executable_contents!r}, encoding='utf-8')\n"
            + "result.chmod(0o700)\n",
            encoding="utf-8",
        )
        path.chmod(0o700)
        return path

    def run_contract(self, compiler, *flags, optimize="-O"):
        env = dict(os.environ, PYTHONOPTIMIZE="2")
        return subprocess.run(
            [sys.executable, optimize, str(self.harness),
             "--compiler", str(compiler), "--fixture", str(self.fixture), *flags],
            capture_output=True, text=True, check=False, timeout=15, env=env,
        )

    def test_native_security_gates_cannot_reintroduce_bare_assert(self):
        # Python strips ast.Assert entirely under -O/-OO. A security
        # regression gate must fail explicitly instead of disappearing.
        for name in (
            "contract.py",
            "native_class_field_move_admission.py",
            "class_multifield_layout_matrix.py",
            "wasm32_owned_classes_matrix.py",
        ):
            module = ast.parse(Path(__file__).with_name(name).read_text(encoding="utf-8"))
            bare_lines = [node.lineno for node in ast.walk(module)
                          if isinstance(node, ast.Assert)]
            self.assertEqual(bare_lines, [], f"{name}: bare assert at {bare_lines}")

    def test_blank_diagnostic_is_never_an_unchecked_positive(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n")
        p = self.run_contract(compiler, "--diagnostic", "")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("--diagnostic requires nonempty text", p.stderr)

    def test_blank_ir_expectation_is_rejected(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n")
        p = self.run_contract(compiler, "--ir", "")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("--ir requires nonempty text", p.stderr)

    def test_negative_source_cannot_skip_requested_native_execution(self):
        compiler = self.compiler(status=1, stderr="private class field")
        p = self.run_contract(compiler, "--diagnostic", "private class field",
                              "--run", "42")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("cannot be combined with positive test modes", p.stderr)

    def test_negative_source_cannot_skip_ir_or_abi_gates(self):
        compiler = self.compiler(status=1, stderr="private class field")
        for flags in (("--ir", "ret i64 42"), ("--no-native-abi",),
                      ("--no-main-link",)):
            with self.subTest(flags=flags):
                p = self.run_contract(compiler, "--diagnostic",
                                      "private class field", *flags)
                self.assertNotEqual(p.returncode, 0)
                self.assertIn("cannot be combined with positive test modes",
                              p.stderr)

    def test_negative_compiler_must_emit_zero_bytes_not_whitespace(self):
        compiler = self.compiler(status=1, stderr="private class field",
                                 stdout="\n")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("compiler emitted stdout after rejection", p.stderr)

    def test_native_status_must_be_possible_unsigned_exit_code(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n")
        for expected in ("-9", "256", "99999"):
            with self.subTest(expected=expected):
                p = self.run_contract(compiler, "--run", expected)
                self.assertNotEqual(p.returncode, 0)
                self.assertIn("--run requires an OS exit code", p.stderr)

    def test_native_runtime_cannot_be_ignored_without_run(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n")
        p = self.run_contract(compiler, "--native-runtime", "/tmp/unused.c")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("--native-runtime requires --run", p.stderr)

    def test_unrelated_linker_error_followed_by_main_note_is_rejected(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker(link_status=1,
                            link_error="ld: undefined reference to alpha\n"
                                       "note: main was invoked")
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("missing-main diagnostic", p.stderr)

    def test_lld_style_missing_main_diagnostic_is_accepted(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker(link_status=1, link_error="ld.lld: undefined symbol: main")
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_valid_negative_still_passes_under_optimization(self):
        compiler = self.compiler(status=1, stderr="private class field")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_rejection_must_not_accept_successful_compilation(self):
        compiler = self.compiler(status=0, stdout="; ModuleID = 'wrong'\n")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("invalid source must produce a compiler diagnostic (exit 1), got 0", p.stderr)

    def test_rejection_must_not_accept_missing_diagnostic(self):
        compiler = self.compiler(status=1, stderr="wrong diagnostic")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)

    def test_rejection_must_not_accept_compiler_usage_error(self):
        # llvmc returns 2 for invalid flags and 1 for source diagnostics.
        # A runner invoking the wrong CLI must never count as negative proof.
        compiler = self.compiler(status=2, stderr="private class field")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("got 2", p.stderr)

    def test_rejection_must_not_accept_compiler_crash(self):
        compiler = self.compiler(status=139, stderr="private class field")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("got 139", p.stderr)

    def test_rejection_must_not_emit_any_ir(self):
        compiler = self.compiler(status=1, stderr="private class field",
                                 stdout="; ModuleID = 'bad'\n")
        p = self.run_contract(compiler, "--diagnostic", "private class field")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("compiler emitted stdout after rejection", p.stderr)

    def test_valid_positive_still_passes_under_optimization(self):
        compiler = self.compiler(stdout="; ModuleID = 'good'\n")
        p = self.run_contract(compiler)
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_positive_checks_bad_status_and_ir(self):
        bad_status = self.compiler(status=1, stderr="compilation failed")
        self.assertNotEqual(self.run_contract(bad_status).returncode, 0)
        bad_ir = self.compiler(stdout="not LLVM IR")
        self.assertNotEqual(self.run_contract(bad_ir).returncode, 0)

    def test_ir_expectation_cannot_be_fulfilled_by_comment(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                 "; ret i64 42\n"
                                 "ret i64 0 ; ret i64 42\n")
        p = self.run_contract(compiler, "--ir", "ret i64 42")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("comments do not count", p.stderr)

    def test_expected_ir_remains_visible_inside_function_definition(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                 "define i64 @answer() { ret i64 42 } ; harmless note\n")
        p = self.run_contract(compiler, "--ir", "ret i64 42")
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_global_string_cannot_spoof_expected_call(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                 '@bait = private constant [14 x i8] c"call i64 @add\\00"\n'
                                 "define i32 @main() { ret i32 42 }\n")
        clang = self.linker(exit_code=42)
        p = self.run_contract(compiler, "--ir", "call i64 @add",
                              "--run", "42", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("expected executable LLVM IR was absent", p.stderr)

    def test_metadata_cannot_spoof_instruction(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                 "define i32 @main() { ret i32 42 }\n"
                                 '!0 = !{!"call void @missing"}\n')
        p = self.run_contract(compiler, "--ir", "call void @missing")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("expected executable LLVM IR was absent", p.stderr)

    def test_comment_token_inside_llvm_string_is_not_comment(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                 '@msg = private constant [11 x i8] c"hello;world"\n'
                                 "define i32 @main() { ret i32 42 }\n")
        p = self.run_contract(compiler, "--ir", 'c"hello;world"')
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("expected executable LLVM IR was absent", p.stderr)

    def test_real_quoted_symbol_and_multiline_header_pass(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                 'define dso_local i64 @"pick$arity0"(\n'
                                 ") {\n"
                                 "entry:\n"
                                 "  ret i64 42\n"
                                 "}\n"
                                 '@str = private constant [14 x i8] c"call i64 @bad\\00"\n')
        self.assertEqual(
            self.run_contract(compiler, "--ir", "pick$arity0").returncode, 0
        )
        self.assertEqual(
            self.run_contract(compiler, "--ir", "ret i64 42").returncode, 0
        )
        self.assertNotEqual(
            self.run_contract(compiler, "--ir", "call i64 @bad").returncode, 0
        )

    def test_positive_rejects_stderr_and_forbidden_abi(self):
        compiler = self.compiler(stdout="; ModuleID = 'good'\n",
                                 stderr="unexpected warning")
        self.assertNotEqual(self.run_contract(compiler).returncode, 0)
        compiler = self.compiler(stdout="; ModuleID = 'good'\n"
                                 "declare void @oreslang_owned_i64_region_fake()\n")
        self.assertNotEqual(
            self.run_contract(compiler, "--no-native-abi").returncode, 0
        )

    def test_required_ir_and_native_run_flags_remain_checked(self):
        compiler = self.compiler(stdout="; ModuleID = 'good'\n")
        self.assertNotEqual(
            self.run_contract(compiler, "--ir", "define i32 @main").returncode, 0
        )
        self.assertNotEqual(
            self.run_contract(compiler, "--run", "42").returncode, 0
        )

    def test_double_optimized_python_also_enforces_rejection(self):
        compiler = self.compiler(status=0, stdout="; ModuleID = 'wrong'\n")
        p = self.run_contract(compiler, "--diagnostic", "private class field",
                              optimize="-OO")
        self.assertNotEqual(p.returncode, 0)

    def test_library_only_must_not_link_successfully(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker()
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("linked as a successful standalone process", p.stderr)

    def test_comment_cannot_spoof_i32_native_entrypoint(self):
        compiler = self.compiler(stdout="; ModuleID = 'spoof'\n"
                                 "; define i32 @main()\n"
                                 "define i64 @main() { ret i64 42 }\n")
        clang = self.linker(exit_code=42)
        p = self.run_contract(compiler, "--run", "42", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("native executable lacks an i32 main entrypoint wrapper", p.stderr)

    def test_inline_ir_comment_cannot_spoof_native_entrypoint(self):
        compiler = self.compiler(stdout="; ModuleID = 'spoof'\n"
                                 "define i64 @main() { ret i64 42 } ; define i32 @main()\n")
        clang = self.linker(exit_code=42)
        p = self.run_contract(compiler, "--run", "42", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("native executable lacks an i32 main entrypoint wrapper", p.stderr)

    def test_host_main_with_visibility_modifier_is_recognized(self):
        compiler = self.compiler(stdout="; ModuleID = 'native'\n"
                                 "define dso_local i32 @main() { ret i32 42 }\n")
        clang = self.linker(exit_code=42)
        p = self.run_contract(compiler, "--run", "42", "--clang", str(clang))
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_native_wrong_exit_code_is_detected(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                 "define i32 @main() { ret i32 41 }\n")
        clang = self.linker(exit_code=41)
        p = self.run_contract(compiler, "--run", "42", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("native exit 41, expected 42", p.stderr)

    def test_valid_native_execution_gate_still_passes(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                 "define i32 @main() { ret i32 42 }\n")
        clang = self.linker(exit_code=42)
        p = self.run_contract(compiler, "--run", "42", "--clang", str(clang))
        self.assertEqual(p.returncode, 0, p.stderr)


    def test_valid_library_ir_fails_link_for_missing_main(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker(link_status=1)
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_other_undefined_library_symbol_cannot_hide_behind_missing_main(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker(link_status=1, stub_link_status=1,
                            link_error="ld: undefined reference to main\n"
                                       "ld: undefined reference to other_dependency")
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("library failed positive link with main stub", p.stderr)

    def test_malformed_library_ir_cannot_pass_missing_main_gate(self):
        compiler = self.compiler(stdout="; ModuleID = 'bad library'\n")
        clang = self.linker(compile_status=1, link_status=1)
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("LLVM IR failed object compilation", p.stderr)

    def test_linker_flag_containing_main_is_not_entrypoint_evidence(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker(link_status=1, link_error="ld: cannot find -lmain")
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)

    def test_unrelated_libc_start_main_symbol_is_not_evidence(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker(
            link_status=1, link_error="ld: undefined reference to __libc_start_main"
        )
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)

    def test_macos_missing_entrypoint_diagnostic_is_accepted(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker(
            link_status=1,
            link_error='Undefined symbols for architecture arm64:\n  "_main", referenced from:\n',
        )
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_unrelated_link_failure_cannot_pass_missing_main_gate(self):
        compiler = self.compiler(stdout="; ModuleID = 'library'\n")
        clang = self.linker(link_status=1, link_error="unrelated library missing")
        p = self.run_contract(compiler, "--no-main-link", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("missing-main diagnostic", p.stderr)



    def test_verify_object_rejects_missing_clang(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n")
        p = self.run_contract(compiler, "--verify-object")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("--verify-object requires --clang", p.stderr)

    def test_verify_object_is_incompatible_with_negative_diagnostic(self):
        compiler = self.compiler(status=1, stderr="private class field")
        p = self.run_contract(compiler, "--diagnostic", "private class field",
                              "--verify-object", "--clang", "/does/not/matter")
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("cannot be combined with positive test modes", p.stderr)

    def test_verify_object_successful_compilation_is_admitted(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                        "define i32 @main() { ret i32 42 }\n")
        clang = self.linker()
        p = self.run_contract(compiler, "--verify-object", "--clang", str(clang))
        self.assertEqual(p.returncode, 0, p.stderr)

    def test_verify_object_rejects_malformed_ir_even_with_matching_token(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                        "define i32 @main() { not-LLVM-IR }\n")
        clang = self.linker(compile_status=1)
        p = self.run_contract(compiler, "--ir", "define i32 @main",
                              "--verify-object", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("failed object compilation", p.stderr)

    def test_verify_object_rejects_missing_output_object(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                        "define i32 @main() { ret i32 42 }\n")
        # Separate fake Clang path: writing over the fake compiler would
        # fail the ModuleID preflight rather than exercising this object gate.
        clang = self.linker()
        clang.write_text("#!" + sys.executable + "\nimport sys\nsys.exit(0)\n",
                         encoding="utf-8")
        p = self.run_contract(compiler, "--verify-object", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("nonempty native object", p.stderr)

    def test_verify_object_rejects_compiler_only_stub_ir(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n")
        # Even a successful Clang exit and object file cannot turn empty
        # compiler output into executable native-source proof.
        clang = self.linker()
        p = self.run_contract(compiler, "--verify-object", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no function definitions", p.stderr)

    def test_verify_object_cannot_accept_declarations_as_definitions(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                        "declare i32 @external()\n")
        clang = self.linker()
        p = self.run_contract(compiler, "--verify-object", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no function definitions", p.stderr)

    def test_verify_object_cannot_accept_commented_function_definition(self):
        compiler = self.compiler(stdout="; ModuleID = 'guest'\n"
                                        "; define i32 @spoof() { ret i32 42 }\n")
        clang = self.linker()
        p = self.run_contract(compiler, "--verify-object", "--clang", str(clang))
        self.assertNotEqual(p.returncode, 0)
        self.assertIn("no function definitions", p.stderr)


if __name__ == "__main__":
    unittest.main()
