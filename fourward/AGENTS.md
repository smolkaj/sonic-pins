# 4ward DVaaS Integration — Agent Guide

## Style

### StatusOr handling

Follow [Abseil Tip #181](https://abseil.io/tips/181):

- **Don't use `auto` for `StatusOr`.** Always spell out the full type.
- **Check `!ok()` immediately** after obtaining a `StatusOr`, then use `*foo`
  to access the value. Don't use `_or` suffixes or deferred checking.

```cpp
// Good:
absl::StatusOr<std::string> result = GetResult();
if (!result.ok()) return result.status();
Use(*result);

// Bad:
auto result_or = GetResult();
```

### Naming consistency

Keep class names, file names, and Bazel target names consistent:

- Class `FourwardMirrorTestbed` → file `fourward_mirror_testbed.h` → target
  `:fourward_mirror_testbed`.
- Don't abbreviate in one place but not another (e.g. `fourward_backend` target
  for `FourwardDataplaneValidationBackend` class).
