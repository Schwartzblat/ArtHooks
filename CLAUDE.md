# ArtHooks

A minimal ART (Android Runtime) method-hooking library — the same idea as Xposed/YAHFA/SandHook,
stripped to the essentials. A hook is installed by overwriting an `art::ArtMethod`'s entry point so
calls to one Java method land in another. Two modules: `:arthooks` is the library (published as an
AAR), `:app` is the demo and the test suite.

Both JNI entry points delegate to one shared `hook_function()` helper in
`arthooks/src/main/cpp/arthooks.cpp`. Tests run from `MainActivity` and report to logcat; there is no
instrumentation-test harness, so "run the tests" means launch the app and read `HookSelfTest`.

## Layout

```
arthooks/src/main/java/com/arthooks/ArtHooks.java  # public API: native decls + library loading
arthooks/src/main/cpp/arthooks.cpp                 # hook_function(), JNI entry points, JNI_OnLoad
arthooks/src/main/cpp/trampoline.{hpp,cpp}         # per-ABI thunk codegen + executable page
arthooks/src/main/cpp/art_method.{hpp,cpp}         # ArtMethod struct mirror, layout probing, accessors
arthooks/src/main/cpp/class_init.{hpp,cpp}         # forcing <clinit> before a hook is installed
arthooks/src/main/cpp/log.hpp                      # LOGD/LOGI/... macros
arthooks/consumer-rules.pro                        # R8 rules shipped to consumers
app/src/main/java/com/example/arthooks/            # demo app + tests, see below
app/src/main/cpp/selftest.cpp                      # demo-only: a JNI method for the tests to hook
tools/                                             # self-test runner, JNI symbol check
.github/workflows/build.yml                        # build + symbol check + emulator self-test
jitpack.yml                                        # how JitPack builds a tagged release
LICENSE                                            # GPL-3.0
```

Each translation unit owns its own state as file-local statics — there is no shared globals header.
`art_method.cpp` holds the SDK level, the `artMethod` field ID and the measured layout;
`class_init.cpp` holds the cached `java.lang.Class` members; `arthooks.cpp` holds the initialized
flag.

Demo and tests, all under `com.example.arthooks`:

```
MainActivity, HookExample   # the visible demo: android:onClick redirected, backup calls through
HookSelfTest                # orchestrator; JIT-survival and class-init checks
Checks                      # pass/fail reporting + unchecked reflection lookups used by the cases
SignatureCases              # one hook per return shape + a stack-spilling argument list
DispatchCases               # constructor, static+backup, private/final, interface, JNI, synchronized
RuntimeCases                # boot-classpath target, chained hooks, install under concurrent calls
LookupCases                 # find_function: each kind, overload picking, misses, find-then-hook
```

## Build

```bash
./gradlew assembleDebug     # APK -> app/build/outputs/apk/debug/
./gradlew installDebug      # requires a connected device
adb logcat -s ArtHooks      # native log tag; HookExample logs under "HookExample"
adb logcat -s HookSelfTest  # self-test verdict; takes ~6s after launch to finish

./tools/run-selftest.sh     # installs, runs, exits non-zero unless every check passed
./tools/check-jni-symbols.sh  # every declared native has a matching exported symbol, per ABI
./gradlew :arthooks:assembleRelease       # AAR -> arthooks/build/outputs/aar/
./gradlew :arthooks:publishToMavenLocal
```

`:app` has its own tiny CMake target (`libselftest.so`) so the JNI-hooking test has a native method
to aim at; it is loaded by `DispatchCases`, not by `ArtHooks`.

Gradle 9.4.1 / AGP 9.2.1, Java toolchain 26, configuration cache enabled. Native code is built by
CMake via `externalNativeBuild`; `ndkVersion` is pinned to 28.2.13676358 in both modules so
JitPack can pre-install it.
`local.properties` (`sdk.dir`) is untracked and machine-local.

## Publishing

Releases go through **JitPack**, which builds a git tag on its own machines the first time a
consumer asks for it. Consumers get `com.github.Schwartzblat.ArtHooks:arthooks:<tag>` -- group is
the repo, artifact is the module, because this is a multi-module build. Nothing is published from
here and there are no credentials anywhere; pushing the tag is the entire release.

Four things make that work, and each is easy to undo by accident:

- **`group` and `version` come from `providers.gradleProperty(...)`, not `findProperty(...)`.**
  JitPack drives the build with `-Pgroup=com.github.<user>.<repo> -Pversion=<tag>`, so the build
  script has to honour those. `findProperty('group')` cannot be used: `group` and `version` are also
  built-in project properties, pre-seeded with the root project's name and the string
  `"unspecified"`, so it never returns null and a `?:` fallback never fires -- the symptom is
  artifacts silently published under group `ArtHooks`. `providers.gradleProperty()` sees only what
  `-P` or `gradle.properties` actually supplied.
- **They are set on the project, not just inside the publication.** A consumer using
  `includeBuild()` substitutes on the project's own `group:name` and fails with
  `Could not find com.arthooks:arthooks:` if the coordinates live only in the publication.
