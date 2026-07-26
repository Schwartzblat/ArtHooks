package com.example.arthooks;

import com.arthooks.ArtHooks;

import java.lang.reflect.Constructor;
import java.lang.reflect.Executable;

import static com.example.arthooks.Checks.declared_constructor;
import static com.example.arthooks.Checks.declared_method;
import static com.example.arthooks.Checks.fail;
import static com.example.arthooks.Checks.hook;
import static com.example.arthooks.Checks.pass;
import static com.example.arthooks.Checks.with_thiz;

/**
 * Covers {@link ArtHooks#find_function}: resolving a target by JNI signature descriptor rather than
 * by {@code Class} objects.
 *
 * <p>The point of the descriptor form is picking one overload out of several, so most of this is
 * about landing on the right one.
 */
class LookupCases {

    static boolean check() {
        return finds_each_kind()
                && distinguishes_overloads()
                && rejects_what_does_not_exist()
                && hooks_what_it_finds();
    }

    static class Target {
        final int value;

        Target(int value) {
            this.value = value;
        }

        String overloaded(int value) {
            return "int";
        }

        String overloaded(String value) {
            return "String";
        }

        String overloaded(int[] values, long other) {
            return "int[],long";
        }

        static long static_method(long value) {
            return value;
        }

        int hooked() {
            return 1;
        }
    }

    static int hooked_replacement(Object thiz) {
        return 2;
    }

    private static boolean finds_each_kind() {
        Executable instance_method = ArtHooks.find_function(
                Target.class, "overloaded", "(I)Ljava/lang/String;");
        if (!declared_method(Target.class, "overloaded", int.class).equals(instance_method)) {
            return fail("find_function did not return the instance method: " + instance_method);
        }

        Executable static_method = ArtHooks.find_function(Target.class, "static_method", "(J)J");
        if (!declared_method(Target.class, "static_method", long.class).equals(static_method)) {
            return fail("find_function did not return the static method: " + static_method);
        }

        // <init> with a void return type is how JNI names a constructor.
        Executable constructor = ArtHooks.find_function(Target.class, "<init>", "(I)V");
        if (!(constructor instanceof Constructor)) {
            return fail("find_function returned " + constructor + " for <init>, expected a "
                    + "Constructor");
        }
        if (!declared_constructor(Target.class, int.class).equals(constructor)) {
            return fail("find_function did not return the constructor: " + constructor);
        }

        return pass("find_function resolves instance methods, static methods and constructors");
    }

    private static boolean distinguishes_overloads() {
        Target target = new Target(0);

        Executable takes_int = ArtHooks.find_function(
                Target.class, "overloaded", "(I)Ljava/lang/String;");
        Executable takes_string = ArtHooks.find_function(
                Target.class, "overloaded", "(Ljava/lang/String;)Ljava/lang/String;");
        Executable takes_array = ArtHooks.find_function(
                Target.class, "overloaded", "([IJ)Ljava/lang/String;");

        if (takes_int == null || takes_string == null || takes_array == null) {
            return fail("an overload was not found: " + takes_int + " / " + takes_string + " / "
                    + takes_array);
        }
        if (takes_int.equals(takes_string) || takes_int.equals(takes_array)
                || takes_string.equals(takes_array)) {
            return fail("find_function returned the same method for different descriptors");
        }

        // Cross-check against what each overload actually does, so this cannot pass on identity
        // alone if the descriptors were parsed loosely.
        if (!declared_method(Target.class, "overloaded", int.class).equals(takes_int)
                || !declared_method(Target.class, "overloaded", String.class).equals(takes_string)
                || !declared_method(Target.class, "overloaded", int[].class, long.class)
                .equals(takes_array)) {
            return fail("find_function matched the wrong overload for a descriptor");
        }
        if (!"int".equals(target.overloaded(1))
                || !"String".equals(target.overloaded("x"))
                || !"int[],long".equals(target.overloaded(new int[0], 1L))) {
            return fail("the overloads under test do not behave as expected");
        }

        return pass("find_function picks the overload the descriptor names");
    }

    private static boolean rejects_what_does_not_exist() {
        if (ArtHooks.find_function(Target.class, "overloaded", "(F)Ljava/lang/String;") != null) {
            return fail("find_function matched a descriptor no overload has");
        }
        if (ArtHooks.find_function(Target.class, "not_a_method", "()V") != null) {
            return fail("find_function matched a name that does not exist");
        }
        return pass("find_function returns null for a signature nothing matches");
    }

    private static boolean hooks_what_it_finds() {
        Executable target = ArtHooks.find_function(Target.class, "hooked", "()I");
        if (target == null) {
            return fail("find_function did not find the method to hook");
        }
        if (!hook(target, declared_method(LookupCases.class, "hooked_replacement", with_thiz()))) {
            return false;
        }

        int result = new Target(0).hooked();
        if (result != 2) {
            return fail("hooking what find_function returned -> " + result + ", expected 2");
        }
        return pass("a target found by signature hooks like any other");
    }
}
