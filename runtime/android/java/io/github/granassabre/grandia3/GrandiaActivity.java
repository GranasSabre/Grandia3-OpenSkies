package io.github.granassabre.grandia3;

import android.app.NativeActivity;
import android.content.Intent;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.view.InputDevice;
import android.widget.Toast;

// The game is native (android_main in libgrandia3.so); this subclass only adds what needs Java:
// the system file picker for the player's disc image, opening the picked document again at later
// starts (the read permission is kept), and messages as toasts. Called from native threads (JNI,
// jni_android.cpp); everything that touches the UI goes to the UI thread.
public class GrandiaActivity extends NativeActivity {
    private static final int PICK = 1;
    private volatile boolean pickDone = false;
    private volatile String pickResult = null;

    public void pickDocument() {
        pickDone = false;
        pickResult = null;
        runOnUiThread(new Runnable() {
            public void run() {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                try {
                    startActivityForResult(i, PICK);
                } catch (Exception e) {
                    pickDone = true;
                }
            }
        });
    }

    public boolean pickDone() { return pickDone; }
    public String pickResult() { return pickResult; }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        if (request != PICK) {
            super.onActivityResult(request, result, data);
            return;
        }
        String r = null;
        if (result == RESULT_OK && data != null && data.getData() != null) {
            Uri uri = data.getData();
            try {
                // keep the read permission across restarts (the path is saved in grandia3.ini)
                getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (Exception e) {
            }
            r = uri.toString();
        }
        pickResult = r;
        pickDone = true;
    }

    // a file descriptor of the document (the caller owns it), -1 without access
    public int openUri(String uri) {
        try {
            ParcelFileDescriptor p = getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return p == null ? -1 : p.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    // the name of a connected game controller (gamepad or joystick, not a virtual device), null without one;
    // the touch controls stay hidden while one is connected (touch_android.cpp)
    public String gamepadName() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            // built-in helper devices of some phones report gamepad sources too (Xiaomi "uinput-xiaomi"):
            // only external controllers (Bluetooth, USB) count
            String n = d.getName();
            if (n != null && n.startsWith("uinput")) continue;
            if (android.os.Build.VERSION.SDK_INT >= 29 && !d.isExternal()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK)
                return d.getName();
        }
        return null;
    }

    public void showMessage(final String text) {
        runOnUiThread(new Runnable() {
            public void run() {
                Toast.makeText(GrandiaActivity.this, text, Toast.LENGTH_LONG).show();
            }
        });
    }
}
