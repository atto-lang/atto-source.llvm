# Indexed actor readiness replayed on current main

This branch semantically replays the readiness-index work from old PR #756
onto the current scheduler instead of rebasing the 144-commit-stale branch.

## Preserved current-main behavior

- execution IDs and reserved-execution admission remain intact;
- NativeExecutionContext construction remains intact;
- physical-turn gate admission and cross-domain wakeups remain intact;
- lazy carrier creation and actor single-flight authority remain intact.

## Changed readiness lookup

CONTROL and DATA readiness are represented by 64-bit leaf bitmaps plus one
summary level. With at most 65,536 actor slots, lookup scans no more than 16
summary words instead of linearly walking every actor lane. CONTROL remains
globally prior to DATA. Same-carrier DATA reuse is still capped at two turns.

The fairness cursor advances only after a physical turn permit is actually
acquired. Failed gate probes therefore cannot consume round-robin position.

Large pools are bounded to 65,536 actors and, above 1,024 actors, at most
1,048,576 mailbox cells.

## Required validation

The branch adds CTests for:

- 1,024 indexed CONTROL/DATA lanes and single-flight execution;
- 1,025 / 4,097 / 16,385 / 65,536 actor boundary cases;
- CONTROL priority while the shared physical-turn permit is unavailable.

Release evidence requires actual GCC and Clang builds and CTest execution.
A source workflow failure or a job with no executed steps is not passing
evidence. Sanitizer validation should use the reviewed private carrier only
after its destination and branch-protection policy is approved.
