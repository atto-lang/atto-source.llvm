#!/usr/bin/env python3
"""Source-free adversarial smoke test for exact-head host receipt verifier.

Uses no private source. A synthetic Git repository must never count as
successfully verified native pointer code just because CTest reports success.
"""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

VERIFIER = Path(sys.argv[1]).resolve()


def run(cmd, cwd, timeout=90):
    return subprocess.run(
        cmd, cwd=cwd, check=False, capture_output=True,
        text=True, timeout=timeout,
    )


def require(condition, label, details=""):
    if not condition:
        raise AssertionError(f"FAIL: {label}: {details}")
    print(f"PASS: {label}")


def main():
    require(VERIFIER.is_file(), "verifier file exists")
    with tempfile.TemporaryDirectory(prefix="atto-native-verifier-selftest-") as root_str:
        root = Path(root_str)
        (root / "src/main/cpp/oreslang/runtime").mkdir(parents=True)
        (root / "tests").mkdir()
        (root / "src/main/cpp/oreslang/runtime/NativeActorSupervisor.hpp").write_text("// synthetic\n")
        (root / "tests/native_actor_supervisor_pointer_integration_test.cpp").write_text("// synthetic\n")
        (root / "tests/mock_test.cpp").write_text("int main() { return 0; }\n")
        (root / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.20)
project(synthetic_verifier LANGUAGES C CXX)
option(ATTO_NATIVE_POINTER_HOST_ONLY "" OFF)
include(CTest)
add_executable(synthetic_native_test tests/mock_test.cpp)
foreach(n IN ITEMS atto_native_actor_supervisor_pointer_host_only
    atto_native_pointer_negative_compile atto_native_region_registry_host_only
    atto_native_region_runtime_authority_host_only atto_trusted_pointer_window_host_only)
  add_test(NAME ${n} COMMAND synthetic_native_test)
endforeach()
""")
        for command in (
            ["git", "init", "-q"],
            ["git", "add", "."],
            ["git", "-c", "user.name=Verifier", "-c", "user.email=synthetic@localhost",
             "commit", "-qm", "synthetic fixture"],
        ):
            result = run(command, root)
            require(result.returncode == 0, "synthetic Git fixture setup", result.stderr)
        sha = run(["git", "rev-parse", "HEAD"], root).stdout.strip()
        receipt = root.parent / f"{root.name}-receipt"
        missing_origin = run(["bash", str(VERIFIER), sha, str(receipt)], root)
        require(
            missing_origin.returncode == 2 and "origin is not the approved" in missing_origin.stderr,
            "unapproved synthetic repository denied before build",
            missing_origin.stderr,
        )
        require(not receipt.exists(), "no false receipt for unapproved origin")
        remote = run(["git", "remote", "add", "origin",
                      "https://github.com/ores-truffle-oreslang/oreslang-source.llvm.git"], root)
        require(remote.returncode == 0, "synthetic remote fixture setup", remote.stderr)
        wrong_sha = run(["bash", str(VERIFIER), "0" * 40, str(receipt)], root)
        require(
            wrong_sha.returncode == 2 and "expected HEAD" in wrong_sha.stderr,
            "wrong commit denied before build", wrong_sha.stderr,
        )
        # A fully named CTest manifest may still execute stubs. The verifier
        # must check the real built target binary names as well.
        fake_binary = run(["bash", str(VERIFIER), sha, str(receipt)], root)
        require(
            fake_binary.returncode == 1 and "expected native security binary" in fake_binary.stderr,
            "synthetic CTest stubs cannot pass native binary gate", fake_binary.stderr,
        )
        manifest = (receipt / "manifest.txt").read_text()
        stages = (receipt / "stages.tsv").read_text()
        require("host_status=EXECUTED_FAIL_OR_INCOMPLETE" in manifest,
                "failed synthetic build receipt fails closed")
        require("atto_guest_aot=NOT_EXECUTED" in manifest,
                "no synthetic guest AOT success claim")
        require("required-binary-gcc-release\tEXECUTED_FAIL" in stages,
                "missing native binary has explicit failed gate")
        shutil.rmtree(receipt, ignore_errors=True)


if __name__ == "__main__":
    main()
