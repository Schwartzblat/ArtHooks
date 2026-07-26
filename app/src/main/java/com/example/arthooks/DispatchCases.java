package com.example.arthooks;

import android.util.Log;

import static com.example.arthooks.Checks.TAG;
import static com.example.arthooks.Checks.declared_constructor;
import static com.example.arthooks.Checks.declared_method;
import static com.example.arthooks.Checks.fail;
import static com.example.arthooks.Checks.hook;
import static com.example.arthooks.Checks.pass;
import static com.example.arthooks.Checks.with_thiz;

/**
 * Hooks one target per dispatch mechanism: constructors, direct/virtual/interface calls, JNI, and a
 * synchronized method.
 *
 * <p>Unlike the signature cases these are not interchangeable -- each one reaches the callee by a
 * different route through ART, and only the route that loads the entry point out of the ArtMethod
 * can be hooked this way.
 */
class DispatchCases {

    static boolean check() {
        return constructor_case()
                && static_target_with_backup()
                && direct_and_final()
                && interface_dispatch()
                && native_target()
                && synchronized_target_case();
    }

    // --- constructors --------------------------------------------------------------------------

    static class Constructed {
        final int value;

        Constructed(int value) {
            this.value = value;
        }
    }

    /** The receiver is already allocated when {@code <init>} runs, so it arrives as thiz. */
    static void constructed_replacement(Object thiz, int value) {
        constructed_backup(thiz, value * 2);
    }

    static void constructed_backup(Object thiz, int value) {
        Log.e(TAG, "constructed_backup ran its own body");
    }

    private static boolean constructor_case() {
        if (!hook(declared_constructor(Constructed.class, int.class),
                declared_method(DispatchCases.class, "constructed_replacement", with_thiz(int.class)),
                declared_method(DispatchCases.class, "constructed_backup", with_thiz(int.class)))) {
            return false;
        }

        Constructed constructed = new Constructed(21);
        if (constructed.value != 42) {
            return fail("constructor -> " + constructed.value + ", expected 42");
        }
        return pass("constructor hooked, and its backup still initialised the object");
    }

    // --- a static target with a backup ---------------------------------------------------------

    static int static_target(int value) {
        return value + 1;
    }

    static int static_replacement(int value) {
        return static_backup(value) * 10;
    }

    static int static_backup(int value) {
        Log.e(TAG, "static_backup ran its own body");
        return Integer.MIN_VALUE;
    }

    private static boolean static_target_with_backup() {
        // A static target has no receiver, so the replacement takes the parameters unchanged.
        if (!hook(declared_method(DispatchCases.class, "static_target", int.class),
                declared_method(DispatchCases.class, "static_replacement", int.class),
                declared_method(DispatchCases.class, "static_backup", int.class))) {
            return false;
        }

        int result = static_target(4);
        if (result != 50) {
            return fail("static target with backup -> " + result + ", expected 50");
        }
        return pass("static target hooked with a working backup");
    }

    // --- direct and final dispatch -------------------------------------------------------------

    private int private_target(int value) {
        return value + 1;
    }

    final int final_target(int value) {
        return value + 1;
    }

    static int private_replacement(Object thiz, int value) {
        return value + 100;
    }

    static int final_replacement(Object thiz, int value) {
        return value + 200;
    }

    private static boolean direct_and_final() {
        if (!hook_instance("private_target", "private_replacement", int.class)
                || !hook_instance("final_target", "final_replacement", int.class)) {
            return false;
        }

        DispatchCases instance = new DispatchCases();
        // invoke-direct, because it is private and called from inside the declaring class.
        if (instance.private_target(1) != 101) {
            return fail("private target -> " + instance.private_target(1));
        }
        if (instance.final_target(1) != 201) {
            return fail("final target -> " + instance.final_target(1));
        }
        return pass("private (invoke-direct) and final targets hooked");
    }

    // --- interface dispatch --------------------------------------------------------------------

    interface Greeter {
        String greet();
    }

    static class GreeterImpl implements Greeter {
        @Override
        public String greet() {
            return "original";
        }
    }

    static String greet_replacement(Object thiz) {
        return "hooked";
    }

    private static boolean interface_dispatch() {
        if (!hook(declared_method(GreeterImpl.class, "greet"),
                declared_method(DispatchCases.class, "greet_replacement", with_thiz()))) {
            return false;
        }

        // Through the interface, so this is invoke-interface rather than invoke-virtual.
        Greeter greeter = new GreeterImpl();
        String greeting = greeter.greet();
        if (!"hooked".equals(greeting)) {
            return fail("interface dispatch -> " + greeting);
        }
        return pass("interface-dispatched target hooked");
    }

    // --- native targets ------------------------------------------------------------------------

    /** Implemented in the demo's own libselftest.so; returns value * 3. */
    static native int native_method(int value);

    static {
        System.loadLibrary("selftest");
    }

    static int native_replacement(int value) {
        return value * 7;
    }

    private static boolean native_target() {
        if (native_method(2) != 6) {
            return fail("native method returned the wrong value before it was hooked");
        }
        if (!hook(declared_method(DispatchCases.class, "native_method", int.class),
                declared_method(DispatchCases.class, "native_replacement", int.class))) {
            return false;
        }

        int result = native_method(2);
        if (result != 14) {
            return fail("native target -> " + result + ", expected 14");
        }
        return pass("native (JNI) target hooked");
    }

    // --- synchronized targets ------------------------------------------------------------------

    static boolean replacement_held_lock;
    static boolean original_body_held_lock;

    synchronized boolean synchronized_target() {
        original_body_held_lock = Thread.holdsLock(this);
        return original_body_held_lock;
    }

    static boolean synchronized_replacement(Object thiz) {
        replacement_held_lock = Thread.holdsLock(thiz);
        return synchronized_backup(thiz);
    }

    static boolean synchronized_backup(Object thiz) {
        Log.e(TAG, "synchronized_backup ran its own body");
        return false;
    }

    private static boolean synchronized_target_case() {
        if (!hook(declared_method(DispatchCases.class, "synchronized_target"),
                declared_method(DispatchCases.class, "synchronized_replacement", with_thiz()),
                declared_method(DispatchCases.class, "synchronized_backup", with_thiz()))) {
            return false;
        }

        // Seeded true so that a hook which silently did not take still fails the check below: the
        // original body would run, holding its own monitor, and leave this untouched.
        replacement_held_lock = true;
        original_body_held_lock = false;
        new DispatchCases().synchronized_target();

        if (replacement_held_lock) {
            return fail("expected the monitor NOT to be held inside the replacement");
        }

        // Whether reaching the original body *through the backup* re-enters ART's synchronized
        // entry sequence is a property of the runtime, not of the hook, and it decides whether
        // calling through restores the locking the hook dropped. Reported rather than asserted
        // until it has been observed on a device.
        Log.i(TAG, "NOTE: reached through the backup, the original body "
                + (original_body_held_lock ? "DID hold" : "did NOT hold") + " the monitor");

        return pass("synchronized target hooked -- and the monitor was NOT taken, "
                + "because the lock lives in the replaced body");
    }

    // --- helpers -------------------------------------------------------------------------------

    /** Hooks an instance method of this class with a static replacement taking a leading thiz. */
    private static boolean hook_instance(String target_name, String replacement_name,
                                         Class<?>... parameters) {
        return hook(declared_method(DispatchCases.class, target_name, parameters),
                declared_method(DispatchCases.class, replacement_name, with_thiz(parameters)));
    }
}
