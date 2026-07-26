package com.example.arthooks;

import android.util.Log;

import com.arthooks.ArtHooks;

import java.lang.reflect.Constructor;
import java.lang.reflect.Executable;
import java.lang.reflect.Method;

/**
 * Reporting and hook-installation helpers shared by the self-test classes.
 *
 * <p>Reflection lookups throw unchecked here so that each case reads as a straight line instead of
 * a try/catch; {@link HookSelfTest} catches at the top and reports the failure.
 */
final class Checks {
    static final String TAG = "HookSelfTest";

    private Checks() {
    }

    static boolean pass(String what) {
        Log.i(TAG, "PASS: " + what);
        return true;
    }

    static boolean fail(String what) {
        Log.e(TAG, "FAIL: " + what);
        return false;
    }

    static Method declared_method(Class<?> owner, String name, Class<?>... parameters) {
        try {
            return owner.getDeclaredMethod(name, parameters);
        } catch (NoSuchMethodException e) {
            throw new AssertionError("no method " + owner.getName() + "." + name, e);
        }
    }

    static Constructor<?> declared_constructor(Class<?> owner, Class<?>... parameters) {
        try {
            return owner.getDeclaredConstructor(parameters);
        } catch (NoSuchMethodException e) {
            throw new AssertionError("no such constructor for " + owner.getName(), e);
        }
    }

    /**
     * Prepends the leading {@code Object thiz} that a replacement for an instance method or a
     * constructor has to declare, since the hook does not touch the arguments already in place.
     */
    static Class<?>[] with_thiz(Class<?>... parameters) {
        Class<?>[] with = new Class<?>[parameters.length + 1];
        with[0] = Object.class;
        System.arraycopy(parameters, 0, with, 1, parameters.length);
        return with;
    }

    static boolean hook(Executable target, Executable replacement) {
        return ArtHooks.hook_function(target, replacement)
                || fail("hook_function() returned false for " + target.getName());
    }

    static boolean hook(Executable target, Executable replacement, Executable backup) {
        return ArtHooks.hook_function(target, replacement, backup)
                || fail("hook_function() returned false for " + target.getName());
    }
}
