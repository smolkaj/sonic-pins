# sonic-pins — Agent Guide

**Always work in a dedicated git worktree — never modify the main tree
directly.** Create one with:

```sh
git worktree add ../sonic-pins-<branch> -b <branch>
```

## Understand ideal before settling for less

Before committing to any design or implementation, define what the ideal
solution looks like — unconstrained by schedule, legacy, or expedience.
You don't have to build the ideal, but you must understand it. A pragmatic
shortcut is a legitimate engineering choice; a shortcut you took because you
never considered the alternative is just a blind spot. Name the north star,
name what you're trading away, and name why.

## Test-driven development

**Write the test first.** The test is the spec — it defines the behavior
you want before you write the code. If you can't write a clear test, you
don't understand the problem yet. A failing test is the starting point for
every change, not an afterthought.

## Key design invariants

1. **4ward is a subprocess.** All communication happens over gRPC. The C++
   code in this directory never links against 4ward's Kotlin code — it only
   depends on proto definitions and gRPC stubs.

2. **The `dvaas` namespace.** All code in `fourward/` lives in `namespace
   dvaas`, not `namespace fourward`. The `fourward` namespace belongs to the
   4ward repo.

3. **The integration is opt-in.** When `P4Specification.fourward_config` is
   present, DVaaS uses 4ward. Otherwise, the existing BMv2 path works
   unchanged.

## Repository map

```
dvaas/portable_pins_backend/          Open-source DVaaS backend using 4ward as reference model.
fourward/                             4ward integration (subprocess, oracle, testbed, bridge).
fourward/designs/                     Design documents (architecture decisions, feature proposals).
fourward/README.md                    Architecture overview and component docs.
```

Unit tests live alongside the source: `foo_test.cc` next to `foo.{h,cc}`.

## Code conventions

1. **We strictly follow the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)**
   and the [Abseil Tips of the Week](https://abseil.io/tips/).

2. **Unit tests for every `.h`/`.cc` pair.** No exceptions. A test for
   `foo.{h,cc}` must be named `foo_<optional_middle_part>_test.cc`.

3. **Use status macros** in production code:
   - `ASSIGN_OR_RETURN` instead of `if (!x.ok()) return x.status();`
   - `RETURN_IF_ERROR` instead of `if (!status.ok()) return status;`

4. **Use test status macros** in tests:
   - `ASSERT_OK_AND_ASSIGN` instead of `ASSERT_TRUE(x.ok()); auto val = *x;`
   - `ASSERT_OK`, `EXPECT_OK` instead of `EXPECT_TRUE(status.ok());`

5. **Propagate errors via status.** `absl::Status` or `absl::StatusOr` for
   all fallible operations. Logging errors (`LOG(ERROR)`) is not an
   acceptable way to report failures.

6. **Exhaustive switch on proto oneofs.** Never use if/else chains to
   dispatch on proto oneof fields.

7. **Use `int` for loop variables**, per Google Style Guide. The
   `-Wno-sign-compare` flag in `.bazelrc` suppresses warnings when comparing
   `int` against `.size()`. Never use `unsigned` or `size_t` for loops.

8. **Never fail silently.** Prefer compile-time failures (exhaustive switch)
   over runtime checks. When runtime checks are needed, return an error
   status with a descriptive message.

9. **Prefer golden tests for proto-to-proto or text-output functions.** Golden
   tests are less brittle than substring assertions — when output format
   changes, you just `--update` the golden file instead of fixing N assertions.
   Use `cmd_diff_test` from `@gutil//gutil:diff_test.bzl` with a runner binary
   that prints input + output for each test case. See
   `trace_conversion_golden_test` for a good example: a runner binary
   (`trace_conversion_golden_test_runner.cc`) prints the input TraceTree and
   output PacketTrace for each case, and `cmd_diff_test` diffs against
   `trace_conversion_test.expected`. Update via:
   `bazel run //fourward:trace_conversion_golden_test -- --update`.

## Build, test, and debug

```sh
bazel build //fourward/...
bazel test //fourward/...
```

When a test crashes, **observe, don't guess**:

```sh
# Debug build with symbols:
bazel run -c dbg //path/to:test -- --gtest_filter="*FailingTest*"

# Run under GDB:
bazel run -c dbg //path/to:test --run_under=gdb -- --gtest_filter="*FailingTest*"
```

## Commits and pull requests

Focus commit messages on *why* the change is being made. Don't restate what
the diff shows.

Open PRs in draft mode (`gh pr create --draft`). Rebase onto `fork/main`
before submitting. Keep descriptions concise — lead with the win.

Before submitting:

- Run `bazel test //fourward/...`. Fix all failures.
- Add unit tests for new behavior.
- Check whether README.md needs updating.
