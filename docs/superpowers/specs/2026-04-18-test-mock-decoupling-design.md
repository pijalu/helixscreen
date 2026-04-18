# Test/Mock Decoupling — Design

**Date:** 2026-04-18
**Status:** Proposed
**Scope:** Approach 2 — Interface extraction + test isolation (no DI pass)

## Motivation

A coupling audit of HelixScreen's test and mock layers surfaced seven distinct smells, most notably:

1. **Silent mock drift** — six backend mocks (~2,500 lines total) manually mirror real class APIs with no compile-time enforcement. A missing or mismatched method in a mock only surfaces at runtime, if at all.
2. **Shared static state in test fixtures** — `XMLTestFixture::s_state`, `s_client`, `s_api`, and `LVGLTestFixture::s_display` are `static` and reused across every test instance. Test isolation is fragile; ordering can mask bugs.

Driver: proactive cleanup, no active failure. This bounds the scope aggressively — we fix only the two smells above, with surgical precision. The audit's other findings (10 `is_test_mode()` branches in production, 42 `*TestAccess` friend classes, 711 singleton references in tests, `RuntimeConfig` as a flag bag rather than a DI seam) are acknowledged and deferred.

## Goals

1. **Compile-time drift protection** for the six mock boundaries. If a real class gains a method, the compiler flags any mock that hasn't followed.
2. **Per-test isolation** — each test gets a fresh `PrinterState` with clean subject bindings. No static state shared between test instances.
3. **Standard test-fixture base** (`HelixTestFixture`) that deterministically resets known-mutated singletons between tests so ordering can't mask bugs.
4. **Keep `--test` runtime mode working identically.** Developers can still launch `./build/bin/helix-screen --test -vv` and get mocks. CLI surface unchanged; mock binary shape unchanged.

## Non-Goals

1. **No DI refactor.** `get_runtime_config()` stays. `PrinterState::instance()` stays. Production code still reaches for singletons; we just make sure tests can reset them.
2. **No removal of `is_test_mode()` branches.** The 10 test-mode checks in production stay. Cleaning those up is a separate, later pass.
3. **No friend-class `*TestAccess` consolidation.** The 42 friends stay where they are.
4. **No stripping mocks from the production binary.** Mocks stay in `include/` under `HELIX_ENABLE_MOCKS`; they just get proper interfaces.
5. **No test rewriting en masse.** Existing tests that happen to work keep working. We touch tests only where the fixture change requires it.
6. **No intra-process parallelism.** LVGL globals and the XML registry preclude it. Test parallelism via separate binaries/processes is an Approach 3 concern, out of scope.

## Architecture

### Interface extraction (the compile-time drift fix)

Actual state per inspection (the original audit was partially wrong):

| Backend | Current state | Action |
|---------|--------------|--------|
| `AmsBackend` | Pure virtual interface; `AmsBackendMock : public AmsBackend` is clean | **Audit only** — verify and move on |
| `EthernetBackend` | Pure virtual interface; `EthernetBackendMock` implements `has_interface()`/`get_info()` | **Audit only** — already drift-safe |
| `UsbBackend` | Pure virtual interface with pure-virtual `start()`/`stop()`/`is_running()` | **Audit only** — already drift-safe |
| `WifiBackend` | Pure virtual interface with `start()`/etc. pure-virtual (some concrete helpers like `set_silent()` are shared) | **Audit only** — already drift-safe |
| `MoonrakerAPI` | Concrete class, no virtual interface; `MoonrakerAPIMock : public MoonrakerAPI` | **Extract `IMoonrakerAPI`** |
| `MoonrakerClient` | Concrete class with `hv::WebSocketClient` base; some methods virtual (`connect`, `disconnect`), most not; `MoonrakerClientMock : public MoonrakerClient` | **Extract `IMoonrakerClient`**; mock drops `hv::WebSocketClient` baggage |

So the real interface-extraction work is **two classes**, not five. The other four are already drift-safe — we just add a compile-only smoke test per backend to formalize the property.

Naming convention for the two new interfaces: **`I`-prefix** (`IMoonrakerAPI`, `IMoonrakerClient`). Concrete class names stay. Mock class names stay. This minimizes call-site churn.

For each new interface:
1. Define `IXxx` as pure virtual with a default destructor.
2. Make real `Xxx` class inherit from `IXxx` (in addition to any existing bases, e.g., `hv::WebSocketClient` for `MoonrakerClient`).
3. Make `XxxMock` inherit from `IXxx` directly — no longer from `Xxx`. The mock sheds the real class's accidental baggage.
4. Update the factory to return `std::unique_ptr<IXxx>`. Factory still branches on `should_mock_*()`.
5. Update call sites to hold interface pointers.

