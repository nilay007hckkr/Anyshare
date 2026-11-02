package com.example.myapplication.nfc;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.IntentFilter;
import android.nfc.NfcAdapter;
import android.nfc.Tag;
import android.nfc.tech.IsoDep;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.IOException;

/**
 * Sender side of the NFC tap: ISO-DEP SELECT of the AnyShare AID on the
 * receiving phone. Reports success only when an actual APDU exchange returned
 * a payload.
 *
 * Two ways to wait for a tap:
 * <ul>
 *   <li>{@link #enable}: reader mode. Most reliable phone-to-phone, but Android
 *       switches card emulation off, so a reader-only device (the ESP32 + Si512
 *       module on a laptop) cannot read this phone.</li>
 *   <li>{@link #enableDispatch}: foreground dispatch. The phone keeps polling for
 *       tags (another phone in Receive) <em>and</em> stays a card (HCE), so the laptop's
 *       module can read the "send" payload and fetch the files. Used by "Send by NFC tap".</li>
 * </ul>
 */
public final class NfcTapReader {
    private static final String TAG = "AnyShare.NFC";

    public interface Callback {   // main thread
        void onHandoverRead(String payloadJson);
        void onTapError(String message);
    }

    /** Why NFC cannot be used right now, or null if it can. */
    public static String unavailableReason(NfcAdapter adapter) {
        if (adapter == null) return "this phone has no NFC hardware";
        if (!adapter.isEnabled()) return "NFC is turned off in system settings";
        return null;
    }

    private final Activity activity;
    private final NfcAdapter adapter;
    private final Handler main = new Handler(Looper.getMainLooper());
    private boolean enabled;
    private boolean dispatching;
    private Callback dispatchCallback;

    public NfcTapReader(Activity activity, NfcAdapter adapter) {
        this.activity = activity;
        this.adapter = adapter;
    }

    public void enable(Callback callback) {
        if (adapter == null || enabled) return;
        int flags = NfcAdapter.FLAG_READER_NFC_A | NfcAdapter.FLAG_READER_NFC_B
                | NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK;
        adapter.enableReaderMode(activity, tag -> readTag(tag, callback), flags, null);
        enabled = true;
    }

    /** Foreground dispatch for ISO-DEP tags; card emulation stays on. Call while the activity is resumed. */
    public void enableDispatch(Callback callback) {
        if (adapter == null || dispatching) return;
        Intent intent = new Intent(activity, activity.getClass()).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        int flags = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ? PendingIntent.FLAG_MUTABLE : 0;   // NFC fills in extras
        PendingIntent pending = PendingIntent.getActivity(activity, 0, intent, flags);
        IntentFilter[] filters = {new IntentFilter(NfcAdapter.ACTION_TECH_DISCOVERED)};
        String[][] techs = {{IsoDep.class.getName()}};
        try {
            adapter.enableForegroundDispatch(activity, pending, filters, techs);
            dispatching = true;
            dispatchCallback = callback;
        } catch (IllegalStateException e) {   // not in the foreground: onResume enables it again
            Log.w(TAG, "Foreground dispatch not enabled: " + e.getMessage());
        }
    }

    /** From Activity.onNewIntent: returns true if it was a tag for us (read on a worker thread). */
    public boolean handleIntent(Intent intent) {
        if (!dispatching || intent == null) return false;
        Tag tag = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                ? intent.getParcelableExtra(NfcAdapter.EXTRA_TAG, Tag.class)
                : legacyTag(intent);
        if (tag == null) return false;
        Callback callback = dispatchCallback;
        new Thread(() -> readTag(tag, callback), "anyshare-nfc-read").start();   // transceive blocks
        return true;
    }

    @SuppressWarnings("deprecation")
    private static Tag legacyTag(Intent intent) {
        return intent.getParcelableExtra(NfcAdapter.EXTRA_TAG);
    }

    public void disable() {
        if (adapter == null) return;
        if (enabled) {
            adapter.disableReaderMode(activity);
            enabled = false;
        }
        if (dispatching) {
            try {
                adapter.disableForegroundDispatch(activity);
            } catch (IllegalStateException e) {   // already paused: Android dropped it anyway
                Log.d(TAG, "Foreground dispatch already off: " + e.getMessage());
            }
            dispatching = false;
        }
    }

    // Runs on an NFC binder thread or our worker thread.
    private void readTag(Tag tag, Callback callback) {
        IsoDep iso = IsoDep.get(tag);
        if (iso == null) {
            main.post(() -> callback.onTapError("That is not a phone running AnyShare (no ISO-DEP)"));
            return;
        }
        try {
            iso.connect();
            iso.setTimeout(3000);
            byte[] response = iso.transceive(ApduProtocol.selectAidCommand());
            String json = ApduProtocol.parseSelectResponse(response);
            Log.i(TAG, "Physical NFC tap: read " + json.length() + "-byte handover payload");
            main.post(() -> callback.onHandoverRead(json));
        } catch (IOException | IllegalArgumentException e) {
            String message = e.getMessage() != null ? e.getMessage() : e.toString();
            main.post(() -> callback.onTapError("NFC tap failed: " + message + " — hold the phones together again"));
        } finally {
            try { iso.close(); } catch (IOException ignored) {}
        }
    }
}
