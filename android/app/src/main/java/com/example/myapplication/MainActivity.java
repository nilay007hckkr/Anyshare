package com.example.myapplication;

import android.Manifest;
import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ObjectAnimator;
import android.animation.PropertyValuesHolder;
import android.animation.ValueAnimator;
import android.annotation.SuppressLint;
import android.app.DownloadManager;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothClass;
import android.bluetooth.BluetoothDevice;
import android.content.ActivityNotFoundException;
import android.content.ComponentName;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetFileDescriptor;
import android.content.res.ColorStateList;
import android.database.Cursor;
import android.net.Uri;
import android.nfc.NfcAdapter;
import android.nfc.cardemulation.CardEmulation;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.InputType;
import android.util.Log;
import android.view.View;
import android.view.animation.AccelerateDecelerateInterpolator;
import android.widget.ArrayAdapter;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.EdgeToEdge;
import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.ContextCompat;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import com.example.myapplication.nfc.AnyShareHceService;
import com.example.myapplication.nfc.NfcTapReader;
import com.example.myapplication.transfer.BluetoothTransport;
import com.example.myapplication.transfer.LanDiscovery;
import com.example.myapplication.transfer.NativeBridge;
import com.example.myapplication.transfer.TransferController;
import com.example.myapplication.transfer.TransferController.OutgoingItem;
import com.example.myapplication.transfer.TransferController.Target;
import com.example.myapplication.transfer.TransferMode;
import com.google.android.material.button.MaterialButton;
import com.google.android.material.button.MaterialButtonToggleGroup;
import com.google.android.material.card.MaterialCardView;
import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import com.google.android.material.progressindicator.CircularProgressIndicator;
import com.google.android.material.progressindicator.LinearProgressIndicator;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * AnyShare main screen: Send and Receive tabs plus a shared transfer card.
 *
 * Sender, per {@link TransferMode}:
 *   HARDWARE_NFC       physical tap only, either way round: this phone reads a receiving phone's HCE
 *                      payload, or a laptop's NFC module reads this phone's "send" payload and fetches.
 *   BLUETOOTH_FALLBACK pick a paired device or type an IP address; no NFC.
 *   DEMO_TAP           tap the animation card to SIMULATE a tap; real Bluetooth or Wi-Fi transfer.
 * In every mode the Send tab also lists receivers found by name on the Wi-Fi
 * (LanDiscovery); tapping one sends over Wi-Fi and is labelled as such.
 * Receiver (all modes): Bluetooth RFCOMM + Wi-Fi TCP servers, HCE payload for taps.
 *
 * The mode chip and transport line always state what is really happening.
 * Transfer state colours match the ESP32 status LED: sending purple,
 * receiving red, done green, error amber.
 */
@SuppressLint("MissingPermission")   // Bluetooth calls happen only after withBluetooth() checks
public class MainActivity extends AppCompatActivity implements TransferController.Listener {

    private static final String TAG = "AnyShare";
    private static final long DISCOVERY_INTERVAL_MS = 4000;
    private static final int DISCOVERY_WINDOW_MS = 1200;
    private static final long NEARBY_EXPIRY_MS = 12000;

    // UI Elements
    private TextView textSubtitle;
    private TextView textFileName;
    private TextView textFileSize;
    private TextView textStatus;
    private TextView textMode;
    private TextView textDemoHint;
    private TextView textPeer;
    private TextView textNearbyEmpty;
    private TextView textReceiveName;
    private TextView textReceiveState;
    private TextView textNfcStatus;
    private TextView textState;
    private TextView textProgressPercent;
    private TextView textTransferDetail;
    private TextView textSpeed;
    private TextView textTransport;
    private LinearProgressIndicator progressBar;
    private CircularProgressIndicator progressNearby;
    private ImageView iconPacket;
    private View layoutPhone;
    private View layoutLaptop;
    private View panelSend;
    private View panelReceive;
    private View pulseRing;
    private View dotState;
    private LinearLayout fileListContainer;
    private LinearLayout nearbyContainer;
    private MaterialCardView cardStage;
    private MaterialCardView cardTransfer;
    private MaterialButtonToggleGroup tabGroup;
    private MaterialButton buttonChooseFile;
    private MaterialButton buttonSend;
    private MaterialButton buttonReceive;
    private MaterialButton buttonCancel;
    private MaterialButton buttonOpenDownloads;

    // Animation
    private ValueAnimator packetAnimator;
    private ImageView iconPeer;
    // Who the next transfer goes to, for the animation (set by whoever starts the send).
    private String nextPeerName;
    private boolean nextPeerIsComputer;
    private ObjectAnimator pulseAnimator;

    // State
    private final List<OutgoingItem> selectedFiles = new ArrayList<>();
    private TransferMode mode;
    private boolean waitingForTap;
    private boolean resumed;
    private boolean transferActive;
    private boolean transferSending;
    private long transferStartMs;

    // Nearby receivers (Send tab), keyed by name + port, in order of discovery
    private static final class NearbyEntry {
        LanDiscovery.Peer peer;
        long lastSeenMs;
    }
    private final Map<String, NearbyEntry> nearby = new LinkedHashMap<>();
    private final Handler ui = new Handler(Looper.getMainLooper());
    private boolean discoveryInFlight;
    private boolean discoveryRoundDone;
    private final Runnable discoveryTick = new Runnable() {
        @Override
        public void run() {
            runDiscoveryOnce();
            ui.postDelayed(this, DISCOVERY_INTERVAL_MS);
        }
    };

    // Hardware + engine
    private NfcAdapter nfcAdapter;
    private NfcTapReader tapReader;
    private BluetoothTransport bluetooth;
    private TransferController controller;

    // Bluetooth permission / enable flow
    private interface BluetoothAction { void run(boolean bluetoothUsable); }
    private BluetoothAction pendingBluetoothAction;
    private boolean pendingBluetoothRequired;

    // Activity Result Launchers
    private final ActivityResultLauncher<String[]> filePickerLauncher =
            registerForActivityResult(new ActivityResultContracts.OpenMultipleDocuments(), this::onFilesSelected);

    private final ActivityResultLauncher<String[]> permissionLauncher =
            registerForActivityResult(new ActivityResultContracts.RequestMultiplePermissions(), result -> {
                boolean granted = !result.containsValue(Boolean.FALSE);
                BluetoothAction action = pendingBluetoothAction;
                pendingBluetoothAction = null;
                if (action == null) return;
                if (granted) {
                    withBluetooth(pendingBluetoothRequired, action);
                } else if (pendingBluetoothRequired) {
                    showError("Bluetooth permission denied — Bluetooth transfers are not possible.");
                } else {
                    action.run(false);
                }
            });

