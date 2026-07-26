# ArtHooks

A minimal ART (Android Runtime) method-hooking library — the same idea as Xposed, YAHFA or SandHook,
stripped to the essentials. It redirects calls to a Java method into one of your own by overwriting
the target `art::ArtMethod`'s entry point.

Methods, constructors, static and instance targets, `private`, `final`, interface-dispatched and
`native` (JNI) methods all hook the same way, including targets on the boot classpath.

The repository is two Gradle modules: `:arthooks` (the library, published as an AAR) and `:app` (a
demo that also carries the test suite).

## Status

Verified end to end on a Pixel 9a running Android 16 (API 36), arm64-v8a, with 16 self-test checks
covering return shapes, dispatch kinds, JIT survival and concurrent installation.

Four ABIs are built. Only arm64-v8a has been exercised on hardware; the armeabi-v7a, x86_64 and x86
trampoline encodings were verified by disassembling the emitted bytes against the NDK assembler.

## Requirements

| | |
|---|---|
| Android | `minSdk` 33, `compileSdk`/`targetSdk` 36 |
| ABIs | arm64-v8a, armeabi-v7a, x86_64, x86 |
| Build | Gradle 9.4.1, AGP 9.2.1, JDK toolchain 26, CMake 3.22.1, NDK (installed default) |

`ArtMethod` layout is measured at runtime rather than assumed, so the library is not pinned to a
particular Android release — but it has only been run against API 36. See
[Limitations](#limitations).

## The one rule

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

## Usage

### Redirect a method, and call through to the original

```java
public final class MyHook {

    public static void on_click(Object thiz, View view) {
        Log.i("MyHook", "intercepted a click on " + thiz.getClass().getName());
        on_click_backup(thiz, view);           // runs the original body
    }

    /** Backup slot. Once hooked, calling this runs MainActivity.on_click instead of this body. */
    public static void on_click_backup(Object thiz, View view) {
    }

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

Failures return `false`/`null` and log the reason under the `ArtHooks` tag rather than throwing.

Hooking forces the target's and the replacement's declaring classes to initialize, because ART
rewrites every method's entry point when it runs a class initializer — which would otherwise
silently drop the hook. Expect `<clinit>` to run earlier than it normally would.

## How it works

1. `ArtHooks`'s static initializer loads `libarthooks.so` and measures `sizeof(art::ArtMethod)` on
   the running platform, deriving the offset of the entry point from it.
2. Hooking overwrites **only** `entry_point_from_quick_compiled_code_` on the target, with a
   generated trampoline — three instructions that load the replacement's `ArtMethod*` into the
   register ART's quick calling convention reserves for it, then tail-jump through that method's
   entry point.
3. The backup is a trampoline for a snapshot of the target's `ArtMethod` taken before it was hooked.

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

## Installing

The library publishes as an AAR to GitHub Packages on tagged builds:

```groovy
dependencies {
    implementation 'com.arthooks:arthooks:0.1.0'
}
```

`consumer-rules.pro` ships with it, so R8 will not rename the native declarations or drop the layout
probes. It cannot protect *your* hooks — a hooked method, its replacement and its backup are located
by exact name and signature, so keep them yourself.

Or build it locally:

```bash
./gradlew :arthooks:assembleRelease            # AAR -> arthooks/build/outputs/aar/
./gradlew :arthooks:publishToMavenLocal
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

adb logcat -s HookSelfTest  # 16 checks, then "PASS: all checks passed"
adb logcat -s ArtHooks      # native log tag
```

`tools/check-jni-symbols.sh` verifies that every native method declared in `ArtHooks.java` is
actually exported by `libarthooks.so`, for every ABI. JNI binds by mangled symbol name and a rename
fails silently until the method is called, so this is worth running after any signature change. CI
runs both.

## Limitations

- **No unhook.** Trampolines and snapshots live for the lifetime of the process. Hooking the same
  method twice chains, second hook outermost.
- **A `synchronized` target's monitor is not taken.** A `synchronized` *method* has no
  `monitor-enter` in its body — the lock is acquired by the callee's own entry sequence, driven by
  `ACC_SYNCHRONIZED` on the method being entered. The hook redirects before any of that runs, into a
  replacement that does not carry the flag, so the lock is silently never taken and callers relying
  on the target for mutual exclusion race. The library logs a warning at hook time. To restore it,
  lock explicitly in the replacement — and note that marking the *replacement* `synchronized` is only
  correct for instance targets:

  | Target | Correct in the replacement |
  |---|---|
  | instance `synchronized void f()` | `synchronized (thiz) { ... }` |
  | static `synchronized void f()` | `synchronized (Target.class) { ... }` |

  A `static synchronized` method locks its *declaring class*, so a `static synchronized` replacement
  would lock the replacement's own class — the wrong object, and no error.
- **Boot-classpath targets work from app call sites, but not necessarily from inside the
  framework.** AOT code can call a known-address callee directly instead of loading its entry point,
  and nothing here detects that.
- **`find_function` searches superclasses**, unlike `getDeclaredMethod`. An inherited method
  resolves to the superclass's `ArtMethod`, so hooking it affects every subclass.
- **The `ArtMethod` mirror in `art_method.hpp` is hand-maintained.** Nothing indexes it — the layout
  is measured at runtime — but a mismatch against the measured size is logged as a warning and means
  the struct no longer describes that platform.
- **The backup's snapshot is not visited by the GC.** It holds a `GcRoot` to the declaring class,
  which is safe only because ART allocates `mirror::Class` as non-movable.

## Layout

```
arthooks/                                      # the library, published as an AAR
  src/main/java/com/arthooks/ArtHooks.java     #   the public API
  src/main/cpp/arthooks.cpp                    #   hook_function(), JNI entry points
  src/main/cpp/trampoline.{hpp,cpp}            #   per-ABI thunk codegen
  src/main/cpp/art_method.{hpp,cpp}            #   ArtMethod mirror, layout probing, accessors
  src/main/cpp/class_init.{hpp,cpp}            #   forcing <clinit> before a hook is installed
  consumer-rules.pro                           #   R8 rules applied to consumers

app/                                           # demo app and self-tests
tools/                                         # self-test runner, JNI symbol check
```