- **`ndkVersion` is pinned in both modules.** `jitpack.yml` pre-installs exactly that NDK with
  `sdkmanager`; an unpinned default would leave the build machine guessing. It is AGP 9.2.1's own
  default, so pinning changed nothing locally.
- **`jitpack.yml` deliberately has no `install:` command.** JitPack's default already runs
  `publishToMavenLocal` with the right `-Pgroup`/`-Pversion`; overriding it would mean hard-coding
  the coordinate. The file only picks the launcher JVM and pre-installs the NDK and CMake.

The `jdk:` in `jitpack.yml` is only the JVM that launches the Gradle wrapper. Gradle then provisions
its own daemon JVM (26) from the URLs in `gradle/gradle-daemon-jvm.properties`, so the two do not
have to match.

The `release` CI job fires on any tag matching `[0-9]*` or `v[0-9]*`. It builds under the coordinate
JitPack serves, attaches the AAR, POM, sources jar and `SHA256SUMS.txt` to the GitHub release, then
asks JitPack to build the tag so the coordinate resolves without waiting for a consumer. It needs
`contents: write`; the JitPack request is `continue-on-error` because the assets are already
published by that point.

**Tag names are versions, literally.** JitPack serves tag `1.0.2` as version `1.0.2` and tag `v1.0.2`
as version `v1.0.2`, so releases use bare semver. `v*` stays matched only so an old-style tag
releases instead of silently matching nothing -- which is exactly what happened to tag `1.0.1`, back
when the trigger was `tags: ['v*']`: no workflow ran at all, and the release sat empty.

`android.builtInKotlin=false` in `gradle.properties` keeps AGP 9 from putting a `kotlin-stdlib`
dependency in the POM. There is no Kotlin here, and without it every consumer inherits the stdlib.

The project is GPL-3.0 (`LICENSE`), which the POM declares. That is copyleft: an app linking this
must ship its own source under a compatible licence.

## How the hook works

1. `ArtHooks`'s static initializer loads `libarthooks.so` and calls `init(Build.VERSION.SDK_INT)`,
   which caches the SDK level and the field ID for the hidden `java.lang.reflect.Executable.artMethod`,
   then measures the `ArtMethod` layout (see below). Everything else refuses to run unless it succeeds.
2. `get_art_method()` turns a `java.lang.reflect.Method` into an `ArtMethod*` — on R+ by reading that
   `artMethod` long field, otherwise via `FromReflectedMethod()`.
3. `hook_function()` forces both declaring classes to initialize, then overwrites *only* the
   original's `entry_point_from_quick_compiled_code_` with a generated trampoline. The 3-arg overload
   first snapshots the original `ArtMethod` into malloc'd memory and points the `backup` method's
   entry point at a trampoline for that snapshot, so the original body stays callable.

`find_function(owner, name, signature)` is the other half of the API: it resolves a target by JNI
descriptor (`"(Ljava/lang/String;I)V"`) instead of by `Class` objects, which is how you name one
overload out of several. It tries `GetMethodID` then `GetStaticMethodID`, so instance methods,
static methods and `<init>` all resolve, and hands back whatever `ToReflectedMethod` produces — a
`Method` or a `Constructor`, ready for `hook_function`. It touches no `ArtMethod` and works even if
`init()` failed.

The API takes `java.lang.reflect.Executable`, so **constructors hook exactly like methods** —
`artMethod` is a field of `Executable`, and nothing below the JNI boundary distinguishes the two. A
constructor's argument layout is the instance-method one (the receiver is already allocated when
`<init>` is entered), so its replacement is `static void (Object thiz, ...)`. Nothing initialises the
object unless the replacement calls the backup.

`HookExample.on_load()` (invoked from `MainActivity.onCreate`) is the end-to-end demo: it redirects
`MainActivity.on_click` to `HookExample.hook_with`, which calls through to `hook_backup` to run the
original. Note the hook target is `static` and takes the receiver as an explicit leading `Object thiz`
parameter — that shape is required for the frame layout to line up; keep it when adding hooks. A
`static` *target* has no receiver, so its replacement takes the parameters unchanged.

### Why a trampoline and not a field copy

Compiled code, nterp and the interpreter bridge all read the method they are executing — declaring
class, dex cache, code item — out of the register ART's quick calling convention reserves for the
`ArtMethod*` (`x0`/`r0`/`rdi`/`eax`). Copying the replacement's entry point onto the original leaves
the *original's* `ArtMethod*` in that register, so the replacement's constants resolve against the
wrong class; it only appears to work while both classes share a dex file. The trampoline loads the
replacement's `ArtMethod*` into that register and tail-jumps, so the callee sees itself.

