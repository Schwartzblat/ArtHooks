package com.example.arthooks;

import android.util.Log;

import java.lang.reflect.Method;
import java.util.StringTokenizer;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

import static com.example.arthooks.Checks.TAG;
import static com.example.arthooks.Checks.declared_method;
import static com.example.arthooks.Checks.fail;
import static com.example.arthooks.Checks.hook;
import static com.example.arthooks.Checks.pass;
import static com.example.arthooks.Checks.with_thiz;

/**
 * Cases about the environment a hook lives in rather than the shape of the method: a target from a
 * different dex file, a method hooked twice, and a hook installed while other threads are calling
 * the target.
 */
class RuntimeCases {

    static boolean check() {
        return cross_dex_replacement() && chained_hooks() && concurrent_install();
    }

    // --- a target from a different dex file ----------------------------------------------------

    /** Only reachable from the replacement below, and deliberately only present in the app's dex. */
    static final class Marker {
        static final String TEXT = "resolved-from-the-app-dex";

        int value() {
            return 4242;
        }
    }

    /**
     * Replaces a boot-classpath method.
     *
     * <p>Every constant this body touches -- the Marker type, the string, the method reference --
     * is an index into the app's dex file. The target lives in the boot classpath's dex, so if
     * constants resolved against the *target's* dex cache rather than the replacement's, these
     * indices would land on unrelated entries. That is precisely what a plain entry-point-and-data
     * copy gets wrong, and it is invisible whenever both classes share a dex file.
     */
    static int count_tokens_replacement(Object thiz) {
        Marker marker = new Marker();
        return marker.value() + Marker.TEXT.length();
    }

    private static boolean cross_dex_replacement() {
        if (new StringTokenizer("a b c").countTokens() != 3) {
            return fail("the boot-classpath target misbehaved before it was hooked");
        }

        if (!hook(declared_method(StringTokenizer.class, "countTokens"),
                declared_method(RuntimeCases.class, "count_tokens_replacement", with_thiz()))) {
            return false;
        }

        int expected = 4242 + Marker.TEXT.length();
        int result = new StringTokenizer("a b c").countTokens();
        if (result != expected) {
            return fail("cross-dex replacement -> " + result + ", expected " + expected);
        }
        return pass("boot-classpath target hooked, and the replacement resolved its own "
                + "dex constants");
    }

    // --- hooking the same method twice ---------------------------------------------------------

    static int chain_target(int value) {
        return value;
    }

    static int chain_first(int value) {
        return chain_first_backup(value) + 10;
    }

    static int chain_first_backup(int value) {
        Log.e(TAG, "chain_first_backup ran its own body");
        return Integer.MIN_VALUE;
    }

    static int chain_second(int value) {
        return chain_second_backup(value) + 100;
    }

    static int chain_second_backup(int value) {
        Log.e(TAG, "chain_second_backup ran its own body");
        return Integer.MIN_VALUE;
    }

    private static boolean chained_hooks() {
        Method target = declared_method(RuntimeCases.class, "chain_target", int.class);

        if (!hook(target,
                declared_method(RuntimeCases.class, "chain_first", int.class),
                declared_method(RuntimeCases.class, "chain_first_backup", int.class))) {
            return false;
        }
        if (chain_target(1) != 11) {
            return fail("after one hook -> " + chain_target(1) + ", expected 11");
        }

        if (!hook(target,
                declared_method(RuntimeCases.class, "chain_second", int.class),
                declared_method(RuntimeCases.class, "chain_second_backup", int.class))) {
            return false;
        }

        // The second backup snapshotted a target that was already hooked, so calling through runs
        // the first hook, which calls through to the untouched original: 1 + 10 + 100.
        int result = chain_target(1);
        if (result != 111) {
            return fail("chained hooks -> " + result + ", expected 111");
        }
        return pass("hooking a method twice chains, second hook outermost");
    }

    // --- installing a hook while the target is being called ------------------------------------

    static int racing_target() {
        return 1;
    }

    static int racing_replacement() {
        return 2;
    }

    private static boolean concurrent_install() {
        AtomicBoolean stop = new AtomicBoolean(false);
        AtomicInteger bad_values = new AtomicInteger();
        AtomicInteger calls = new AtomicInteger();
        Thread[] callers = new Thread[4];

        for (int i = 0; i < callers.length; i++) {
            callers[i] = new Thread(() -> {
                while (!stop.get()) {
                    // Either the original or the replacement is fine; anything else means a call
                    // landed somewhere it should not have.
                    int value = racing_target();
                    if (value != 1 && value != 2) {
                        bad_values.incrementAndGet();
                    }
                    calls.incrementAndGet();
                }
            }, "arthooks-racer-" + i);
            callers[i].start();
        }

        boolean hooked;
        try {
            Thread.sleep(50);
            hooked = hook(declared_method(RuntimeCases.class, "racing_target"),
                    declared_method(RuntimeCases.class, "racing_replacement"));
            Thread.sleep(50);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        } finally {
            stop.set(true);
        }

        if (!join_all(callers) || !hooked) {
            return false;
        }
        if (bad_values.get() != 0) {
            return fail(bad_values.get() + " calls returned a value that was neither the original "
                    + "nor the replacement");
        }
        if (racing_target() != 2) {
            return fail("the hook was not in place after the race");
        }
        return pass("hook installed under " + calls.get()
                + " concurrent calls without a torn dispatch");
    }

    private static boolean join_all(Thread[] threads) {
        for (Thread thread : threads) {
            try {
                thread.join(2000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return true;
    }
}
