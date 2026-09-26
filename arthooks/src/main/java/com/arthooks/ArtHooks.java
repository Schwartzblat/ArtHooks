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
    /**
     * Set this to {@code true} <em>before</em> anything touches this class to keep ART's
     * ahead-of-time compiled code, at the cost of hooks that dex2oat inlined away. See
     * {@link #disable_aot()} for what that trade is.
     *
     * <pre>{@code
     * System.setProperty("arthooks.keep_aot", "true");   // in attachBaseContext, before hooking
     * }</pre>
     */
    public static final String KEEP_AOT_PROPERTY = "arthooks.keep_aot";

    private static final boolean AVAILABLE;
    private static final boolean AOT_DISABLED;

    static {
        System.loadLibrary("arthooks");
        AVAILABLE = init(Build.VERSION.SDK_INT);
        // Before anything else can load: this only protects classes ART has not linked yet, and it
        // rewrites entry points, so a hook installed first would be overwritten. See disable_aot().
        AOT_DISABLED = AVAILABLE
                && !Boolean.parseBoolean(System.getProperty(KEEP_AOT_PROPERTY))
                && disable_aot();
    }

    /**
     * Whether the native side came up. When false every hook_function() call fails, and the reason
     * was logged under the ArtHooks tag.
     */
    public static boolean is_available() {
        return AVAILABLE;
    }

    /**
     * Whether ART was told to stop running ahead-of-time compiled code.
     *
     * <p>This class's static initializer does that automatically, so the useful thing to know is
     * when it did <em>not</em> happen — either {@link #KEEP_AOT_PROPERTY} asked for it to be
     * skipped, or libart did not export what it takes. Hooks then work only for targets dex2oat did
     * not inline into their callers, and a target that was inlined goes on running its original
     * body with nothing reporting an error. See {@link #disable_aot()}.
     */
    public static boolean is_aot_disabled() {
        return AOT_DISABLED;
    }

    /**
     * Stops ART running ahead-of-time compiled code, so that hooks survive a {@code speed} or
     * {@code speed-profile} build. Called automatically when this class loads; calling it again is
     * a no-op.
     *
     * <p>Overwriting an entry point only redirects calls that <em>go through</em> the entry point.
     * dex2oat inlines: with an AOT compiler filter it copies a small method's body into every
     * caller it compiles, and such a caller never loads the callee's entry point at all. The hook
     * installs, reports success and silently never fires — which is why an app can work when it is
     * installed and start misbehaving hours later, once background dexopt has compiled it. Setting
     * {@code kAccCompileDontBother} on the target stops the <em>JIT</em> inlining it, but cannot
     * undo code dex2oat emitted before the process started.
     *
     * <p>What this does instead is make that code unreachable. ART consults
     * {@code Instrumentation::CanUseAotCode()} before giving a freshly linked method its compiled
     * body, and that says no for a Java-debuggable runtime, so marking the runtime Java-debuggable
     * makes every class ART links afterwards run under nterp and dispatch through the
     * {@code ArtMethod} again.
     *
     * <p><b>It only covers classes ART has not linked yet.</b> Touch this class as early as you
     * can — {@code Application.attachBaseContext} is the usual place — so that it runs before the
     * code you intend to hook, and that code's callers, are first loaded. A target still on AOT
     * code when it is hooked is reported as a warning under the {@code ArtHooks} tag.
     *
     * <p>The cost is process-wide: the app runs nterp plus JIT instead of AOT code, and while
     * debuggable the JIT compiles without inlining. Set {@link #KEEP_AOT_PROPERTY} to opt out of
     * paying it. Nothing about the app's own debuggable flag changes — this is ART's internal
     * state, not {@code ApplicationInfo.FLAG_DEBUGGABLE}, and it is not visible to
     * {@code ApplicationInfo.flags} or to a debugger.
     *
     * <p>Returns false if libart does not export what this needs, in which case nothing was
     * changed and the reason was logged.
     */
    public static native boolean disable_aot();

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
     * changes.
     *
     * <p><b>The backup must be declared {@code native}</b>, with the same signature as the
     * replacement and no body:
     *
     * <pre>{@code
     * static native boolean gate_backup(Object thiz);
     * }</pre>
     *
     * <p>Only the entry point is swapped, so every call to the backup has to go through it. A
     * backup with a Java body does not: it is small and returns nothing interesting, so the
     * compiler inlines it into the replacement and the call site ends up holding a copy of the
     * backup's own body. dex2oat does that at install time, before any of this runs, so the swap is
     * invisible and the replacement silently gets the backup's own answer instead of the
     * original's. A native method has no body to copy. A backup that is not native is refused
     * rather than installed, because the alternative is a hook that reports success and returns the
     * wrong value from then on.
     *
     * <p>A {@code static} target is the exception, and is only warned about: backing one up is
     * unreliable in a release build whether or not the backup is native, and the rule that would
     * make it work is not known yet. See the TODO in the README.</p>
     *
     * <p>Returns false on failure.
     */
    public static native boolean hook_function(Executable original, Executable replacement,
                                               Executable backup);

    private static native boolean init(int sdk_version);

    // Used at startup to locate art::ArtMethod::access_flags_, by finding the one offset whose word
    // matches each probe's own getModifiers(). That needs two probes whose modifiers differ in as
    // many bits as possible, so a word at some other offset cannot match both by accident. The two
    // shapes here are load-bearing: `private static` is 0x0a and `public final` is 0x11, which share
    // no bits at all.
    //
    // Use only modifiers ART stores verbatim. `synchronized` in particular is not one of them -- it
    // is kept as ACC_DECLARED_SYNCHRONIZED (0x20000) and only mapped back to 0x20 by getModifiers().
    //
    // These sort before layout_probe_* (f < l), so they do not come between the two of those.
    private static void flag_probe_a() {
    }

    public final void flag_probe_b() {
    }

    // Measured against each other at startup to recover sizeof(art::ArtMethod) on the running
    // platform: ART stores a class's methods in one contiguous array, so two adjacent direct
    // methods sit exactly one ArtMethod apart. Unused on purpose, and must stay adjacent -- the
    // dex file orders methods by name, so nothing may be named between these two.
    private static void layout_probe_a() {
    }

    private static void layout_probe_b() {
    }
}
