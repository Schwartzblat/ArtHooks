package com.example.arthooks;

import android.app.Activity;
import android.util.Log;
import android.view.View;
import android.widget.Toast;

public class MainActivity extends Activity {
    @Override
    public void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        HookExample.on_load();
        HookSelfTest.run();
    }

    public void on_click(View view) {
        Log.i("MainActivity", "on_click: original body running");
        Toast.makeText(this, "Hello, World!", android.widget.Toast.LENGTH_SHORT).show();
    }
}
