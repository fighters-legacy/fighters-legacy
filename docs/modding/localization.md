<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Localization Guide

This guide is for community translators and modders adding locale support to Fighters Legacy.

---

## 1. Overview

All user-visible strings in the engine are accessed through keys rather than hardcoded text.
Keys have the form `<namespace>.<section>.<key>` (e.g. `ui.main_menu.campaign`). The engine
looks up the active locale at runtime, falling back to English (`en`) for any key that is
not translated.

---

## 2. Key naming convention

```
<namespace>.<section>.<key>
```

- **namespace** — matches the TOML file stem (e.g. `ui` → `locale/en/ui.toml`)
- **section** — a TOML table header `[section]`
- **key** — the leaf entry inside that table

Examples:

| Key | File | TOML path |
|---|---|---|
| `ui.main_menu.campaign` | `locale/en/ui.toml` | `[main_menu]` → `campaign` |
| `engine.content.pack_init_failed` | `locale/en/engine.toml` | `[content]` → `pack_init_failed` |
| `hud.rwr.lock_warning` | `locale/en/hud.toml` | `[rwr]` → `lock_warning` |

---

## 3. File layout

```
locale/
    en/                   ← required base locale
        meta.toml
        engine.toml
        ui.toml
        hud.toml          ← add new namespace files as needed
    fr/
        meta.toml
        ui.toml
    fr-CA/
        ui.toml           ← overrides fr for this namespace only
```

Each locale has its own directory named by BCP 47 tag (e.g. `fr`, `fr-CA`, `ar`, `zh-Hans`).
Within a locale directory there is one TOML file per namespace, plus an optional `meta.toml`.

---

## 4. `meta.toml` fields

```toml
# SPDX-License-Identifier: GPL-3.0-or-later
name = "Français"   # Native display name shown in the language selector
rtl  = false        # true for Arabic, Hebrew, Persian, etc.
```

`meta.toml` is required for a locale to appear in `listLocales()`. Both fields are optional;
`name` defaults to the BCP 47 tag and `rtl` defaults to `false`.

---

## 5. Plural forms

The engine supports three plural forms per key: `.zero`, `.one`, and `.other`.

**TOML example** (`locale/en/ui.toml`):

```toml
[missile_count]
one   = "{n} missile"
other = "{n} missiles"
```

In code: `loc.getPlural("ui.missile_count", n)` automatically selects the right form and
substitutes `{n}`.

Fallback behaviour:

| n | Form tried | Fallback |
|---|---|---|
| 0 | `.zero` | `.other` |
| 1 | `.one` | `.other` |
| ≥ 2 | `.other` | `.one` |

> **Known limitation (Phase 1):** Only `.zero`, `.one`, and `.other` are supported.
> Languages with more plural categories (Russian: one/few/many/other; Arabic:
> zero/one/two/few/many/other) must use `.other` as the general-case fallback in Phase 1.
> Full CLDR plural rule support is planned for a later workstream.

---

## 6. Interpolation

Use `{placeholder}` syntax in strings. Named placeholders are replaced at runtime.

```toml
[content]
pack_init_failed = "Content pack '{name}' failed to initialise."
```

Escape literal braces with `{{` (produces `{`) and `}}` (produces `}`).

Unknown placeholders are left as-is in the output.

---

## 7. Mod locale

Mods can ship their own translations by mirroring the `locale/` structure inside the mod
directory:

```
mods/my-mod/
    manifest.toml
    locale/
        en/
            ui.toml       ← adds or overrides engine keys for English
        fr/
            ui.toml       ← adds or overrides engine keys for French
```

Higher-priority mods win on key conflicts. The priority is set in `manifest.toml`
(`priority = 100`).

---

## 8. Developer workflow — adding a new key

1. Add the call in C++ source:
   ```cpp
   loc.get("ui.main_menu.new_option")
   ```
2. Run `locale-extract` to register the key in `locale/en/ui.toml`:
   ```bash
   ./build/debug/tools/locale-extract --src . --locale locale
   ```
   This injects `new_option = ""` under `[main_menu]` in `locale/en/ui.toml`.
3. Fill in the English value in `locale/en/ui.toml` (replace `""` with the string).
4. Commit the source change and the TOML update together.
5. Translators will see `new_option` as missing in their locale's coverage report.

---

## 9. Testing locally

Run `locale-extract` in dry-run mode to check for drift between source and TOML without
modifying any files:

```bash
./build/debug/tools/locale-extract --src . --locale locale --dry-run
```

Exit code 0 = all keys in sync. Exit code 1 = new or orphaned keys found.

---

## 10. Submitting a translation

1. Create a new directory `locale/<tag>/` (e.g. `locale/fr-CA/`).
2. Add `meta.toml` with at least `name` and `rtl`.
3. Copy `locale/en/*.toml` as a starting template; translate each value in place.
   Leave `= ""` for keys you have not yet translated — the engine falls back to `en`.
4. Open a pull request. CI will check that the locale files parse and that `meta.toml` exists.

---

## 11. Standalone locale mod

A locale mod is a mod that only contains `locale/` files:

```
mods/community-fr/
    manifest.toml         ← priority, depends = []
    locale/
        fr/
            ui.toml
            engine.toml
```

Install it like any other mod. No compiled code is required.
