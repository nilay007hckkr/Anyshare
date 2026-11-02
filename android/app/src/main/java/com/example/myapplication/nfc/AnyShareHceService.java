package com.example.myapplication.nfc;

import android.nfc.cardemulation.HostApduService;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * Receiver side of the NFC tap (Host Card Emulation). Replaces Android Beam,
 * which was removed from Android 10+ and could never fire on current phones.
 *
 * While the app is in receive mode it serves the receiver's handover JSON
 * (transport, Bluetooth name / IP:port, one-time token) to any reader that
 * SELECTs the AnyShare AID: another phone in reader mode, or a PN532 / ESP32
 * reader on Linux. While waiting in "Send by NFC tap" it serves {"role":"send",...}
 * instead: the reader connects back and fetches the files.
 * Otherwise it answers 6A82 and reveals nothing.
 */
public class AnyShareHceService extends HostApduService {
    private static final String TAG = "AnyShare.HCE";

    /** Notified on the main thread when a reader actually read our payload. */
    public interface TapListener {
        void onPayloadRead();
    }

    private static volatile String payload;
    private static volatile TapListener listener;
    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    public static void setPayload(String json, TapListener tapListener) {
        payload = json;
        listener = tapListener;
    }

    public static void clearPayload() {
        payload = null;
        listener = null;
    }

    @Override
    public byte[] processCommandApdu(byte[] apdu, Bundle extras) {
        if (!ApduProtocol.isSelectAid(apdu)) return ApduProtocol.SW_INS_NOT_SUPPORTED;
        String current = payload;
        if (current == null) {
            Log.i(TAG, "SELECT received but not in receive mode");
            return ApduProtocol.SW_FILE_NOT_FOUND;
        }
        Log.i(TAG, "Physical NFC tap: serving handover payload (" + current.length() + " bytes)");
        TapListener l = listener;
        if (l != null) MAIN.post(l::onPayloadRead);
        return ApduProtocol.buildSelectResponse(current);
    }

    @Override
    public void onDeactivated(int reason) {
        Log.d(TAG, "Deactivated, reason " + reason);
    }
}