It loads the entry point *from* the `ArtMethod` on every call instead of baking it in, which is what
makes the hook survive ART replacing that entry point later — this is not theoretical: the self-test
observes the replacement move into the JIT cache mid-run. Because the original's `ArtMethod` is
otherwise untouched, its declaring class, access flags and dex/vtable indices stay intact, so
reflection (`Method.invoke`, and therefore `android:onClick`) and virtual dispatch still see the
method they expect. Copying those fields instead breaks both.

## Things that will bite you

- **`ArtMethod` in `art_method.hpp` is a hand-maintained mirror of AOSP's `art::ArtMethod`.** Its
  layout is not ABI-stable and changes between Android releases. Nothing indexes it any more —
  `init_art_method_access()` recovers `sizeof(ArtMethod)` at runtime from the distance between the
  two adjacent `ArtHooks.layout_probe_*` methods, and derives the trailing `ptr_sized_fields_` offsets
  from that, because those fields are always last. The struct is documentation, and a mismatch against
  the measured size only logs a warning — but that warning means it no longer describes the platform,
  so diff it against that version's `art/runtime/art_method.h` before trusting anything written
  against it. The probes must stay adjacent in the dex method ordering (methods sort by name, so
  nothing may be named between them) or the measurement is rejected and the library disables itself.
- **JNI symbol names encode the overload signatures.** Both `hook_function` overloads use the long
  mangled form (`..._hook_1function__Ljava_lang_reflect_Executable_2...`). Changing a parameter type,
  adding an overload, or moving the class breaks the link silently at runtime — regenerate with
  `javac -h`, don't hand-edit. Return types are not mangled, so those are safe to change.
  `find_function` has no overloads, so it uses the short form — adding one would rename it.
- **`find_function` searches superclasses**, because that is what `GetMethodID` does and unlike
  `getDeclaredMethod`. An inherited method therefore resolves to the *superclass's* ArtMethod, and
  hooking that redirects it for every subclass, not just the one you named.
- **Hooking a `synchronized` method does not take the monitor.** `ACC_SYNCHRONIZED` drives the
  *callee's* entry sequence, and the hook redirects before it runs; the caller does not participate,
  so nothing here can fix it. `hook_function` logs a warning. Marking the replacement `synchronized`
  is only right for instance targets — a `static synchronized` method locks its declaring class, so
  a `static synchronized` replacement locks the wrong object. Calling through the backup *does*
  re-acquire it — the snapshot still carries `ACC_SYNCHRONIZED`, so ART's entry sequence locks the
  receiver — so the unprotected window is only the replacement's own code. `DispatchCases` asserts
  both halves.
- **Trampoline codegen in `trampoline.cpp` is per-ABI and all four are built.** Only arm64 is exercised
  on a real device here; the arm/x86/x86_64 encodings were checked against the NDK assembler. If you
  touch them, verify the emitted bytes disassemble to the intended instructions rather than eyeballing
  the hex.
- **Entry points are the only thing the hook writes.** Access flags are deliberately left alone —
  nothing here needs `kAccCompileDontBother`, and the runtime-only flag bits are not stable enough
  across releases to poke blind. The self-test is what establishes that ART does not overwrite a
  hooked or backup entry point under JIT pressure; re-run it before assuming that still holds on a
  newer platform.
- **Hooking forces the target's and replacement's classes to initialize** (via `Class.forName`),
  because ART rewrites every method's entry point when it finally runs a class initializer, which
  would silently drop a hook installed first. This means hooking has the side effect of running
  `<clinit>` earlier than the app would have.
- **`namespace` is `com.arthooks` but the demo classes are in `com.example.arthooks`**, so the
  generated resource class is `com.arthooks.R`, imported explicitly by `MainActivity`.
- **`minSdk` is 33**, so the pre-R `FromReflectedMethod` branch in `get_art_method()` is currently
  unreachable. It is kept deliberately for lower-minSdk use; don't delete it as dead code.
- **The backup's snapshot `ArtMethod` lives in malloc'd memory, so the GC never visits it.** Its
  `declaring_class_` is a `GcRoot` that nothing will update. This is safe only because ART allocates
  `mirror::Class` as non-movable; if that ever changes, backups become a use-after-move.
- **There is no unhook.** Trampolines and snapshots are allocated for the lifetime of the process.
  Hooking the same method twice chains, second hook outermost: the second backup snapshots an
  already-hooked target, so calling through it runs the first hook, which calls through to the
  original. `RuntimeCases.chained_hooks` pins that ordering down.
- **Boot-classpath targets work, but only from app call sites.** `RuntimeCases` hooks
  `StringTokenizer.countTokens()` and the app's calls land in the replacement. Calls made *inside*
  the boot image may not: AOT code can call a known-address callee directly instead of loading the
  entry point. Nothing here detects that case, so a framework-internal caller can keep running the
  original.
- `artMethod` is a non-SDK field. If `GetFieldID` ever fails on a newer platform, check hidden-API
  enforcement first — `init()` returns false and logs, and every later call refuses to run.
- `release` builds set `optimization { enable false }` — R8 is off, which matters because the hooked
  and hooking methods must survive by exact name and signature.
