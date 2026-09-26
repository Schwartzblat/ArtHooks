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
| survives `FixupStaticTrampolines` | hooks `VisiblyInitializedCallback::MarkVisiblyInitialized` and re-applies | settles the target off the resolution stub *before* writing its entry point — see §5.1 |
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

### 5.1 Static hooks may be silently clobbered — CLOSED by settling the target first

`ClassLinker::FixupStaticTrampolines` (`class_linker.cc`) runs when a class becomes visibly
initialized and does, for every direct method where `NeedsClinitCheckBeforeCall()` holds:

```cpp
const void* quick_code = instrumentation->GetCodeForInvoke(method);
... UpdateMethodsCode(method, quick_code) ...
```

If `hook_function()` wrote its trampoline into a static target's entry point *before* that callback
fired, the callback would overwrite it. This is the same root as §4.3's backup recursion: a static
target whose class is not yet visibly initialized is parked on the quick resolution stub, and both
capturing that stub (backup recurses) and writing over it (hook clobbered) are wrong.

**Task 1 built `HookSelfTest.static_hook_survives_visible_initialization()`** — it hooks
`LateVisible.describe()`, forces the visible-init flush with a `Class.forName(name, true, loader)`
loop, and re-checks the hook fired. It passed, but nothing in it confirmed `LateVisible` was ever on
the stub, so the pass might have been vacuous.

**Task 2 makes the library log the fact**, and R8b uses those logs as the evidence. `hook_function`
now settles a static target off the resolution stub before touching its entry point, logging whether
the target was on the stub and how many nudges it took to leave. For `LateVisible.describe` on the
Pixel 9a / API 36 / arm64 device (verbatim `ArtHooks`-tag lines):

- **`tools/run-selftest.sh`** (debug APK, never AOT, `aot_code == nullptr`): `static target
  com.example.arthooks.HookSelfTest$LateVisible is not on the resolution stub; no settling needed` —
  a non-native static method with no AOT code starts on the interpreter bridge, not the stub, so
  neither the recursion nor the clobber is reachable, and Task 1's check passed for that reason.
- **`tools/run-aot-selftest.sh speed`** (release APK, `-m speed`): `static target
  com.example.arthooks.HookSelfTest$LateVisible was on the resolution stub; left it after 128 nudges`
  — here the target *was* stub-parked, and `hook_function` drove the class visibly-initialized
  (running `FixupStaticTrampolines`) and confirmed the target left the stub **before** writing the
  hook.

So under `speed` the hook is now installed *after* the fixup, which is exactly why it survives: there
is no later batched callback left to overwrite it. Task 1's pass was hooking a target that, under
`speed`, was on the stub — the pass would have been at risk of both the clobber and (with a backup)
the recursion; settling first closes both. §5.1 is closed by construction rather than by "not
reproduced," and the same mechanism fixes §4.3 — success criterion 1 is met: 27/27 under `-m speed`.

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
