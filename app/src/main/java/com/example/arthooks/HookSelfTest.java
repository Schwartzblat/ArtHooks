package com.example.arthooks;

import android.util.Log;

import com.arthooks.ArtHooks;

import java.lang.reflect.Method;

import static com.example.arthooks.Checks.TAG;
import static com.example.arthooks.Checks.declared_method;
import static com.example.arthooks.Checks.fail;
import static com.example.arthooks.Checks.hook;
import static com.example.arthooks.Checks.pass;
import static com.example.arthooks.Checks.with_thiz;

/**
 * Runs every self-test and reports the verdict to logcat.
 *
 * <p>The two cases that live here are the ones about ART's behaviour over time rather than about a
 * particular kind of method: surviving JIT compilation, and forcing a class initialiser. The rest
 * are grouped in {@link SignatureCases}, {@link DispatchCases} and {@link RuntimeCases}.
 */
public class HookSelfTest {
    private static final int ITERATIONS = 200_000;
    private static final int PASSES = 4;

    private static final int GC_ROUNDS = 6;
    private static final int ALLOCATIONS_PER_ROUND = 400;
    private static final int ALLOCATION_SIZE = 64 * 1024;

    /** Static so the allocations below cannot be optimised away as dead. */
    static byte[] garbage;

    static int original_calls;
    static int replacement_calls;

    /** Runs the checks off the main thread; results land in logcat under HookSelfTest. */
    public static void run() {
        new Thread(HookSelfTest::check, "arthooks-selftest").start();
    }

    private static void check() {
        try {
            if (hook_and_backup_survive_the_jit()
                    && backup_survives_a_relocating_gc()
                    && static_target_is_hooked_and_initialized()
                    && static_hook_survives_visible_initialization()
                    && static_backup_on_a_fresh_class()
                    && settled_and_single_static_classes()
                    && throwing_class_initializer_fails_the_hook()
                    && a_successful_hook_is_actually_installed()
                    && SignatureCases.check()
                    && DispatchCases.check()
                    && RuntimeCases.check()
                    && LookupCases.check()
                    && ArityCases.check()
                    && LifecycleCases.check()) {
                Log.i(TAG, "PASS: all checks passed");
            }
        } catch (Throwable t) {
            Log.e(TAG, "FAIL: a check threw", t);
        }
    }

    // --- surviving the JIT ---------------------------------------------------------------------

    /** The method under test. Virtual, so calls to it go through the vtable. */
    public int target(int value) {
        original_calls++;
        return value + 1;
    }

    /** Replaces {@link #target}: static, with the receiver as an explicit leading parameter. */
    public static int replacement(Object thiz, int value) {
        replacement_calls++;
        return backup(thiz, value) * 10;
    }

    /** Backup slot for {@link #target}. Native, so nothing can inline it out of the replacement. */
    public static native int backup(Object thiz, int value);

