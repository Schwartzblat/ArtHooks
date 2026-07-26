package com.example.arthooks;

import android.content.Context;
import android.util.Log;
import android.view.View;
import android.widget.Toast;

import com.arthooks.ArtHooks;

import java.lang.reflect.Method;

public class HookExample {
    private static final String TAG = "HookExample";

    /**
     * Backup slot for MainActivity.on_click. After hooking, calling this runs on_click's original
     * body instead of the body below -- so reaching this log line means the backup never got
     * installed.
     */
    public static void hook_backup(Object thiz, View view) {
        Log.e(TAG, "hook_backup ran its own body: the backup was not installed");
    }

    public static void hook_with(Object thiz, View view) {
        Log.i(TAG, "hook_with: hook ran, thiz is a " + thiz.getClass().getName());
        Toast.makeText((Context) thiz, "Got hooked!", android.widget.Toast.LENGTH_SHORT).show();
        hook_backup(thiz, view);
    }

    public static void on_load() {
        Log.i(TAG, "on_load called");
        try {
            Method to_hook = MainActivity.class.getDeclaredMethod("on_click", View.class);
            Method target = HookExample.class.getDeclaredMethod("hook_with", Object.class, View.class);
            Method backup = HookExample.class.getDeclaredMethod("hook_backup", Object.class, View.class);
            boolean hooked = ArtHooks.hook_function(to_hook, target, backup);
            Log.i(TAG, "hook installed: " + hooked);
        } catch (Exception e) {
            Log.e(TAG, "could not install the hook", e);
        }
    }
}
