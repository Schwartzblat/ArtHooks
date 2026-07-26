package com.example.arthooks;

import android.util.Log;

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

    static int original_calls;
    static int replacement_calls;

    /** Runs the checks off the main thread; results land in logcat under HookSelfTest. */
    public static void run() {
        new Thread(HookSelfTest::check, "arthooks-selftest").start();
    }

    private static void check() {
        try {
            if (hook_and_backup_survive_the_jit()
                    && static_target_is_hooked_and_initialized()
                    && SignatureCases.check()
                    && DispatchCases.check()
                    && RuntimeCases.check()
                    && LookupCases.check()) {
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

    /** Backup slot for {@link #target}; reaching this body means the backup was not installed. */
    public static int backup(Object thiz, int value) {
        Log.e(TAG, "backup ran its own body: the backup was not installed");
        return Integer.MIN_VALUE;
    }

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
}
