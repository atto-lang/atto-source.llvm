#!/bin/sh
set -eu
compiler=$1
fixture=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
reject() {
    set +e
    "$compiler" "$@" > "$tmp/out" 2> "$tmp/err"
    status=$?
    set -e
    test "$status" -eq 1
    grep -Fq 'E_ACTOR_PUBSUB_UNSUPPORTED:' "$tmp/err"
    test ! -s "$tmp/out"
}
# Exercise frontend-only and normal emitting entrypoints with the exact source.
reject --check "$fixture"
reject "$fixture"
printf 'fnc main(): int { val receipt = local_topic /* gap */ . try_publish (share! event)?; return 0; }\n' > "$tmp/alias.atto"
reject --check "$tmp/alias.atto"
reject "$tmp/alias.atto"
printf 'fnc main(): int { val receipt = local_topic.try_publish(share !event)?; return 0; }\n' > "$tmp/split.atto"
reject --check "$tmp/split.atto"
# Inert comments and an ordinary standalone identifier must not be reserved.
printf 'fnc main(): int { /* local_topic.try_publish(share! event)? */ return 42; }\n' > "$tmp/good.atto"
"$compiler" --check "$tmp/good.atto" > "$tmp/out" 2> "$tmp/err"
test ! -s "$tmp/out"
"$compiler" "$tmp/good.atto" > "$tmp/out" 2> "$tmp/err"
test -s "$tmp/out"
printf 'fnc try_publish(): int { return 42; }\nfnc main(): int { return try_publish(); }\n' > "$tmp/ordinary.atto"
"$compiler" --check "$tmp/ordinary.atto" > "$tmp/out" 2> "$tmp/err"
test ! -s "$tmp/out"
# Source-only negative gate, not native topic execution.
echo 'LLVM_ENSEMBLE_COMPILER_ADMISSION_ONLY'
