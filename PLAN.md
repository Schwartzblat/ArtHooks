# ArtHooks Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the failure modes where ArtHooks installs a hook, reports success, and the hook then
does nothing — so that a hook either works or says it did not.

**Architecture:** Keep the existing design (overwrite only the target's
`entry_point_from_quick_compiled_code_`, resolve only exported libart symbols, measure every offset
at runtime). Add four things around it: force the target's class to *visibly* initialized before
touching any entry point, read every write back, let a caller ask whether a hook is still in place,
and let a caller remove one. The one change that needs an unmeasurable offset is opt-in and labelled
as such.

**Tech Stack:** C++17 via CMake/NDK 28.2.13676358, JNI, Java 17 source level, Gradle 9.4.1 / AGP
9.2.1. Tests are the on-device self-test suite in `:app`, run by `tools/run-selftest.sh` (debug) and
`tools/run-aot-selftest.sh` (release, AOT).

**Spec:** `docs/superpowers/specs/2026-09-26-arthooks-reliability.md`

## Global Constraints

- **No root, no ptrace, no injected agent, no APK repackaging.** ArtHooks is an ordinary AAR linked
  into the app.
- **Only exported libart symbols, resolved by name** through `find_libart_symbol()`. One exception
  is allowed, in Task 6, and it is opt-in and off by default.
- **No new hardcoded struct offset.** `sizeof(ArtMethod)` and `access_flags_` are measured from probe
  methods; anything new follows that rule or is opt-in.
- `minSdk` 33, `compileSdk`/`targetSdk` 36, `ndkVersion` 28.2.13676358 pinned in both modules.
- Four ABIs must keep building: arm64-v8a, armeabi-v7a, x86_64, x86.
- `release` builds keep `optimization { enable false }` — R8 off, because hooked and hooking methods
  must survive by exact name and signature.
- Java methods in this project are `snake_case`. Match the surrounding code.
- Every new native method declared in `ArtHooks.java` must pass `tools/check-jni-symbols.sh` on all
  four ABIs before the task is done.
- `layout_probe_a` and `layout_probe_b` must stay adjacent in dex method ordering, which is
  alphabetical. **Do not add a method to `ArtHooks` whose name sorts between them.**
- GPL-3.0 headers/licence unchanged.

## Review Focus

Failure modes the spec implies that no task's happy path exercises. Each has a test attached to the
task that owns the code.

1. **A static target whose class is *already* visibly initialized** must not pay the nudge loop's
   full budget or be refused. Test in Task 2.
2. **A class whose `<clinit>` throws** must leave `hook_function()` returning false, not looping.
   Test in Task 2.
3. **A target with exactly one static method** gives the sibling-comparison precondition nothing to
   compare against; it must fall through to nudging rather than skipping. Test in Task 2.
4. **`set_entry_point()` onto a read-only page** must report failure rather than silently not
   hooking. Test in Task 3.
5. **`unhook_function()` on a method that was hooked twice** must not resurrect the first hook's
   trampoline into a dangling state. Test in Task 4.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `arthooks/src/main/cpp/class_init.{hpp,cpp}` | forcing `<clinit>`, and now forcing *visible* initialization | modify |
| `arthooks/src/main/cpp/art_method.{hpp,cpp}` | ArtMethod layout, entry-point accessors, `access_flags_` | modify — `set_entry_point` gains a return value |
| `arthooks/src/main/cpp/hook_registry.{hpp,cpp}` | remembers what each hook replaced, for `is_hooked()` and `unhook_function()` | **create** |
| `arthooks/src/main/cpp/arthooks.cpp` | `hook_function()`, JNI entry points | modify |
| `arthooks/src/main/cpp/deoptimize.{hpp,cpp}` | stopping ART using AOT code | modify — split the boot-image half out |
| `arthooks/src/main/java/com/arthooks/ArtHooks.java` | public API | modify |
| `app/src/main/java/com/example/arthooks/HookSelfTest.java` | orchestrator + class-init cases | modify — new cases |
| `app/src/main/java/com/example/arthooks/LifecycleCases.java` | unhook and is_hooked cases | **create** |
| `.github/workflows/build.yml` | CI | modify — add the AOT self-test |

`hook_registry` is a new file rather than more statics in `arthooks.cpp` because it owns real
per-hook state with a lifetime, and `arthooks.cpp` currently owns only a boolean.

---

## Task 1: Prove or disprove the static-hook clobber

The spec's §5.1 says `FixupStaticTrampolines` may overwrite a hook installed on a static method of a
freshly initialized class. **This has never been reproduced.** This task writes the test that
settles it. Everything after it depends on the answer, so it ships on its own.

**Files:**
- Modify: `app/src/main/java/com/example/arthooks/HookSelfTest.java`

**Interfaces:**
- Consumes: `Checks.hook`, `Checks.declared_method`, `Checks.pass`, `Checks.fail` (existing).
- Produces: `HookSelfTest.static_hook_survives_visible_initialization()`, called from
  `HookSelfTest.check()`.

- [ ] **Step 1: Write the failing test**

Add to `HookSelfTest.java`, after `static_target_is_hooked_and_initialized()`:

```java
    // --- a static hook outliving its class's visible initialization -----------------------------

    /**
     * Untouched until the hook runs, so hooking it is what first initializes the class -- which is
     * the state the check below is about.
     */
    static class LateVisible {
        static int marker = 1;

        static String describe() {
            return "original";
        }
    }

    /** Replaces {@link LateVisible#describe}. Static target, so no receiver to stand in for. */
    public static String describe_replacement() {
        return "hooked";
    }

    /**
     * Checks that a hook on a static method is still there once ART makes the class *visibly*
     * initialized.
     *
     * <p>On arm64 that transition is batched: ClassLinker::MarkClassInitialized only sets
     * kInitialized and queues a VisiblyInitializedCallback. When the callback finally runs,
     * ClassLinker::FixupStaticTrampolines rewrites the entry point of every direct method that
     * needs a class-init check -- which is where the hook lives. Hooking therefore has a window in
     * which it appears to work and is then silently undone.
     *
     * <p>Class.forName(name, true, loader) reaches ClassLinker::EnsureInitialized, which bumps a
     * per-thread counter and asks for the transition once it trips, so calling it in a loop is what
     * forces the flush rather than waiting for one to happen by chance.
     */
    private static boolean static_hook_survives_visible_initialization() {
        if (!hook(declared_method(LateVisible.class, "describe"),
                declared_method(HookSelfTest.class, "describe_replacement"))) {
            return false;
        }

        if (!"hooked".equals(LateVisible.describe())) {
            return fail("the static hook was not in place even before the class settled");
        }

        String name = LateVisible.class.getName();
        ClassLoader loader = LateVisible.class.getClassLoader();
        for (int i = 0; i < 2048; i++) {
            try {
                Class.forName(name, true, loader);
            } catch (ClassNotFoundException e) {
                return fail("could not re-resolve " + name);
            }
        }

        String greeting = LateVisible.describe();
        if (!"hooked".equals(greeting)) {
            return fail("the static hook was lost once the class became visibly initialized -> \""
                    + greeting + "\"");
        }
        return pass("static hook survived its class becoming visibly initialized");
    }
```

Wire it into `check()`:

```java
            if (hook_and_backup_survive_the_jit()
                    && backup_survives_a_relocating_gc()
                    && static_target_is_hooked_and_initialized()
                    && static_hook_survives_visible_initialization()
                    && SignatureCases.check()
```

- [ ] **Step 2: Run it against a debug build**

```bash
./tools/run-selftest.sh
```

