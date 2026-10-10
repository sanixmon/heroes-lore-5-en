package co.kr.eamobile.hl5.runner;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;
import android.graphics.Color;

public class MainActivity extends Activity {
    private static final String TAG = "HL5_Runner";

    static {
        try {
            System.loadLibrary("hl5_arm64_bridge");
            Log.i(TAG, "Successfully loaded native library libhl5_arm64_bridge.so");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load native library libhl5_arm64_bridge.so: " + e.getMessage());
        }
    }

    private native String nativeRunDiagnosticTests();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ScrollView scrollView = new ScrollView(this);
        scrollView.setBackgroundColor(Color.BLACK);

        TextView textView = new TextView(this);
        textView.setTextColor(Color.GREEN);
        textView.setTextSize(14.0f);
        textView.setPadding(24, 24, 24, 24);

        try {
            String diagResults = nativeRunDiagnosticTests();
            Log.i(TAG, "Diagnostics Result:\n" + diagResults);
            textView.setText(diagResults);
        } catch (Throwable t) {
            String err = "Error running bridge diagnostics: " + t.getMessage();
            Log.e(TAG, err, t);
            textView.setText(err);
        }

        scrollView.addView(textView);
        setContentView(scrollView);
    }
}
