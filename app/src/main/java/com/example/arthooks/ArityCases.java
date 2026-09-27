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
 * Hooks targets whose arguments do not all fit in the registers the quick ABI reserves for them.
 *
 * <p>Every other case in the self-test is small enough that the receiver and every argument reach
 * the callee in a register. arm64 passes the ArtMethod* in x0 and the first seven arguments in
 * x1-x7, so a target with eight argument slots -- a receiver plus seven references, say -- puts its
 * last one on the stack, and a backup only works if that slot survives the trip through the
 * trampoline. Nothing above exercises that.
 *
 * <p>The cases vary one thing at a time: the same eight-slot shape as a constructor, as an instance
 * method and as a static method, plus the seven-slot constructor immediately below the boundary. A
 * failure in all of them means the stack argument is the problem; a failure in only the constructor
 * means constructors are.
 *
 * <p>The two {@link StaticTarget} cases once failed in a release build compiled {@code speed}, where
 * the backup recursed into the replacement until the stack ran out. That was a separate defect from
 * the inlined-backup one these cases were written for: an AOT-compiled static method sits on the
 * quick resolution stub until its class is visibly initialized, and a backup built on that stub
 * re-dispatches through the hook. {@code hook_function} now settles a static target off the stub
 * before capturing its entry point (see {@code class_init.cpp}), so these pass under both filters.
 */
class ArityCases {

    static boolean check() {
        boolean ok = constructor_below_the_boundary();
        ok &= constructor_with_a_stack_argument();
        ok &= instance_method_with_a_stack_argument();
        ok &= static_method_in_registers();
        ok &= static_method_with_a_stack_argument();
        ok &= small_instance_method();
        return ok;
    }

    // --- a two-slot virtual method, to separate arity from dispatch ------------------------------

    static class Small {
        int bump(int value) {
            return value + 1;
        }
    }

    static int small_replacement(Object thiz, int value) {
        return small_backup(thiz, value);
    }

    static native int small_backup(Object thiz, int value);

    private static boolean small_instance_method() {
        if (!hook(declared_method(Small.class, "bump", int.class),
                declared_method(ArityCases.class, "small_replacement", with_thiz(int.class)),
                declared_method(ArityCases.class, "small_backup", with_thiz(int.class)))) {
            return false;
        }

        int bumped = new Small().bump(1);
        if (bumped != 2) {
            return fail("2-slot virtual method returned " + bumped + ", expected 2");
        }
        return pass("virtual method with two slots: backup ran the original body");
    }

    private static Class<?>[] strings(int count) {
        Class<?>[] parameters = new Class<?>[count];
        for (int i = 0; i < count; i++) {
            parameters[i] = String.class;
        }
        return parameters;
    }

    // --- six strings: receiver plus six arguments still fits in x1-x7 ----------------------------

    static class SixStrings {
        final String a;
        final String f;

        SixStrings(String a, String b, String c, String d, String e, String f) {
            this.a = a;
            this.f = f;
        }
    }

    static void six_replacement(Object thiz, String a, String b, String c, String d, String e,
                                String f) {
        six_backup(thiz, a, b, c, d, e, f);
    }

    static native void six_backup(Object thiz, String a, String b, String c, String d, String e,
                                  String f);

    private static boolean constructor_below_the_boundary() {
        if (!hook(declared_constructor(SixStrings.class, strings(6)),
                declared_method(ArityCases.class, "six_replacement", with_thiz(strings(6))),
                declared_method(ArityCases.class, "six_backup", with_thiz(strings(6))))) {
            return false;
        }

        SixStrings six = new SixStrings("a", "b", "c", "d", "e", "f");
        if (!"a".equals(six.a) || !"f".equals(six.f)) {
            return fail("6-String constructor left a=" + six.a + " f=" + six.f
                    + ", expected a=a f=f");
        }
        return pass("constructor with six arguments: backup initialised the object");
    }

    // --- seven strings: the last argument is passed on the stack ---------------------------------

    static class SevenStrings {
        final String a;
        final String g;

        SevenStrings(String a, String b, String c, String d, String e, String f, String g) {
            this.a = a;
            this.g = g;
        }
    }

    static void seven_replacement(Object thiz, String a, String b, String c, String d, String e,
                                  String f, String g) {
        seven_backup(thiz, a, b, c, d, e, f, g);
    }

    static native void seven_backup(Object thiz, String a, String b, String c, String d, String e,
                                    String f, String g);

