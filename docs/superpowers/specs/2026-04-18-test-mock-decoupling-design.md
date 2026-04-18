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

Five of the six mocked boundaries use the anti-pattern "mock inherits concrete":

| Backend | Current state | Target |
|---------|--------------|--------|
| `AmsBackend` | Already a pure virtual interface; `AmsBackendMock : public AmsBackend` is clean | Verify and move on |
| `EthernetBackend` | Concrete; `EthernetBackendMock : public EthernetBackend` | Extract `IEthernetBackend` |
| `UsbBackend` | Concrete; `UsbBackendMock : public UsbBackend` | Extract `IUsbBackend` |
| `WifiBackend` | Concrete; `WifiBackendMock : public WifiBackend` | Extract `IWifiBackend` |
| `MoonrakerAPI` | Concrete; `MoonrakerAPIMock : public MoonrakerAPI` | Extract `IMoonrakerAPI` |
| `MoonrakerClient` | `MoonrakerClient : public hv::WebSocketClient`; `MoonrakerClientMock : public MoonrakerClient` | Extract `IMoonrakerClient`; mock drops WebSocket baggage |

Naming convention: **`I`-prefix** for interfaces (`IEthernetBackend`, etc.). Concrete class names stay. Mock class names stay. This minimizes call-site churn — the `AmsBackend` precedent of naming the interface with no prefix only maps cleanly when multiple real implementations exist (AFC, CFS, HappyHare), which isn't the case for the other five.

For each extraction:
1. Define `IXxxBackend` as pure virtual with a default destructor.
2. Make `XxxBackend` inherit from `IXxxBackend` (in addition to any existing bases, e.g., `hv::WebSocketClient` for `MoonrakerClient`).
3. Make `XxxBackendMock` inherit from `IXxxBackend` directly — no longer from `XxxBackend`. The mock sheds the real class's accidental baggage.
4. Update the factory (whether static member or free function — per-backend judgment) to return `std::unique_ptr<IXxxBackend>`. Factory still branches on `should_mock_*()`.
5. Update call sites to hold interface pointers.

Mocks stay in `include/` under `HELIX_ENABLE_MOCKS`. Production binary shape unchanged.

### Test isolation (the static-state fix)

Root cause: `PrinterState` registers ~100 subjects into LVGL's **global XML scope**. Tearing down `PrinterState` between tests would leave stale subject pointers in the global scope. Current workaround: make `PrinterState` static and never tear it down.

Fix: use LVGL's existing **per-component scope** primitive (confirmed in `lv_xml_component.c`; already used by modal dialogs per MEMORY.md).

1. **`PrinterState::register_xml_subjects(lv_xml_component_scope_t* scope = nullptr)`** — when `scope == nullptr`, register into the global scope (current production behavior). When a scope is provided, register there instead.
2. **`XMLTestFixture` creates a per-test scope** in its constructor, passes it to `PrinterState` and to `lv_xml_create(scope, ...)` when instantiating components. In the destructor, unregister the component + scope; `PrinterState` is now safe to destruct. Static `s_state`, `s_client`, `s_api` are removed; each test owns its own.
3. **`LVGLTestFixture::s_display` stays static.** Re-creating a virtual DRM display per test is slow and gains nothing; widgets are cleaned between tests regardless.

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

### Phase 2 — Easy interfaces

Three small backends, locks in the pattern before Moonraker:
- `IEthernetBackend`
- `IUsbBackend`
- `IWifiBackend`

Order within phase: `IEthernetBackend` first (smallest mock at 39 lines), then the other two.

### Phase 3 — Moonraker pair

One interface per PR due to size and risk:
- `IMoonrakerAPI` first (smaller, less framework entanglement).
- `IMoonrakerClient` second (WebSocket decoupling required — mock drops `hv::WebSocketClient` base).

### Phase 4 — Verification and cleanup

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
