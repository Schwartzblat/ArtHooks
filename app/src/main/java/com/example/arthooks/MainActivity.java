package com.example.arthooks;

import android.app.Activity;
import android.content.Intent;
import android.util.Log;
import android.view.View;
import android.widget.Toast;

import com.arthooks.ArtHooks;

public class MainActivity extends Activity {
    @Override
    public void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // `am start ... --ez benchmark true` measures throughput instead of running the demo, so
        // that neither the demo hook nor the self-test competes with it for the JIT. ArtHooks is
        // still loaded, because loading it is what drops ART's AOT code.
        Intent intent = getIntent();
        if (intent != null && intent.getBooleanExtra("benchmark", false)) {
            run_benchmark(intent);
            return;
        }

        HookExample.on_load();
        HookSelfTest.run();
    }

    private void run_benchmark(Intent intent) {
        // Spelled out rather than read from ArtHooks.KEEP_AOT_PROPERTY: the property has to be set
        // before anything touches that class, since its static initializer is what reads it.
        String keep_aot = intent.getStringExtra("arthooks_keep_aot");
        if (keep_aot != null) {
            System.setProperty("arthooks.keep_aot", keep_aot);
        }

        Log.i("MainActivity", "benchmark mode; ArtHooks available=" + ArtHooks.is_available());
        Benchmark.run();
    }

    public void on_click(View view) {
        Log.i("MainActivity", "on_click: original body running");
        Toast.makeText(this, "Hello, World!", android.widget.Toast.LENGTH_SHORT).show();
    }
}