    private static boolean hook_and_backup_survive_the_jit() {
        HookSelfTest instance = new HookSelfTest();
        if (instance.target(1) != 2) {
            return fail("target is already misbehaving before it was hooked");
        }

        if (!hook(declared_method(HookSelfTest.class, "target", int.class),
                declared_method(HookSelfTest.class, "replacement", with_thiz(int.class)),
                declared_method(HookSelfTest.class, "backup", with_thiz(int.class)))) {
            return false;
        }

        original_calls = 0;
        replacement_calls = 0;

        // The JIT compiles on its own thread and installs the result by overwriting the very entry
        // point the hook lives in, so the pass that matters is the one after it has caught up.
        for (int pass = 1; pass <= PASSES; pass++) {
            for (int i = 0; i < ITERATIONS; i++) {
                // (value + 1) * 10 means the replacement ran and called through to the original.
                int result = instance.target(i);
                if (result != (i + 1) * 10) {
                    return fail("pass " + pass + " call " + i + " returned " + result
                            + ", expected " + ((i + 1) * 10));
                }
            }
            try {
                Thread.sleep(1500);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }

        int expected = ITERATIONS * PASSES;
        if (replacement_calls != expected || original_calls != expected) {
            return fail("ran the replacement " + replacement_calls + " times and the original "
                    + original_calls + " times, expected " + expected + " of each");
        }
        return pass("hook and backup both survived " + expected + " calls across "
                + PASSES + " passes");
    }

    // --- surviving a relocating GC ----------------------------------------------------------

    /** The method under test. Its declaring class is what the GC gets a chance to relocate. */
    public int gc_target(int value) {
        return value + 7;
    }

    /** Replaces {@link #gc_target} and calls through, so every call exercises the backup. */
    public static int gc_replacement(Object thiz, int value) {
        return gc_backup(thiz, value) * 3;
    }

    /** Backup slot for {@link #gc_target}. */
    public static native int gc_backup(Object thiz, int value);

    /**
     * Calls the backup across repeated collections.
     *
     * <p>ArtMethod::declaring_class_ is a GcRoot, and the runtime rewrites it in every real
     * ArtMethod when the compacting collector relocates the class. A backup built on an ArtMethod
     * the runtime does not own never receives that fixup, and nterp dereferences that field on
     * every invocation -- so the first call after a relocation reads a dead class.
     *
     * <p>Note the failure mode: the process takes SIGSEGV inside the backup rather than reaching
     * {@link Checks#fail}, so a regression here shows up as the self-test dying mid-run, with the
     * last log line being this case's name.
     */
    private static boolean backup_survives_a_relocating_gc() {
        HookSelfTest instance = new HookSelfTest();
        if (!hook(declared_method(HookSelfTest.class, "gc_target", int.class),
                declared_method(HookSelfTest.class, "gc_replacement", with_thiz(int.class)),
                declared_method(HookSelfTest.class, "gc_backup", with_thiz(int.class)))) {
            return false;
        }

        if (instance.gc_target(1) != (1 + 7) * 3) {
            return fail("the backup was already wrong before any collection");
        }

        for (int round = 1; round <= GC_ROUNDS; round++) {
            // Churn first, so the explicit collection below has something to compact.
            for (int i = 0; i < ALLOCATIONS_PER_ROUND; i++) {
                garbage = new byte[ALLOCATION_SIZE];
            }
            System.gc();
            System.runFinalization();
            System.gc();

            int result = instance.gc_target(round);
            if (result != (round + 7) * 3) {
                return fail("round " + round + " returned " + result
                        + ", expected " + ((round + 7) * 3));
            }
        }
        return pass("hook and backup survived " + GC_ROUNDS + " rounds of collection");
    }

    // --- forcing a class initialiser -----------------------------------------------------------

    /** Untouched until the hook runs, so hooking it has to drive the class initialiser itself. */
    static class Lazy {
        static boolean initializer_ran;

        static {
            initializer_ran = true;
        }

        static String greet(String name) {
            return "hello " + name;
        }
    }

    /** Replaces {@link Lazy#greet}: static target, so there is no receiver to stand in for. */
    public static String greet_replacement(String name) {
        return "hooked " + name;
    }

    private static boolean static_target_is_hooked_and_initialized() {
        if (!hook(declared_method(Lazy.class, "greet", String.class),
                declared_method(HookSelfTest.class, "greet_replacement", String.class))) {
            return false;
        }

        if (!Lazy.initializer_ran) {
            return fail("hooking did not initialise the target's class");
        }

        String greeting = Lazy.greet("world");
        if (!"hooked world".equals(greeting)) {
            return fail("static target returned \"" + greeting + "\"");
        }
        return pass("static target hooked, and its class initialiser ran first");
    }

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

    // --- a static backup on a class the hook itself initialized ---------------------------------

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

    // --- a settled class, and a class with a single static method -------------------------------

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
                    + "ms, so the settled-class check is not short-circuiting the nudge loop");
        }

        // Single static method: nothing to compare against, so this must still settle off the stub
        // and hook rather than being skipped or refused.
        if (!hook(declared_method(OneStatic.class, "only"),
                declared_method(HookSelfTest.class, "only_replacement"))) {
            return false;
        }
        if (OneStatic.only() != 7) {
            return fail("hooking a class with a single static method did not take");
        }
        return pass("settled classes short-circuit, and a single-static-method class still hooks");
    }

    // --- a class initializer that throws --------------------------------------------------------

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

    // --- a hook that reports success is verifiably installed ------------------------------------

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
}
