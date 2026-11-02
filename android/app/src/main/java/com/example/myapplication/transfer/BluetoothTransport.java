package com.example.myapplication.transfer;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.os.Build;
import android.util.Log;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.UUID;

/**
 * Bluetooth Classic RFCOMM transport. Uses *secure* (authenticated +
 * encrypted) sockets, so unpaired devices are asked to pair first.
 *
 * Callers must hold BLUETOOTH_CONNECT on Android 12+ (checked by MainActivity).
 */
@SuppressLint("MissingPermission")
public final class BluetoothTransport {
    private static final String TAG = "AnyShare.BT";

    /** Must match protocol::kBluetoothServiceUuid in core-engine/include/Protocol.hpp. */
    public static final UUID SERVICE_UUID = UUID.fromString("a9f3c6e2-5b1d-4e8a-9c47-2d6b8e1f0a35");
    public static final String SERVICE_NAME = "AnyShare";

    private final BluetoothAdapter adapter;

    public BluetoothTransport(Context context) {
        BluetoothManager manager = context.getSystemService(BluetoothManager.class);
        adapter = manager != null ? manager.getAdapter() : null;
    }

    public boolean isSupported() {
        return adapter != null;
    }

    public boolean isEnabled() {
        return adapter != null && adapter.isEnabled();
    }

    /** Name other devices see (Android hides its own MAC from apps since 6.0). */
    public String localName() {
        String name = adapter != null ? adapter.getName() : null;
        return name != null ? name : Build.MODEL;
    }

    public List<BluetoothDevice> bondedDevices() {
        List<BluetoothDevice> list = new ArrayList<>();
        if (adapter == null) return list;
        Set<BluetoothDevice> bonded = adapter.getBondedDevices();
        if (bonded != null) list.addAll(bonded);
        return list;
    }

    public BluetoothDevice deviceFor(String address) {
        return adapter.getRemoteDevice(address);
    }

    public static String displayName(BluetoothDevice device) {
        String name = null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) name = device.getAlias();
        if (name == null) name = device.getName();
        return name != null ? name : device.getAddress();
    }

    public BluetoothServerSocket listen() throws IOException {
        if (!isEnabled()) throw new IOException("Bluetooth is off");
        return adapter.listenUsingRfcommWithServiceRecord(SERVICE_NAME, SERVICE_UUID);
    }

    /** Blocking connect to the AnyShare service on {@code device}. */
    public BluetoothSocket connect(BluetoothDevice device) throws IOException {
        if (!isEnabled()) throw new IOException("Bluetooth is off");
        // A running discovery slows connections down drastically. AnyShare never starts one, and on
        // Android 12+ cancelDiscovery() needs BLUETOOTH_SCAN, which the app does not request
        // (verified on a Galaxy M34: SecurityException). So: best effort only.
        try {
            adapter.cancelDiscovery();
        } catch (SecurityException e) {
            Log.d(TAG, "cancelDiscovery not allowed (no BLUETOOTH_SCAN); connecting anyway");
        }
        BluetoothSocket socket = device.createRfcommSocketToServiceRecord(SERVICE_UUID);
        try {
            socket.connect();
            return socket;
        } catch (IOException e) {
            try { socket.close(); } catch (IOException ignored) {}
            throw new IOException("Bluetooth connection to " + displayName(device) + " failed: " + e.getMessage(), e);
        }
    }

    /**
     * A tap payload from an Android receiver contains its Bluetooth *name*, not
     * its address. Paired devices with that name are tried first, then the rest;
     * only a device actually running the AnyShare service accepts.
     */
    public List<BluetoothDevice> candidatesFor(String receiverName) {
        List<BluetoothDevice> matches = new ArrayList<>();
        List<BluetoothDevice> others = new ArrayList<>();
        for (BluetoothDevice d : bondedDevices()) {
            if (receiverName != null && (receiverName.equals(d.getName()) || receiverName.equals(displayName(d)))) {
                matches.add(d);
            } else {
                others.add(d);
            }
        }
        Log.i(TAG, "Candidates for '" + receiverName + "': " + matches.size() + " by name, " + others.size() + " others");
        matches.addAll(others);
        return matches;
    }
}
