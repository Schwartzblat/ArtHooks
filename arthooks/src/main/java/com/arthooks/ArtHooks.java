package com.arthooks;

import android.os.Build;

import java.lang.reflect.Executable;

/**
 * Minimal ART method hooking: redirects calls to one Java method into another by overwriting the
 * original {@code art::ArtMethod}'s entry point.
 *
 * <p>Targets are {@link Executable}, so both {@link java.lang.reflect.Method} and
 * {@link java.lang.reflect.Constructor} can be hooked -- {@code artMethod} is a field of
 * {@code Executable}, so nothing below cares which one it was handed.
 *
 * <p>A replacement must be {@code static} and take the receiver as an explicit leading
 * {@code Object thiz} parameter. The hook redirects the call without touching the arguments that
 * are already in place, so a static {@code (Object thiz, ...)} is what an instance method's
 * argument layout looks like from the callee's side. The same shape applies to the backup. A
 * {@code static} target has no receiver, so its replacement takes the parameters unchanged.
 *
 * <p>A constructor's layout is the instance-method one: the receiver is already allocated when
 * {@code <init>} is entered, so a replacement takes {@code (Object thiz, ...)} and returns
 * {@code void}. Nothing initialises the object unless the replacement calls the backup.
 */
public class ArtHooks {
    private static final boolean AVAILABLE;

    static {
        System.loadLibrary("arthooks");
        AVAILABLE = init(Build.VERSION.SDK_INT);
    }

    /**
     * Whether the native side came up. When false every hook_function() call fails, and the reason
     * was logged under the ArtHooks tag.
     */
    public static boolean is_available() {
        return AVAILABLE;
    }

    /**
     * Finds a method or constructor by its JNI signature descriptor, for feeding to
     * {@link #hook_function}.
     *
     * <p>The descriptor is the runtime's own form -- {@code "(Ljava/lang/String;I)V"} -- which is
     * what makes this useful for picking one overload out of several by exact signature, and for
     * naming a method whose parameter types are awkward to reach as {@code Class} objects. Pass
     * {@code "<init>"} with a {@code V} return type to get a {@link java.lang.reflect.Constructor}.
     *
     * <p>Instance methods, static methods and constructors are all found. Like JNI's own lookup and
     * unlike {@code getDeclaredMethod}, this searches superclasses too, so an inherited method
     * resolves to the superclass's method -- hooking that redirects it for every subclass.
     *
     * <p>Returns null if nothing matches, logging the reason under the ArtHooks tag.
     */
    public static native Executable find_function(Class<?> owner, String name, String signature);

    /** Redirects calls to {@code original} into {@code replacement}. Returns false on failure. */
    public static native boolean hook_function(Executable original, Executable replacement);

    /**
     * Redirects calls to {@code original} into {@code replacement}, and wires {@code backup} up to
     * run {@code original}'s pre-hook body so the replacement can call through to it.
     *
     * <p>Call the backup by its own name; it keeps its Java identity and only its entry point
     * changes. Returns false on failure.
     */
    public static native boolean hook_function(Executable original, Executable replacement,
                                               Executable backup);

    private static native boolean init(int sdk_version);

    // Measured against each other at startup to recover sizeof(art::ArtMethod) on the running
    // platform: ART stores a class's methods in one contiguous array, so two adjacent direct
    // methods sit exactly one ArtMethod apart. Unused on purpose, and must stay adjacent -- the
    // dex file orders methods by name, so nothing may be named between these two.
    private static void layout_probe_a() {
    }

    private static void layout_probe_b() {
    }
}
