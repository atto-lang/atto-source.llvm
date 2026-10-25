#!/usr/bin/env python3
import subprocess, sys
compiler, source = sys.argv[1], sys.argv[2]
target = "aarch64-unknown-linux-android24"
run = subprocess.run([compiler, "--target=" + target, source],
                     capture_output=True, text=True)
assert run.returncode == 0, run.stderr
assert 'target triple = "aarch64-unknown-linux-android24"' in run.stdout
assert 'target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"' in run.stdout
assert "define i64 @android_step(i64" in run.stdout
for bad in ("aarch64-unknown-linux-gnu", "aarch64-linux-android", "aarch64-unknown-linux-android23"):
    rejected = subprocess.run([compiler, "--target=" + bad, source],
                              capture_output=True, text=True)
    assert rejected.returncode != 0 and not rejected.stdout
print("Android AArch64 API 24 scalar target IR and fail-closed aliases verified")
