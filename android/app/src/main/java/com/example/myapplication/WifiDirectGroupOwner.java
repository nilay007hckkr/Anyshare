package com.example.myapplication;

import android.annotation.SuppressLint;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.os.Build;

import java.util.Locale;

/**
 * Wi-Fi Direct Group Owner lifecycle, moved out of the original MainActivity
 * (Android branch) unchanged in behaviour.
 *
 * STATUS: not wired into the transfer flow. The TCP transport already works on
 * any shared network, including a Wi-Fi Direct group, but automatic group
 * creation + peer join (nmcli on Linux) is not implemented. To wire it in:
 *   1. add NEARBY_WIFI_DEVICES (API 33+, neverForLocation) and
 *      ACCESS_FINE_LOCATION (maxSdkVersion 32) back to the manifest and request them;
 *   2. call start() in receive mode and add ssid/psk/ip "192.168.49.1" to the
 *      handover payload (the parser already accepts ssid/psk).
 */
@SuppressLint("MissingPermission")
public final class WifiDirectGroupOwner {

    public interface Listener {
        void onGroupInfo(String ssid, String passphrase);
        void onStatus(String status);
    }

    private final Context context;
    private final Listener listener;
    private WifiP2pManager wifiP2pManager;
    private WifiP2pManager.Channel wifiDirectChannel;
    private BroadcastReceiver wifiDirectReceiver;

    public WifiDirectGroupOwner(Context context, Listener listener) {
        this.context = context;
        this.listener = listener;
    }

    /** Returns false if Wi-Fi Direct is not supported on this device. */
    public boolean initialize() {
        wifiP2pManager = (WifiP2pManager) context.getSystemService(Context.WIFI_P2P_SERVICE);
        if (wifiP2pManager == null) {
            listener.onStatus("Wi-Fi Direct: Not supported on this device");
            return false;
        }
        wifiDirectChannel = wifiP2pManager.initialize(context, context.getMainLooper(), null);

        IntentFilter filter = new IntentFilter();
        filter.addAction(WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION);
        filter.addAction(WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION);

        wifiDirectReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context ctx, Intent intent) {
                if (WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION.equals(intent.getAction())) {
                    WifiP2pInfo p2pInfo = intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_INFO);
                    if (p2pInfo != null && p2pInfo.groupFormed && p2pInfo.isGroupOwner) {
                        requestCurrentGroupInfo();
                    }
                }
            }
        };
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(wifiDirectReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            context.registerReceiver(wifiDirectReceiver, filter);
        }
        return true;
    }

    public void createGroup() {
        if (wifiP2pManager == null || wifiDirectChannel == null) return;
        wifiP2pManager.createGroup(wifiDirectChannel, new WifiP2pManager.ActionListener() {
            @Override
            public void onSuccess() {
                listener.onStatus("Group Owner initialized");
                requestCurrentGroupInfo();
            }

            @Override
            public void onFailure(int reason) {
                listener.onStatus(String.format(Locale.US, "GO creation status (Code %d)", reason));
                if (reason == WifiP2pManager.BUSY) requestCurrentGroupInfo();
            }
        });
    }

    private void requestCurrentGroupInfo() {
        if (wifiP2pManager == null || wifiDirectChannel == null) return;
        wifiP2pManager.requestGroupInfo(wifiDirectChannel, group -> {
            if (group != null) listener.onGroupInfo(group.getNetworkName(), group.getPassphrase());
        });
    }

    public void teardown() {
        if (wifiDirectReceiver != null) {
            context.unregisterReceiver(wifiDirectReceiver);
            wifiDirectReceiver = null;
        }
        if (wifiP2pManager != null && wifiDirectChannel != null) {
            wifiP2pManager.removeGroup(wifiDirectChannel, null);
        }
    }
}