    private final ActivityResultLauncher<Intent> enableBluetoothLauncher =
            registerForActivityResult(new ActivityResultContracts.StartActivityForResult(), result -> {
                BluetoothAction action = pendingBluetoothAction;
                pendingBluetoothAction = null;
                if (action == null) return;
                if (bluetooth.isEnabled()) {
                    action.run(true);
                } else if (pendingBluetoothRequired) {
                    showError("Bluetooth is off — turn it on to transfer over Bluetooth.");
                } else {
                    action.run(false);
                }
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        EdgeToEdge.enable(this);
        setContentView(R.layout.activity_main);

        applyWindowInsets();
        bindViews();

        nfcAdapter = NfcAdapter.getDefaultAdapter(this);
        tapReader = new NfcTapReader(this, nfcAdapter);
        bluetooth = new BluetoothTransport(this);
        controller = new TransferController(this, bluetooth, this);
        mode = TransferMode.load(this);
        if (mode == TransferMode.HARDWARE_NFC && nfcAdapter == null) {
            mode = TransferMode.BLUETOOTH_FALLBACK;   // no NFC chip: start in the mode that works
        }

        String name = controller.deviceName();
        textSubtitle.setText("This phone: " + name);
        textReceiveName.setText(name);
        applyMode();
        refreshNfcStatus();
        updateSelectionCard();

        if (!NativeBridge.isAvailable()) {
            // No fallback simulation: without the engine nothing can be transferred.
            showError("Native transfer engine failed to load: " + NativeBridge.loadError());
            buttonSend.setEnabled(false);
            buttonReceive.setEnabled(false);
        }
    }

    private void bindViews() {
        textSubtitle = findViewById(R.id.textSubtitle);
        textFileName = findViewById(R.id.textFileName);
        textFileSize = findViewById(R.id.textFileSize);
        textStatus = findViewById(R.id.textStatus);
        textMode = findViewById(R.id.textMode);
        textDemoHint = findViewById(R.id.textDemoHint);
        textPeer = findViewById(R.id.textPeer);
        textNearbyEmpty = findViewById(R.id.textNearbyEmpty);
        textReceiveName = findViewById(R.id.textReceiveName);
        textReceiveState = findViewById(R.id.textReceiveState);
        textNfcStatus = findViewById(R.id.textNfcStatus);
        textState = findViewById(R.id.textState);
        textProgressPercent = findViewById(R.id.textProgressPercent);
        textTransferDetail = findViewById(R.id.textTransferDetail);
        textSpeed = findViewById(R.id.textSpeed);
        textTransport = findViewById(R.id.textTransport);
        progressBar = findViewById(R.id.progressBar);
        progressNearby = findViewById(R.id.progressNearby);
        iconPacket = findViewById(R.id.iconPacket);
        layoutPhone = findViewById(R.id.layoutPhone);
        layoutLaptop = findViewById(R.id.layoutLaptop);
        iconPeer = findViewById(R.id.iconLaptop);
        panelSend = findViewById(R.id.panelSend);
        panelReceive = findViewById(R.id.panelReceive);
        pulseRing = findViewById(R.id.pulseRing);
        dotState = findViewById(R.id.dotState);
        fileListContainer = findViewById(R.id.fileListContainer);
        nearbyContainer = findViewById(R.id.nearbyContainer);
        cardStage = findViewById(R.id.cardStage);
        cardTransfer = findViewById(R.id.cardTransfer);
        tabGroup = findViewById(R.id.tabGroup);
        buttonChooseFile = findViewById(R.id.buttonChooseFile);
        buttonSend = findViewById(R.id.buttonSend);
        buttonReceive = findViewById(R.id.buttonReceive);
        buttonCancel = findViewById(R.id.buttonCancel);
        buttonOpenDownloads = findViewById(R.id.buttonOpenDownloads);
        MaterialButton buttonRefresh = findViewById(R.id.buttonRefresh);
        View modeHotspot = findViewById(R.id.modeHotspot);

        buttonChooseFile.setOnClickListener(v -> filePickerLauncher.launch(new String[]{"*/*"}));
        buttonSend.setOnClickListener(v -> onSendPressed());
        buttonReceive.setOnClickListener(v -> toggleReceive());
        buttonCancel.setOnClickListener(v -> cancelEverything());
        buttonRefresh.setOnClickListener(v -> runDiscoveryOnce());
        buttonOpenDownloads.setOnClickListener(v -> openDownloads());

        tabGroup.addOnButtonCheckedListener((group, checkedId, isChecked) -> {
            if (isChecked) showTab(checkedId == R.id.tabReceive);
        });

        // Transfer-mode control: long-press the transparent area at the end of
        // the header (or tap the mode chip). Opens an explicit, labelled dialog.
        modeHotspot.setOnLongClickListener(v -> {
            showModeDialog();
            return true;
        });
        textMode.setOnClickListener(v -> showModeDialog());

        cardStage.setOnClickListener(v -> {
            if (mode == TransferMode.DEMO_TAP) onDemoTap();
        });
        cardStage.setOnLongClickListener(v -> {
            if (mode != TransferMode.DEMO_TAP) return false;
            withBluetooth(false, usable -> showTargetPicker(true, usable));
            return true;
        });
    }

    private void applyWindowInsets() {
        ViewCompat.setOnApplyWindowInsetsListener(findViewById(R.id.main), (v, insets) -> {
            Insets systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars());
            v.setPadding(systemBars.left, systemBars.top, systemBars.right, systemBars.bottom);
            return insets;
        });
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        refreshNfcStatus();
        if (waitingForTap) tapReader.enableDispatch(tapCallback);
        setPreferredHceService(controller.isReceiving() || waitingForTap);
        updateDiscoveryLoop();
    }

    @Override
    protected void onPause() {
        super.onPause();
        resumed = false;
        updateDiscoveryLoop();
        tapReader.disable();   // reader mode is only allowed in the foreground
        setPreferredHceService(false);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        ui.removeCallbacksAndMessages(null);
        stopPacketAnimation();
        stopPulse();
        AnyShareHceService.clearPayload();
        controller.shutdown();
    }

