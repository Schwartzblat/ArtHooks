# Applied automatically to anything that depends on this library.

# The native declarations are bound by their mangled JNI symbol names, which encode the Java method
# name and parameter types. Renaming any of them breaks the link silently at runtime.
#
# layout_probe_a/b are looked up from native by name, and their addresses are subtracted to recover
# sizeof(art::ArtMethod). They look unused to R8, and they must stay adjacent in the dex method
# ordering -- which is alphabetical, so keeping their names is what keeps them adjacent.
#
# flag_probe_a/b are looked up the same way, to locate access_flags_. Their *shapes* are what makes
# that work: `private static` (0x0a) and `public final` (0x11) share no bits, so exactly one offset
# can match both. R8 changing either one's modifiers, or dropping them, silently disables
# kAccCompileDontBother -- which costs more than the hot-method race it is named for, because it is
# also what stops the JIT inlining a hooked method into a caller.
-keep class com.arthooks.ArtHooks {
    native <methods>;
    private static void layout_probe_a();
    private static void layout_probe_b();
    private static void flag_probe_a();
    public final void flag_probe_b();
}

# Note for consumers: this file cannot protect *your* hooks. A hooked method, its replacement and
# its backup are all located by exact name and signature, so keep them yourself, e.g.
#
#   -keep class com.example.MyHooks { *; }
#   -keep class com.example.SomeHookedClass { *; }
