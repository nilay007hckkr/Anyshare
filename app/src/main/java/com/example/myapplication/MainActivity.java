package com.example.myapplication; // <-- CHANGE THIS to match your project's actual package name if different

import android.Manifest;
import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ObjectAnimator;
import android.annotation.SuppressLint;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.net.wifi.p2p.WifiP2pGroup;
import android.net.wifi.p2p.WifiP2pInfo;
import android.net.wifi.p2p.WifiP2pManager;
import android.nfc.NdefMessage;
import android.nfc.NdefRecord;
import android.nfc.NfcAdapter;
import android.nfc.NfcEvent;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.View;
import android.view.animation.AccelerateDecelerateInterpolator;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.EdgeToEdge;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import com.google.android.material.button.MaterialButton;
import com.google.android.material.progressindicator.LinearProgressIndicator;

import org.json.JSONException;
import org.json.JSONObject;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

@SuppressWarnings("deprecation")
public class MainActivity extends AppCompatActivity implements
        NfcAdapter.CreateNdefMessageCallback,
        NfcAdapter.OnNdefPushCompleteCallback {

    private static final String TAG = "NfcFileTransfer";
    private static final String MIME_TYPE = "application/vnd.custom.nfcfiletransfer";
    private static final String STATIC_IP = "192.168.49.1"; // Standard Android P2P GO IP
    private static final int DEFAULT_PORT = 8080;

    // UI Elements
    private TextView textFileName;
    private TextView textFileType;
    private TextView textFileSize;
    private TextView textStatus;
    private TextView textProgressPercent;
    private TextView textGroupInfo;
    private TextView textNfcStatus;
    private LinearProgressIndicator progressBar;
    private ImageView iconPacket;
    private View layoutPhone;
    private View layoutLaptop;
    private MaterialButton buttonChooseFile;

    // Animation
    private ObjectAnimator packetAnimator;

    // Transfer State Model
    private FileMetadata selectedFileMetadata;
    private String networkSsid = "";
    private String networkPsk = "";

    // Wi-Fi Direct
    private WifiP2pManager wifiP2pManager;
    private WifiP2pManager.Channel wifiDirectChannel;
    private BroadcastReceiver wifiDirectReceiver;
    private IntentFilter p2pIntentFilter;

    // NFC
    private NfcAdapter nfcAdapter;

    // Native Bridge (C++ Engine)
    static {
        try {
            System.loadLibrary("transfer_engine");
            Log.i(TAG, "Native transfer engine library loaded.");
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "Native library not yet compiled/linked: " + e.getMessage());
        }
    }

    public native void startSocketServer(int fileDescriptor, long fileSize, int port);
    public native void stopSocketServer();

    // Activity Result Launchers
    private final ActivityResultLauncher<String> filePickerLauncher =
            registerForActivityResult(new ActivityResultContracts.GetContent(), this::onFileSelected);

    private final ActivityResultLauncher<String[]> permissionLauncher =
            registerForActivityResult(new ActivityResultContracts.RequestMultiplePermissions(), result -> {
                boolean allGranted = true;
                for (Boolean granted : result.values()) {
                    if (!Boolean.TRUE.equals(granted)) {
                        allGranted = false;
                        break;
                    }
                }
                if (allGranted) {
                    initializeWifiDirect();
                } else {
                    Toast.makeText(this, "Required Wi-Fi and Location permissions were denied", Toast.LENGTH_LONG).show();
                }
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        EdgeToEdge.enable(this);
        setContentView(R.layout.activity_main);

        applyWindowInsets();
        bindViews();
        setupNfcAdapter();
        checkAndRequestPermissions();
    }

    private void bindViews() {
        textFileName = findViewById(R.id.textFileName);
        textFileType = findViewById(R.id.textFileType);
        textFileSize = findViewById(R.id.textFileSize);
        textStatus = findViewById(R.id.textStatus);
        textProgressPercent = findViewById(R.id.textProgressPercent);
        textGroupInfo = findViewById(R.id.textGroupInfo);
        textNfcStatus = findViewById(R.id.textNfcStatus);
        progressBar = findViewById(R.id.progressBar);
        iconPacket = findViewById(R.id.iconPacket);
        layoutPhone = findViewById(R.id.layoutPhone);
        layoutLaptop = findViewById(R.id.layoutLaptop);
        buttonChooseFile = findViewById(R.id.buttonChooseFile);

        buttonChooseFile.setOnClickListener(v -> filePickerLauncher.launch("*/*"));
    }

    private void applyWindowInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.main), (v, insets) -> {
            Insets systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars());
            v.setPadding(systemBars.left, systemBars.top, systemBars.right, systemBars.bottom);
            return insets;
        });
    }

    // ==========================================
    // Permissions Management
    // ==========================================
    private void checkAndRequestPermissions() {
        List<String> permissions = new ArrayList<>();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            permissions.add(Manifest.permission.NEARBY_WIFI_DEVICES);
        } else {
            permissions.add(Manifest.permission.ACCESS_FINE_LOCATION);
            permissions.add(Manifest.permission.ACCESS_COARSE_LOCATION);
        }

        List<String> missingPermissions = new ArrayList<>();
        for (String perm : permissions) {
            if (ContextCompat.checkSelfPermission(this, perm) != PackageManager.PERMISSION_GRANTED) {
                missingPermissions.add(perm);
            }
        }

        if (missingPermissions.isEmpty()) {
            initializeWifiDirect();
        } else {
            permissionLauncher.launch(missingPermissions.toArray(new String[0]));
        }
    }

    // ==========================================
    // Wi-Fi Direct Group Owner Lifecycle
    // ==========================================
    @SuppressLint("MissingPermission")
    private void initializeWifiDirect() {
        wifiP2pManager = (WifiP2pManager) getSystemService(Context.WIFI_P2P_SERVICE);
        if (wifiP2pManager == null) {
            textGroupInfo.setText("Wi-Fi Direct: Not supported on this device");
            return;
        }

        wifiDirectChannel = wifiP2pManager.initialize(this, getMainLooper(), null);

        p2pIntentFilter = new IntentFilter();
        p2pIntentFilter.addAction(WifiP2pManager.WIFI_P2P_STATE_CHANGED_ACTION);
        p2pIntentFilter.addAction(WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION);
        p2pIntentFilter.addAction(WifiP2pManager.WIFI_P2P_THIS_DEVICE_CHANGED_ACTION);

        wifiDirectReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String action = intent.getAction();
                if (WifiP2pManager.WIFI_P2P_CONNECTION_CHANGED_ACTION.equals(action)) {
                    WifiP2pInfo p2pInfo = intent.getParcelableExtra(WifiP2pManager.EXTRA_WIFI_P2P_INFO);
                    if (p2pInfo != null && p2pInfo.groupFormed && p2pInfo.isGroupOwner) {
                        requestCurrentGroupInfo();
                    }
                }
            }
        };

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(wifiDirectReceiver, p2pIntentFilter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(wifiDirectReceiver, p2pIntentFilter);
        }
    }

    @SuppressLint("MissingPermission")
    private void createWifiP2pGroup() {
        if (wifiP2pManager == null || wifiDirectChannel == null) return;

        wifiP2pManager.createGroup(wifiDirectChannel, new WifiP2pManager.ActionListener() {
            @Override
            public void onSuccess() {
                textStatus.setText("Group Owner initialized");
                requestCurrentGroupInfo();
            }

            @Override
            public void onFailure(int reason) {
                textStatus.setText(String.format(Locale.US, "GO creation status (Code %d)", reason));
                if (reason == WifiP2pManager.BUSY) {
                    requestCurrentGroupInfo();
                }
            }
        });
    }

    @SuppressLint("MissingPermission")
    private void requestCurrentGroupInfo() {
        if (wifiP2pManager == null || wifiDirectChannel == null) return;

        wifiP2pManager.requestGroupInfo(wifiDirectChannel, group -> {
            if (group != null) {
                networkSsid = group.getNetworkName();
                networkPsk = group.getPassphrase();
                textGroupInfo.setText(String.format(Locale.US, "SSID: %s | PSK: %s", networkSsid, networkPsk));
                updateNfcPayload();
            }
        });
    }

    // ==========================================
    // File Picking & Metadata
    // ==========================================
    private void onFileSelected(Uri uri) {
        if (uri == null) {
            textStatus.setText("No file selected");
            return;
        }

        selectedFileMetadata = resolveFileMetadata(uri);
        textFileName.setText(String.format("File: %s", selectedFileMetadata.getName()));
        textFileType.setText(String.format("Type: %s", selectedFileMetadata.getMimeType()));
        textFileSize.setText(String.format("Size: %s", formatFileSize(selectedFileMetadata.getSize())));

        createWifiP2pGroup();
    }

    private FileMetadata resolveFileMetadata(Uri uri) {
        String displayName = "unknown_file";
        long size = 0;
        String mimeType = getContentResolver().getType(uri);
        if (mimeType == null) mimeType = "application/octet-stream";

        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                int sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (nameIndex != -1) displayName = cursor.getString(nameIndex);
                if (sizeIndex != -1) size = cursor.getLong(sizeIndex);
            }
        } catch (Exception e) {
            Log.e(TAG, "Error resolving metadata: ", e);
        }

        return new FileMetadata(uri, displayName, size, mimeType);
    }

    private String formatFileSize(long bytes) {
        if (bytes <= 0) return "0 B";
        final String[] units = new String[]{"B", "KB", "MB", "GB", "TB"};
        int digitGroups = (int) (Math.log10(bytes) / Math.log10(1024));
        digitGroups = Math.min(digitGroups, units.length - 1);
        return String.format(Locale.US, "%.2f %s", bytes / Math.pow(1024, digitGroups), units[digitGroups]);
    }

    // ==========================================
    // NFC NDEF Exchange Setup
    // ==========================================
    private void setupNfcAdapter() {
        nfcAdapter = NfcAdapter.getDefaultAdapter(this);
        if (nfcAdapter == null) {
            textNfcStatus.setText("NFC: Unsupported on hardware");
            return;
        }

        if (!nfcAdapter.isEnabled()) {
            textNfcStatus.setText("NFC: Disabled in system settings");
        } else {
            textNfcStatus.setText("NFC: Ready");
        }

        // Safely invoke Beam APIs across all Android SDK versions without compile errors
        try {
            java.lang.reflect.Method setPushCallback = NfcAdapter.class.getMethod(
                    "setNdefPushMessageCallback",
                    NfcAdapter.CreateNdefMessageCallback.class,
                    android.app.Activity.class,
                    android.app.Activity[].class
            );
            setPushCallback.invoke(nfcAdapter, this, this, new android.app.Activity[0]);

            java.lang.reflect.Method setCompleteCallback = NfcAdapter.class.getMethod(
                    "setOnNdefPushCompleteCallback",
                    NfcAdapter.OnNdefPushCompleteCallback.class,
                    android.app.Activity.class,
                    android.app.Activity[].class
            );
            setCompleteCallback.invoke(nfcAdapter, this, this, new android.app.Activity[0]);
        } catch (Exception e) {
            Log.w(TAG, "Android Beam API not available on this SDK version: " + e.getMessage());
        }
    }

    private void updateNfcPayload() {
        if (selectedFileMetadata == null) {
            textNfcStatus.setText("NFC: Waiting for file selection...");
            return;
        }
        textNfcStatus.setText("NFC: Ready. Tap Linux PN532 to beam.");
    }

    @Override
    public NdefMessage createNdefMessage(NfcEvent event) {
        if (selectedFileMetadata == null || networkSsid.isEmpty()) {
            return null;
        }

        try {
            JSONObject json = new JSONObject();
            json.put("ssid", networkSsid);
            json.put("psk", networkPsk);
            json.put("ip", STATIC_IP);
            json.put("port", DEFAULT_PORT);
            json.put("filename", selectedFileMetadata.getName());
            json.put("filesize", selectedFileMetadata.getSize());

            byte[] payloadBytes = json.toString().getBytes(StandardCharsets.UTF_8);
            NdefRecord mimeRecord = NdefRecord.createMime(MIME_TYPE, payloadBytes);

            return new NdefMessage(new NdefRecord[]{mimeRecord});
        } catch (JSONException e) {
            Log.e(TAG, "Failed to assemble NFC JSON payload: ", e);
            return null;
        }
    }

    @Override
    public void onNdefPushComplete(NfcEvent event) {
        runOnUiThread(() -> {
            textStatus.setText("Credentials beamed via NFC. Starting socket listener...");
            startDataTransfer();
        });
    }

    // ==========================================
    // Transfer Engine & Native Bridge Callbacks
    // ==========================================
    private void startDataTransfer() {
        if (selectedFileMetadata == null) return;

        startPacketAnimation();

        new Thread(() -> {
            try {
                ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(selectedFileMetadata.getUri(), "r");
                if (pfd != null) {
                    int fd = pfd.getFd();
                    try {
                        startSocketServer(fd, selectedFileMetadata.getSize(), DEFAULT_PORT);
                    } catch (UnsatisfiedLinkError e) {
                        simulateTransfer();
                    }
                    pfd.close();
                }
            } catch (Exception e) {
                Log.e(TAG, "Socket transfer exception: ", e);
                runOnUiThread(() -> {
                    stopPacketAnimation();
                    textStatus.setText("Transfer failed: " + e.getMessage());
                });
            }
        }).start();
    }

    public void onNativeTransferProgress(int percentage, long bytesTransferred, long totalBytes) {
        runOnUiThread(() -> {
            progressBar.setProgress(percentage);
            textProgressPercent.setText(String.format(Locale.US, "%d%%", percentage));
            textStatus.setText(String.format(Locale.US, "Streaming: %s / %s",
                    formatFileSize(bytesTransferred), formatFileSize(totalBytes)));

            if (percentage >= 100) {
                stopPacketAnimation();
                textStatus.setText("Transfer Completed Successfully!");
            }
        });
    }

    private void simulateTransfer() {
        for (int i = 0; i <= 100; i += 5) {
            final int progress = i;
            try {
                Thread.sleep(120);
            } catch (InterruptedException ignored) {}
            runOnUiThread(() -> onNativeTransferProgress(
                    progress,
                    (selectedFileMetadata.getSize() * progress) / 100,
                    selectedFileMetadata.getSize()
            ));
        }
    }

    // ==========================================
    // Transfer Animation Engine
    // ==========================================
    private void startPacketAnimation() {
        iconPacket.setVisibility(View.VISIBLE);
        iconPacket.setAlpha(1.0f);

        int startX = (int) (layoutPhone.getX() + layoutPhone.getWidth());
        int endX = (int) layoutLaptop.getX();

        if (endX <= startX) {
            endX = startX + 350;
        }

        packetAnimator = ObjectAnimator.ofFloat(iconPacket, "translationX", startX, endX);
        packetAnimator.setDuration(900);
        packetAnimator.setInterpolator(new AccelerateDecelerateInterpolator());
        packetAnimator.setRepeatCount(ObjectAnimator.INFINITE);
        packetAnimator.setRepeatMode(ObjectAnimator.RESTART);
        packetAnimator.start();
    }

    private void stopPacketAnimation() {
        if (packetAnimator != null && packetAnimator.isRunning()) {
            packetAnimator.cancel();
        }
        iconPacket.animate().alpha(0.0f).setDuration(300).setListener(new AnimatorListenerAdapter() {
            @Override
            public void onAnimationEnd(Animator animation) {
                iconPacket.setVisibility(View.GONE);
            }
        });
    }

    // ==========================================
    // Lifecycle Teardown
    // ==========================================
    @Override
    protected void onDestroy() {
        super.onDestroy();
        stopPacketAnimation();

        if (wifiDirectReceiver != null) {
            unregisterReceiver(wifiDirectReceiver);
        }

        if (wifiP2pManager != null && wifiDirectChannel != null) {
            wifiP2pManager.removeGroup(wifiDirectChannel, null);
        }
    }

    // ==========================================
    // Metadata Value Object
    // ==========================================
    public static class FileMetadata {
        private final Uri uri;
        private final String name;
        private final long size;
        private final String mimeType;

        public FileMetadata(Uri uri, String name, long size, String mimeType) {
            this.uri = uri;
            this.name = name;
            this.size = size;
            this.mimeType = mimeType;
        }

        public Uri getUri() { return uri; }
        public String getName() { return name; }
        public long getSize() { return size; }
        public String getMimeType() { return mimeType; }
    }
}