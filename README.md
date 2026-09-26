# ArtHooks

A minimal ART (Android Runtime) method-hooking library — the same idea as Xposed, YAHFA or SandHook,
stripped to the essentials. It redirects calls to a Java method into one of your own by overwriting
the target `art::ArtMethod`'s entry point.

Methods, constructors, static and instance targets, `private`, `final`, interface-dispatched and
`native` (JNI) methods all hook the same way, including targets on the boot classpath.

The repository is two Gradle modules: `:arthooks` (the library, published as an AAR) and `:app` (a
demo that also carries the test suite).

## Status

Verified end to end on a Pixel 9a running Android 16 (API 36), arm64-v8a, with 27 self-test checks
covering return shapes, dispatch kinds, argument lists that spill to the stack, JIT survival,
concurrent installation and static targets whose class is not yet visibly initialized. All 27 pass
in a debug build, all 27 in a **release** build compiled `verify`, and all 27 in a release build
compiled `speed` — the harshest case, where dex2oat has compiled everything. See
[AOT](#aot-and-why-hooks-used-to-break-in-release-builds).

Four ABIs are built. Only arm64-v8a has been exercised on hardware; the armeabi-v7a, x86_64 and x86
trampoline encodings were verified by disassembling the emitted bytes against the NDK assembler.

## Requirements

| | |
|---|---|
| Android | `minSdk` 33, `compileSdk`/`targetSdk` 36 |
| ABIs | arm64-v8a, armeabi-v7a, x86_64, x86 |
| Build | Gradle 9.4.1, AGP 9.2.1, JDK toolchain 26, CMake 3.22.1, NDK 28.2.13676358 |

`ArtMethod` layout is measured at runtime rather than assumed, so the library is not pinned to a
particular Android release — but it has only been run against API 36. See
[Limitations](#limitations).

## The two rules

**A replacement must be `static`, and takes the receiver as an explicit leading `Object thiz`.**

The hook redirects the call without touching the arguments already in place, so the replacement's
parameter list has to match the target's *as the callee sees it* — and an instance method's first
argument is its receiver.

| Target | Replacement |
|---|---|
| instance `int f(String)` | `static int r(Object thiz, String s)` |
| static `int f(String)` | `static int r(String s)` |
| constructor `T(int)` | `static void r(Object thiz, int i)` |

A constructor is the instance-method case: the object is already allocated when `<init>` runs, so it
arrives as `thiz`. Nothing initialises it unless the replacement calls the backup.

**A backup must be `native`**, with the replacement's signature and no body.

Only the backup's entry point is swapped, which assumes every call to it goes through that field. A
backup with a Java body does not: it is small and returns nothing interesting, so the compiler
inlines it into the replacement, and the call site ends up holding a copy of the backup's own body.
dex2oat does that at install time, before any of this runs, so the swap is invisible and the
replacement silently gets the backup's own answer instead of the original's — no error, no log, just
the wrong value from then on. A native method has no body to copy.

```java
static native boolean gate_backup(Object thiz);
```

A non-native backup is refused rather than installed, for a target that is not static. A `static`
target's backup does not need this — a static backup is settled off the quick resolution stub before
its entry point is captured (see [below](#static-targets-and-the-resolution-stub)), so calling
through it runs the original body rather than re-entering the hook.

## AOT, and why hooks used to break in release builds

Overwriting an entry point only redirects calls that *go through* the entry point. **dex2oat
inlines**: under a `speed` or `speed-profile` filter it copies a small method's body into every
caller it compiles, and a caller holding a copy never loads the callee's entry point at all. The
hook installs, reports success, and silently never fires — which is why an app could work when it
was installed and start misbehaving hours later, once background dexopt had compiled it.

ArtHooks handles this for you. When the library loads it tells ART the runtime is Java-debuggable,
which makes `Instrumentation::CanUseAotCode()` answer no — ART's own words are *"for simplicity, we
never use AOT code for debuggable"* — so every class it links afterwards runs nterp and dispatches
through the `ArtMethod` again. It also deoptimizes the boot image, so framework code that was
already linked stops using its AOT bodies too. Nothing else changes: this is ART's internal state,
not `ApplicationInfo.FLAG_DEBUGGABLE`, and it is invisible to the app and to a debugger.

Three things to know:

- **It only covers classes ART has not linked yet.** Touch `com.arthooks.ArtHooks` as early as you
  can — `Application.attachBaseContext` is the usual place — so this runs before the code you intend
  to hook, and that code's callers, are first loaded. Hooking a target that is still on AOT code
  logs a warning under the `ArtHooks` tag naming exactly that.
- **It costs performance, and the cost is all in cold code.** Measured on a Pixel 9a with
  `tools/run-benchmark.sh` (minimum of 3 interleaved passes, so DVFS and thermal drift cannot pick a
  winner):

  | | first run of a code path | once the JIT has caught up |
  |---|---|---|
  | tight loop over small method calls | **203x** slower | 1.0x |
  | recursive `fib`, float matmul | 21–33x slower | 0.9–1.0x |
  | array/loop work (sieve, quicksort, bit twiddling) | 2.5–3.8x slower | 1.0–1.6x |
  | `StringBuilder`, `HashMap` (boot classpath) | 3.4–6.2x slower | 1.0x |

  Steady-state throughput is a wash — the JIT still inlines, so hot code ends up where it started.
  What you pay for is every code path's *first* execution, which lands in nterp. `vmSafeMode` alone
  is far gentler on that first run (9.7x rather than 203x on the same loop) because it leaves the
  runtime non-debuggable; `Jit::TryPatternMatch`, ART's fast path for trivial getters and setters,
  is explicitly gated on `!Runtime::Current()->IsJavaDebuggable()`, and that is one of the things
  `disable_aot()` gives up.

  Cold start on the demo app: 89 ms baseline, 95 ms with `vmSafeMode`, 124 ms with `disable_aot()`.

  To keep AOT code and take the inlining risk, set the property *before* anything touches the class:

  ```java
  System.setProperty(ArtHooks.KEEP_AOT_PROPERTY, "true");   // "arthooks.keep_aot"
  ```

  `ArtHooks.is_aot_disabled()` reports whether it actually happened.
- **It needs libart to export three symbols** (`art::Runtime::instance_`,
  `SetRuntimeDebugState`, `DeoptimizeBootImage`). They are resolved by name out of the already-mapped
  libart.so; a platform that renames or hides one makes `disable_aot()` return false and change
  nothing, rather than guessing at a struct offset.

If you control the manifest of the app being hooked — repackaging an APK, say — `android:vmSafeMode`
is a belt-and-braces alternative that needs no ART internals at all. It makes the package manager
refuse to AOT-compile the app in the first place, so there is never any inlined code to work around,
even under a forced `cmd package compile -m speed -f`:

```xml
<application android:vmSafeMode="true" ... >
```

It does not help with the boot classpath, and it costs the same AOT performance app-wide, so it is a
substitute for the runtime fix rather than an addition to it.

The JIT is a separate, already-handled case: AOSP's `HInliner` refuses to inline a method that is not
compilable, and ArtHooks sets `kAccCompileDontBother` on every target, so the JIT will not inline a
hooked method into a hot caller either.

## Usage

### Redirect a method, and call through to the original

```java
public final class MyHook {

    public static void on_click(Object thiz, View view) {
        Log.i("MyHook", "intercepted a click on " + thiz.getClass().getName());
        on_click_backup(thiz, view);           // runs the original body
    }

    /** Backup slot. Native: a body here would be inlined into the hook above and never run. */
    public static native void on_click_backup(Object thiz, View view);

    static boolean install() throws NoSuchMethodException {
        return ArtHooks.hook_function(
                MainActivity.class.getDeclaredMethod("on_click", View.class),
                MyHook.class.getDeclaredMethod("on_click", Object.class, View.class),
                MyHook.class.getDeclaredMethod("on_click_backup", Object.class, View.class));
    }
}
```

The backup keeps its own Java identity — only its entry point changes — so you call it by name like
any other method. Drop the third argument if you don't need the original.

### Find a target by signature

`find_function` resolves a target from a JNI descriptor, which is how you name one overload out of
several without assembling `Class` objects:

```java
Executable target = ArtHooks.find_function(
        StringTokenizer.class, "countTokens", "()I");

ArtHooks.hook_function(
        target, MyHook.class.getDeclaredMethod("count_tokens", Object.class));
```

`"<init>"` with a `V` return type gives you a constructor:

```java
ArtHooks.hook_function(
        ArtHooks.find_function(Session.class, "<init>", "(Ljava/lang/String;)V"),
        MyHook.class.getDeclaredMethod("session_init", Object.class, String.class),
        MyHook.class.getDeclaredMethod("session_init_backup", Object.class, String.class));
```

## API

All of `com.arthooks.ArtHooks`:

| Method | Returns | |
|---|---|---|
| `is_available()` | `boolean` | Whether the native side came up. When false, every hook fails. |
| `find_function(Class<?> owner, String name, String signature)` | `Executable` or `null` | Resolves a method or constructor by JNI descriptor. Searches superclasses, like JNI's own lookup. |
| `hook_function(Executable original, Executable replacement)` | `boolean` | Redirects `original` to `replacement`. |
| `hook_function(Executable original, Executable replacement, Executable backup)` | `boolean` | As above, and wires `backup` to the original body. |
| `is_aot_disabled()` | `boolean` | Whether ART was told to stop running AOT code. See [AOT](#aot-and-why-hooks-used-to-break-in-release-builds). |
| `disable_aot()` | `boolean` | Does that. Called automatically when the class loads; calling it again is a no-op. |
| `KEEP_AOT_PROPERTY` | `String` | `"arthooks.keep_aot"` — set it to `true` before touching this class to opt out. |

Failures return `false`/`null` and log the reason under the `ArtHooks` tag rather than throwing.

Hooking forces the target's and the replacement's declaring classes to initialize, because ART
rewrites every method's entry point when it runs a class initializer — which would otherwise
silently drop the hook. Expect `<clinit>` to run earlier than it normally would.

## How it works

1. `ArtHooks`'s static initializer loads `libarthooks.so` and measures `sizeof(art::ArtMethod)` on
   the running platform, deriving the offset of the entry point from it.
2. It then tells ART to stop using ahead-of-time compiled code, so that calls actually reach the
   entry point instead of running a copy dex2oat inlined into the caller. This has to happen before
   any hook is installed, since it rewrites entry points — see
   [AOT](#aot-and-why-hooks-used-to-break-in-release-builds).
3. Hooking overwrites **only** `entry_point_from_quick_compiled_code_` on the target, with a
   generated trampoline — three instructions that load the replacement's `ArtMethod*` into the
   register ART's quick calling convention reserves for it, then tail-jump through that method's
   entry point.
4. The backup is a trampoline that names the target's real `ArtMethod` but jumps to the entry point
   captured from it before the hook went in.

The trampoline is the part that isn't obvious. Compiled code, nterp and the interpreter bridge all
read the method they are executing — its declaring class, dex cache, code item — out of that
register. Copying the replacement's entry point onto the target leaves the *target's* `ArtMethod*`
there, so the replacement's constants resolve against the wrong class; that only appears to work
while both classes share a dex file. Swapping the register first makes the callee see itself.

Because it loads the entry point *from* the `ArtMethod` on every call rather than baking it in, the
hook keeps working when ART later replaces that entry point — JIT compilation, class-init
resolution, deoptimization. And because everything else in the target `ArtMethod` is untouched, its
identity survives: reflection, vtable and interface dispatch still see the method they expect.

`CLAUDE.md` has the details, including what breaks if you copy `ArtMethod` fields instead.

## Using it in another project

The consumer needs `minSdk` 33 or higher, and nothing else — the AAR carries all four ABIs and has
no transitive dependencies.

Releases are served by [JitPack](https://jitpack.io/#Schwartzblat/ArtHooks), which builds them from
a git tag. Add the repository, then the dependency:

```groovy
// settings.gradle
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url 'https://jitpack.io' }
    }
}
```

```groovy
// app/build.gradle
dependencies {
    implementation 'com.github.Schwartzblat.ArtHooks:arthooks:1.0.5'
}
```

The group is the *repository* and the artifact is the *module*, because this is a multi-module
build — `com.github.Schwartzblat:ArtHooks:1.0.5`, the single-module form, will not resolve.

Any git tag works as a version, and so does `main-SNAPSHOT` for the tip of the branch. The first
request for a given tag makes JitPack build it, which takes a few minutes and can fail; the log is
at `https://jitpack.io/com/github/Schwartzblat/ArtHooks/<tag>/build.log`.

> **Licensing.** ArtHooks is GPL-3.0. Linking it into an app makes that app a derivative work, so
> you must release your app's source under a GPL-compatible license. If you cannot do that, you
> cannot use this library.

### Maven Local

Publish once from this repo, then resolve it like any other artifact:

```bash
./gradlew :arthooks:publishToMavenLocal          # -> ~/.m2/repository/com/arthooks/
```

```groovy
// settings.gradle -- mavenLocal() must come first, it is not a default repository
dependencyResolutionManagement {
    repositories {
        mavenLocal()
        google()
        mavenCentral()
    }
}
```

```groovy
// app/build.gradle
dependencies {
    implementation 'com.arthooks:arthooks:1.0.5'
}
```

Republish after every change — Gradle caches the resolved artifact, so bump `arthooksVersion` or run
the consumer with `--refresh-dependencies` if a rebuild appears to do nothing.

### A composite build, if you are changing the library too

Point the consumer's `settings.gradle` at this checkout. Gradle substitutes the coordinate for the
local project, so edits to the C++ or Java are picked up on the next build with no publish step:

```groovy
// settings.gradle
includeBuild("/path/to/ArtHooks")
```

```groovy
// app/build.gradle -- no version; the included build supplies it
dependencies {
    implementation 'com.arthooks:arthooks'
}
```

### Just the file

```bash
./gradlew :arthooks:assembleRelease              # -> arthooks/build/outputs/aar/
```

Copy `arthooks-release.aar` into the consumer's `app/libs/` and:

```groovy
dependencies {
    implementation files('libs/arthooks-release.aar')
}
```

This drops the POM, so nothing records the version — fine for a quick trial, worse for anything you
have to reproduce later.

### Cutting a release

Push a **bare semver tag** — no `v` prefix:

```bash
git tag 1.0.5 && git push origin 1.0.5
```

The prefix matters here in a way it usually does not: **JitPack serves a tag under its literal
name**, so tag `v1.0.5` would make the dependency `...:arthooks:v1.0.5`. The workflow still matches
`v*` tags so an old-style one releases rather than silently doing nothing, and it strips the `v` from
the version inside the artifacts — but the JitPack coordinate keeps whatever you typed.

That push runs the `release` job, which after the build and the emulator self-test pass will:

1. Build the AAR, POM and sources jar under the exact coordinate JitPack serves, failing the release
   if the AAR or POM is missing rather than publishing an empty one.
2. Attach them, plus `SHA256SUMS.txt`, to the GitHub release for that tag — creating the release if
   it does not already exist.
3. Ask JitPack to build the tag, so the Maven coordinate resolves straight away instead of making
   the first consumer sit through a multi-minute build. This step cannot fail the release: the
   GitHub assets are already published by then, and a consumer's own request would trigger the
   build anyway.

`jitpack.yml` controls JitPack's side of that build: it selects the JVM that launches the Gradle
wrapper and pre-installs the NDK and CMake, without which the native half will not compile.
`arthooks/build.gradle` reads `-Pgroup` and `-Pversion`, which is how JitPack injects the coordinate
it intends to serve.

### R8

`consumer-rules.pro` ships inside the AAR, so R8 will not rename the native declarations or drop the
layout probes. It cannot protect *your* hooks: a hooked method, its replacement and its backup are
located by exact name and signature, so keep them yourself.

```proguard
-keep class com.example.myapp.MyHook { *; }
-keepclassmembers class com.example.myapp.TargetClass { *; }
```

## Building and running

```bash
./gradlew assembleDebug     # APK -> app/build/outputs/apk/debug/
./gradlew installDebug      # requires a connected device

adb shell am start -n com.arthooks/com.example.arthooks.MainActivity
```

The demo redirects `MainActivity.on_click` to `HookExample.hook_with`, which calls through to the
original — tap the button and both toasts fire.

Tests run from `MainActivity` and report to logcat; there is no instrumentation-test harness. The
suite takes about six seconds to finish, most of it deliberately waiting for the JIT.

```bash
./tools/run-selftest.sh     # installs, runs, and exits non-zero unless every check passed

adb logcat -s HookSelfTest  # 23 checks, then "PASS: all checks passed"
adb logcat -s ArtHooks      # native log tag
```

`tools/run-aot-selftest.sh` runs the same suite against a non-debuggable build compiled `speed` or
`verify`, which is the only configuration that can catch a hook being inlined away — a debuggable
APK is never inlined, so the debug self-test is structurally blind to it.
`tools/run-benchmark.sh` measures what dropping AOT code costs, across all four configurations.

`tools/check-jni-symbols.sh` verifies that every native method declared in `ArtHooks.java` is
actually exported by `libarthooks.so`, for every ABI. JNI binds by mangled symbol name and a rename
fails silently until the method is called, so this is worth running after any signature change. CI
runs both.

## Limitations

- **Hooking a method that is already hot is not fully safe.** The hook lives in the target's entry
  point, and so does the JIT's output. If ART has already queued the target for compilation, a
  compile that finishes after the hook is installed overwrites it — silently, and permanently, so the
  method simply runs its original body again. ArtHooks sets `kAccCompileDontBother` on the target
  before writing the entry point, which stops ART compiling it and closes the common case, but a
  compilation already in flight can still land. Hook during startup, before the methods you are
  hooking have been called thousands of times.
- **No unhook.** Trampolines and snapshots live for the lifetime of the process. Hooking the same
  method twice chains, second hook outermost.
- **A `synchronized` target's monitor is not taken.** A `synchronized` *method* has no
  `monitor-enter` in its body — the lock is acquired by the callee's own entry sequence, driven by
  `ACC_SYNCHRONIZED` on the method being entered. The hook redirects before any of that runs, into a
  replacement that does not carry the flag, so the lock is silently never taken and callers relying
  on the target for mutual exclusion race. The library logs a warning at hook time.

  **Calling through the backup does restore it.** The backup jumps to the snapshot's pre-hook entry
  point, and the snapshot still carries `ACC_SYNCHRONIZED`, so ART's entry sequence locks the
  receiver exactly as it would have — verified on device. The unprotected window is only the
  replacement's own code, outside the call-through.

  To close that window, lock explicitly. Marking the *replacement* `synchronized` is only correct for
  instance targets:

  | Target | Correct in the replacement |
  |---|---|
  | instance `synchronized void f()` | `synchronized (thiz) { ... }` |
  | static `synchronized void f()` | `synchronized (Target.class) { ... }` |

  A `static synchronized` method locks its *declaring class*, so a `static synchronized` replacement
  would lock the replacement's own class — the wrong object, and no error.
- **A target whose class ART linked before ArtHooks loaded may still have been inlined away.**
  Dropping AOT code only governs classes linked afterwards, so a class that was already in use keeps
  the bodies dex2oat gave it, and a caller that inlined the target has no call left to redirect.
  This is now *detected* — hooking a target that is still on AOT code logs a warning naming it — but
  it cannot be repaired after the fact. Load `ArtHooks` as early as you can. See
  [AOT](#aot-and-why-hooks-used-to-break-in-release-builds).
- **Boot-classpath targets work from app call sites, and from inside the framework only once the
  boot image has been deoptimized** — which `disable_aot()` does, but again only for frames entered
  afterwards.
- **`find_function` searches superclasses**, unlike `getDeclaredMethod`. An inherited method
  resolves to the superclass's `ArtMethod`, so hooking it affects every subclass.
- **The `ArtMethod` mirror in `art_method.hpp` is hand-maintained.** Nothing indexes it — the layout
  is measured at runtime — but a mismatch against the measured size is logged as a warning and means
  the struct no longer describes that platform.

## Static targets and the resolution stub

A `static`, non-constructor method that dex2oat compiled (or that is `native`) does not start on its
real entry point. `Instrumentation::GetInitialEntrypoint` gives every method where
`NeedsClinitCheckBeforeCall()` holds — exactly `IsStatic() && !IsConstructor()` — the quick
**resolution stub**, and leaves it there until `ClassLinker::FixupStaticTrampolines` runs, which
waits for the declaring class to become *visibly* initialized. On arm64 that transition is batched
behind a `VisiblyInitializedCallback`, so a class can be initialized and running for a long time with
its static methods still parked on the stub.

That stub is a problem for backups. A backup captured while the target is on it re-dispatches through
the stub, which re-reads the (now hooked) entry point and lands back in the replacement — the backup
recurses until the stack overflows. That one is real: before this was handled, a release build
compiled `speed` failed four self-test cases with a `StackOverflowError`. A hook *written* while the
target is on the stub also sits under a `FixupStaticTrampolines` that has not run yet; reading AOSP
suggests the fixup could overwrite it, but that was never observed — a self-test hook written onto
the stub survived a forced fixup — so it is a question settling first avoids, not a demonstrated
failure.

ArtHooks settles the target first. It **measures the stub's address at startup** — the same
measure-don't-assume move used for `sizeof(ArtMethod)` — from `ArtHooks.ResolutionStubProbe`, a class
that is loaded but never initialized so its `static native` method sits on the stub by construction;
the address is validated (non-null, inside `libart.so`, and different from a resolved native's entry)
before it is trusted. When a static target is found on that stub, `hook_function` nudges it off with
`Class.forName(name, true, loader)` — which trips `ClassLinker::EnsureInitialized`'s per-thread
counter and makes ART flush the visible-initialization batch — until its entry point leaves the stub,
then captures the real body. A static target that will not leave the stub is **refused** rather than
hooked while its fixup is still pending. If the stub's address cannot be measured, `hook_function`
instead watches the target's own entry point, treats "never moved" as already settled, and hooks it
with a warning that a backup may recurse. This narrows one residual case
rather than closing it: a static target whose class is not visibly initialized but which is *not* on
the stub (no AOT code, e.g. a debug build, where it starts on the interpreter bridge) is not settled —
but that case does not recurse, because the interpreter bridge runs the original body directly.

Instance-method and constructor targets never reach this branch, so they are unaffected.

## Layout

```
arthooks/                                      # the library, published as an AAR
  src/main/java/com/arthooks/ArtHooks.java     #   the public API
  src/main/cpp/arthooks.cpp                    #   hook_function(), JNI entry points
  src/main/cpp/trampoline.{hpp,cpp}            #   per-ABI thunk codegen
  src/main/cpp/art_method.{hpp,cpp}            #   ArtMethod mirror, layout probing, accessors
  src/main/cpp/class_init.{hpp,cpp}            #   forcing <clinit> before a hook is installed
  src/main/cpp/deoptimize.{hpp,cpp}            #   stopping ART running AOT code; AOT detection
  src/main/cpp/art_symbols.{hpp,cpp}           #   resolving libart's exported symbols by name
  consumer-rules.pro                           #   R8 rules applied to consumers

app/                                           # demo app and self-tests
tools/                                         # self-test runner, JNI symbol check
```

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).

This is a copyleft license, and a library linked into an application makes that application a
derivative work. An app that ships ArtHooks must therefore be distributed under a GPL-compatible
license, with source available to its users. That rules out most closed-source applications.
