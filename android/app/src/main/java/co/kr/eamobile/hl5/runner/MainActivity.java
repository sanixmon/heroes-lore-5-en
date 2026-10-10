package co.kr.eamobile.hl5.runner;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.FrameLayout;
import android.widget.TextView;
import android.graphics.Color;
import android.opengl.GLSurfaceView;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public class MainActivity extends Activity implements GLSurfaceView.Renderer {
    private static final String TAG = "HL5_Runner";

    static {
        try {
            System.loadLibrary("hl5_arm64_bridge");
            Log.i(TAG, "Successfully loaded native library libhl5_arm64_bridge.so");
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "Failed to load native library libhl5_arm64_bridge.so: " + e.getMessage());
        }
    }

    private GLSurfaceView glSurfaceView;
    private TextView overlayTextView;
    private String internalPath = "";
    private String externalPath = "";
    private int frameCount = 0;

    private native String nativeRunDiagnosticTests(String internalPath, String externalPath);
    private native void nativeOnSurfaceCreated(String internalPath, String externalPath);
    private native void nativeOnSurfaceChanged(int width, int height);
    private native String nativeOnDrawFrame();
    private native void nativeOnPause();
    private native void nativeOnResume();
    private native void nativeOnDestroy();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        try {
            java.io.File internalDir = getFilesDir();
            java.io.File externalDir = getExternalFilesDir(null);
            internalPath = (internalDir != null) ? internalDir.getAbsolutePath() : "/data/data/" + getPackageName() + "/files";
            externalPath = (externalDir != null) ? externalDir.getAbsolutePath() : internalPath;
            if (internalDir != null && !internalDir.exists()) internalDir.mkdirs();
            if (externalDir != null && !externalDir.exists()) externalDir.mkdirs();

            // Extract libHeroesLore5.so from assets if not already extracted
            if (internalDir != null) {
                java.io.File targetSo = new java.io.File(internalDir, "libHeroesLore5.so");
                if (!targetSo.exists() || targetSo.length() == 0) {
                    try (java.io.InputStream in = getAssets().open("libHeroesLore5.so");
                         java.io.FileOutputStream out = new java.io.FileOutputStream(targetSo)) {
                        byte[] buf = new byte[16384];
                        int len;
                        while ((len = in.read(buf)) > 0) {
                            out.write(buf, 0, len);
                        }
                        Log.i(TAG, "Extracted libHeroesLore5.so to " + targetSo.getAbsolutePath());
                    } catch (Exception ex) {
                        Log.w(TAG, "Asset extraction note: " + ex.getMessage());
                    }
                }
            }
        } catch (Throwable t) {
            Log.e(TAG, "Storage dir init error: " + t.getMessage());
        }

        FrameLayout layout = new FrameLayout(this);
        glSurfaceView = new GLSurfaceView(this);
        glSurfaceView.setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        glSurfaceView.setRenderer(this);

        overlayTextView = new TextView(this);
        overlayTextView.setTextColor(Color.GREEN);
        overlayTextView.setBackgroundColor(Color.argb(180, 0, 0, 0));
        overlayTextView.setTextSize(12.0f);
        overlayTextView.setPadding(20, 20, 20, 20);
        overlayTextView.setText("HL5 ARM64 Runner initializing...");

        layout.addView(glSurfaceView);
        layout.addView(overlayTextView);
        setContentView(layout);
    }

    @Override
    public void onSurfaceCreated(GL10 gl, EGLConfig config) {
        Log.i(TAG, "GLThread: onSurfaceCreated entered");
        nativeOnSurfaceCreated(internalPath, externalPath);
    }

    @Override
    public void onSurfaceChanged(GL10 gl, int width, int height) {
        Log.i(TAG, "GLThread: onSurfaceChanged: " + width + "x" + height);
        nativeOnSurfaceChanged(width, height);
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        frameCount++;
        final String frameStatus = nativeOnDrawFrame();
        if (frameCount <= 5 || frameCount % 60 == 0) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    overlayTextView.setText(frameStatus);
                }
            });
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (glSurfaceView != null) glSurfaceView.onPause();
        nativeOnPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (glSurfaceView != null) glSurfaceView.onResume();
        nativeOnResume();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        nativeOnDestroy();
    }
}