Mocks stay in `include/` under `HELIX_ENABLE_MOCKS`. Production binary shape unchanged.

### Test isolation (the static-state fix)

Root cause: `PrinterState` registers ~100 subjects into LVGL's **global XML scope**. Tearing down `PrinterState` between tests would leave stale subject pointers in the global scope. Current workaround: make `PrinterState` static and never tear it down.

**As-shipped design (simplified from original plan).** The original plan called for a per-test LVGL XML scope via `lv_xml_component_scope_t` so each test owned an isolated subject registry. Implementation surfaced three blockers: (a) `lv_xml_component_unregister` `lv_free`s every subject in the scope, which would heap-corrupt `PrinterState` member subjects; (b) `lv_xml_get_subject(nullptr, name)` only searches globals, so widget lookups would miss scope-registered subjects; (c) ~175 direct `lv_xml_register_subject(nullptr, ...)` call sites in production code bypass the scope-aware helper. The simpler shipped design achieves the real goal (per-test `PrinterState`) without per-test scopes:

1. **`XMLTestFixture` owns per-instance state** — `PrinterState m_state`, `unique_ptr<MoonrakerClient> m_client`, `unique_ptr<MoonrakerAPI> m_api`. Static `s_state`/`s_client`/`s_api` removed.
2. **Subjects register into the global LVGL scope** as before. Each test's `m_state.init_subjects(true)` overwrites global entries with fresh pointers; the destructor tears the screen down **before** `m_state`'s subjects are deinitialized so no widget dereferences stale pointers.
3. **`LVGLTestFixture::s_display` stays static.** Re-creating a virtual DRM display per test is slow and gains nothing.
4. **Scope infrastructure still shipped** (`helix::xml::ScopedSubjectRegistryOverride` + `register_subject_in_current_scope()`). Production and tests don't currently use it, but it's ready for future per-component work (modals, wizards) where local subject scopes make sense.

**Known latent hazard:** `PrinterState::init_subjects(true)` appends a `[this]{ deinit_subjects(); }` lambda to `StaticSubjectRegistry`. After `XMLTestFixture`'s dtor, that `this` dangles. Not hit today (no process-exit `deinit_all()` path), but would segfault if one is added. Fix when relevant: add `unregister()` on `StaticSubjectRegistry`.

### Singleton reset (`HelixTestFixture`)

A thin base fixture that deterministically resets process-wide singletons tests are known to mutate:

```cpp
class HelixTestFixture {
public:
    HelixTestFixture();   // reset_all() at start — idempotent
    ~HelixTestFixture();  // reset_all() at end — leaves clean slate
private:
    static void reset_all();
};
```

Initial reset list (not a kitchen sink; only things tests are known to mutate):
- `UpdateQueue::instance()` — drain pending callbacks
- `SystemSettingsManager::instance()` — reset language/theme to defaults
- `ui_theme` — reset to default theme
- `NavigationManager::instance()` — clear stack
- `ModalStack::instance()` — clear

Expand reactively via flaky-test triage, not speculatively.

`LVGLTestFixture` and most unit-test fixtures inherit from `HelixTestFixture`. Tests that mutate these singletons remain legal — they just no longer leak to subsequent tests.

### Removing `FirstRunTour::reset_for_test()`

With `HelixTestFixture` and per-test scopes, the single `*_for_test()` method in production becomes dead code. Remove it.

## Migration Plan

Four phases, each independently shippable. `make test-run` must stay green and `--test` mode must keep launching after every phase.

### Phase 1 — Test isolation plumbing (no interface work)

- Introduce `HelixTestFixture` base with `reset_all()`.
- Add `scope` parameter to `PrinterState::register_xml_subjects()` (default `nullptr` = global).
- Migrate `XMLTestFixture` to per-test scope + per-test `PrinterState`; kill the statics.
- Delete `FirstRunTour::reset_for_test()`.
- Gate: `make test-run` green; `./build/bin/helix-screen --test` launches identically.

### Phase 2 — Drift-protection smoke tests (four already-abstract backends)

- `AmsBackend`, `EthernetBackend`, `UsbBackend`, `WifiBackend` are already pure virtual interfaces.
- Add one compile-only smoke test per backend that instantiates the mock through a `unique_ptr<BaseInterface>`. If the mock ever falls behind, compilation fails.
- Audit each base class for non-virtual methods that *should* be virtual (drift gap).