Record the result verbatim. A debug APK is compiled `verify`, so `aot_code == nullptr` and the
method starts on the interpreter bridge rather than the resolution stub — this run tells you whether
the clobber happens even without AOT code.

- [ ] **Step 3: Run it against an AOT build, which is where it should bite hardest**

```bash
./tools/run-aot-selftest.sh speed
```

Expected if §5.1 is real: `FAIL: the static hook was lost once the class became visibly
initialized -> "original"`.

- [ ] **Step 4: Record the verdict in the spec**

Edit `docs/superpowers/specs/2026-09-26-arthooks-reliability.md` §5.1, replacing "This has **not**
been reproduced" with what actually happened, naming the compiler filter and the observed string.
**If the hook was not lost, say so plainly and mark §5.1 closed** — Task 2 is still worth doing for
the static-backup bug, but its justification changes and the plan should stop claiming this one.

- [ ] **Step 5: Commit**

```bash
git add app/src/main/java/com/example/arthooks/HookSelfTest.java docs/superpowers/specs/2026-09-26-arthooks-reliability.md
git commit -m "test: pin down whether FixupStaticTrampolines clobbers a static hook"
```

---

## Task 2: Force visible class initialization before touching an entry point

Fixes the static-backup recursion (spec §4.3) and, if Task 1 confirmed it, the static-hook clobber
(§5.1). Both have the same cause and the same fix: do not read or write a static method's entry
point while its class is still queued for visible initialization.

**Files:**
- Modify: `arthooks/src/main/cpp/class_init.hpp`
- Modify: `arthooks/src/main/cpp/class_init.cpp`
- Modify: `arthooks/src/main/cpp/arthooks.cpp`
- Test: `app/src/main/java/com/example/arthooks/HookSelfTest.java` (Task 1's case), plus the four
  existing static-backup cases in `DispatchCases`, `ArityCases` and `RuntimeCases`

**Interfaces:**
- Consumes: `get_entry_point(const ArtMethod*)` from `art_method.hpp`; `ensure_class_initialized`
  from `class_init.hpp`.
- Produces:
  ```cpp
  // class_init.hpp
  bool ensure_class_visibly_initialized(JNIEnv *env, jobject executable, ArtMethod *method);
  ```
  Returns true when the method's class is believed visibly initialized (including when it never
  needed to be), false when that could not be achieved.

- [ ] **Step 1: Write the failing test**

The failing tests already exist — the four static-backup cases fail under `-m speed`. Pin the
*precondition* explicitly so a future regression names itself. Add to `HookSelfTest.java` next to
Task 1's case:

```java
    /** A second static method, so the check below has a sibling to compare entry points against. */
    static class TwoStatics {
        static int marker = 1;

        static int first() {
            return 1;
        }

        static int second() {
            return 2;
        }
    }

    public static int first_replacement() {
        return 10;
    }

    public static native int first_backup();

    /**
     * Hooks a static method with a backup in the state that used to recurse: the class is
     * initialized by the hook itself, so it is still only kInitialized when the entry point is
     * captured, and an AOT build leaves it on the quick resolution stub.
     */
    private static boolean static_backup_on_a_fresh_class() {
        if (!hook(declared_method(TwoStatics.class, "first"),
                declared_method(HookSelfTest.class, "first_replacement"),
                declared_method(HookSelfTest.class, "first_backup"))) {
            return false;
        }
        int result = TwoStatics.first();
        if (result != 10) {
            return fail("static target with a backup on a fresh class -> " + result
                    + ", expected 10");
        }
        return pass("static target with a backup hooked on a class the hook itself initialized");
    }
```

Wire it into `check()` immediately after `static_hook_survives_visible_initialization()`.

- [ ] **Step 2: Run it to verify it fails**

```bash
./tools/run-aot-selftest.sh speed
```

Expected: the run dies with `java.lang.StackOverflowError`, reported as `FAIL: a check threw`.

- [ ] **Step 3: Declare the new helper**

Add to `arthooks/src/main/cpp/class_init.hpp`, keeping the existing include of `<jni.h>` and adding
`#include "art_method.hpp"`:

```cpp
/**
 * Waits for ART to make a method's declaring class *visibly* initialized, which is a different
 * thing from initialized and is the state its entry point depends on.
 *
 * Only static methods care. ArtMethod::NeedsClinitCheckBeforeCall() is IsStatic() &&
 * !IsConstructor(), and for those Instrumentation::GetInitialEntrypoint() installs the quick
 * resolution stub -- a stub that re-reads entry_point_from_quick_compiled_code_ when entered --
 * and leaves it there until ClassLinker::FixupStaticTrampolines() runs. That waits for the class to
 * become visibly initialized, which arm64 batches behind a VisiblyInitializedCallback. Reading the
 * entry point inside that window gets the stub; writing one inside it gets overwritten when the
 * callback fires.
 *
 * ART flushes the batch itself: ClassLinker::EnsureInitialized bumps a per-thread counter for an
 * initialized-but-not-visible class and calls MakeInitializedClassesVisiblyInitialized once it
 * trips. Class.forName(name, true, loader) reaches EnsureInitialized, so calling it repeatedly is
 * how this is driven without needing a ClassLinker*, which is not reachable from here.
 *
 * The flush is asynchronous, so the result is *observed* rather than assumed: the method's entry
 * point is watched until it changes. Returns false when it never did, which leaves the caller to
 * decide whether that is fatal.
 */
bool ensure_class_visibly_initialized(JNIEnv *env, jobject executable, ArtMethod *method);
```

- [ ] **Step 4: Implement it**

Add to `arthooks/src/main/cpp/class_init.cpp`. Inside the anonymous namespace, add the cached
members and the sibling check; then the public function.

```cpp
jmethodID g_get_declared_methods = nullptr;
jmethodID g_get_modifiers = nullptr;

// java.lang.reflect.Modifier.STATIC.
constexpr jint kAccStatic = 0x0008;

/** How many EnsureInitialized calls to spend before giving up on the transition. */
constexpr int kMaxNudges = 4096;

/**
 * Whether another static method of the same class shares this one's entry point.
 *
 * Distinct compiled methods do not share an address, but every static method of a class that is not
 * yet visibly initialized sits on the same quick resolution stub. That makes a match a reliable
 * "still on the stub" signal, and a mismatch a reliable "already settled" -- which is what keeps the
 * nudge loop off the common path. A class with only one static method gives nothing to compare, so
 * this answers true and lets the loop decide.
 */
bool shares_entry_point_with_a_sibling(JNIEnv *env, jobject declaring_class, ArtMethod *method) {
    jobjectArray methods = static_cast<jobjectArray>(
            env->CallObjectMethod(declaring_class, g_get_declared_methods));
    if (clear_exception(env, "could not list a class's declared methods") || methods == nullptr) {
        return true;
    }

    const void *entry = get_entry_point(method);
    bool shared = true;
    bool saw_a_sibling = false;
    const jsize count = env->GetArrayLength(methods);
    for (jsize i = 0; i < count; i++) {
        jobject sibling = env->GetObjectArrayElement(methods, i);
        if (sibling == nullptr) {
            continue;
        }
        const jint modifiers = env->CallIntMethod(sibling, g_get_modifiers);
        if (!env->ExceptionCheck() && (modifiers & kAccStatic) != 0) {
            ArtMethod *art_method = get_art_method(env, sibling);
            if (art_method != nullptr && art_method != method) {
                saw_a_sibling = true;
                if (get_entry_point(art_method) != entry) {
                    shared = false;
                }
            }
        }
        env->ExceptionClear();
        env->DeleteLocalRef(sibling);
        if (!shared) {
            break;
        }
    }

    env->DeleteLocalRef(methods);
    return shared && saw_a_sibling;
}
```

