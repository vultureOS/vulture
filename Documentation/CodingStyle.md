# Coding Style

`.clang-format` at the repository root is the formatting authority. Run it
before committing:

```sh
find Boot Kernel Libraries Tests -name '*.cpp' -o -name '*.h' | xargs /opt/homebrew/opt/llvm@22/bin/clang-format -i
```

## Language

- C++23, as supported by the pinned clang (22).
- No exceptions, no RTTI, no C++ standard library and no system headers.
  Every part builds with `-nostdinc -nostdinc++`.
- Use LibBase for vocabulary types: `Optional`, `Span`, `Array`,
  `StringView`, `ErrorOr`, `format_to`.
- Report failures with `ErrorOr<T>`. Propagate with `TRY(...)`, and use
  `MUST(...)` only where failure is a bug.
- Check invariants with `VERIFY(...)`. It stays on in every build.
- No raw owning `new`/`delete` outside LibBase and kernel bring-up code.
  Smart pointers arrive with threads in M3, the first objects with shared
  ownership.
- Prefer concepts and `requires` to SFINAE.
- No floating point in the kernel (`-mgeneral-regs-only`).

## Naming

| Kind | Style | Example |
|---|---|---|
| Types, classes, enums, concepts, namespaces | `PascalCase` | `StringView`, `FormatSpec` |
| Functions, methods, variables | `snake_case` | `find_last`, `descriptor_size` |
| Data members | `m_snake_case` | `m_length` |
| Static members / file-scope statics | `s_snake_case` | `s_first_test` |
| Globals | `g_snake_case` | `g_console` |
| Enum values | `PascalCase` | `Align::Center` |
| Macros (rare) | `UPPER_SNAKE_CASE` | `VERIFY`, `TRY` |

ABI structures that mirror an external specification (UEFI, ELF, the device
tree, ACPI, virtio) keep the specification's field names, so they can be
cross-referenced directly. For example, `UEFI::SystemTable::ConOut`.

## Files

- One main class per file, named after it: `StringView.h`, `Format.cpp`.
- `#pragma once` in every header.
- Every file starts with the copyright and SPDX license header:

  ```cpp
  /*
   * Copyright (c) 2022-2026, Krisna Pranav
   *
   * SPDX-License-Identifier: GPL-3.0-or-later
   */
  ```

- Include with angle brackets relative to the repository root or
  `Libraries/`: `#include <LibBase/Format.h>`, `#include <Boot/Console.h>`.

## Comments

Explain *why*, not *what*: constraints, invariants and surprises. Keep them
short.

## Commits

`Subsystem: Imperative summary`, for example `LibBase: Add StringView::find_last`
or `Boot: Report the framebuffer format`.
