# ArtHooks reliability: gap analysis against Frida

**Date:** 2026-09-26
**Platform all measurements were taken on:** Pixel 9a, Android 16 / API 36, arm64-v8a.

This is the spec the implementation plan argues from. It records what was measured, what was read
out of AOSP, and what Frida does differently — so the plan's reasoning survives the conversation it
came from.

## 1. Why this exists

A hook that reports success and then does nothing is the worst failure this library can have, and it
has had several. Two were found and one was fixed in the session that produced this document; a
third was found by reading AOSP and has not been reproduced yet. Frida hits none of them, and the
reason is architectural rather than incidental.

## 2. What ArtHooks does today

Overwrites **only** `art::ArtMethod::entry_point_from_quick_compiled_code_` on the target, with a
generated trampoline that loads the replacement's `ArtMethod*` into the quick-ABI method register
and tail-jumps through that method's entry point. The backup is a second trampoline that names the
**real** target `ArtMethod` but has the target's pre-hook entry point address baked in.

Constraints that are not up for negotiation:

- In-process library shipped as an AAR. **No root, no ptrace, no injected agent, no repackaging.**
- Resolves only **exported** libart symbols, by name. Indexes **no** runtime struct offsets.
  `sizeof(ArtMethod)` and `access_flags_`'s offset are measured at runtime from probe methods.
- Four ABIs built; only arm64-v8a exercised on hardware.
- GPL-3.0, `minSdk` 33.

## 3. What Frida does, and why it does not hit these bugs

Frida never touches the target's entry point. It clones the `ArtMethod`, turns the clone into a
synthetic JNI native method (`kAccNative`, `jniCode` = the callback, `quickCode` =
`art_quick_generic_jni_trampoline`), leaves the original alone, and then redirects **inside ART** by
inline-patching `art_quick_generic_jni_trampoline`, `art_quick_to_interpreter_bridge`,
`art_quick_resolution_trampoline`, the target's own compiled prologue, and
`art::interpreter::DoCall`.

Consequences that matter here:

| | Frida | ArtHooks |
|---|---|---|
| captures an entry point as "the original" | **no** — calls the untouched original via JNI | yes, which is the whole static-backup bug |
| survives `FixupStaticTrampolines` | hooks `VisiblyInitializedCallback::MarkVisiblyInitialized` and re-applies | **unverified — see §5.1** |
| `synchronized` semantics | correct, free: the replacement *is* native, so the generic JNI trampoline takes the monitor | replacement does not hold it |
| unhook | full revert; the original was never modified | none |
| AOT inlining | **opt-in only**, and its own docs recommend `dex2oat-flags --inline-max-code-units=0` | handled by default since `disable_aot()` |

Its price: ptrace/root or a repackaged APK, ~70 mangled ART symbols, 43 API-level branches,
**hardcoded** struct offsets for `Runtime`/`ClassLinker`/`Thread`/`ManagedStack`/`ArtField`, byte
pattern scanning, and arm64-only paths that silently no-op elsewhere. It mutates libart's `.text`,
which is globally visible and mutually exclusive between agents.

**The plan does not propose adopting Frida's architecture.** Inline-hooking ART needs an instruction
relocator per ABI and access to non-exported symbols; both are out of scope and out of philosophy.
What the plan takes from Frida is the list of *failure modes worth engineering against*.

## 4. Measured facts (do not re-derive these)

### 4.1 AOT inlining — fixed

dex2oat under `speed`/`speed-profile` inlines a small method's body into its callers; such a caller
never loads the callee's entry point. Same release APK: `hook_and_backup_survive_the_jit` fails on
the first call under `compile -m speed`, passes under `-m verify`.

Fixed by `disable_aot()`, which sets `Runtime::SetRuntimeDebugState(kJavaDebuggable)` so
`Instrumentation::CanUseAotCode()` returns false, plus `Runtime::DeoptimizeBootImage()`.

**Residual hole:** it only governs classes ART links *afterwards*. A class already linked keeps its
AOT bodies. This is the single highest-value remaining gap, and Frida's `DeoptimizeEverything` does
not have it because `InstallStubsClassVisitor` walks `ClassLinker::VisitClasses` — every loaded
class, not just future ones.

### 4.2 `HInliner` gates, from AOSP

`HInliner` refuses to inline when any of these hold — all confirmed in
`art/compiler/optimizing/inliner.cc`:

- `graph_->IsDebuggable()` — "for simplicity, we currently never inline when the graph is
  debuggable". **A debuggable APK therefore cannot reproduce any inlining bug**, which is why the
  debug self-test is structurally blind to this whole class.
- `!method->IsCompilable()` — exactly `kAccCompileDontBother`. So the JIT half is already covered.
- the `@dalvik.annotation.optimization.NeverInline` annotation.

`disable_aot()` does **not** disable JIT inlining — `HInliner` gates on debuggable *graphs*, and
nothing in `disable_aot()` sets the JIT's debuggable compiler option. Confirmed by benchmark: 6M
method calls in 1.5 ms in every configuration is only possible inlined.

### 4.3 Static backup recursion — known cause, not fixed

`Instrumentation::GetInitialEntrypoint`, in `runtime/instrumentation-inl.h`:

```cpp
if (ArtMethod::NeedsClinitCheckBeforeCall(method_access_flags)) {
  return (aot_code != nullptr || ArtMethod::IsNative(method_access_flags))
      ? GetQuickResolutionStub()
      : GetQuickToInterpreterBridge();
}
```

`NeedsClinitCheckBeforeCall()` is exactly `IsStatic() && !IsConstructor()`. So an AOT-compiled static
method sits on the **quick resolution stub** until `ClassLinker::FixupStaticTrampolines()` replaces
it, and that waits for the declaring class to become *visibly* initialized. `install_backup()`
captures the stub; calling the backup re-enters it, it re-reads
`entry_point_from_quick_compiled_code_` — by then the hook — and lands in the replacement. Forever.

Note the condition is `aot_code != nullptr`, **not** `CanUseAotCode()`, so `disable_aot()` does not
affect this branch. What decides it is whether dex2oat compiled the method.

On arm64 the visibly-initialized transition is batched: `ClassLinker::MarkClassInitialized()` sets
only `kInitialized` and queues a `VisiblyInitializedCallback`; `kRuntimeISA == kX86` skips the
batching entirely, so this should not reproduce on x86 at all.

**The escape hatch, from `ClassLinker::EnsureInitialized`:**

```cpp
if (c->IsInitialized()) {
  ...
  } else if (UNLIKELY(!c->IsVisiblyInitialized())) {
    if (self->IncrementMakeVisiblyInitializedCounter()) {
      MakeInitializedClassesVisiblyInitialized(self, /*wait=*/ false);
    }
  }
  return true;
}
```

`Class.forName(name, true, loader)` reaches `EnsureInitialized`. So repeated calls trip a per-thread
counter and make ART itself request the transition — **no `ClassLinker*` needed**. The flush is
asynchronous (`wait=false`), so the result has to be observed rather than assumed.

Measured 2×2 (release APK, self-test):

| | `keep_aot` | `disable_aot` |
|---|---|---|
| `-m speed` | AOT inlining fails first | static backup recurses |
| `-m verify` | **static backup recurses** | **23/23 pass** |

### 4.4 Performance of giving up AOT

`tools/run-benchmark.sh`, minimum of 3 interleaved passes.

- **Steady state is a wash.** Every workload within noise of AOT; the JIT still inlines.
- **First execution of any code path is what you pay for**: 203x on a tight loop of small calls
  under `disable_aot()`, versus 9.7x under `vmSafeMode` alone. One confirmed contributor is
  `Jit::TryPatternMatch`, ART's fast path for trivial getters and setters, gated on
  `!Runtime::Current()->IsJavaDebuggable()`.
- Boot-classpath-heavy workloads are where `DeoptimizeBootImage()` specifically shows:
  `strings` 6.2x and `hashmap` 3.4x under `disable_aot()` versus 1.4x and 1.2x under `vmSafeMode`.
- Cold start on the demo: 89 ms baseline, 95 ms `vmSafeMode`, 124 ms `disable_aot()`.

**Implication:** `DeoptimizeBootImage()` is the expensive half and buys only boot-classpath hooks
called from inside the framework. It should not be automatic.

## 5. Open problems this plan addresses

### 5.1 Static hooks may be silently clobbered — CLOSED, not reproduced

`ClassLinker::FixupStaticTrampolines` (`class_linker.cc`) runs when a class becomes visibly
initialized and does, for every direct method where `NeedsClinitCheckBeforeCall()` holds:

```cpp
const void* quick_code = instrumentation->GetCodeForInvoke(method);
... UpdateMethodsCode(method, quick_code) ...
```

`hook_function()` forces `<clinit>` via `ensure_class_initialized()` and then writes its trampoline
into the entry point. If the class was not *already* visibly initialized, the batched callback fires
**after** the hook is installed and overwrites it. The hook would work briefly and then vanish, with
nothing reporting an error.

`HookSelfTest.static_target_is_hooked_and_initialized` cannot catch this: it checks immediately,
before the batch flushes.

**Task 1 built the test and ran it; the clobber did not happen.**
`HookSelfTest.static_hook_survives_visible_initialization()` hooks `LateVisible.describe()` — a
static method of a class that is untouched (and therefore not yet even `kInitialized`) until the
hook forces its `<clinit>` — confirms the hook fires on the very next call, then calls
`Class.forName(name, true, loader)` 2048 times to trip `EnsureInitialized`'s per-thread counter and
force the batched `MakeInitializedClassesVisiblyInitialized` flush described above, then checks
again.

Measured on the Pixel 9a / API 36 / arm64 device this plan uses throughout:

- **`tools/run-selftest.sh`** (debug APK, compiled `verify`, never AOT — `aot_code == nullptr`, so
  the target starts on the interpreter bridge rather than the resolution stub): logged
  `PASS: static hook survived its class becoming visibly initialized`, and the run finished
  `PASS: all checks passed` (24/24).
- **`tools/run-aot-selftest.sh speed`** (release APK, compiled `-m speed` — the harsher case, where
  §4.3's stub is actually reachable): logged the identical
  `PASS: static hook survived its class becoming visibly initialized`, before the check chain
  reached `DispatchCases.static_target_with_backup` and failed the overall run with a
  `StackOverflowError` in `DispatchCases.static_replacement` — that is §4.3's already-documented
  static-backup recursion, an unrelated defect that has nothing to do with `LateVisible.describe()`
  or `FixupStaticTrampolines` clobbering anything.

In neither build did the hook get lost. §5.1 is closed: `FixupStaticTrampolines` overwriting a hook
was a real reading of the AOSP source but is not, on this measurement, an actual failure mode of
this library. Task 2 is still worth doing, but only for §4.3's static-backup recursion — its
justification is no longer "and this might also be a clobber," because this task found no clobber
to fix.

### 5.2 `disable_aot()` misses already-linked classes

§4.1. Frida reaches `Instrumentation::DeoptimizeEverything`, which covers every loaded class, by
**disassembling `Runtime::DeoptimizeBootImage` to recover `Runtime::instrumentation_`'s offset**.
That is derived from the running binary rather than a hardcoded table, but it is instruction pattern
matching and a wrong answer means calling a C++ member function on a garbage `this`.

### 5.3 No unhook

Trampolines live for the process lifetime. ArtHooks already captures the target's pre-hook entry
point for backups, so restoring it is mostly bookkeeping.

### 5.4 Writes are not read back

`set_entry_point()` stores and returns. Nothing confirms the store landed, and nothing lets a caller
ask later whether a hook is still in place.

### 5.5 Three ABIs have never run

arm/x86/x86_64 trampoline encodings were checked against the NDK assembler and never executed. CI
runs an x86_64 emulator and only runs the *debug* self-test, which cannot see AOT failures.

### 5.6 Stale documentation

`README.md:454` and `CLAUDE.md:304` describe "the backup's snapshot ArtMethod lives in malloc'd
memory, so the GC never visits it". That mechanism was replaced — `install_backup()` names the real
target `ArtMethod`. `CLAUDE.md:142` describes the old mechanism too. The Frida research agent read
these and repeated the claim, so the error is actively propagating.

## 6. Success criteria

1. `tools/run-aot-selftest.sh speed` passes 23/23 on arm64. It is currently 19/23.
2. A hook installed on a static method of a freshly initialized class is still in place after the
   class becomes visibly initialized.
3. `hook_function()` returns false rather than installing a hook that cannot work.
4. `ArtHooks` can report whether a given hook is still installed.
5. No new hardcoded struct offset. Anything that cannot be measured is opt-in and says so.
6. CI runs the AOT self-test, so §5.5 and this whole failure class are exercised on every push.