Extend `init_class_initializer()` to cache the two new IDs, next to the existing lookups:

```cpp
    g_get_declared_methods = env->GetMethodID(class_class, "getDeclaredMethods",
                                              "()[Ljava/lang/reflect/Method;");
    g_get_modifiers = env->GetMethodID(executable_class, "getModifiers", "()I");
```

and add both to the null check that already guards the others.

Then the public function:

```cpp
bool ensure_class_visibly_initialized(JNIEnv *env, jobject executable, ArtMethod *method) {
    if (method == nullptr) {
        return false;
    }

    jint modifiers = env->CallIntMethod(executable, g_get_modifiers);
    if (clear_exception(env, "could not read a method's modifiers")) {
        return false;
    }
    if ((modifiers & kAccStatic) == 0) {
        // Only NeedsClinitCheckBeforeCall() methods are ever parked on the resolution stub, and a
        // constructor is not one of them even though it is a direct method.
        return true;
    }

    jobject declaring_class = env->CallObjectMethod(executable, g_get_declaring_class);
    if (clear_exception(env, "could not get a method's declaring class")
        || declaring_class == nullptr) {
        return false;
    }
    if (!shares_entry_point_with_a_sibling(env, declaring_class, method)) {
        env->DeleteLocalRef(declaring_class);
        return true;  // already settled
    }

    jstring name = static_cast<jstring>(env->CallObjectMethod(declaring_class, g_get_name));
    jobject loader = env->CallObjectMethod(declaring_class, g_get_class_loader);
    env->DeleteLocalRef(declaring_class);
    if (clear_exception(env, "could not describe a method's declaring class")) {
        return false;
    }

    const void *before = get_entry_point(method);
    bool settled = false;
    for (int nudge = 0; nudge < kMaxNudges && !settled; nudge++) {
        env->CallStaticObjectMethod(g_class_class, g_for_name, name, JNI_TRUE, loader);
        if (clear_exception(env, "class initialisation failed while forcing visibility")) {
            break;
        }
        settled = get_entry_point(method) != before;
    }

    env->DeleteLocalRef(name);
    env->DeleteLocalRef(loader);

    if (!settled) {
        LOGW("could not get ART to make a class visibly initialized after %d attempts; a static "
             "target's entry point may still be the resolution stub", kMaxNudges);
    }
    return settled;
}
```

- [ ] **Step 5: Use it in `hook_function()`**

In `arthooks/src/main/cpp/arthooks.cpp`, after the ArtMethods are resolved and **before**
`warn_if_aot_compiled()` and `install_backup()`:

```cpp
    // Before the entry point is read or written: a static method of a class ART has initialized but
    // not yet made *visibly* initialized is parked on the quick resolution stub, and
    // FixupStaticTrampolines will overwrite whatever is there when the class finally settles.
    // Reading it gives a stub that re-dispatches through the hook; writing it gets undone.
    const bool settled = ensure_class_visibly_initialized(env, original, target);
    if (!settled && backup != nullptr) {
        LOGE("refusing to back up a static target whose class is not visibly initialized yet: the "
             "captured entry point would be the resolution stub, and calling the backup would "
             "re-enter the hook until the stack overflows");
        return false;
    }
```

Then **delete** the `if (target_is_static)` warning block in `reject_mismatched_backup()` and the
paragraph about it in that function's doc comment, replacing the paragraph with a pointer to
`ensure_class_visibly_initialized()`.

- [ ] **Step 6: Add the new source dependency**

`class_init.cpp` now calls `get_entry_point` and `get_art_method`. It already gets them through the
new `#include "art_method.hpp"` in `class_init.hpp`. No `CMakeLists.txt` change is needed — no file
was added.

- [ ] **Step 7: Run the tests**

```bash
./tools/run-selftest.sh
./tools/run-aot-selftest.sh verify
./tools/run-aot-selftest.sh speed
```

Expected: **23/23 plus the two new cases, in all three.** In particular
`DispatchCases.static_target_with_backup`, `RuntimeCases.chained_hooks` and both `ArityCases`
`StaticTarget` cases now pass under `-m speed`, which is the spec's success criterion 1.

- [ ] **Step 8: Cover the Review Focus cases**

Add to `HookSelfTest.java`, wired into `check()` after `static_backup_on_a_fresh_class()`:

```java
    /** Review Focus 1 and 3: a settled class, and a class with a single static method. */
    static class OneStatic {
        static int only() {
            return 1;
        }
    }

    public static int only_replacement() {
        return 7;
    }

    private static boolean settled_and_single_static_classes() {
        // Settled: TwoStatics was initialized and hooked in the previous check, so by now its
        // transition has happened. Hooking its other method must not need any nudging.
        long started = System.nanoTime();
        if (!hook(declared_method(TwoStatics.class, "second"),
                declared_method(HookSelfTest.class, "only_replacement"))) {
            return false;
        }
        long elapsed_ms = (System.nanoTime() - started) / 1_000_000L;
        if (TwoStatics.second() != 7) {
            return fail("hooking an already-settled static target did not take");
        }
        if (elapsed_ms > 250) {
            return fail("hooking an already-settled static target took " + elapsed_ms
                    + "ms, so the sibling check is not short-circuiting the nudge loop");
        }

        // Single static method: nothing to compare against, so this must fall through to nudging
        // and still work rather than being skipped or refused.
        if (!hook(declared_method(OneStatic.class, "only"),
                declared_method(HookSelfTest.class, "only_replacement"))) {
            return false;
        }
        if (OneStatic.only() != 7) {
            return fail("hooking a class with a single static method did not take");
        }
        return pass("settled classes short-circuit, and a single-static-method class still hooks");
    }
```

Review Focus 2 — a throwing `<clinit>` — is covered by the existing `clear_exception` path:
`ensure_class_initialized()` already returns false before `ensure_class_visibly_initialized()` is
reached, so `hook_function()` returns false. Add a case asserting that, in the same file:

```java
    /** Review Focus 2: a class initializer that throws must fail the hook, not loop. */
    static class Exploding {
        static {
            if (Boolean.parseBoolean("true")) {
                throw new IllegalStateException("boom");
            }
        }

        static int value() {
            return 1;
        }
    }

    public static int exploding_replacement() {
        return 2;
    }

    private static boolean throwing_class_initializer_fails_the_hook() {
        boolean hooked = ArtHooks.hook_function(
                declared_method(Exploding.class, "value"),
                declared_method(HookSelfTest.class, "exploding_replacement"));
        if (hooked) {
            return fail("hooking a class whose <clinit> throws reported success");
        }
        return pass("a throwing class initializer fails the hook instead of looping");
    }
```

This needs `import com.arthooks.ArtHooks;` in `HookSelfTest.java`, because it calls
`hook_function` directly rather than through `Checks.hook` — `Checks.hook` treats false as a
failure, and here false is the expected answer.

- [ ] **Step 9: Run the tests again**

```bash
./tools/run-selftest.sh && ./tools/run-aot-selftest.sh speed
```

Expected: PASS, including the three new cases.

- [ ] **Step 10: Update the documentation this makes wrong**

- `README.md`: delete the TODO section's static-backup entry; move what remains of it into
  Limitations only if anything is still true. Update Status to the new counts.
- `README.md` "The two rules": the sentence "A non-native backup is refused rather than installed,
  for a target that is not static. Static targets are exempt only because the rule there is not yet
  known" is now wrong — static targets are handled, not exempt.
