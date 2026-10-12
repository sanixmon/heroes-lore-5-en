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
    private TextView aspectToggleBtn;
    private String internalPath = "";
    private String externalPath = "";
    private int frameCount = 0;
    private boolean isStretchMode = false;

    // Audio Bridge (MediaPlayer for BGM, SoundPool for SFX)
    private java.io.File vfsFile;
    private java.io.FileInputStream vfsAudioStream;
    private android.media.MediaPlayer bgmPlayer;
    private android.media.SoundPool soundPool;
    private final java.util.HashMap<Long, Integer> soundPoolMap = new java.util.HashMap<>();
    private final java.util.HashMap<Long, Integer> soundStreamMap = new java.util.HashMap<>();
    private float musicVolume = 0.8f;
    private float soundVolume = 0.8f;
    private boolean bgmWasPlayingOnPause = false;

    private native String nativeRunDiagnosticTests(String internalPath, String externalPath);
    private native void nativeOnSurfaceCreated(String internalPath, String externalPath);
    private native void nativeOnSurfaceChanged(int width, int height);
    private native String nativeOnDrawFrame();
    private native void nativeOnTouchEvent(int action, float rawX, float rawY, int pointerId);
    private native void nativeSetStretchMode(boolean stretch);
    private native void nativeOnPause();
    private native void nativeOnResume();
    private native void nativeOnDestroy();
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemUI();
        Log.i(TAG, "MainActivity.onCreate() started");

        Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
            @Override
            public void uncaughtException(Thread thread, Throwable throwable) {
                Log.e(TAG, "FATAL CRASH on thread " + thread.getName() + ": " + throwable.getMessage(), throwable);
            }
        });

        try {
            java.io.File internalDir = getFilesDir();
            java.io.File externalDir = getExternalFilesDir(null);
            internalPath = (internalDir != null) ? internalDir.getAbsolutePath() : "/data/data/" + getPackageName() + "/files";
            externalPath = (externalDir != null) ? externalDir.getAbsolutePath() : internalPath;
            Log.i(TAG, "Storage paths configured: internal=" + internalPath + ", external=" + externalPath);
            if (internalDir != null && !internalDir.exists()) internalDir.mkdirs();
            if (externalDir != null && !externalDir.exists()) externalDir.mkdirs();
            // Extract libHeroesLore5.so and data.vfs.mp3 from assets if not already extracted
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
                        Log.w(TAG, "Asset extraction note (so): " + ex.getMessage());
                    }
                }

                java.io.File targetVfs = new java.io.File(internalDir, "data.vfs.mp3");
                if (!targetVfs.exists() || targetVfs.length() == 0) {
                    try (java.io.InputStream in = getAssets().open("data.vfs.mp3");
                         java.io.FileOutputStream out = new java.io.FileOutputStream(targetVfs)) {
                        byte[] buf = new byte[32768];
                        int len;
                        while ((len = in.read(buf)) > 0) {
                            out.write(buf, 0, len);
                        }
                        Log.i(TAG, "Extracted data.vfs.mp3 to " + targetVfs.getAbsolutePath() + " (" + targetVfs.length() + " bytes)");
                    } catch (Exception ex) {
                        Log.w(TAG, "Asset extraction note (vfs): " + ex.getMessage());
                    }
                }
                vfsFile = targetVfs;
                if (vfsFile.exists()) {
                    vfsAudioStream = new java.io.FileInputStream(vfsFile);
                }
            }
            android.media.AudioAttributes audioAttrs = new android.media.AudioAttributes.Builder()
                .setUsage(android.media.AudioAttributes.USAGE_GAME)
                .setContentType(android.media.AudioAttributes.CONTENT_TYPE_SONIFICATION)
                .build();
            soundPool = new android.media.SoundPool.Builder()
                .setMaxStreams(8)
                .setAudioAttributes(audioAttrs)
                .build();
            bgmPlayer = new android.media.MediaPlayer();
        } catch (Throwable t) {
            Log.e(TAG, "Storage dir init error: " + t.getMessage());
        }

        FrameLayout layout = new FrameLayout(this);
        glSurfaceView = new GLSurfaceView(this);
        glSurfaceView.setEGLConfigChooser(8, 8, 8, 8, 16, 0);
        glSurfaceView.setRenderer(this);
        glSurfaceView.setOnTouchListener(new android.view.View.OnTouchListener() {
            @Override
            public boolean onTouch(android.view.View v, android.view.MotionEvent event) {
                int actionMasked = event.getActionMasked();
                int pointerCount = event.getPointerCount();

                if (actionMasked == android.view.MotionEvent.ACTION_MOVE) {
                    // Dispatch MOVE for all active pointers (supports multi-touch D-Pad + Attack)
                    for (int i = 0; i < pointerCount; i++) {
                        int pid = event.getPointerId(i);
                        nativeOnTouchEvent(2, event.getX(i), event.getY(i), pid);
                    }
                    return true;
                }

                if (actionMasked == android.view.MotionEvent.ACTION_CANCEL) {
                    for (int i = 0; i < pointerCount; i++) {
                        int pid = event.getPointerId(i);
                        nativeOnTouchEvent(1, event.getX(i), event.getY(i), pid);
                    }
                    return true;
                }

                int pointerIndex = event.getActionIndex();
                int pointerId = event.getPointerId(pointerIndex);
                float x = event.getX(pointerIndex);
                float y = event.getY(pointerIndex);

                if (actionMasked == android.view.MotionEvent.ACTION_DOWN ||
                    actionMasked == android.view.MotionEvent.ACTION_POINTER_DOWN) {
                    nativeOnTouchEvent(0, x, y, pointerId);
                } else if (actionMasked == android.view.MotionEvent.ACTION_UP ||
                           actionMasked == android.view.MotionEvent.ACTION_POINTER_UP) {
                    nativeOnTouchEvent(1, x, y, pointerId);
                }
                return true;
            }
        });
        overlayTextView = new TextView(this);
        overlayTextView.setTextColor(Color.GREEN);
        overlayTextView.setBackgroundColor(Color.argb(180, 0, 0, 0));
        overlayTextView.setTextSize(11.0f);
        overlayTextView.setPadding(16, 12, 16, 12);
        overlayTextView.setText("HL5 ARM64 Runner v1.2 initializing...");
        FrameLayout.LayoutParams overlayParams = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            android.view.Gravity.BOTTOM | android.view.Gravity.LEFT);
        overlayParams.setMargins(20, 20, 20, 20);
        overlayTextView.setLayoutParams(overlayParams);
        overlayTextView.setOnClickListener(new android.view.View.OnClickListener() {
            @Override
            public void onClick(android.view.View v) {
                overlayTextView.setVisibility(android.view.View.GONE);
            }
        });

        aspectToggleBtn = new TextView(this);
        aspectToggleBtn.setTextColor(Color.GREEN);
        aspectToggleBtn.setBackgroundColor(Color.argb(220, 20, 20, 20));
        aspectToggleBtn.setTextSize(14.0f);
        aspectToggleBtn.setPadding(28, 16, 28, 16);
        aspectToggleBtn.setText("[ Rasio: 5:3 ]");
        aspectToggleBtn.setElevation(100.0f);
        FrameLayout.LayoutParams btnParams = new FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            FrameLayout.LayoutParams.WRAP_CONTENT,
            android.view.Gravity.TOP | android.view.Gravity.LEFT);
        btnParams.setMargins(24, 24, 24, 24);
        aspectToggleBtn.setLayoutParams(btnParams);
        aspectToggleBtn.setOnClickListener(new android.view.View.OnClickListener() {
            @Override
            public void onClick(android.view.View v) {
                isStretchMode = !isStretchMode;
                aspectToggleBtn.setText(isStretchMode ? "[ Rasio: FULL ]" : "[ Rasio: 5:3 ]");
                nativeSetStretchMode(isStretchMode);
            }
        });

        layout.addView(glSurfaceView);
        layout.addView(overlayTextView);
        layout.addView(aspectToggleBtn);
        aspectToggleBtn.bringToFront();
        setContentView(layout);
        hideSystemUI();
    }

    // --- Audio Bridge Callbacks (Invoked from native_runner.cpp via JNI) ---
    public synchronized int onPutMusicClip(long clipId, long offset, long length) {
        try {
            if (bgmPlayer == null) {
                bgmPlayer = new android.media.MediaPlayer();
            } else {
                bgmPlayer.reset();
            }
            if (vfsFile != null && vfsFile.exists()) {
                try (java.io.FileInputStream fis = new java.io.FileInputStream(vfsFile)) {
                    bgmPlayer.setDataSource(fis.getFD(), offset, length);
                    bgmPlayer.setVolume(musicVolume, musicVolume);
                    bgmPlayer.prepare();
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "onPutMusicClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized int onPlayMusicClip(long clipId, boolean loop) {
        try {
            if (bgmPlayer != null) {
                bgmPlayer.setLooping(loop);
                bgmPlayer.setVolume(musicVolume, musicVolume);
                bgmPlayer.start();
            }
        } catch (Exception e) {
            Log.w(TAG, "onPlayMusicClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized int onStopMusicClip(long clipId) {
        try {
            if (bgmPlayer != null && bgmPlayer.isPlaying()) {
                bgmPlayer.stop();
            }
        } catch (Exception e) {
            Log.w(TAG, "onStopMusicClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized int onPauseMusicClip(long clipId) {
        try {
            if (bgmPlayer != null && bgmPlayer.isPlaying()) {
                bgmPlayer.pause();
            }
        } catch (Exception e) {
            Log.w(TAG, "onPauseMusicClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized int onResumeMusicClip(long clipId) {
        try {
            if (bgmPlayer != null && !bgmPlayer.isPlaying()) {
                bgmPlayer.start();
            }
        } catch (Exception e) {
            Log.w(TAG, "onResumeMusicClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized int onFreeMusicClip(long clipId) {
        try {
            if (bgmPlayer != null) {
                bgmPlayer.reset();
            }
        } catch (Exception e) {
            Log.w(TAG, "onFreeMusicClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized void onSetMusicVolume(int vol) {
        if (vol <= 0) {
            musicVolume = 0.0f;
        } else {
            // Game default level 1 passes vol=10; boost curve so level 1 is clearly audible (0.6f..1.0f)
            musicVolume = Math.min(1.0f, Math.max(0.6f, vol / 100.0f));
        }
        try {
            if (bgmPlayer != null) {
                bgmPlayer.setVolume(musicVolume, musicVolume);
            }
        } catch (Exception ignored) {}
    }

    public synchronized int onPutSoundClip(long clipId, long offset, long length, int type) {
        try {
            if (soundPool != null && vfsFile != null && vfsFile.exists()) {
                Integer oldSoundId = soundPoolMap.remove(clipId);
                if (oldSoundId != null) {
                    soundPool.unload(oldSoundId);
                }
                try (java.io.FileInputStream fis = new java.io.FileInputStream(vfsFile)) {
                    int soundId = soundPool.load(fis.getFD(), offset, length, 1);
                    soundPoolMap.put(clipId, soundId);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "onPutSoundClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized int onPlaySoundClip(long clipId, boolean loop) {
        try {
            if (soundPool != null) {
                Integer soundId = soundPoolMap.get(clipId);
                if (soundId != null) {
                    int streamId = soundPool.play(soundId, soundVolume, soundVolume, 1, loop ? -1 : 0, 1.0f);
                    soundStreamMap.put(clipId, streamId);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "onPlaySoundClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized int onStopSoundClip(long clipId) {
        try {
            if (soundPool != null) {
                Integer streamId = soundStreamMap.get(clipId);
                if (streamId != null) {
                    soundPool.stop(streamId);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "onStopSoundClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized int onFreeSoundClip(long clipId) {
        try {
            if (soundPool != null) {
                Integer soundId = soundPoolMap.remove(clipId);
                if (soundId != null) {
                    soundPool.unload(soundId);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "onFreeSoundClip exception: " + e.getMessage());
        }
        return 0;
    }

    public synchronized void onSetSoundVolume(int vol) {
        if (vol <= 0) {
            soundVolume = 0.0f;
        } else {
            soundVolume = Math.min(1.0f, Math.max(0.6f, vol / 100.0f));
        }
    }
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemUI();
        }
    }

    private void hideSystemUI() {
        android.view.View decorView = getWindow().getDecorView();
        decorView.setSystemUiVisibility(
            android.view.View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            | android.view.View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
            | android.view.View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
            | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
            | android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
            | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
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
        nativeSetStretchMode(isStretchMode);
    }

    @Override
    public void onDrawFrame(GL10 gl) {
        frameCount++;
        final String frameStatus = nativeOnDrawFrame();
        if (frameCount == 180) {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    overlayTextView.setVisibility(android.view.View.GONE);
                }
            });
        }
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
        try {
            if (bgmPlayer != null && bgmPlayer.isPlaying()) {
                bgmWasPlayingOnPause = true;
                bgmPlayer.pause();
            } else {
                bgmWasPlayingOnPause = false;
            }
            if (soundPool != null) {
                soundPool.autoPause();
            }
        } catch (Exception ignored) {}
        if (glSurfaceView != null) glSurfaceView.onPause();
        nativeOnPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemUI();
        if (glSurfaceView != null) glSurfaceView.onResume();
        nativeOnResume();
        try {
            if (bgmPlayer != null && bgmWasPlayingOnPause) {
                bgmPlayer.start();
            }
            if (soundPool != null) {
                soundPool.autoResume();
            }
        } catch (Exception ignored) {}
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        try {
            if (bgmPlayer != null) {
                bgmPlayer.release();
                bgmPlayer = null;
            }
            if (soundPool != null) {
                soundPool.release();
                soundPool = null;
            }
            if (vfsAudioStream != null) {
                vfsAudioStream.close();
                vfsAudioStream = null;
            }
        } catch (Exception ignored) {}
        nativeOnDestroy();
    }
}
