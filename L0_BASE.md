# L0 Base Branch

`l0-base` is the reusable NAND scheduling baseline extracted from `nand-core`.
Simulation behavior evolves on this branch toward `design_spec_v1.0.md`.

## Module layout

| File | Role |
|------|------|
| `config.c/h` | Defaults, `.conf` parsing |
| `timing.c/h` | Config validation, derived timings, ceilings |
| `host_enqueue.c/h` | Global-die target selection, host enqueue |
| `nand_sched.c` | Channel/die FSM, `perf_init/run/cleanup` |
| `stats.c/h` | IOPS and bandwidth |
| `cmd_generate.c` | Host command injection |
| `sched_internal.h` | Shared types, queues, inline helpers |

Public API remains in `cmd_sched.h` (`perf_init`, `perf_run`, …).

## Address placement (no stripe)

- **`io_pattern=sequential`**: round-robin global die index.
- **`io_pattern=random`**: uniform random global die.
- Multi-page host blocks fan out on **one die** via multi-plane slots (`max_planes_per_die`).
- Legacy `stripe_mode` in `.conf` is **ignored** (compat only).

## Regression

```bash
make test-nand
```

## Spec evolution

Build `design_spec_v1.0.md` milestones on this branch (M1+). Keep `nand-core`
frozen as legacy reference.