    private static boolean constructor_with_a_stack_argument() {
        if (!hook(declared_constructor(SevenStrings.class, strings(7)),
                declared_method(ArityCases.class, "seven_replacement", with_thiz(strings(7))),
                declared_method(ArityCases.class, "seven_backup", with_thiz(strings(7))))) {
            return false;
        }

        SevenStrings seven = new SevenStrings("a", "b", "c", "d", "e", "f", "g");
        if (!"a".equals(seven.a) || !"g".equals(seven.g)) {
            return fail("7-String constructor left a=" + seven.a + " g=" + seven.g
                    + ", expected a=a g=g");
        }
        return pass("constructor with a stack argument: backup initialised the object");
    }

    // --- the same shape, but not a constructor ---------------------------------------------------

    static class SevenArguments {
        String joined = "";

        void join(String a, String b, String c, String d, String e, String f, String g) {
            joined = a + b + c + d + e + f + g;
        }
    }

    static void join_replacement(Object thiz, String a, String b, String c, String d, String e,
                                 String f, String g) {
        join_backup(thiz, a, b, c, d, e, f, g);
    }

    static native void join_backup(Object thiz, String a, String b, String c, String d, String e,
                                   String f, String g);

    private static boolean instance_method_with_a_stack_argument() {
        if (!hook(declared_method(SevenArguments.class, "join", strings(7)),
                declared_method(ArityCases.class, "join_replacement", with_thiz(strings(7))),
                declared_method(ArityCases.class, "join_backup", with_thiz(strings(7))))) {
            return false;
        }

        SevenArguments target = new SevenArguments();
        target.join("a", "b", "c", "d", "e", "f", "g");
        if (!"abcdefg".equals(target.joined)) {
            return fail("7-argument instance method left \"" + target.joined
                    + "\", expected \"abcdefg\"");
        }
        return pass("instance method with a stack argument: backup ran the original body");
    }

    // --- eight arguments with no receiver at all -------------------------------------------------

    static class StaticTarget {
        static String joined = "";
        static String joined7 = "";

        static void static_join7(String a, String b, String c, String d, String e, String f,
                                 String g) {
            joined7 = a + b + c + d + e + f + g;
        }

        static void static_join(String a, String b, String c, String d, String e, String f,
                                String g, String h) {
            joined = a + b + c + d + e + f + g + h;
        }
    }

    static void static_join_replacement(String a, String b, String c, String d, String e, String f,
                                        String g, String h) {
        static_join_backup(a, b, c, d, e, f, g, h);
    }

    static void static_join_backup(String a, String b, String c, String d, String e, String f,
                                   String g, String h) {
        Log.e(TAG, "static_join_backup ran its own body");
    }

    static void static_join7_replacement(String a, String b, String c, String d, String e,
                                         String f, String g) {
        static_join7_backup(a, b, c, d, e, f, g);
    }

    static void static_join7_backup(String a, String b, String c, String d, String e, String f,
                                     String g) {
        Log.e(TAG, "static_join7_backup ran its own body");
    }

    private static boolean static_method_in_registers() {
        if (!hook(declared_method(StaticTarget.class, "static_join7", strings(7)),
                declared_method(ArityCases.class, "static_join7_replacement", strings(7)),
                declared_method(ArityCases.class, "static_join7_backup", strings(7)))) {
            return false;
        }

        StaticTarget.static_join7("a", "b", "c", "d", "e", "f", "g");
        if (!"abcdefg".equals(StaticTarget.joined7)) {
            return fail("7-argument static method left \"" + StaticTarget.joined7
                    + "\", expected \"abcdefg\"");
        }
        return pass("static method wholly in registers: backup ran the original body");
    }

    private static boolean static_method_with_a_stack_argument() {
        if (!hook(declared_method(StaticTarget.class, "static_join", strings(8)),
                declared_method(ArityCases.class, "static_join_replacement", strings(8)),
                declared_method(ArityCases.class, "static_join_backup", strings(8)))) {
            return false;
        }

        StaticTarget.static_join("a", "b", "c", "d", "e", "f", "g", "h");
        if (!"abcdefgh".equals(StaticTarget.joined)) {
            return fail("8-argument static method left \"" + StaticTarget.joined
                    + "\", expected \"abcdefgh\"");
        }
        return pass("static method with a stack argument: backup ran the original body");
    }
}
