#!/usr/bin/env python3
"""Compare real byte/scalar heaps and optional independent backend builds.

The C++ runner checks each value/lifetime transition against its own model.
This driver additionally demands identical seeded observations across paths,
and SIGABRT (not merely a nonzero exit) for each shared misuse contract.
"""
import argparse
import json
import os
import signal
import subprocess


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", action="append", required=True)
    parser.add_argument("--actor-runner", action="append", default=[])
    parser.add_argument("--seeds", type=int, default=32)
    parser.add_argument("--steps", type=int, default=1500)
    args = parser.parse_args()
    require(1 <= args.seeds <= 256 and 1000 <= args.steps <= 10000, "bounded nontrivial corpus required")
    require(len(set(args.runner)) == len(args.runner), "duplicate backend runner")
    require(len(set(args.actor_runner)) == len(args.actor_runner), "duplicate actor runner")
    transitions = 0
    observations = []
    for seed in range(1, args.seeds + 1):
        reference = None
        for runner in args.runner:
            for mode in ("byte", "scalar"):
                result = subprocess.run([runner, "model", mode, str(seed), str(args.steps)],
                                        capture_output=True, text=True, timeout=30)
                require(result.returncode == 0, f"{runner}/{mode}/seed={seed}: {result.stderr}")
                row = [int(value) for value in result.stdout.split()]
                require(len(row) == 13 and row[0] == seed and all(n > 0 for n in row[2:]),
                        f"missing model coverage: {runner}/{mode}/{seed}")
                if reference is None:
                    reference = row
                    transitions += sum(row[2:])
                    observations.append(row[1])
                require(row == reference, f"backend/API divergence: {runner}/{mode}/seed={seed}")
    rejections = 0
    if os.name == "posix":
        for runner in args.runner:
            for mode in ("byte", "scalar"):
                for case in range(28):
                    result = subprocess.run([runner, "misuse", mode, str(case), "0"],
                                            capture_output=True, text=True, timeout=10)
                    require(result.returncode == -signal.SIGABRT,
                            f"misuse {case}: expected SIGABRT from {runner}/{mode}, "
                            f"got {result.returncode}: {result.stderr}")
                    rejections += 1
    messages = 0
    for seed in range(1, args.seeds + 1):
        reference = None
        for runner in args.actor_runner:
            result = subprocess.run([runner, str(seed)], capture_output=True, text=True, timeout=30)
            require(result.returncode == 0, f"actor {runner}/seed={seed}: {result.stderr}")
            row = [int(value) for value in result.stdout.split()]
            require(len(row) == 3 and row[0] == seed and 0 < row[1] <= 1536,
                    f"invalid actor receipt: {runner}/{seed}")
            if reference is None:
                reference = row
                messages += row[1]
            require(row == reference, f"actor runtime divergence: {runner}/seed={seed}")
    print(json.dumps({"scope": "host-memory-model-parity", "backend_builds": len(args.runner),
                      "api_paths": 2 * len(args.runner), "seeds": args.seeds,
                      "transitions_per_path": transitions, "misuse_processes_rejected": rejections,
                      "digest_count": len(set(observations)),
                      "actor_runtime_paths": len(args.actor_runner), "messages_per_actor_path": messages,
                      "guest_compiler_execution": False}, sort_keys=True))


if __name__ == "__main__":
    main()
