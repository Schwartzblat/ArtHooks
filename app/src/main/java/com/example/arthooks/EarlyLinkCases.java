package com.example.arthooks;

import android.os.Build;
import android.util.Log;

import static com.example.arthooks.Checks.TAG;
import static com.example.arthooks.Checks.declared_method;
import static com.example.arthooks.Checks.fail;
import static com.example.arthooks.Checks.hook;
import static com.example.arthooks.Checks.pass;
import static com.example.arthooks.Checks.with_thiz;

/**
 * A caller ART linked before ArtHooks loaded, holding the target inlined in its AOT body.
 *
 * <p>This is the position a {@code speed-profile} install puts much of an app in. dex2oat writes an
 * app image next to the compiled code, ART maps it when the class loader opens -- before any of the
 * app's own code runs -- and every class in it is linked on the spot, AOT bodies included. Telling
 * ART to stop using AOT code from then on does not reach them, so a target dex2oat inlined into one
 * of them stays inlined unless ArtHooks also moves the already-linked classes off their AOT code.
 * {@link MainActivity} stands in for the app image by calling {@link #link()} before anything
 * touches ArtHooks.
 *
 * <p>Only a non-debuggable build compiled {@code speed} can fail this: a debuggable one is never
 * inlined, and {@code verify} compiles nothing. tools/run-aot-selftest.sh is what exercises it.
 */
final class EarlyLinkCases {
    private final int value;

    private EarlyLinkCases(int value) {
        this.value = value;
    }

    /** The target: a one-field getter, the shape dex2oat inlines into every caller it compiles. */
    int value() {
        return value;
    }

    /**
     * The caller. An instance method on purpose: ART gives one its AOT body the moment the class
     * links, whereas a static method waits on the resolution stub until its class is visibly
     * initialized -- a batched step that can land after ArtHooks has loaded, handing it nterp
     * instead and hiding exactly the case this is for.
     */
    int read() {
        return value();
    }

    static int value_replacement(Object thiz) {
        return -1;
    }

    /** Called by MainActivity before ArtHooks loads, so this class is linked with its AOT bodies. */
    static void link() {
        new EarlyLinkCases(1).read();
    }

    static boolean check() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            // Android 13's ART has no Instrumentation::UpdateEntrypointsForDebuggable(), so a class
            // it linked before ArtHooks loaded keeps its AOT bodies there; init logs as much.
            Log.i(TAG, "SKIP: a caller linked before ArtHooks loaded keeps its AOT code below API 34");
            return true;
        }

        if (!hook(declared_method(EarlyLinkCases.class, "value"),
                declared_method(EarlyLinkCases.class, "value_replacement", with_thiz()))) {
            return false;
        }
        int result = new EarlyLinkCases(1).read();
        if (result != -1) {
            return fail("a caller linked before ArtHooks loaded returned " + result
                    + ", expected the replacement's -1 -- its AOT body still holds the target inlined");
        }
        return pass("a caller ART linked before ArtHooks loaded reaches the hook");
    }
}