### Phase 3 — `IMoonrakerAPI` extraction

**As-shipped (option B, narrow interface).** After reading the 732-line concrete header, the full-surface interface would have forced the 614-line mock to implement ~45 methods (sub-API accessors + non-virtual helpers). Narrower scope instead: `IMoonrakerAPI` contains only the 16 methods currently marked `virtual` on `MoonrakerAPI`. Concrete inherits the interface. Mock stays `public MoonrakerAPI` — it inherits non-virtual helpers and sub-API composition unchanged. Call sites not migrated. Drift protection via the concrete-inheritance chain: adding a pure virtual to `IMoonrakerAPI` breaks concrete's compile, which cascades into the mock.

### Phase 4 — `IMoonrakerClient` extraction

**As-shipped (option B, narrow interface).** Verification showed the 1072-line mock uses zero `hv::WebSocketClient`-specific APIs — its bulk is simulation state. Rebasing to shed the WebSocket base class would have forced reimplementing ~60 non-virtual concrete methods for no real WebSocket-baggage win. So `helix::IMoonrakerClient` contains only the 10 methods currently marked `virtual` on `MoonrakerClient`. Concrete inherits `public hv::WebSocketClient, public IMoonrakerClient`. Mock stays `public helix::MoonrakerClient`. Same drift-protection chain as Phase 3.

### Phase 5 — Verification and cleanup

- Grep for `dynamic_cast<MoonrakerClient*>` and equivalents; resolve any hits.
- Audit remaining `#ifdef HELIX_ENABLE_MOCKS` — some may compact now that mocks are leaner.
- Update `docs/devel/` to reflect the interface pattern.
- Update CLAUDE.md mock guidance.

## Testing Strategy

- **Every phase keeps `make test-run` green.** Non-negotiable.
- **Every phase keeps `./build/bin/helix-screen --test -vv` functional.** Smoke-check by launching and navigating one panel.
- **Interface-completeness smoke test per extraction.** One compile-only test per interface:

  ```cpp
  TEST_CASE("IEthernetBackend mock satisfies interface", "[compile]") {
      std::unique_ptr<IEthernetBackend> p = std::make_unique<EthernetBackendMock>();
      REQUIRE(p != nullptr);
  }
  ```

  If the mock is missing a pure-virtual method, this fails to compile. That is the entire drift-protection story — the compiler does the work.

- **No behavioral test rewrites required.** Friend-class access patterns, test fixtures, and assertions stay. We're tightening the compile-time surface, not the runtime one.

## Risks and Unknowns

**LVGL XML per-test scope may have hidden global caching.** The code I traced supports per-component scopes (modals use them), but `PrinterState` has ~100 subjects and tests instantiate many components. If anything deep in LVGL caches global-scope pointers after registration, the per-test scope plan falls apart.
- *Mitigation:* Phase 1 is test-only and revertable. Fallback is to keep the current `static s_state` model and pursue only interface work. Isolation collapses to "singleton reset via `HelixTestFixture` base" — still a real improvement.

**Moonraker mock leans on real-class internals.** 1072 lines of `MoonrakerClientMock : public MoonrakerClient` probably inherits protected helpers or state. Pulling the mock off `MoonrakerClient` will expose hidden dependencies that need re-expressing against the interface.
- *Mitigation:* Phase 3 runs last, after the pattern is proven on Phase 2 cases. If Moonraker proves intractable, Phases 1–2 still ship independently; we get 3 of 5 interfaces plus full test isolation.

**Singleton reset list is incomplete.** `HelixTestFixture::reset_all()` starts with a handful of known-mutated singletons; lurking mutations will surface as flakiness.
- *Mitigation:* Expand reactively via flaky-test triage. Don't try to be exhaustive upfront.

**No intra-process test parallelism.** LVGL's global display, the XML registry, and `SettingsManager` are all process-wide. This refactor improves determinism (tests pass regardless of order) but doesn't enable parallel execution within a single binary. Separate-process parallelism is an Approach 3 concern.

## Deferred Work

Explicitly out of scope, to be reconsidered when a concrete pain demands them:

- Removing `is_test_mode()` branches in production code.
- Consolidating or retiring the 42 `*TestAccess` friend classes.
- Moving to constructor-injected dependencies (`RuntimeConfig` as a true DI container).
- Splitting the test binary for parallel execution.
- Stripping mocks from the production binary.
