package com.example.arthooks;

import com.arthooks.ArtHooks;

import java.lang.reflect.Method;

import static com.example.arthooks.Checks.declared_method;
import static com.example.arthooks.Checks.fail;
import static com.example.arthooks.Checks.hook;
import static com.example.arthooks.Checks.pass;
import static com.example.arthooks.Checks.with_thiz;

/** Cases about a hook's lifetime rather than its dispatch: removing one, and removing a chain. */
class LifecycleCases {

    static boolean check() {
        return unhook_restores_the_original() && unhook_unwinds_a_chain();
    }

    // --- removing a hook -------------------------------------------------------------------------

    static class Target {
        int value() {
            return 1;
        }
    }

    static int value_replacement(Object thiz) {
        return 2;
    }

    private static boolean unhook_restores_the_original() {
        Method target = declared_method(Target.class, "value");
        if (!hook(target, declared_method(LifecycleCases.class, "value_replacement", with_thiz()))) {
            return false;
        }
        if (new Target().value() != 2) {
            return fail("the hook did not take before unhooking was tried");
        }

        if (!ArtHooks.unhook_function(target)) {
            return fail("unhook_function() returned false for a hooked method");
        }
        if (ArtHooks.is_hooked(target)) {
            return fail("is_hooked() is still true after unhooking");
        }
        int result = new Target().value();
        if (result != 1) {
            return fail("after unhooking -> " + result + ", expected the original 1");
        }
        if (ArtHooks.unhook_function(target)) {
            return fail("unhook_function() reported success for a method that is not hooked");
        }
        return pass("unhooking restores the original body and is idempotent");
    }

    // --- removing one of two chained hooks -------------------------------------------------------

    static class Chained {
        int value() {
            return 1;
        }
    }

    static int chain_outer(Object thiz) {
        return 100;
    }

    static int chain_inner(Object thiz) {
        return 10;
    }

    /**
     * Review Focus 5. Hooking twice chains, second hook outermost, so unhooking once has to leave
     * the first hook in place and working rather than restoring the original or dangling.
     */
    private static boolean unhook_unwinds_a_chain() {
        Method target = declared_method(Chained.class, "value");
        if (!hook(target, declared_method(LifecycleCases.class, "chain_inner", with_thiz()))
                || !hook(target, declared_method(LifecycleCases.class, "chain_outer",
                        with_thiz()))) {
            return false;
        }
        if (new Chained().value() != 100) {
            return fail("the second hook is not outermost before unhooking was tried");
        }

        if (!ArtHooks.unhook_function(target)) {
            return fail("unhook_function() returned false for the outer hook");
        }
        int after_one = new Chained().value();
        if (after_one != 10) {
            return fail("after removing the outer hook -> " + after_one + ", expected 10");
        }

        if (!ArtHooks.unhook_function(target)) {
            return fail("unhook_function() returned false for the inner hook");
        }
        int after_both = new Chained().value();
        if (after_both != 1) {
            return fail("after removing both hooks -> " + after_both + ", expected 1");
        }
        return pass("unhooking a chained hook unwinds one layer at a time");
    }
}
