# Memory-safety follow-ups

Tracking list from the UAF/TOCTOU audit after the cu_json + scoped-cleanup
migration landed. Each item is something the migration *didn't* close
structurally. Ranked by realistic risk in this codebase.

## Medium priority

### 3. cJSON whitelist regression risk

Three files still use cJSON directly behind `CUTILS_CJSON_ALLOW`:

- `src/api/routes/config.c` — `reg_to_json` + `out_of_range` err object
- `src/weather/weather.c`   — NWS response parsing
- `src/cli/commands.c`      — `print_json` + `/api/commands` array parse

Today they're safe because every extracted string is either compared
immediately or copied into an owned buffer before `cJSON_Delete`. A
future handler added to any of these files could stash a `valuestring`
past the delete and reintroduce the exact bug class 754820e fixed.
The fence won't catch it — they already have the opt-in.

**Action:** periodic review gate. When adding code to any of these
three files, re-read the file-head comment and confirm no new
borrowed-pointer stash pattern has crept in. Consider a pre-commit
check that greps for `valuestring` usage patterns.

### 4. Inventory / shared-state reads without the mutex

`monitor_thread` reads `mon->ups->inventory` and `mon->ups->has_inventory`
without taking any lock. Works today because the UPS driver sets
inventory once during connect and never mutates — but that's a
convention, not a compiler-enforced guarantee. A future driver change
could violate it silently.

**Action:** pick one:
- Document the convention explicitly at the `ups_t` struct definition
  (the "inventory fields are write-once during connect" contract).
- Add a mutex to `ups_t` that guards inventory writes (heavyweight
  but compiler-visible).
- Clang thread-safety annotations (`__attribute__((guarded_by(...)))`)
  on the fields — enforceable with `-Wthread-safety` but a meaningful
  adoption effort.

### 5. Iterator escape (theoretical but unprevented)

`cutils_json_iter_t`'s internal pointers (`_current`, `_array`) point
into the request tree. Stashing an iterator past the request's
lifetime would produce UAF when next used. Docs say "must not outlive
the request handle"; the compiler doesn't enforce.

**Action:** nothing immediate. Revisit if a real incident appears.
A future c-utils MR could constrain the iterator further (e.g.
make fields fully opaque + runtime lifetime check).

## Low priority / theoretical

### 6. `CUTILS_AUTOFREE` pointer stored in longer-lived state

Hand an `AUTOFREE`-owned pointer to anything that stores-but-doesn't-copy
(a global, a struct outliving the local scope, a registration callback
that keeps the pointer) and the cleanup attribute deletes the buffer
while the stored copy dangles.

Currently not instantiated anywhere I could find — `api_ok` is the
only API of this shape and we always use `CUTILS_MOVE` on it.

**Action:** code-review hazard. Watch for `global = name;` or
`something.field = name;` patterns where `name` is an AUTOFREE local
and the assignment target outlives the function.

### 7. Lock-after-check patterns

Reading a flag unlocked, then locking to act on it, is a classic
race. Our ported code mostly gets this right (e.g. `monitor_get_status`
checks `has_data` inside the lock). But the pattern isn't compiler-
enforced.

**Action:** code-review hazard. Any future `if (shared_state) { LOCK_GUARD; ... }`
should move the read inside the lock.

### 8. First-run setup race

`/api/auth/setup` checks "is the password already set?" then sets it.
Two concurrent setup requests could both pass the check and set
different passwords; last writer wins.

Low realistic risk (single-admin tool over LAN). Fix would need a
transaction + re-check inside the write.

**Action:** defer unless it's ever exploited or concurrent setup
becomes a realistic scenario.

### 9. Non-atomic `CUTILS_MOVE`

`CUTILS_MOVE(p)` reads the old value and nulls `p` in two separate
operations. Two threads racing through it could both see the old
value or get torn state.

Nobody should be doing multi-thread MOVE on the same pointer, but
nothing prevents it.

**Action:** documented gotcha, no code fix.

## Bigger-picture items (not strictly follow-ups)

### 10. yyjson migration for whitelist files

Porting `weather.c`'s NWS parse to yyjson would eliminate item #3's
regression risk structurally the way cu_json did for routes — yyjson's
immutable-view API ties string lifetimes to the document, so
stash-past-free can't compile. Also the fastest C JSON parser by a
wide margin, which matters for the weather poll path.

Cost: vendoring yyjson, wrapping it in a cu_json-style parse-side
API (cu_json only has a build-side wrapper over cJSON), porting the
four NWS parse functions. Medium-sized effort.

**Action:** defer. Revisit if weather.c grows or if a regression
appears in the whitelist files.

### 11. Clang thread-safety annotations

`-Wthread-safety` with `guarded_by` / `requires` / `acquired_by`
attributes on pthread primitives would catch lock-after-check
(item #7), inventory-read-without-lock (item #4), and a broader
class of concurrency hazards at compile time.

Cost: non-trivial annotation effort across `ups.c`, `monitor.c`,
`shutdown.c`. Needs clang specifically; gcc has a very different
story. Our Makefile is gcc-only today.

**Action:** evaluate as part of a future "hardening pass" MR.
Not urgent.

## Completed in the initiative (for reference)

What the migration *did* close, so we remember what's already done:

- `valuestring` stashed past `cJSON_Delete` → wrapper returns owned copies
- Half-built cJSON trees leaked on error returns → `AUTO_JSON_RESP` + `AUTO_JSON_ELEM`
- Manual free-on-every-return-path forgotten on new early returns → `AUTOFREE`
- Mutex unlock forgotten on early return → `LOCK_GUARD`
- DB result not freed on early return → `AUTO_DBRES`
- FILE / fd not closed on error path → `AUTOCLOSE` / `AUTOCLOSE_FD`
- `#include <cJSON.h>` silently added to a route handler → fence + `CUTILS_CJSON_ALLOW` opt-in
- Dropped return codes from DB / config writes → `CUTILS_MUST_USE` + `CUTILS_UNUSED`
- Silent failures in `handle_shutdown_settings_set` → now propagates 500 + error
- UAF in `handle_setup_test` (754820e) → structural via owned-string returns
