package com.example.arthooks;

import static com.example.arthooks.Checks.declared_method;
import static com.example.arthooks.Checks.fail;
import static com.example.arthooks.Checks.hook;
import static com.example.arthooks.Checks.pass;
import static com.example.arthooks.Checks.with_thiz;

/**
 * Hooks one method per return-value and argument shape.
 *
 * <p>The trampoline swaps a single register and jumps, so it never touches arguments or return
 * values -- these cases are here to hold that property down rather than because any one of them is
 * expected to be special. {@link Target#wide_args} is the exception worth having: it has more
 * integral and floating-point parameters than the quick calling convention has registers, so some
 * of them arrive on the stack.
 */
class SignatureCases {

    /** Targets. Every body returns the "original" value, so a passing check proves it was replaced. */
    static class Target {
        boolean ret_boolean() {
            return false;
        }

        int ret_int() {
            return 1;
        }

        long ret_long() {
            return 1L;
        }

        float ret_float() {
            return 1.0f;
        }

        double ret_double() {
            return 1.0d;
        }

        String ret_object() {
            return "original";
        }

        int[] ret_array() {
            return new int[]{1};
        }

        void ret_void(int[] out) {
            out[0] = 1;
        }

        long wide_args(int a, long b, float c, double d, Object e, int f, int g,
                       int h, long i, double j, float k, int l) {
            return 1L;
        }
    }

    // Replacements. Each is named after the target it replaces, and takes the same parameters
    // behind a leading Object thiz.

    static boolean ret_boolean(Object thiz) {
        return true;
    }

    static int ret_int(Object thiz) {
        return 0x7F1234;
    }

    static long ret_long(Object thiz) {
        return 0x1122334455667788L;
    }

    static float ret_float(Object thiz) {
        return 2.5f;
    }

    static double ret_double(Object thiz) {
        return 1.0e300d;
    }

    static String ret_object(Object thiz) {
        return "hooked";
    }

    static int[] ret_array(Object thiz) {
        return new int[]{2, 3};
    }

    static void ret_void(Object thiz, int[] out) {
        out[0] = 2;
    }

    /** Adds every argument up, so a value landing in the wrong slot changes the result. */
    static long wide_args(Object thiz, int a, long b, float c, double d, Object e, int f, int g,
                          int h, long i, double j, float k, int l) {
        return a + b + (long) c + (long) d + ((String) e).length() + f + g + h + i + (long) j
                + (long) k + l;
    }

    static boolean check() {
        if (!hook_all()) {
            return false;
        }

        Target target = new Target();
        if (!target.ret_boolean()) {
            return fail("ret_boolean");
        }
        if (target.ret_int() != 0x7F1234) {
            return fail("ret_int -> " + target.ret_int());
        }
        if (target.ret_long() != 0x1122334455667788L) {
            return fail("ret_long -> " + target.ret_long());
        }
        if (target.ret_float() != 2.5f) {
            return fail("ret_float -> " + target.ret_float());
        }
        if (target.ret_double() != 1.0e300d) {
            return fail("ret_double -> " + target.ret_double());
        }
        if (!"hooked".equals(target.ret_object())) {
            return fail("ret_object -> " + target.ret_object());
        }

        int[] array = target.ret_array();
        if (array.length != 2 || array[0] != 2 || array[1] != 3) {
            return fail("ret_array");
        }

        int[] out = new int[1];
        target.ret_void(out);
        if (out[0] != 2) {
            return fail("ret_void -> " + out[0]);
        }

        // 1 + 2 + 3 + 4 + "12345".length() + 6 + 7 + 8 + 9 + 10 + 11 + 12 = 78
        long sum = target.wide_args(1, 2L, 3.0f, 4.0d, "12345", 6, 7, 8, 9L, 10.0d, 11.0f, 12);
        if (sum != 78L) {
            return fail("wide_args -> " + sum + ", expected 78");
        }

        return pass("every return shape and a stack-spilling argument list survived hooking");
    }

    private static boolean hook_all() {
        return hook_shape("ret_boolean")
                && hook_shape("ret_int")
                && hook_shape("ret_long")
                && hook_shape("ret_float")
                && hook_shape("ret_double")
                && hook_shape("ret_object")
                && hook_shape("ret_array")
                && hook_shape("ret_void", int[].class)
                && hook_shape("wide_args", int.class, long.class, float.class, double.class,
                Object.class, int.class, int.class, int.class, long.class, double.class,
                float.class, int.class);
    }

    /** Hooks {@code Target.name(...)} with the same-named replacement above. */
    private static boolean hook_shape(String name, Class<?>... parameters) {
        return hook(declared_method(Target.class, name, parameters),
                declared_method(SignatureCases.class, name, with_thiz(parameters)));
    }
}
