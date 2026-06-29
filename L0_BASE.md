# L0 Base Branch

`l0-base` is the reusable NAND scheduling baseline extracted from `nand-core`.
Simulation **behavior is unchanged**; sources are split for spec v1.0 evolution.

## Module layout

| File | Role |
|------|------|
| `config.c/h` | Defaults, `.conf` parsing |
| `timing.c/h` | Config validation, derived timings, ceilings |
| `stripe.c/h` | Target selection, page_stripe, host enqueue |
| `nand_sched.c` | Channel/die FSM, `perf_init/run/cleanup` |
| `stats.c/h` | IOPS and bandwidth |
| `cmd_generate.c` | Host command injection |
| `sched_internal.h` | Shared types, queues, inline helpers |

Public API remains in `cmd_sched.h` (`perf_init`, `perf_run`, …).

## Regression

```bash
make test-nand
```

Results should match `nand-core` at the same commit (pre-split).

## Spec evolution

Build `design_spec_v1.0.md` milestones on this branch (M1+). Keep `nand-core`
frozen as legacy reference.
