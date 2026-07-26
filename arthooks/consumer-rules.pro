# Applied automatically to anything that depends on this library.

# The native declarations are bound by their mangled JNI symbol names, which encode the Java method
# name and parameter types. Renaming any of them breaks the link silently at runtime.
#
# layout_probe_a/b are looked up from native by name, and their addresses are subtracted to recover
# sizeof(art::ArtMethod). They look unused to R8, and they must stay adjacent in the dex method
# ordering -- which is alphabetical, so keeping their names is what keeps them adjacent.
-keep class com.arthooks.ArtHooks {
    native <methods>;
    private static void layout_probe_a();
    private static void layout_probe_b();
}

# Note for consumers: this file cannot protect *your* hooks. A hooked method, its replacement and
# its backup are all located by exact name and signature, so keep them yourself, e.g.
#
#   -keep class com.example.MyHooks { *; }
#   -keep class com.example.SomeHookedClass { *; }
