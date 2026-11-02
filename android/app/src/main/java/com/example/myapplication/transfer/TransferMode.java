package com.example.myapplication.transfer;

import android.content.Context;
import android.content.SharedPreferences;

/**
 * How the *sender* finds the receiver. The payload always travels over a real
 * transport (Bluetooth RFCOMM or TCP); NFC is only used to exchange the
 * receiver's address + one-time token. The receiving side is identical in
 * every mode.
 */
public enum TransferMode {
    /** Default. Tap phones: reader mode reads the receiver's HCE payload. */
    HARDWARE_NFC("Hardware NFC", "NFC tap → payload via Bluetooth / Wi-Fi"),
    /** Pick a paired device (or an IP address) manually. No NFC involved. */
    BLUETOOTH_FALLBACK("Bluetooth fallback", "No NFC · paired device → Bluetooth RFCOMM"),
    /**
     * Presentation mode: shows the tap-to-share interaction without NFC
     * hardware. The tap is SIMULATED and labelled as such; the transfer itself
     * is real: Bluetooth to a paired device, or Wi-Fi TCP to a saved address.
     */
    DEMO_TAP("NFC-style demo", "SIMULATED tap · real Bluetooth / Wi-Fi transfer");

    public final String title;
    public final String summary;

    TransferMode(String title, String summary) {
        this.title = title;
        this.summary = summary;
    }

    private static final String PREFS = "anyshare";
    private static final String KEY_MODE = "transfer_mode";
    private static final String KEY_DEMO_PEER = "demo_peer_address";

    public static TransferMode load(Context context) {
        String name = prefs(context).getString(KEY_MODE, HARDWARE_NFC.name());
        try {
            return valueOf(name);
        } catch (IllegalArgumentException e) {
            return HARDWARE_NFC;
        }
    }

    public static void save(Context context, TransferMode mode) {
        prefs(context).edit().putString(KEY_MODE, mode.name()).apply();
    }

    /** Demo partner (remembered): a Bluetooth address, or "tcp:" + host:port. */
    public static String loadDemoPeer(Context context) {
        return prefs(context).getString(KEY_DEMO_PEER, null);
    }

    public static void saveDemoPeer(Context context, String address) {
        prefs(context).edit().putString(KEY_DEMO_PEER, address).apply();
    }

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }
}