- `CLAUDE.md`: rewrite the "Backing up a `static` target can recurse" bullet to describe the fix.
- **`README.md:454` and `CLAUDE.md:304,142`**: delete the claim that the backup is a snapshot in
  malloc'd memory that the GC never visits. `install_backup()` names the real target `ArtMethod`;
  that limitation no longer exists and the wrong mechanism is being repeated by readers.

- [ ] **Step 11: Commit**

```bash
git add arthooks/src/main/cpp/class_init.hpp arthooks/src/main/cpp/class_init.cpp \
        arthooks/src/main/cpp/arthooks.cpp \
        app/src/main/java/com/example/arthooks/HookSelfTest.java README.md CLAUDE.md
git commit -m "fix: settle a static target's class before reading or writing its entry point"
```

---

## Task 3: Remember hooks, verify every write, and expose `is_hooked()`

Spec §5.4. `set_entry_point()` stores and returns; nothing confirms it landed, and nothing lets a
caller ask later whether a hook is still there. Both halves share one deliverable — a hook that
reports success is verifiably installed — so they ship together.

**Files:**
- Create: `arthooks/src/main/cpp/hook_registry.hpp`
- Create: `arthooks/src/main/cpp/hook_registry.cpp`
- Modify: `arthooks/src/main/cpp/CMakeLists.txt`
- Modify: `arthooks/src/main/cpp/art_method.hpp`
- Modify: `arthooks/src/main/cpp/art_method.cpp`
- Modify: `arthooks/src/main/cpp/arthooks.cpp`
- Modify: `arthooks/src/main/java/com/arthooks/ArtHooks.java`
- Test: `app/src/main/java/com/example/arthooks/HookSelfTest.java`

**Interfaces:**
- Consumes: `ArtMethod`, `get_entry_point`, `get_art_method` from `art_method.hpp`.
- Produces:
  ```cpp
  // art_method.hpp -- signature CHANGES from void to bool
  bool set_entry_point(ArtMethod *art_method, void *entry_point);

  // hook_registry.hpp
  void remember_hook(ArtMethod *target, void *previous_entry, void *installed_entry);
  bool hook_is_installed(ArtMethod *target);
  bool forget_hook(ArtMethod *target, void **previous_entry_out);
  ```
  and, in Java, `public static native boolean is_hooked(Executable method);`

- [ ] **Step 1: Write the failing test**

Covers Review Focus 4. There is no way to make ART's own page read-only from Java, so the test
asserts the observable contract instead. Add to `HookSelfTest.java`, and add
`import java.lang.reflect.Method;`:

```java
    static class Verified {
        static int marker = 1;

        int value() {
            return 1;
        }
    }

    public static int verified_replacement(Object thiz) {
        return 5;
    }

    private static boolean a_successful_hook_is_actually_installed() {
        Method target = declared_method(Verified.class, "value");
        if (ArtHooks.is_hooked(target)) {
            return fail("is_hooked() was true before anything was hooked");
        }
        if (!hook(target, declared_method(HookSelfTest.class, "verified_replacement",
                with_thiz()))) {
            return false;
        }
        if (!ArtHooks.is_hooked(target)) {
            return fail("hook_function() returned true but is_hooked() says otherwise");
        }
        if (new Verified().value() != 5) {
            return fail("is_hooked() agreed but the hook did not fire");
        }
        return pass("a hook that reports success is verifiably installed");
    }
```

Wire it into `check()` after `throwing_class_initializer_fails_the_hook()`.

- [ ] **Step 2: Run it to verify it fails**

Run: `./gradlew :app:assembleDebug`
Expected: `cannot find symbol: method is_hooked(java.lang.reflect.Method)`

- [ ] **Step 3: Write the registry header**

Create `arthooks/src/main/cpp/hook_registry.hpp`:

```cpp
//
// Created by alon on 9/26/26.
//

#ifndef ARTHOOKS_HOOK_REGISTRY_HPP
#define ARTHOOKS_HOOK_REGISTRY_HPP

#include "art_method.hpp"

/**
 * Remembers what each hooked method's entry point was, and what was put there.
 *
 * Two things need this. A caller asking "is this still hooked?" needs something to compare the
 * current entry point against, because the field is the only evidence and ART rewrites it for
 * reasons of its own -- class initialisation, JIT compilation, deoptimisation. And unhooking needs
 * the address that was displaced.
 *
 * Entries are removed only by forget_hook(), and the trampolines they name are never freed, so a
 * stored address stays valid for the process's lifetime.
 */
void remember_hook(ArtMethod *target, void *previous_entry, void *installed_entry);

/**
 * Whether this method's entry point still holds the trampoline that was installed for it.
 *
 * False for a method that was never hooked, and false for one whose hook ART has since overwritten
 * -- which is the interesting answer, because nothing else reports that.
 */
bool hook_is_installed(ArtMethod *target);

/**
 * Drops the most recent hook on this method and hands back the entry point it displaced.
 *
 * Returns false when the method is not in the registry. A method hooked twice has two entries, so
 * unhooking twice walks back through them in order.
 */
bool forget_hook(ArtMethod *target, void **previous_entry_out);

#endif //ARTHOOKS_HOOK_REGISTRY_HPP
```

- [ ] **Step 4: Write the registry**

Create `arthooks/src/main/cpp/hook_registry.cpp`:

```cpp
//
// Created by alon on 9/26/26.
//

#include "hook_registry.hpp"

#include <iterator>
#include <mutex>
#include <vector>

namespace {

struct Entry {
    ArtMethod *target;
    void *previous_entry;
    void *installed_entry;
};

// A vector rather than a map: hooks are counted in tens, lookups happen at install time and at an
// explicit is_hooked() call, and a stack is exactly the shape unwinding a chained hook needs.
std::vector<Entry> g_hooks;
std::mutex g_lock;

}  // namespace

void remember_hook(ArtMethod *target, void *previous_entry, void *installed_entry) {
    std::lock_guard<std::mutex> guard(g_lock);
    g_hooks.push_back(Entry{target, previous_entry, installed_entry});
}

bool hook_is_installed(ArtMethod *target) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto entry = g_hooks.rbegin(); entry != g_hooks.rend(); ++entry) {
        if (entry->target == target) {
            return get_entry_point(target) == entry->installed_entry;
        }
    }
    return false;
}

bool forget_hook(ArtMethod *target, void **previous_entry_out) {
    std::lock_guard<std::mutex> guard(g_lock);
    for (auto entry = g_hooks.rbegin(); entry != g_hooks.rend(); ++entry) {
        if (entry->target == target) {
            *previous_entry_out = entry->previous_entry;
            g_hooks.erase(std::next(entry).base());
            return true;
        }
    }
    return false;
}
```

- [ ] **Step 5: Add it to the build**

`arthooks/src/main/cpp/CMakeLists.txt`:

```cmake
add_library(${CMAKE_PROJECT_NAME} SHARED
        arthooks.cpp art_method.cpp art_symbols.cpp class_init.cpp deoptimize.cpp
        hook_registry.cpp trampoline.cpp)
```

- [ ] **Step 6: Make `set_entry_point` report**

In `art_method.hpp`, change the declaration and document it:

```cpp
/**
 * Writes an entry point, and reports whether the write is visible afterwards.
 *
 * The store can silently not happen -- a read-only mapping is the plausible cause -- and a hook
 * that did not take looks exactly like one that did from the caller's side. Reading it back is the
 * only thing that distinguishes them.
 */
bool set_entry_point(ArtMethod *art_method, void *entry_point);
```

In `art_method.cpp`:

```cpp
bool set_entry_point(ArtMethod *art_method, void *entry_point) {
    void **slot = entry_point_slot(art_method);
    make_page_writable(slot);
    // Another thread can be dispatching through this method right now, and the trampoline it may
    // pick up has to be fully written before the pointer to it becomes visible.
    __atomic_store_n(slot, entry_point, __ATOMIC_RELEASE);

    void *stored = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (stored != entry_point) {
        LOGE("entry point write to %p did not take: wanted %p, read back %p",
             art_method, entry_point, stored);
        return false;
    }
    return true;
}
```

- [ ] **Step 7: Use both in `hook_function()` and `install_backup()`**

In `arthooks.cpp`, add `#include "hook_registry.hpp"`. In `install_backup()`:

```cpp
    if (!set_entry_point(backup, trampoline)) {
        return false;
    }
    LOGD("backup ArtMethod %p now runs the body of %p (entry %p)", backup, target, original_entry);
    return true;
```

and at the end of `hook_function()`:

```cpp
    void *previous_entry = get_entry_point(target);
    if (!set_entry_point(target, trampoline)) {
        return false;
    }
    remember_hook(target, previous_entry, trampoline);

    LOGI("hooked ArtMethod %p with %p", target, hook);
    return true;
```

- [ ] **Step 8: Add the JNI entry point**

In `arthooks.cpp`:

```cpp
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_arthooks_ArtHooks_is_1hooked(JNIEnv *env, jclass clazz, jobject method) {
    if (!g_initialized || method == nullptr) {
        return JNI_FALSE;
    }
    ArtMethod *art_method = get_art_method(env, method);
    return (art_method != nullptr && hook_is_installed(art_method)) ? JNI_TRUE : JNI_FALSE;
}
```

- [ ] **Step 9: Declare it in Java**

In `ArtHooks.java`, after the `hook_function` overloads. **Name check: `is_hooked` sorts after
`flag_probe_b` and before `layout_probe_a`, so it does not come between the layout probes.**

```java
    /**
     * Whether {@code method}'s entry point still holds the trampoline ArtHooks put there.
     *
     * <p>False for a method that was never hooked — and, more usefully, false for one whose hook ART
     * has since overwritten. ART rewrites entry points for reasons of its own (class
     * initialization, JIT compilation, deoptimization), and when that lands on a hooked method the
     * hook is gone with nothing reporting it. This is how to find out.
     */
    public static native boolean is_hooked(Executable method);
```

- [ ] **Step 10: Run the tests and the symbol check**

```bash
./gradlew :arthooks:assembleDebug :arthooks:assembleRelease :app:assembleDebug :app:assembleRelease
./tools/check-jni-symbols.sh
./tools/run-selftest.sh && ./tools/run-aot-selftest.sh speed
```

Expected: `ok` for all four ABIs twice, and PASS for both suites.

- [ ] **Step 11: Commit**

```bash
git add arthooks/src/main/cpp/hook_registry.hpp arthooks/src/main/cpp/hook_registry.cpp \
        arthooks/src/main/cpp/CMakeLists.txt arthooks/src/main/cpp/art_method.hpp \
        arthooks/src/main/cpp/art_method.cpp arthooks/src/main/cpp/arthooks.cpp \
        arthooks/src/main/java/com/arthooks/ArtHooks.java \
        app/src/main/java/com/example/arthooks/HookSelfTest.java
git commit -m "feat: verify every entry-point write and expose is_hooked()"
```

---

## Task 4: Unhook

Spec §5.3.

**Files:**
- Modify: `arthooks/src/main/cpp/arthooks.cpp`
- Modify: `arthooks/src/main/java/com/arthooks/ArtHooks.java`
- Create: `app/src/main/java/com/example/arthooks/LifecycleCases.java`
- Modify: `app/src/main/java/com/example/arthooks/HookSelfTest.java`

**Interfaces:**
- Consumes: `forget_hook`, `hook_is_installed` from `hook_registry.hpp`; `set_entry_point` from
  `art_method.hpp`.
- Produces: `public static native boolean unhook_function(Executable method);` and
  `LifecycleCases.check()`.

- [ ] **Step 1: Write the failing test**

Create `app/src/main/java/com/example/arthooks/LifecycleCases.java`:

```java
package com.example.arthooks;

import com.arthooks.ArtHooks;

import java.lang.reflect.Method;

import static com.example.arthooks.Checks.declared_method;
import static com.example.arthooks.Checks.fail;
import static com.example.arthooks.Checks.hook;
import static com.example.arthooks.Checks.pass;
import static com.example.arthooks.Checks.with_thiz;

/** Cases about a hook's lifetime rather than its dispatch: removing one, and removing a chain. */
class LifecycleCases {

    static boolean check() {
        return unhook_restores_the_original() && unhook_unwinds_a_chain();
    }

    // --- removing a hook -------------------------------------------------------------------------

    static class Target {
        int value() {
            return 1;
        }
    }

    static int value_replacement(Object thiz) {
        return 2;
    }

    private static boolean unhook_restores_the_original() {
        Method target = declared_method(Target.class, "value");
        if (!hook(target, declared_method(LifecycleCases.class, "value_replacement", with_thiz()))) {
            return false;
        }
        if (new Target().value() != 2) {
            return fail("the hook did not take before unhooking was tried");
        }

        if (!ArtHooks.unhook_function(target)) {
            return fail("unhook_function() returned false for a hooked method");
        }
        if (ArtHooks.is_hooked(target)) {
            return fail("is_hooked() is still true after unhooking");
        }
        int result = new Target().value();
        if (result != 1) {
            return fail("after unhooking -> " + result + ", expected the original 1");
        }
        if (ArtHooks.unhook_function(target)) {
            return fail("unhook_function() reported success for a method that is not hooked");
        }
        return pass("unhooking restores the original body and is idempotent");
    }

    // --- removing one of two chained hooks -------------------------------------------------------

    static class Chained {
        int value() {
            return 1;
        }
    }

    static int chain_outer(Object thiz) {
        return 100;
    }

    static int chain_inner(Object thiz) {
        return 10;
    }

    /**
     * Review Focus 5. Hooking twice chains, second hook outermost, so unhooking once has to leave
     * the first hook in place and working rather than restoring the original or dangling.
     */
    private static boolean unhook_unwinds_a_chain() {
        Method target = declared_method(Chained.class, "value");
        if (!hook(target, declared_method(LifecycleCases.class, "chain_inner", with_thiz()))
                || !hook(target, declared_method(LifecycleCases.class, "chain_outer",
                        with_thiz()))) {
            return false;
        }
        if (new Chained().value() != 100) {
            return fail("the second hook is not outermost before unhooking was tried");
        }

        if (!ArtHooks.unhook_function(target)) {
            return fail("unhook_function() returned false for the outer hook");
        }
        int after_one = new Chained().value();
        if (after_one != 10) {
            return fail("after removing the outer hook -> " + after_one + ", expected 10");
        }

        if (!ArtHooks.unhook_function(target)) {
            return fail("unhook_function() returned false for the inner hook");
        }
        int after_both = new Chained().value();
        if (after_both != 1) {
            return fail("after removing both hooks -> " + after_both + ", expected 1");
        }
        return pass("unhooking a chained hook unwinds one layer at a time");
    }
}
```

Wire it into `HookSelfTest.check()` after `ArityCases.check()`:

```java
                    && ArityCases.check()
                    && LifecycleCases.check()) {
```

- [ ] **Step 2: Run it to verify it fails**

```bash
./gradlew :app:assembleDebug
```

Expected: `cannot find symbol: method unhook_function(java.lang.reflect.Method)`.

- [ ] **Step 3: Implement unhooking**

In `arthooks.cpp`:

```cpp
/**
 * Puts back the entry point a hook displaced.
 *
 * Only the entry point is restored, because only the entry point was changed -- with one exception
 * that is deliberately not undone: kAccCompileDontBother stays set. Clearing it would let ART
 * compile a method that other hooks in a chain may still be redirecting, and the flag costs nothing
 * but some JIT throughput on a method that was hot enough to be worth hooking.
 *
 * The trampoline is not freed. Another thread can be inside it right now, there is no way to know
 * when it is not, and trampolines are bump-allocated out of a shared page that nothing can return
 * memory to anyway.
 */
bool unhook_function(JNIEnv *env, jobject method) {
    if (!g_initialized) {
        LOGE("ArtHooks failed to initialise; refusing to unhook");
        return false;
    }
    if (method == nullptr) {
        LOGE("unhook_function() needs a non-null method");
        return false;
    }

    ArtMethod *target = get_art_method(env, method);
    if (target == nullptr) {
        LOGE("could not resolve an ArtMethod to unhook");
        return false;
    }

    void *previous_entry = nullptr;
    if (!forget_hook(target, &previous_entry)) {
        LOGW("ArtMethod %p is not hooked", target);
        return false;
    }
    if (!set_entry_point(target, previous_entry)) {
        return false;
    }

    LOGI("unhooked ArtMethod %p, entry point restored to %p", target, previous_entry);
    return true;
}
```

and the JNI entry point:

```cpp
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_arthooks_ArtHooks_unhook_1function(JNIEnv *env, jclass clazz, jobject method) {
    return unhook_function(env, method) ? JNI_TRUE : JNI_FALSE;
}
```

- [ ] **Step 4: Declare it in Java**

In `ArtHooks.java`. **Name check: `unhook_function` sorts after `layout_probe_b`, so it does not
come between the layout probes.**

```java
    /**
     * Removes the most recent hook on {@code method}, restoring the entry point it displaced.
     *
     * <p>A method hooked twice has two hooks, and this removes one layer: the first call leaves the
     * earlier hook in place and working, the second restores the original. Returns false if the
     * method is not hooked.
     *
     * <p>The trampoline is not freed — another thread may be executing it, and there is no way to
     * know when none is. Nor is {@code kAccCompileDontBother} cleared, because other hooks in a
     * chain may still depend on ART not compiling the method.
     *
     * <p><b>This does not synchronize with calls in flight.</b> A thread already inside the
     * replacement stays there, and one that has already loaded the entry point still jumps to the
     * trampoline. Unhook when you know the method is quiet.
     */
    public static native boolean unhook_function(Executable method);
```

- [ ] **Step 5: Run the tests and the symbol check**

```bash
./gradlew :arthooks:assembleDebug :arthooks:assembleRelease :app:assembleDebug :app:assembleRelease
./tools/check-jni-symbols.sh
./tools/run-selftest.sh && ./tools/run-aot-selftest.sh speed
```

Expected: `ok` for all four ABIs, and PASS including the two new lifecycle cases.

- [ ] **Step 6: Update the documentation**

- `README.md` Limitations: replace "**No unhook.** Trampolines and snapshots live for the lifetime
  of the process" with the accurate version — unhooking exists, trampolines are still never freed,
  and it does not synchronize with calls in flight.
- `README.md` API table: add `is_hooked` and `unhook_function`.
- `CLAUDE.md`: same two corrections, and add `hook_registry.{hpp,cpp}` to the Layout block.

- [ ] **Step 7: Commit**

```bash
git add arthooks/src/main/cpp/arthooks.cpp arthooks/src/main/java/com/arthooks/ArtHooks.java \
        app/src/main/java/com/example/arthooks/LifecycleCases.java \
        app/src/main/java/com/example/arthooks/HookSelfTest.java README.md CLAUDE.md
git commit -m "feat: add unhook_function()"
```

---

## Task 5: Split the boot-image deoptimization out of `disable_aot()`

Spec §4.4 measured `DeoptimizeBootImage()` as the expensive half — 6.2x on `strings`, 3.4x on
`hashmap` versus 1.4x and 1.2x without it — bought only for boot-classpath hooks called from inside
the framework.

**Files:**
- Modify: `arthooks/src/main/cpp/deoptimize.hpp`
- Modify: `arthooks/src/main/cpp/deoptimize.cpp`
- Modify: `arthooks/src/main/java/com/arthooks/ArtHooks.java`
- Modify: `README.md`, `CLAUDE.md`

**Interfaces:**
- Consumes: `find_libart_symbol` from `art_symbols.hpp`.
- Produces: `bool disable_aot_code(bool deoptimize_boot_image);` — the existing no-argument form is
  replaced, and `arthooks.cpp`'s `Java_com_arthooks_ArtHooks_disable_1aot` passes the flag through.

- [ ] **Step 1: Write the failing test**

There is no on-device assertion for a performance split; the benchmark is the test. Record the
baseline first:

```bash
./tools/run-benchmark.sh 3 | tee /tmp/benchmark-before.txt
```

- [ ] **Step 2: Make the boot-image half a parameter**

In `deoptimize.hpp`, change the declaration and add to the doc comment:

```cpp
/**
 * ...existing comment...
 *
 * `deoptimize_boot_image` controls the expensive half. Without it, framework code that was already
 * linked keeps its AOT bodies, so a boot-classpath method hooked here is redirected for the app's
 * own calls but not necessarily for calls made inside the framework. With it, the whole boot image
 * falls back to nterp plus JIT, which was measured at 6.2x on the first run of a StringBuilder
 * workload and 3.4x on a HashMap one, against 1.4x and 1.2x without. Steady state is unaffected
 * either way.
 */
bool disable_aot_code(bool deoptimize_boot_image);
```

In `deoptimize.cpp`, thread it through:

```cpp
bool disable_aot_code(bool deoptimize_boot_image_too) {
    ...
    if (!mark_java_debuggable(runtime)) {
        return false;
    }
    g_disabled = true;

    const bool boot_image = deoptimize_boot_image_too && deoptimize_boot_image(runtime);

    LOGI("ART will no longer run AOT code for classes loaded from here on%s",
         boot_image ? ", and the boot image was deoptimized" : "");
    if (deoptimize_boot_image_too && !boot_image) {
        LOGW("could not deoptimize the boot image, so framework code that was already linked keeps "
             "its AOT bodies -- hooking a boot-classpath method may still miss calls made inside "
             "the framework");
    }
    return true;
}
```

- [ ] **Step 3: Pass the flag from Java**

In `arthooks.cpp`:

```cpp
extern "C"
JNIEXPORT jboolean JNICALL
Java_com_arthooks_ArtHooks_disable_1aot(JNIEnv *env, jclass clazz, jboolean deoptimize_boot_image) {
    return disable_aot_code(deoptimize_boot_image == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}
```

In `ArtHooks.java`:

```java
    /**
     * Set this to {@code true} <em>before</em> anything touches this class to also deoptimize the
     * boot image, which is what makes a hook on a boot-classpath method fire for calls the
     * framework makes internally. It is the expensive half: measured at 6.2x on the first run of a
     * {@code StringBuilder} workload against 1.4x without. Off by default.
     */
    public static final String DEOPTIMIZE_BOOT_IMAGE_PROPERTY = "arthooks.deoptimize_boot_image";

    ...

    static {
        System.loadLibrary("arthooks");
        AVAILABLE = init(Build.VERSION.SDK_INT);
        AOT_DISABLED = AVAILABLE
                && !Boolean.parseBoolean(System.getProperty(KEEP_AOT_PROPERTY))
                && disable_aot(Boolean.parseBoolean(
                        System.getProperty(DEOPTIMIZE_BOOT_IMAGE_PROPERTY)));
    }

    ...

    public static native boolean disable_aot(boolean deoptimize_boot_image);
```