    // ==========================================
    // Tabs
    // ==========================================
    private boolean isSendTabShown() {
        return panelSend.getVisibility() == View.VISIBLE;
    }

    private void showTab(boolean receive) {
        panelSend.setVisibility(receive ? View.GONE : View.VISIBLE);
        panelReceive.setVisibility(receive ? View.VISIBLE : View.GONE);
        updateDiscoveryLoop();
        refreshStage();
    }

    // ==========================================
    // Transfer mode (hardware NFC / Bluetooth fallback / demo)
    // ==========================================
    private void showModeDialog() {
        TransferMode[] modes = TransferMode.values();
        String[] labels = new String[modes.length];
        for (int i = 0; i < modes.length; i++) labels[i] = modes[i].title + "\n" + modes[i].summary;
        new MaterialAlertDialogBuilder(this)
                .setTitle("Transfer mode (sender)")
                .setSingleChoiceItems(labels, mode.ordinal(), (dialog, which) -> {
                    setMode(modes[which]);
                    dialog.dismiss();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void setMode(TransferMode newMode) {
        if (waitingForTap) stopWaitingForTap();
        mode = newMode;
        TransferMode.save(this, newMode);
        applyMode();
    }

    private void applyMode() {
        textMode.setText(String.format("Mode: %s · %s", mode.title, mode.summary));
        textDemoHint.setVisibility(mode == TransferMode.DEMO_TAP ? View.VISIBLE : View.GONE);
        switch (mode) {
            case HARDWARE_NFC: buttonSend.setText(R.string.send_nfc); break;
            case BLUETOOTH_FALLBACK: buttonSend.setText(R.string.send_other_ways); break;
            case DEMO_TAP: buttonSend.setText(R.string.send_demo_partner); break;
        }
        refreshStage();
        updateSendButton();
        Log.i(TAG, "Transfer mode: " + mode.name());
    }

    /** The device-to-device stage is the demo "tap" card, and the animation during a transfer. */
    private void refreshStage() {
        boolean demoHere = mode == TransferMode.DEMO_TAP && isSendTabShown();
        cardStage.setVisibility(demoHere || transferActive ? View.VISIBLE : View.GONE);
    }

    private void refreshNfcStatus() {
        // NFC line is only shown when NFC can actually be used; otherwise it is hidden.
        boolean usable = NfcTapReader.unavailableReason(nfcAdapter) == null;
        textNfcStatus.setVisibility(usable ? View.VISIBLE : View.GONE);
        if (usable && !waitingForTap && !controller.isReceiving()) {
            textNfcStatus.setText("NFC: ready (hardware)");
        }
    }

    // ==========================================
    // File Picking & Metadata
    // ==========================================
    /** "Add files" adds to the selection; the same file is never listed twice. */
    private void onFilesSelected(List<Uri> uris) {
        if (uris == null || uris.isEmpty()) return;
        List<String> skipped = new ArrayList<>();
        for (Uri uri : uris) {
            boolean already = false;
            for (OutgoingItem f : selectedFiles) already |= f.uri.equals(uri);
            if (already) continue;
            OutgoingItem item = resolveFileMetadata(uri);
            if (item.size < 0) {
                skipped.add(item.name);   // protocol needs the exact size up front
            } else {
                selectedFiles.add(item);
            }
        }
        if (!skipped.isEmpty()) showError("Skipped (size unknown): " + String.join(", ", skipped));
        updateSelectionCard();
    }

    private void updateSelectionCard() {
        fileListContainer.removeAllViews();
        long total = 0;
        for (OutgoingItem f : selectedFiles) {
            total += f.size;
            View row = getLayoutInflater().inflate(R.layout.item_file, fileListContainer, false);
            ((TextView) row.findViewById(R.id.fileBadge)).setText(fileBadge(f));
            ((TextView) row.findViewById(R.id.fileName)).setText(f.name);
            ((TextView) row.findViewById(R.id.fileSize)).setText(formatFileSize(f.size));
            row.findViewById(R.id.fileRemove).setOnClickListener(v -> {
                if (controller.isBusy()) return;
                selectedFiles.remove(f);
                updateSelectionCard();
            });
            fileListContainer.addView(row);
        }
        textFileName.setVisibility(selectedFiles.isEmpty() ? View.VISIBLE : View.GONE);
        textFileSize.setText(selectedFiles.isEmpty() ? ""
                : String.format(Locale.US, "%d file%s · %s", selectedFiles.size(),
                        selectedFiles.size() == 1 ? "" : "s", formatFileSize(total)));
        updateSendButton();
    }

    /** Short type tag for a file row ("IMG", "PDF", ...), from the MIME type or the extension. */
    private static String fileBadge(OutgoingItem f) {
        String mime = f.mimeType == null ? "" : f.mimeType;
        if (mime.startsWith("image/")) return "IMG";
        if (mime.startsWith("video/")) return "VID";
        if (mime.startsWith("audio/")) return "AUD";
        if (mime.equals("application/pdf")) return "PDF";
        if (mime.equals("application/vnd.android.package-archive")) return "APK";
        if (mime.contains("zip") || mime.contains("compressed")) return "ZIP";
        if (mime.startsWith("text/")) return "TXT";
        int dot = f.name.lastIndexOf('.');
        if (dot >= 0 && dot < f.name.length() - 1) {
            String ext = f.name.substring(dot + 1).toUpperCase(Locale.US);
            return ext.length() > 4 ? ext.substring(0, 4) : ext;
        }
        return "FILE";
    }

    private OutgoingItem resolveFileMetadata(Uri uri) {
        String displayName = "unknown_file";
        long size = -1;
        String mimeType = getContentResolver().getType(uri);
        if (mimeType == null) mimeType = "application/octet-stream";

        try (Cursor cursor = getContentResolver().query(uri, null, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                int sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (nameIndex != -1) displayName = cursor.getString(nameIndex);
                if (sizeIndex != -1 && !cursor.isNull(sizeIndex)) size = cursor.getLong(sizeIndex);
            }
        } catch (Exception e) {
            Log.e(TAG, "Error resolving metadata: ", e);
        }
        if (size < 0) {
            try (AssetFileDescriptor afd = getContentResolver().openAssetFileDescriptor(uri, "r")) {
                if (afd != null) size = afd.getLength();   // -1 if still unknown
            } catch (Exception e) {
                Log.w(TAG, "No length for " + uri, e);
            }
        }
        return new OutgoingItem(uri, displayName, size, mimeType);
    }

    private String formatFileSize(long bytes) {
        if (bytes <= 0) return "0 B";
        final String[] units = new String[]{"B", "KB", "MB", "GB", "TB"};
        int digitGroups = (int) (Math.log10(bytes) / Math.log10(1024));
        digitGroups = Math.min(digitGroups, units.length - 1);
        return String.format(Locale.US, digitGroups == 0 ? "%.0f %s" : "%.1f %s",
                bytes / Math.pow(1024, digitGroups), units[digitGroups]);
    }

    private static String formatDuration(long seconds) {
        if (seconds < 60) return seconds + " s";
        if (seconds < 3600) return String.format(Locale.US, "%d min %02d s", seconds / 60, seconds % 60);
        return String.format(Locale.US, "%d h %02d min", seconds / 3600, (seconds % 3600) / 60);
    }

    // ==========================================
    // Nearby devices (found by name on the Wi-Fi)
    // ==========================================
    /** Discovery runs only while the Send tab is on screen. */
    private void updateDiscoveryLoop() {
        ui.removeCallbacks(discoveryTick);
        if (resumed && isSendTabShown() && !isFinishing()) {
            ui.post(discoveryTick);
        }
    }

    private void runDiscoveryOnce() {
        if (discoveryInFlight || isFinishing()) return;
        discoveryInFlight = true;
        progressNearby.setVisibility(View.VISIBLE);
        // While this phone is receiving it answers discovery too: do not list ourselves.
        final String self = controller.isReceiving() ? controller.deviceName() : null;
        new Thread(() -> {
            List<LanDiscovery.Peer> found = LanDiscovery.discover(DISCOVERY_WINDOW_MS, LanDiscovery.PORT,
                    LanDiscovery.broadcastTargets(), null);
            ui.post(() -> {
                discoveryInFlight = false;
                if (isFinishing()) return;
                progressNearby.setVisibility(View.GONE);
                mergeNearby(found, self);
            });
        }, "anyshare-nearby").start();
    }

    private void mergeNearby(List<LanDiscovery.Peer> found, String self) {
        long now = SystemClock.elapsedRealtime();
        for (LanDiscovery.Peer p : found) {
            if (p.name.equals(self)) continue;
            String key = p.name + "|" + p.port;
            NearbyEntry e = nearby.get(key);
            if (e == null) {
                e = new NearbyEntry();
                nearby.put(key, e);
            }
            e.peer = p;
            e.lastSeenMs = now;
        }
        for (Iterator<NearbyEntry> it = nearby.values().iterator(); it.hasNext(); ) {
            if (now - it.next().lastSeenMs > NEARBY_EXPIRY_MS) it.remove();
        }
        discoveryRoundDone = true;
        renderNearby();
    }

    private void renderNearby() {
        nearbyContainer.removeAllViews();
        for (NearbyEntry e : nearby.values()) {
            LanDiscovery.Peer p = e.peer;
            View row = getLayoutInflater().inflate(R.layout.item_device, nearbyContainer, false);
            ((ImageView) row.findViewById(R.id.deviceIcon)).setImageResource(
                    "linux".equals(p.os) ? R.drawable.ic_laptop : R.drawable.ic_phone);
            ((TextView) row.findViewById(R.id.deviceName)).setText(p.name);
            String kind = p.os.isEmpty() ? "" : Character.toUpperCase(p.os.charAt(0)) + p.os.substring(1) + " · ";
            ((TextView) row.findViewById(R.id.deviceDetail)).setText(kind + p.host);
            ((TextView) row.findViewById(R.id.deviceAction)).setText(mode == TransferMode.DEMO_TAP ? "Use" : "Send");
            row.setOnClickListener(v -> onNearbyClicked(p));
            nearbyContainer.addView(row);
        }
        textNearbyEmpty.setVisibility(nearby.isEmpty() ? View.VISIBLE : View.GONE);
        textNearbyEmpty.setText(discoveryRoundDone ? R.string.nearby_none : R.string.nearby_searching);
    }

    private void onNearbyClicked(LanDiscovery.Peer peer) {
        if (controller.isBusy()) return;
        if (mode == TransferMode.DEMO_TAP) {
            pickDiscovered(peer, true);
            return;
        }
        if (selectedFiles.isEmpty()) {
            Toast.makeText(this, "Add files first", Toast.LENGTH_SHORT).show();
            return;
        }
        pickDiscovered(peer, false);
    }

    // ==========================================
    // Sending
    // ==========================================
    private void updateSendButton() {
        boolean engine = NativeBridge.isAvailable();
        boolean busy = controller != null && controller.isBusy();
        buttonSend.setEnabled(engine && !busy && (mode == TransferMode.DEMO_TAP || !selectedFiles.isEmpty()));
    }

    private void onSendPressed() {
        if (controller.isBusy()) return;
        if (mode == TransferMode.DEMO_TAP) {
            // Choosing the demo partner does not need files yet.
            withBluetooth(false, usable -> showTargetPicker(true, usable));
            return;
        }
        if (selectedFiles.isEmpty()) {
            Toast.makeText(this, "Add files first", Toast.LENGTH_SHORT).show();
            return;
        }
        switch (mode) {
            case HARDWARE_NFC: {
                String reason = NfcTapReader.unavailableReason(nfcAdapter);
                if (reason != null) {
                    // Never pretend: say NFC is unavailable and offer the explicit fallback.
                    new MaterialAlertDialogBuilder(this)
                            .setTitle("NFC unavailable")
                            .setMessage("Cannot use NFC: " + reason + ".\n\nSend over Bluetooth to a paired device instead?")
                            .setPositiveButton("Use Bluetooth fallback", (d, w) -> {
                                setMode(TransferMode.BLUETOOTH_FALLBACK);
                                onSendPressed();
                            })
                            .setNeutralButton("NFC settings", (d, w) -> startActivity(new Intent(Settings.ACTION_NFC_SETTINGS)))
                            .setNegativeButton(android.R.string.cancel, null)
                            .show();
                    return;
                }
                // Bluetooth is optional here: the tap payload may offer Wi-Fi TCP.
                withBluetooth(false, this::startWaitingForTap);
                break;
            }
            case BLUETOOTH_FALLBACK:
                withBluetooth(true, usable -> showTargetPicker(false, true));
                break;
            default:
                break;
        }
    }

    private final NfcTapReader.Callback tapCallback = new NfcTapReader.Callback() {
        @Override
        public void onHandoverRead(String payloadJson) {
            String canonical;
            JSONObject handover;
            try {
                canonical = NativeBridge.parseHandover(payloadJson);   // validated by the C++ parser
                handover = new JSONObject(canonical);
            } catch (IllegalArgumentException | JSONException e) {
                textStatus.setText("Invalid tap payload: " + e.getMessage());   // keep waiting
                return;
            }
            if ("send".equals(handover.optString("role"))) {   // the other phone is sending too
                textStatus.setText("That device is also sending. Put it in Receive mode, then tap again.");
                return;
            }
            stopWaitingForTap();
            try {
                textNfcStatus.setText("NFC: physical tap read OK");
                setNextPeer(handover.optString("name", "Phone"), false);   // a phone answered the tap
                startSend(Target.fromHandover(canonical, "physical NFC tap (this phone read the receiver)"));
            } catch (JSONException e) {
                showError("Invalid tap payload: " + e.getMessage());
            }
        }

        @Override
        public void onTapError(String message) {
            textStatus.setText(message);   // keep waiting for another tap
        }
    };

    /**
     * "Send by NFC tap" works both ways round, whichever device can read the other:
     * <ul>
     *   <li>this phone reads a receiver (another phone in Receive) and connects to it;</li>
     *   <li>a reader-only device (the laptop's ESP32 + Si512 module, which cannot act as a card)
     *       reads this phone's {"role":"send"} payload, connects here and fetches the files.</li>
     * </ul>
     * So the phone stays a card (HCE) and listens for tags through foreground dispatch.
     */
    private void startWaitingForTap(boolean bluetoothUsable) {
        if (controller.isReceiving()) stopReceiving();
        String payload;
        try {
            payload = controller.startServingFiles(new ArrayList<>(selectedFiles), bluetoothUsable);
        } catch (IOException e) {
            showError("Cannot wait for a tap: " + e.getMessage()
                    + ". Turn on Wi-Fi or Bluetooth, or use Bluetooth fallback mode.");
            return;
        }
        waitingForTap = true;
        transferSending = true;
        AnyShareHceService.setPayload(payload, () ->
                textNfcStatus.setText("NFC: tapped — the other device is connecting to fetch the files"));
        setPreferredHceService(true);
        tapReader.enableDispatch(tapCallback);
        textNfcStatus.setText("NFC: waiting for a tap (this phone can be read, and reads receivers)");
        String hint = "Hold this phone on the laptop's NFC module, or back-to-back with a phone in Receive";
        textStatus.setText(hint);
        showTransferCard("Waiting for NFC tap", R.color.primary);
        textTransferDetail.setText(hint);
        buttonCancel.setVisibility(View.VISIBLE);
        Log.i(TAG, "Send-by-tap payload: " + payload);
    }

    private void stopWaitingForTap() {
        waitingForTap = false;
        tapReader.disable();
        controller.stopServingFiles();
        AnyShareHceService.clearPayload();
        setPreferredHceService(controller.isReceiving());
        refreshNfcStatus();
        if (!controller.isBusy()) {
            transferSending = false;
            buttonCancel.setVisibility(View.GONE);
            if (!transferActive) cardTransfer.setVisibility(View.GONE);
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (waitingForTap) tapReader.handleIntent(intent);   // a tag found by foreground dispatch
    }

    /**
     * "Send to" list: AnyShare receivers found on the Wi-Fi by name (they appear
     * while the list is open), paired Bluetooth devices, and a manual IP entry.
     * forDemo: choose the remembered demo partner instead of sending now.
     * Without Bluetooth permission paired devices are not listed (it would throw).
     */
    private void showTargetPicker(boolean forDemo, boolean bluetoothUsable) {
        List<String> labels = new ArrayList<>();
        List<Runnable> actions = new ArrayList<>();
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, labels);

        labels.add("Searching for nearby devices…");
        actions.add(() -> showTargetPicker(forDemo, bluetoothUsable));   // tapping it searches again
        if (bluetoothUsable) {
            for (BluetoothDevice d : bluetooth.bondedDevices()) {
                labels.add("Bluetooth: " + BluetoothTransport.displayName(d) + "  (" + d.getAddress() + ")");
                actions.add(() -> pickBluetooth(d, forDemo));
            }
        }
        labels.add("Wi-Fi: enter IP address…");
        actions.add(() -> showIpDialog(forDemo));
        labels.add("Pair a new device…");
        actions.add(() -> startActivity(new Intent(Settings.ACTION_BLUETOOTH_SETTINGS)));

        androidx.appcompat.app.AlertDialog dialog = new MaterialAlertDialogBuilder(this)
                .setTitle(forDemo ? "Demo partner" : "Send to")
                .setAdapter(adapter, (d, which) -> actions.get(which).run())
                .setNegativeButton(android.R.string.cancel, null)
                .show();

        // Receivers answer within ~1 s; each one is inserted above "Searching…" as it answers.
        new Thread(() -> {
            List<LanDiscovery.Peer> found = LanDiscovery.discover(1500, LanDiscovery.PORT,
                    LanDiscovery.broadcastTargets(), peer -> runOnUiThread(() -> {
                        if (!dialog.isShowing()) return;
                        int at = labels.indexOf("Searching for nearby devices…");
                        labels.add(at, peer.label());
                        actions.add(at, () -> pickDiscovered(peer, forDemo));
                        adapter.notifyDataSetChanged();
                    }));
            runOnUiThread(() -> {
                if (!dialog.isShowing()) return;
                int at = labels.indexOf("Searching for nearby devices…");
                labels.set(at, found.isEmpty()
                        ? "No AnyShare device found nearby (it must be in Receive mode, same Wi-Fi) · search again"
                        : "Search again");
                adapter.notifyDataSetChanged();
            });
        }, "anyshare-discover").start();
    }

    private void pickBluetooth(BluetoothDevice device, boolean forDemo) {
        if (forDemo) {
            TransferMode.saveDemoPeer(this, device.getAddress());
            textStatus.setText("Demo partner: " + BluetoothTransport.displayName(device)
                    + ". Tap the card above to simulate the NFC tap.");
        } else {
            setNextPeer(BluetoothTransport.displayName(device), isComputer(device));
            startSend(Target.bluetooth(device, "manual: paired device (no NFC)"));
        }
    }

    private void pickDiscovered(LanDiscovery.Peer peer, boolean forDemo) {
        if (forDemo) {
            TransferMode.saveDemoPeer(this, DEMO_TCP_PREFIX + peer.host + ":" + peer.port);
            textStatus.setText("Demo partner: " + peer.name + ". Tap the card above to simulate the NFC tap.");
        } else {
            setNextPeer(peer.name, "linux".equals(peer.os));
            startSend(Target.tcp(peer.host, peer.port, "Wi-Fi discovery: '" + peer.name + "' (no NFC)"));
        }
    }

    /** forDemo: remember host:port as the demo partner instead of sending now. */
    private void showIpDialog(boolean forDemo) {
        EditText input = new EditText(this);
        input.setHint("192.168.1.20:8080");
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        new MaterialAlertDialogBuilder(this)
                .setTitle("Receiver address (host:port)")
                .setMessage("Shown by `anyshare receive` on Linux or in another phone's receive screen.")
                .setView(input)
                .setPositiveButton(forDemo ? "Save" : "Send", (d, w) -> {
                    String text = input.getText().toString().trim();
                    String[] hostPort = parseHostPort(text);
                    if (hostPort == null) {
                        showError("Expected host:port, e.g. 192.168.1.20:8080");
                    } else if (forDemo) {
                        TransferMode.saveDemoPeer(this, DEMO_TCP_PREFIX + text);
                        textStatus.setText("Demo partner: Wi-Fi " + text + ". Tap the card above to simulate the NFC tap.");
                    } else {
                        setNextPeer(hostPort[0], true);   // typed addresses are usually a laptop
                        startSend(Target.tcp(hostPort[0], Integer.parseInt(hostPort[1]), "manual: IP address (no NFC)"));
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    /** Returns {host, port} or null if {@code text} is not host:port with a valid port. */
    private static String[] parseHostPort(String text) {
        int colon = text.lastIndexOf(':');
        if (colon <= 0) return null;
        try {
            int port = Integer.parseInt(text.substring(colon + 1));
            if (port < 1 || port > 65535) return null;
        } catch (NumberFormatException e) {
            return null;
        }
        return new String[]{text.substring(0, colon), text.substring(colon + 1)};
    }

    /** Stored demo partners are a Bluetooth address, or "tcp:" + host:port. */
    private static final String DEMO_TCP_PREFIX = "tcp:";

    /**
     * Demo mode: the tap is SIMULATED (and labelled so); the transfer is real,
     * over Bluetooth or Wi-Fi depending on the chosen demo partner.
     */
    private void onDemoTap() {
        if (controller.isBusy()) return;
        if (selectedFiles.isEmpty()) {
            Toast.makeText(this, "Add files first", Toast.LENGTH_SHORT).show();
            return;
        }
        String peer = TransferMode.loadDemoPeer(this);
        if (peer == null) {
            withBluetooth(false, usable -> showTargetPicker(true, usable));
            return;
        }
        if (peer.startsWith(DEMO_TCP_PREFIX)) {
            String[] hostPort = parseHostPort(peer.substring(DEMO_TCP_PREFIX.length()));
            if (hostPort == null) {
                withBluetooth(false, usable -> showTargetPicker(true, usable));
                return;
            }
            textNfcStatus.setText("NFC: not used (simulated tap — demo mode)");
            setNextPeer(hostPort[0], nearbyIsComputer(hostPort[0], true));
            playTapAnimation(() -> startSend(Target.tcp(hostPort[0], Integer.parseInt(hostPort[1]),
                    "SIMULATED tap (demo mode, no NFC)")));
            return;
        }
        withBluetooth(true, usable -> {
            BluetoothDevice device = bluetooth.deviceFor(peer);
            textNfcStatus.setText("NFC: not used (simulated tap — demo mode)");
            setNextPeer(BluetoothTransport.displayName(device), isComputer(device));
            playTapAnimation(() -> startSend(Target.bluetooth(device, "SIMULATED tap (demo mode, no NFC)")));
        });
    }

    private void startSend(Target target) {
        Log.i(TAG, "Send: handover=" + target.handoverDescription + ", files=" + selectedFiles.size());
        transferSending = true;
        transferActive = true;
        setBusyUi(true);
        refreshStage();
        setStagePeer(nextPeerName != null ? nextPeerName : "Receiver", nextPeerIsComputer);
        nextPeerName = null;
        nextPeerIsComputer = false;
        showTransferCard("Sending", R.color.state_send);
        textTransferDetail.setText("Connecting…");
        textTransport.setText("Handover: " + target.handoverDescription);
        controller.send(target, new ArrayList<>(selectedFiles));
    }

    // ==========================================
    // Receiving
    // ==========================================
    private void toggleReceive() {
        if (controller.isReceiving()) {
            stopReceiving();
            return;
        }
        // Bluetooth optional: without it the phone still receives over Wi-Fi.
        withBluetooth(false, this::startReceiving);
    }

    private void startReceiving(boolean bluetoothUsable) {
        try {
            String payload = controller.startReceiving(bluetoothUsable);
            AnyShareHceService.setPayload(payload, () ->
                    textNfcStatus.setText("NFC: physical tap — details sent to the sender, waiting for connection"));
            setPreferredHceService(true);
            boolean hce = nfcAdapter != null && nfcAdapter.isEnabled()
                    && getPackageManager().hasSystemFeature(PackageManager.FEATURE_NFC_HOST_CARD_EMULATION);
            textNfcStatus.setVisibility(hce ? View.VISIBLE : View.GONE);
            if (hce) textNfcStatus.setText("NFC: card emulation on — senders can tap this phone");
            textStatus.setText("Receive mode: waiting for a sender");
            textReceiveState.setText("Ready — visible as " + controller.deviceName() + " on this Wi-Fi");
            textPeer.setText("Sender");
            buttonReceive.setText(R.string.stop_receiving);
            startPulse();
            Log.i(TAG, "Receive payload: " + payload);
        } catch (Exception e) {
            showError("Cannot start receive mode: " + e.getMessage());
        }
    }

    private void stopReceiving() {
        controller.stopReceiving();
        AnyShareHceService.clearPayload();
        setPreferredHceService(false);
        buttonReceive.setText(R.string.start_receiving);
        textReceiveState.setText(R.string.receive_not_visible);
        stopPulse();
        textStatus.setText("Ready");
        if (!transferActive) textTransport.setText("Transport: idle");
        refreshNfcStatus();
    }

    private void setPreferredHceService(boolean enable) {
        if (nfcAdapter == null
                || !getPackageManager().hasSystemFeature(PackageManager.FEATURE_NFC_HOST_CARD_EMULATION)) return;
        try {
            CardEmulation emulation = CardEmulation.getInstance(nfcAdapter);
            if (enable) {
                emulation.setPreferredService(this, new ComponentName(this, AnyShareHceService.class));
            } else {
                emulation.unsetPreferredService(this);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "Preferred HCE service not set: " + e.getMessage());
        }
    }

    private void cancelEverything() {
        if (waitingForTap) stopWaitingForTap();
        controller.cancel();
    }

    private void openDownloads() {
        try {
            startActivity(new Intent(DownloadManager.ACTION_VIEW_DOWNLOADS));
        } catch (ActivityNotFoundException e) {
            Toast.makeText(this, "Files are in Downloads/AnyShare", Toast.LENGTH_LONG).show();
        }
    }

    // ==========================================
    // Bluetooth permission + enable flow
    // ==========================================
    /**
     * Runs {@code action} once Bluetooth may be used. If {@code required} is
     * false and the user declines, the action still runs with false (callers
     * then use Wi-Fi only).
     */
    private void withBluetooth(boolean required, BluetoothAction action) {
        if (!bluetooth.isSupported()) {
            if (required) showError("This phone has no Bluetooth.");
            else action.run(false);
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                && ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT)
                != PackageManager.PERMISSION_GRANTED) {
            pendingBluetoothAction = action;
            pendingBluetoothRequired = required;
            permissionLauncher.launch(new String[]{Manifest.permission.BLUETOOTH_CONNECT});
            return;
        }
        if (!bluetooth.isEnabled()) {
            pendingBluetoothAction = action;
            pendingBluetoothRequired = required;
            enableBluetoothLauncher.launch(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE));
            return;
        }
        action.run(true);
    }

    // ==========================================
    // TransferController.Listener (main thread)
    // ==========================================
    @Override
    public void onStatus(String status) {
        textStatus.setText(status);
        if (transferActive) textTransferDetail.setText(status);
    }

    @Override
    public void onTransport(String description) {
        textTransport.setText(description);
        Log.i(TAG, description);
    }

    @Override
    public void onFetchConnection(String description) {
        // Only a reader-only device (the laptop's ESP32 module) fetches; the core checks its token next.
        setStagePeer("Computer", true);
        textStatus.setText("Connected (" + description + "). Checking the tap…");
        if (transferActive || waitingForTap) textTransferDetail.setText("Connected. Checking the tap…");
    }

    @Override
    public void onSessionStarted(int fileCount, long totalBytes) {
        if (waitingForTap) stopWaitingForTap();   // fetched after a tap: the token matched, files flow now
        transferActive = true;
        setBusyUi(true);
        refreshStage();
        startPacketAnimation();   // only once a real link exists and the offer was accepted
        transferStartMs = SystemClock.elapsedRealtime();
        if (transferSending) showTransferCard("Sending", R.color.state_send);
        else showTransferCard("Receiving", R.color.state_receive);
        String summary = String.format(Locale.US, "%d file(s), %s", fileCount, formatFileSize(totalBytes));
        textTransferDetail.setText(summary);
        textStatus.setText((transferSending ? "Sending " : "Receiving ") + summary);
    }

    @Override
    public void onProgress(long done, long total) {
        int percent = total > 0 ? (int) (done * 100 / total) : 100;
        progressBar.setProgress(percent);
        textProgressPercent.setText(String.format(Locale.US, "%d%%", percent));
        textTransferDetail.setText(String.format(Locale.US, "%s of %s", formatFileSize(done), formatFileSize(total)));
        long elapsedMs = SystemClock.elapsedRealtime() - transferStartMs;
        if (elapsedMs > 500 && done > 0) {
            double bytesPerSec = done * 1000.0 / elapsedMs;
            long secondsLeft = (long) Math.ceil((total - done) / bytesPerSec);
            textSpeed.setText(String.format(Locale.US, "%s/s · %s left",
                    formatFileSize((long) bytesPerSec), formatDuration(Math.max(0, secondsLeft))));
        }
    }

    @Override
    public void onOffer(String sender, String[] names, long[] sizes, TransferController.OfferReply reply) {
        identifySender(sender);
        long total = 0;
        StringBuilder list = new StringBuilder();
        for (int i = 0; i < names.length; i++) {
            total += sizes[i];
            if (i < 10) list.append("• ").append(names[i]).append(" (").append(formatFileSize(sizes[i])).append(")\n");
        }
        if (names.length > 10) list.append("… and ").append(names.length - 10).append(" more\n");
        boolean[] answered = {false};
        new MaterialAlertDialogBuilder(this)
                .setTitle(String.format(Locale.US, "Accept %d file(s) from %s?", names.length, sender.isEmpty() ? "unknown device" : sender))
                .setMessage(list + "\nTotal: " + formatFileSize(total) + "\n\n(No NFC tap was used; accept only if you expect these files.)")
                .setPositiveButton("Accept", (d, w) -> { answered[0] = true; reply.answer(true); })
                .setNegativeButton("Decline", (d, w) -> { answered[0] = true; reply.answer(false); })
                .setOnDismissListener(d -> { if (!answered[0]) reply.answer(false); })
                .setCancelable(false)
                .show();
    }

    @Override
    public void onFileSaved(String name, String location) {
        Toast.makeText(this, "Saved " + name + " → " + location, Toast.LENGTH_SHORT).show();
    }

    @Override
    public void onFinished(boolean success, boolean cancelled, String message) {
        boolean wasSending = transferSending;
        if (waitingForTap) stopWaitingForTap();   // a fetch that failed before any data: stop waiting too
        stopPacketAnimation();
        transferActive = false;
        transferSending = false;
        setBusyUi(false);
        refreshStage();
        cardTransfer.setVisibility(View.VISIBLE);   // the result stays until the next action
        if (success) {
            showTransferCard(wasSending ? "Sent" : "Received", R.color.state_done);
            progressBar.setProgress(100);
            textProgressPercent.setText("100%");
            long seconds = Math.max(1, (SystemClock.elapsedRealtime() - transferStartMs) / 1000);
            textSpeed.setText("Took " + formatDuration(seconds));
        } else {
            showTransferCard(cancelled ? "Cancelled" : "Failed", R.color.state_error);
        }
        buttonOpenDownloads.setVisibility(success && !wasSending ? View.VISIBLE : View.GONE);
        textTransferDetail.setText(message);
        textStatus.setText(controller.isReceiving() ? message + " — still in receive mode" : message);
    }

    /** Shows the transfer card in a state; the colour matches the ESP32 status LED. */
    private void showTransferCard(String state, int colorRes) {
        int color = ContextCompat.getColor(this, colorRes);
        cardTransfer.setVisibility(View.VISIBLE);
        textState.setText(state);
        textState.setTextColor(color);
        dotState.setBackgroundTintList(ColorStateList.valueOf(color));
        progressBar.setIndicatorColor(color);
        buttonOpenDownloads.setVisibility(View.GONE);
    }

    private void setBusyUi(boolean busy) {
        buttonCancel.setVisibility(busy || waitingForTap ? View.VISIBLE : View.GONE);
        buttonChooseFile.setEnabled(!busy);
        updateSendButton();
        if (busy) {
            progressBar.setProgress(0);
            textProgressPercent.setText("0%");
            textSpeed.setText("");
        }
    }

    private void showError(String message) {
        textStatus.setText(message);
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
        Log.w(TAG, message);
    }

    // ==========================================
    // Animations
    // ==========================================
    /**
     * A packet flies from the sender to the receiver while data flows: this phone -> peer
     * when sending, peer -> this phone when receiving. It takes the state colour
     * (purple sending, red receiving), like the transfer card and the ESP32 LED.
     */
    private void startPacketAnimation() {
        if (packetAnimator != null) packetAnimator.cancel();
        iconPacket.animate().cancel();
        iconPacket.animate().setListener(null);
        final boolean sending = transferSending;
        int color = ContextCompat.getColor(this, sending ? R.color.state_send : R.color.state_receive);
        iconPacket.setImageTintList(ColorStateList.valueOf(color));
        iconPacket.setVisibility(View.VISIBLE);
        iconPacket.setAlpha(0.0f);
        cardStage.post(() -> {   // view positions are only valid after the stage has been laid out
            if (!transferActive) return;
            float base = iconPacket.getLeft();
            float phoneEdge = layoutPhone.getRight() - base;
            float peerEdge = Math.max(phoneEdge + 120, layoutLaptop.getLeft() - iconPacket.getWidth() - base);
            final float from = sending ? phoneEdge : peerEdge;
            final float to = sending ? peerEdge : phoneEdge;
            packetAnimator = ValueAnimator.ofFloat(0f, 1f);
            packetAnimator.setDuration(1100);
            packetAnimator.setInterpolator(new AccelerateDecelerateInterpolator());
            packetAnimator.setRepeatCount(ValueAnimator.INFINITE);
            packetAnimator.addUpdateListener(a -> {
                float t = (float) a.getAnimatedValue();
                iconPacket.setTranslationX(from + (to - from) * t);
                iconPacket.setAlpha(Math.min(1f, Math.min(t, 1f - t) * 5f));   // fade in and out at the ends
            });
            packetAnimator.start();
        });
    }

    private void setStagePeer(String name, boolean isComputer) {
        textPeer.setText(name);
        iconPeer.setImageResource(isComputer ? R.drawable.ic_laptop : R.drawable.ic_phone);
    }

    private void setNextPeer(String name, boolean isComputer) {
        nextPeerName = name;
        nextPeerIsComputer = isComputer;
    }

    /** Laptop or phone? A paired device's Bluetooth class says so. */
    private static boolean isComputer(BluetoothDevice device) {
        try {
            BluetoothClass c = device.getBluetoothClass();
            return c != null && c.getMajorDeviceClass() == BluetoothClass.Device.Major.COMPUTER;
        } catch (SecurityException e) {
            return false;
        }
    }

    /** Laptop or phone, by name or IP, from the nearby-devices list (fallback when unknown). */
    private boolean nearbyIsComputer(String nameOrHost, boolean fallback) {
        for (NearbyEntry e : nearby.values()) {
            if (e.peer.name.equals(nameOrHost) || e.peer.host.equals(nameOrHost)) return "linux".equals(e.peer.os);
        }
        return fallback;
    }

    /** Incoming files: show the sender's name now, and its device type once discovery answers. */
    private void identifySender(String sender) {
        String name = sender.isEmpty() ? "Sender" : sender;
        setStagePeer(name, nearbyIsComputer(name, false));
        new Thread(() -> {
            for (LanDiscovery.Peer p : LanDiscovery.discover(700, LanDiscovery.PORT, LanDiscovery.broadcastTargets(), null)) {
                if (p.name.equals(name)) {
                    boolean computer = "linux".equals(p.os);
                    runOnUiThread(() -> setStagePeer(name, computer));
                    return;
                }
            }
        }, "anyshare-identify").start();
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

    /** Receive tab: a ring grows and fades around the phone while it is visible. */
    private void startPulse() {
        stopPulse();
        pulseAnimator = ObjectAnimator.ofPropertyValuesHolder(pulseRing,
                PropertyValuesHolder.ofFloat(View.SCALE_X, 1.0f, 1.45f),
                PropertyValuesHolder.ofFloat(View.SCALE_Y, 1.0f, 1.45f),
                PropertyValuesHolder.ofFloat(View.ALPHA, 0.8f, 0.0f));
        pulseAnimator.setDuration(1600);
        pulseAnimator.setRepeatCount(ObjectAnimator.INFINITE);
        pulseAnimator.start();
    }

    private void stopPulse() {
        if (pulseAnimator != null) pulseAnimator.cancel();
        pulseAnimator = null;
        pulseRing.setAlpha(0.0f);
        pulseRing.setScaleX(1.0f);
        pulseRing.setScaleY(1.0f);
    }

    /** Demo-only visual: the two devices "touch". Purely presentational. */
    private void playTapAnimation(Runnable then) {
        float distance = Math.max(0, layoutLaptop.getX() - layoutPhone.getX() - layoutPhone.getWidth()) / 2f;
        layoutPhone.animate().translationX(distance).setDuration(350).withEndAction(() ->
                layoutPhone.animate().translationX(0).setDuration(250).withEndAction(then).start()).start();
        layoutLaptop.animate().translationX(-distance).setDuration(350).withEndAction(() ->
                layoutLaptop.animate().translationX(0).setDuration(250).start()).start();
    }
}