**Symbol note:** `disable_aot` has no overloads, so the mangled name stays
`Java_com_arthooks_ArtHooks_disable_1aot` — parameter types are only mangled in for overloaded
natives. `tools/check-jni-symbols.sh` confirms this; do not hand-edit the C++ name.

- [ ] **Step 4: Run the tests and the symbol check**

```bash
./gradlew :arthooks:assembleDebug :arthooks:assembleRelease :app:assembleDebug :app:assembleRelease
./tools/check-jni-symbols.sh
./tools/run-selftest.sh && ./tools/run-aot-selftest.sh speed
```

Expected: `ok`, and PASS. **`RuntimeCases.cross_dex_replacement` must still pass** — it hooks
`StringTokenizer.countTokens()` and calls it from the app, which does not need the boot image
deoptimized.

- [ ] **Step 5: Confirm the saving**

```bash
./tools/run-benchmark.sh 3 | tee /tmp/benchmark-after.txt
diff /tmp/benchmark-before.txt /tmp/benchmark-after.txt
```

Expected: the `AOT + disable_aot` column's first-round figures for `strings` and `hashmap` drop
towards the `vmSafeMode only` column. If they do not, the boot image was not the cause and the
split is not worth keeping — **say so and revert the task** rather than shipping a knob that buys
nothing.

- [ ] **Step 6: Update the documentation**

- `README.md` AOT section: document `DEOPTIMIZE_BOOT_IMAGE_PROPERTY` and that the boot-image half is
  now opt-in, with the measured numbers already in that section.
- `README.md` Limitations: the boot-classpath bullet now depends on the property.
- `CLAUDE.md`: the same, in the "Why AOT breaks hooks" section and the boot-classpath bullet.

- [ ] **Step 7: Commit**

```bash
git add arthooks/src/main/cpp/deoptimize.hpp arthooks/src/main/cpp/deoptimize.cpp \
        arthooks/src/main/cpp/arthooks.cpp arthooks/src/main/java/com/arthooks/ArtHooks.java \
        README.md CLAUDE.md
git commit -m "perf: make boot-image deoptimization opt-in"
```

---

## Task 6: Run the AOT self-test in CI

Spec §5.5. CI runs an x86_64 emulator and only the debug self-test, which cannot see any inlining
failure. x86_64 also exercises trampoline encodings that have never executed anywhere.

**Files:**
- Modify: `.github/workflows/build.yml`

**Interfaces:**
- Consumes: `tools/run-aot-selftest.sh`.
- Produces: nothing other tasks use.

- [ ] **Step 1: Read the existing workflow**

```bash
cat .github/workflows/build.yml
```

Find the job that boots the emulator and runs `tools/run-selftest.sh`, and note its `runs-on`, the
emulator action and API level it uses, and whether it already has a step that depends on a
connected device.

- [ ] **Step 2: Add the AOT run to that job**

Immediately after the existing `tools/run-selftest.sh` step, in the same job so the emulator is
already up:

```yaml
      - name: Self-test on an AOT-compiled release build
        run: ./tools/run-aot-selftest.sh speed
```

The script builds and signs the release APK itself and creates a debug keystore if the runner has
none, so it needs no extra setup.

- [ ] **Step 3: Verify the script works on x86_64 before trusting CI**

If an x86_64 emulator is available locally, run it there:

```bash
./tools/run-aot-selftest.sh speed
```

Expected: PASS. Note from the spec §4.3 that the static-backup defect should not reproduce on x86 at
all, because `ClassLinker::MarkClassInitialized` skips the visibly-initialized batching on x86 —
so after Task 2 this should pass on both architectures, and **a failure here on x86_64 means Task
2's fix is wrong rather than unnecessary.**

- [ ] **Step 4: Push and watch the run**

```bash
git add .github/workflows/build.yml
git commit -m "ci: run the self-test against an AOT-compiled build"
git push
gh run watch
```

Expected: green. If the emulator cannot apply a compiler filter (some images refuse
`cmd package compile`), the script reports the filter it actually got — if it is not `speed`, the
step is not testing anything and should be dropped with a comment saying why.

---

## Task 7 (optional, opt-in, risky): Cover already-linked classes

Spec §5.2. `disable_aot()` only governs classes ART links afterwards. Reaching
`Instrumentation::DeoptimizeEverything` or `UpdateEntrypointsForDebuggable` — both of which walk
every loaded class — needs an `Instrumentation*`, which means `Runtime::GetInstrumentation()`'s
result, which is an inline accessor over a struct offset.

**This is the one thing in this plan that cannot be measured.** Frida recovers the offset by
disassembling `Runtime::DeoptimizeBootImage`. A wrong answer means calling a C++ member function on
a garbage `this`, which is a segfault, not a returned error. It therefore ships **off by default**,
behind a property, and the task is worth doing only if someone actually needs it.

**Do not start this task before Tasks 1–6 are merged and green.**

**Files:**
- Modify: `arthooks/src/main/cpp/deoptimize.hpp`
- Modify: `arthooks/src/main/cpp/deoptimize.cpp`
- Modify: `arthooks/src/main/java/com/arthooks/ArtHooks.java`

**Interfaces:**
- Consumes: `find_libart_symbol` from `art_symbols.hpp`.
- Produces: `bool deoptimize_all_loaded_classes();` and
  `public static final String DEOPTIMIZE_EVERYTHING_PROPERTY = "arthooks.deoptimize_everything";`

- [ ] **Step 1: Write the failing test**

Add to `HookSelfTest.java`. The point is a class ART has *already* linked, so it must be loaded and
used before ArtHooks would have helped it — which in this suite means a boot-classpath class the
framework has certainly touched.

```java
    private static boolean already_linked_class_is_still_hookable() {
        if (!Boolean.parseBoolean(System.getProperty("arthooks.deoptimize_everything"))) {
            return pass("skipped: deoptimize_everything is off");
        }
        // java.util.Random is loaded and used long before this app's code runs.
        if (!hook(declared_method(java.util.Random.class, "nextInt"),
                declared_method(HookSelfTest.class, "next_int_replacement", with_thiz()))) {
            return false;
        }
        int value = new java.util.Random().nextInt();
        if (value != 4242) {
            return fail("an already-linked class's method -> " + value + ", expected 4242");
        }
        return pass("a class ART had already linked is still hookable");
    }

    public static int next_int_replacement(Object thiz) {
        return 4242;
    }
```

- [ ] **Step 2: Run it to verify it fails**

```bash
ADB_PROPS=1 ./tools/run-aot-selftest.sh speed
```

The property has to reach the app before `ArtHooks` loads, which `run-aot-selftest.sh` does not do.
Add the same `--es`-to-`System.setProperty` bridge `MainActivity.run_benchmark()` already uses, for
the non-benchmark path, and pass `--es arthooks_deoptimize_everything true`. Expected: `FAIL: an
already-linked class's method -> <some random int>`.

- [ ] **Step 3: Recover the offset, and refuse to guess**

In `deoptimize.cpp`. Add to the constants:

```cpp
// art::instrumentation::Instrumentation::UpdateEntrypointsForDebuggable().
constexpr const char *kUpdateEntrypointsForDebuggable =
        "_ZN3art15instrumentation15Instrumentation29UpdateEntrypointsForDebuggableEv";

using UpdateEntrypointsForDebuggableFn = void (*)(void *instrumentation);
```

and, in the anonymous namespace:

```cpp
/**
 * Recovers the offset of Runtime::instrumentation_ by reading Runtime::DeoptimizeBootImage().
 *
 * That function's only use of `this` before its first call is to form &instrumentation_, so on
 * arm64 its prologue contains exactly one `ADD Xd, X0, #imm` against the incoming Runtime*. Scanning
 * for it is how Frida does the same job, and it is the one thing in this library that is pattern
 * matching rather than measurement -- which is why every path to it is opt-in.
 *
 * Returns kUnknownOffset unless exactly one candidate is found before the first branch. There is
 * deliberately no fallback guess: a wrong offset means calling a C++ member function on a pointer
 * into the middle of Runtime, and the failure mode is a segfault rather than a bad return value.
 */
constexpr size_t kUnknownOffset = static_cast<size_t>(-1);

size_t find_instrumentation_offset() {
#if defined(__aarch64__)
    const uint32_t *code = reinterpret_cast<const uint32_t *>(
            find_libart_symbol(kDeoptimizeBootImage));
    if (code == nullptr) {
        return kUnknownOffset;
    }

    size_t found = 0;
    size_t offset = kUnknownOffset;
    for (int i = 0; i < 32; i++) {
        const uint32_t instruction = code[i];

        // B (0b000101xx) and BL (0b100101xx) end the prologue; anything after the first call may be
        // forming a pointer for some other purpose.
        if ((instruction & 0x7C000000u) == 0x14000000u) {
            break;
        }
        // RET.
        if ((instruction & 0xFFFFFC1Fu) == 0xD65F0000u) {
            break;
        }

        // ADD (immediate), 64-bit, shift 0: sf=1 op=0 S=0 0b100010 sh=0 imm12 Rn Rd.
        const bool is_add_immediate = (instruction & 0xFFC00000u) == 0x91000000u;
        const uint32_t rn = (instruction >> 5) & 0x1Fu;
        if (is_add_immediate && rn == 0) {
            found++;
            offset = (instruction >> 10) & 0xFFFu;
        }
        // MOV Xd, X0 is an alias for ORR Xd, XZR, X0 -- the offset-zero case. The mask fixes the
        // opcode, Rm=0, imm6=0 and Rn=31, leaving only Rd free.
        if ((instruction & 0xFFFFFFE0u) == 0xAA0003E0u) {
            found++;
            offset = 0;
        }
    }

    if (found != 1) {
        LOGW("found %zu candidates for Runtime::instrumentation_ (wanted exactly 1); "
             "not deoptimizing already-linked classes", found);
        return kUnknownOffset;
    }
    LOGI("Runtime::instrumentation_ looks like +%zu", offset);
    return offset;
#else
    LOGW("recovering Runtime::instrumentation_ is only implemented for arm64");
    return kUnknownOffset;
#endif
}
```

- [ ] **Step 4: Validate before trusting it**

A candidate that is wrong must be rejected *before* it is used for anything that walks a data
structure. `ReinitializeMethodsCode()` recomputes one method's entry point from scratch and writes
it back, which is the smallest observable thing an `Instrumentation*` can be asked to do.

Add to `art_method.hpp` and `art_method.cpp` a way to reach a probe whose entry point is predictable
— `flag_probe_b` is an instance method, so it never carries the resolution stub:

```cpp
// art_method.hpp
/** ArtHooks.flag_probe_b's ArtMethod, or nullptr. Resolved during init_art_method_access(). */
ArtMethod *aot_probe_method();
```

```cpp
// art_method.cpp -- add the static, set it in find_access_flags_offset() where `b` is resolved
ArtMethod *g_aot_probe = nullptr;

ArtMethod *aot_probe_method() {
    return g_aot_probe;
}
```

Then in `deoptimize.cpp`:

```cpp
// art::instrumentation::Instrumentation::ReinitializeMethodsCode(art::ArtMethod*).
constexpr const char *kReinitializeMethodsCode =
        "_ZN3art15instrumentation15Instrumentation23ReinitializeMethodsCodeEPNS_9ArtMethodE";

using ReinitializeMethodsCodeFn = void (*)(void *instrumentation, ArtMethod *method);

/**
 * Confirms a candidate Instrumentation* by using it on a method whose answer we can predict.
 *
 * The runtime is Java-debuggable by this point, so CanUseAotCode() is false and a recomputed entry
 * point must be one of ART's own stubs -- mapped, and inside libart rather than an oat file. A
 * candidate that produces anything else is wrong, and using it further would be the crash this
 * check exists to avoid. The probe is restored afterwards, because nothing else may have asked for
 * it to change.
 */
bool instrumentation_pointer_works(void *instrumentation) {
    ArtMethod *probe = aot_probe_method();
    ReinitializeMethodsCodeFn reinitialize =
            reinterpret_cast<ReinitializeMethodsCodeFn>(
                    find_libart_symbol(kReinitializeMethodsCode));
    if (probe == nullptr || reinitialize == nullptr) {
        return false;
    }

    void *before = get_entry_point(probe);
    reinitialize(instrumentation, probe);
    void *after = get_entry_point(probe);

    const bool plausible = after != nullptr && !is_aot_code(after);
    if (!plausible) {
        LOGW("candidate Instrumentation* produced entry point %p for the probe; rejecting it",
             after);
    }
    if (after != before) {
        set_entry_point(probe, before);
    }
    return plausible;
}
```

**Note the ordering requirement:** `instrumentation_pointer_works()` must run *after*
`mark_java_debuggable()`, or `CanUseAotCode()` is still true and an AOT answer would be a false
rejection.

- [ ] **Step 5: Wire it up behind the property**

`deoptimize_all_loaded_classes()` resolves `kUpdateEntrypointsForDebuggable`, computes and validates
the offset, and calls it inside a `ScopedSuspendAll` exactly as `deoptimize_boot_image()` does.
Return false, changing nothing, if any step is unsure.

In `ArtHooks.java`, add the property and call it from the static initializer after `disable_aot`.

- [ ] **Step 6: Run the tests**

```bash
./tools/run-selftest.sh
./tools/run-aot-selftest.sh speed
```

Expected: PASS with the property off (the new case reports "skipped"), and PASS with it on.

- [ ] **Step 7: Document it as the exception it is**

`README.md` and `CLAUDE.md`: state plainly that this path pattern-matches instructions rather than
measuring, that it is off by default, that it is arm64-only, and that a failed validation disables
it rather than guessing.

- [ ] **Step 8: Commit**

```bash
git add arthooks/src/main/cpp/deoptimize.hpp arthooks/src/main/cpp/deoptimize.cpp \
        arthooks/src/main/cpp/art_method.hpp arthooks/src/main/cpp/art_method.cpp \
        arthooks/src/main/java/com/arthooks/ArtHooks.java \
        app/src/main/java/com/example/arthooks/HookSelfTest.java README.md CLAUDE.md
git commit -m "feat: optional full deoptimization for already-linked classes"
```

---

## Not in this plan, and why

- **Inline-hooking ART's quick stubs, the way Frida redirects.** It needs an instruction relocator
  per ABI and access to non-exported symbols, and it mutates libart globally. Out of scope and out
  of philosophy; the spec's §3 records what it would buy.
- **Making the replacement a native method to fix `synchronized`.** Frida gets correct monitor
  semantics free because its replacement *is* native. ArtHooks' replacements are user-written Java,
  so the same trick is not available without generating a dex at runtime.
- **`@NeverInline` on user code.** `dalvik.annotation.optimization.NeverInline` is matched by
  descriptor string in the dex, so an app can declare its own copy and dex2oat will honour it. That
  is a note for the README, not a library change — ArtHooks cannot annotate someone else's APK.
