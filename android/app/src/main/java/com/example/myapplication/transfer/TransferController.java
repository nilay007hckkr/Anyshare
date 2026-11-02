package com.example.myapplication.transfer;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothServerSocket;
import android.bluetooth.BluetoothSocket;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.MediaStore;
import android.provider.Settings;
import android.util.Log;
import android.webkit.MimeTypeMap;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.Closeable;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

/**
 * Owns sockets, worker threads and cancellation for both directions and
 * drives the native ASP/1 core through {@link NativeBridge}. All Listener
 * callbacks are delivered on the main thread.
 *
 * Success is reported only when the native core returns RESULT_SUCCESS, i.e.
 * after the receiver acknowledged every file's checksum. There is no simulated
 * path: if the native library is missing, transfers fail with an error.
 *
 * Bluetooth permissions (BLUETOOTH_CONNECT on Android 12+) are checked by
 * MainActivity before any method here is called.
 */
@SuppressLint("MissingPermission")
public final class TransferController {
    private static final String TAG = "AnyShare.Transfer";
    private static final int PREFERRED_TCP_PORT = 8080;
    private static final long OFFER_DECISION_TIMEOUT_S = 90;

    public interface Listener {
        void onStatus(String status);
        /** Send by NFC tap: a reader connected to fetch this phone's files (the token is checked next). */
        void onFetchConnection(String description);
        /** Diagnostics: the transport and handover actually in use. */
        void onTransport(String description);
        /** Real link established and offer accepted: bytes are about to flow. */
        void onSessionStarted(int fileCount, long totalBytes);
        void onProgress(long done, long total);
        /** Receiver: ask the user. Call reply.answer(...) exactly once. */
        void onOffer(String sender, String[] names, long[] sizes, OfferReply reply);
        void onFileSaved(String name, String location);
        void onFinished(boolean success, boolean cancelled, String message);
    }

    public static final class OfferReply {
        private final CountDownLatch latch = new CountDownLatch(1);
        private volatile boolean accepted;

        public void answer(boolean accept) {
            accepted = accept;
            latch.countDown();
        }

        boolean await() {
            try {
                return latch.await(OFFER_DECISION_TIMEOUT_S, TimeUnit.SECONDS) && accepted;
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
    }

    /** A file chosen through the Storage Access Framework. */
    public static final class OutgoingItem {
        public final Uri uri;
        public final String name;
        public final long size;
        public final String mimeType;

        public OutgoingItem(Uri uri, String name, long size, String mimeType) {
            this.uri = uri;
            this.name = name;
            this.size = size;
            this.mimeType = mimeType;
        }
    }

    /** Where to send, and how we learned it (shown verbatim in diagnostics). */
    public static final class Target {
        final BluetoothDevice device;     // explicit paired device
        final String bluetoothAddress;    // from a Linux receiver's payload
        final String bluetoothName;       // from an Android receiver's payload: resolve among paired devices
        final String host;                // TCP
        final int port;
        final String token;
        public final String handoverDescription;

        private Target(BluetoothDevice device, String bluetoothAddress, String bluetoothName, String host, int port,
                       String token, String how) {
            this.device = device;
            this.bluetoothAddress = bluetoothAddress;
            this.bluetoothName = bluetoothName;
            this.host = host;
            this.port = port;
            this.token = token;
            this.handoverDescription = how;
        }

        public static Target bluetooth(BluetoothDevice device, String how) {
            return new Target(device, null, null, null, 0, "", how);
        }

        public static Target tcp(String host, int port, String how) {
            return new Target(null, null, null, host, port, "", how);
        }

        /** From a handover payload read over NFC (validated by the native parser). */
        public static Target fromHandover(String canonicalJson, String how) throws JSONException {
            JSONObject j = new JSONObject(canonicalJson);
            return new Target(null, j.optString("bt", null), j.optString("name", null), j.optString("ip", null),
                    j.optInt("port", 0), j.optString("token", ""), how);
        }
    }

    private final Context context;
    private final BluetoothTransport bluetooth;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService sendExecutor = Executors.newSingleThreadExecutor();
    private final Object sessionLock = new Object();

    private final AtomicBoolean cancelled = new AtomicBoolean();
    private volatile Closeable activeLink;
    private volatile boolean busy;

    private volatile boolean receiving;
    private BluetoothServerSocket btServer;
    private ServerSocket tcpServer;
    private LanDiscovery.Responder discoveryResponder;
    private String receiveToken;

    // "Send by NFC tap" for readers that cannot act as a card: this phone waits to be fetched from.
    private volatile boolean serving;
    private BluetoothServerSocket serveBtServer;
    private ServerSocket serveTcpServer;
    private String serveToken;
    private List<OutgoingItem> serveItems;

    public TransferController(Context context, BluetoothTransport bluetooth, Listener listener) {
        this.context = context.getApplicationContext();
        this.bluetooth = bluetooth;
        this.listener = listener;
    }

    public boolean isBusy() {
        return busy;
    }

    public boolean isReceiving() {
        return receiving;
    }

    /** Stops the current transfer. The peer sees the link drop; partial files are deleted. */
    public void cancel() {
        cancelled.set(true);
        closeQuietly(activeLink);
    }

    public void shutdown() {
        cancel();
        stopReceiving();
        stopServingFiles();
        sendExecutor.shutdownNow();
    }

    public boolean isServingFiles() {
        return serving;
    }

    // ======================================================================
    // Receiving
    // ======================================================================

    /**
     * Opens a Bluetooth RFCOMM server (if Bluetooth is usable) and a TCP server,
     * and returns the handover JSON that the HCE service should serve.
     */
    public String startReceiving(boolean useBluetooth) throws IOException {
        if (receiving) stopReceiving();
        receiveToken = randomToken();
        List<String> listening = new ArrayList<>();
        JSONObject payload = new JSONObject();
        try {
            payload.put("v", 1);
            if (useBluetooth && bluetooth.isEnabled()) {
                btServer = bluetooth.listen();
                String name = bluetooth.localName();
                if (name.length() > 64) name = name.substring(0, 64);
                payload.put("transport", "rfcomm");
                payload.put("name", name);
                listening.add("Bluetooth RFCOMM as \"" + name + "\"");
            }
            String ip = localIpv4();
            if (ip != null) {
                tcpServer = new ServerSocket();
                tcpServer.setReuseAddress(true);
                try {
                    tcpServer.bind(new InetSocketAddress(PREFERRED_TCP_PORT));
                } catch (IOException busyPort) {
                    tcpServer.bind(new InetSocketAddress(0));
                }
                if (!payload.has("transport")) payload.put("transport", "tcp");
                payload.put("ip", ip);
                payload.put("port", tcpServer.getLocalPort());
                listening.add("Wi-Fi TCP " + ip + ":" + tcpServer.getLocalPort());
                try {   // optional: senders can then pick this phone by name
                    discoveryResponder = new LanDiscovery.Responder(deviceName(), tcpServer.getLocalPort(),
                            "android", LanDiscovery.PORT);
                    listening.add("visible as \"" + deviceName() + "\"");
                } catch (IOException e) {
                    Log.w(TAG, "Not discoverable by name: " + e.getMessage());
                }
            }
            if (listening.isEmpty()) {
                throw new IOException("Neither Bluetooth nor a Wi-Fi network is available");
            }
            payload.put("token", receiveToken);
        } catch (JSONException e) {
            throw new IOException(e);
        }

        receiving = true;
        BluetoothServerSocket bt = btServer;
        ServerSocket tcp = tcpServer;
        if (bt != null) startAcceptLoop("bt-accept", () -> receiving, () -> acceptBluetooth(bt), this::runReceiveSession);
        if (tcp != null) startAcceptLoop("tcp-accept", () -> receiving, () -> acceptTcp(tcp), this::runReceiveSession);
        Log.i(TAG, "Receive mode: " + listening);
        main.post(() -> listener.onTransport("Listening: " + String.join(" + ", listening)));
        return payload.toString();
    }

    /** The name other devices show in their "Send to" list: the phone's device name, else its model. */
    public String deviceName() {
        String name = Settings.Global.getString(context.getContentResolver(), Settings.Global.DEVICE_NAME);
        return name == null || name.trim().isEmpty() ? Build.MODEL : name.trim();
    }

    public void stopReceiving() {
        receiving = false;
        closeQuietly(btServer);
        closeQuietly(tcpServer);
        closeQuietly(discoveryResponder);
        btServer = null;
        tcpServer = null;
        discoveryResponder = null;
    }

    private interface Acceptor {
        Link accept() throws IOException;
    }

    private static Link acceptBluetooth(BluetoothServerSocket server) throws IOException {
        BluetoothSocket s = server.accept();
        return new Link(s, s.getInputStream(), s.getOutputStream(),
                "Bluetooth RFCOMM from " + BluetoothTransport.displayName(s.getRemoteDevice()));
    }

    private static Link acceptTcp(ServerSocket server) throws IOException {
        Socket s = server.accept();
        s.setTcpNoDelay(true);
        return new Link(s, s.getInputStream(), s.getOutputStream(),
                "Wi-Fi TCP from " + s.getInetAddress().getHostAddress());
    }

    private void startAcceptLoop(String name, BooleanSupplier running, Acceptor acceptor, Consumer<Link> session) {
        Thread t = new Thread(() -> {
            while (running.getAsBoolean()) {
                Link link;
                try {
                    link = acceptor.accept();
                } catch (IOException e) {
                    if (running.getAsBoolean()) Log.w(TAG, name + " stopped: " + e.getMessage());
                    return;
                }
                synchronized (sessionLock) {   // one session at a time
                    session.accept(link);
                }
            }
        }, name);
        t.setDaemon(true);
        t.start();
    }

    // ======================================================================
    // Send by NFC tap on a reader-only device: this phone waits to be fetched from
    // ======================================================================

    /**
     * For readers that cannot act as an NFC card (the ESP32 + Si512 module on a laptop): opens a
     * Bluetooth RFCOMM server (if usable) and a TCP server, and returns the {"role":"send",...}
     * payload for the HCE service. The device that reads it by NFC connects and fetches
     * {@code items}; it must present the payload's one-time token (checked by the native core),
     * so other devices on the same Wi-Fi get nothing.
     */
    public String startServingFiles(List<OutgoingItem> items, boolean useBluetooth) throws IOException {
        if (serving) stopServingFiles();
        if (receiving) stopReceiving();   // same Bluetooth service UUID: one server at a time
        serveToken = randomToken();
        serveItems = new ArrayList<>(items);
        List<String> listening = new ArrayList<>();
        JSONObject payload = new JSONObject();
        try {
            payload.put("v", 1);
            payload.put("role", "send");
            if (useBluetooth && bluetooth.isEnabled()) {
                serveBtServer = bluetooth.listen();
                String name = bluetooth.localName();
                if (name.length() > 64) name = name.substring(0, 64);
                payload.put("transport", "rfcomm");
                payload.put("name", name);   // a laptop finds a *paired* phone by this name
                listening.add("Bluetooth RFCOMM as \"" + name + "\"");
            }
            String ip = localIpv4();
            if (ip != null) {
                serveTcpServer = new ServerSocket();
                serveTcpServer.setReuseAddress(true);
                serveTcpServer.bind(new InetSocketAddress(0));
                if (!payload.has("transport")) payload.put("transport", "tcp");
                if (!payload.has("name")) payload.put("name", deviceName());
                payload.put("ip", ip);
                payload.put("port", serveTcpServer.getLocalPort());
                listening.add("Wi-Fi TCP " + ip + ":" + serveTcpServer.getLocalPort());
            }
            if (listening.isEmpty()) {
                throw new IOException("Neither Bluetooth nor a Wi-Fi network is available");
            }
            payload.put("token", serveToken);
        } catch (JSONException e) {
            stopServingFiles();
            throw new IOException(e);
        } catch (IOException e) {
            stopServingFiles();
            throw e;
        }

        serving = true;
        BluetoothServerSocket bt = serveBtServer;
        ServerSocket tcp = serveTcpServer;
        if (bt != null) startAcceptLoop("bt-serve", () -> serving, () -> acceptBluetooth(bt), this::runFetchSession);
        if (tcp != null) startAcceptLoop("tcp-serve", () -> serving, () -> acceptTcp(tcp), this::runFetchSession);
        Log.i(TAG, "Waiting to be fetched from: " + listening);
        main.post(() -> listener.onTransport("Waiting for a tap. Listening: " + String.join(" + ", listening)));
        return payload.toString();
    }

    public void stopServingFiles() {
        serving = false;
        closeQuietly(serveBtServer);
        closeQuietly(serveTcpServer);
        serveBtServer = null;
        serveTcpServer = null;
    }

    private void runFetchSession(Link link) {
        if (!serving) {   // stopped while this connection waited for the session lock
            closeQuietly(link);
            return;
        }
        busy = true;
        cancelled.set(false);
        activeLink = link;
        List<OutgoingItem> items = serveItems;
        main.post(() -> {
            listener.onFetchConnection(link.description);
            listener.onTransport("Transport: " + link.description + " · Handover: physical NFC tap (fetched by the reader)");
        });
        NativeBridge.Result result;
        try {
            result = NativeBridge.sendFilesOnPull(link.in, link.out, link.description, bluetooth.localName(),
                    serveToken, names(items), mimes(items), sizes(items), sourceCallbacks(items));
        } catch (Throwable t) {
            result = unexpected(t);
        } finally {
            closeQuietly(link);
            activeLink = null;
        }
        if (result.code == NativeBridge.RESULT_PULL_REFUSED) {   // not the device that read our tap
            busy = false;
            Log.w(TAG, "Ignored " + link.description + ": " + result.message);
            post("Ignored a connection that did not come from the NFC tap. Still waiting…");
            return;
        }
        stopServingFiles();   // one tap, one transfer
        String who = result.senderName == null || result.senderName.isEmpty() ? "" : " to " + result.senderName;
        finish(result, "sent" + who);
    }

    private static String[] names(List<OutgoingItem> items) {
        String[] out = new String[items.size()];
        for (int i = 0; i < out.length; i++) out[i] = items.get(i).name;
        return out;
    }

    private static String[] mimes(List<OutgoingItem> items) {
        String[] out = new String[items.size()];
        for (int i = 0; i < out.length; i++) out[i] = items.get(i).mimeType;
        return out;
    }

    private static long[] sizes(List<OutgoingItem> items) {
        long[] out = new long[items.size()];
        for (int i = 0; i < out.length; i++) out[i] = items.get(i).size;
        return out;
    }

    private ProgressCallbacks sourceCallbacks(List<OutgoingItem> items) {
        ContentResolver resolver = context.getContentResolver();
        return new ProgressCallbacks() {
            @Override public InputStream openSource(int index) throws IOException {
                InputStream in = resolver.openInputStream(items.get(index).uri);
                if (in == null) throw new IOException("cannot open " + items.get(index).name);
                return in;
            }
        };
    }

    private void runReceiveSession(Link link) {
        busy = true;
        cancelled.set(false);
        activeLink = link;
        File inbox = new File(context.getExternalFilesDir(Environment.DIRECTORY_DOWNLOADS), "AnyShare");
        main.post(() -> {
            listener.onTransport("Transport: " + link.description);
            listener.onStatus("Incoming connection…");
        });
        List<String[]> saved = Collections.synchronizedList(new ArrayList<>());
        NativeBridge.Result result;
        try {
            result = NativeBridge.receiveFiles(link.in, link.out, link.description, inbox.getAbsolutePath(),
                    receiveToken, 0, new ProgressCallbacks() {
                        @Override public boolean onOffer(String sender, String[] names, long[] sizes) {
                            OfferReply reply = new OfferReply();
                            main.post(() -> listener.onOffer(sender, names, sizes, reply));
                            return reply.await();
                        }
                        @Override public void onFileCompleted(int index, String name, String savedPath) {
                            saved.add(new String[]{name, savedPath});
                        }
                    });
        } catch (Throwable t) {   // report, never fake
            result = unexpected(t);
        } finally {
            closeQuietly(link);
            activeLink = null;
        }

        if (result.isSuccess()) {
            for (String[] file : saved) {
                String location = publishToDownloads(new File(file[1]), file[0]);
                main.post(() -> listener.onFileSaved(file[0], location));
            }
        }
        finish(result, (result.tokenMatched ? "tap-authorised" : "accepted") + " transfer from " + result.senderName);
    }

    // ======================================================================
    // Sending
    // ======================================================================

    public void send(Target target, List<OutgoingItem> items) {
        if (busy) return;
        busy = true;
        cancelled.set(false);
        sendExecutor.execute(() -> {
            Link link = null;
            NativeBridge.Result result;
            try {
                link = connect(target);
                activeLink = link;
                final String transport = link.description;
                main.post(() -> listener.onTransport("Transport: " + transport + " · Handover: " + target.handoverDescription));
                post("Connected. Waiting for the receiver to accept…");
                result = NativeBridge.sendFiles(link.in, link.out, link.description, bluetooth.localName(),
                        target.token, names(items), mimes(items), sizes(items), sourceCallbacks(items));
            } catch (IOException e) {
                result = new NativeBridge.Result(cancelled.get() ? NativeBridge.RESULT_CANCELLED : NativeBridge.RESULT_TRANSPORT_FAILED,
                        e.getMessage(), 0, false, "");
            } catch (Throwable t) {
                result = unexpected(t);
            } finally {
                closeQuietly(link);
                activeLink = null;
            }
            finish(result, "sent");
        });
    }

    /** Bluetooth first (paired device / tap name), then Wi-Fi TCP if the payload offers it. */
    private Link connect(Target target) throws IOException {
        List<String> failures = new ArrayList<>();
        if (target.device != null) {
            post("Connecting to " + BluetoothTransport.displayName(target.device) + " over Bluetooth…");
            return bluetoothLink(bluetooth.connect(target.device));
        }
        boolean bluetoothAlternative = bluetooth.isEnabled()
                && (target.bluetoothAddress != null || target.bluetoothName != null);
        // Wi-Fi first: it is many times faster. When the tap also offered Bluetooth, give up on
        // Wi-Fi quickly (the devices may simply not share a network) and use Bluetooth instead.
        if (target.host != null && target.port > 0 && !cancelled.get()) {
            post("Connecting to " + target.host + ":" + target.port + " over Wi-Fi…");
            Socket s = new Socket();
            activeLink = s;
            try {
                s.connect(new InetSocketAddress(target.host, target.port), bluetoothAlternative ? 3000 : 10000);
                s.setTcpNoDelay(true);
                return new Link(s, s.getInputStream(), s.getOutputStream(), "Wi-Fi TCP " + target.host + ":" + target.port);
            } catch (IOException e) {
                closeQuietly(s);
                failures.add("Wi-Fi: " + e.getMessage());
            }
        }
        if (cancelled.get()) throw new IOException("cancelled");
        if (target.bluetoothAddress != null && bluetooth.isEnabled()) {
            post("Connecting to " + target.bluetoothAddress + " over Bluetooth…");
            try {
                return bluetoothLink(bluetooth.connect(bluetooth.deviceFor(target.bluetoothAddress)));
            } catch (IOException | IllegalArgumentException e) {
                failures.add(e.getMessage());
            }
        }
        if (target.bluetoothName != null && bluetooth.isEnabled()) {
            for (BluetoothDevice d : bluetooth.candidatesFor(target.bluetoothName)) {
                if (cancelled.get()) throw new IOException("cancelled");
                post("Trying " + BluetoothTransport.displayName(d) + " over Bluetooth…");
                try {
                    return bluetoothLink(bluetooth.connect(d));
                } catch (IOException e) {
                    failures.add(e.getMessage());
                }
            }
            if (failures.isEmpty()) failures.add("\"" + target.bluetoothName + "\" is not a paired device");
        }
        throw new IOException(failures.isEmpty() ? "no reachable transport in handover" : String.join("; ", failures));
    }

    private static Link bluetoothLink(BluetoothSocket s) throws IOException {
        return new Link(s, s.getInputStream(), s.getOutputStream(),
                "Bluetooth RFCOMM " + BluetoothTransport.displayName(s.getRemoteDevice()));
    }

    // ======================================================================
    // Helpers
    // ======================================================================

    /** Shared progress forwarding, throttled to ~10 UI updates per second. */
    private abstract class ProgressCallbacks extends NativeBridge.BaseCallbacks {
        private long lastPost;

        @Override public boolean isCancelled() {
            return cancelled.get();
        }
        @Override public void onSessionStarted(int fileCount, long totalBytes) {
            main.post(() -> listener.onSessionStarted(fileCount, totalBytes));
        }
        @Override public void onFileStarted(int index, String name, long size) {
            post(String.format(Locale.US, "File %d: %s", index + 1, name));
        }
        @Override public void onBytes(long done, long total) {
            long now = System.currentTimeMillis();
            if (done == total || now - lastPost > 100) {
                lastPost = now;
                main.post(() -> listener.onProgress(done, total));
            }
        }
        @Override public void onError(String message) {
            Log.w(TAG, message);
        }
    }

    /** An exception outside the native transfer, labelled truthfully (it used to say "native engine" for all). */
    private static NativeBridge.Result unexpected(Throwable t) {
        String what = t instanceof UnsatisfiedLinkError ? "native engine unavailable"
                : t instanceof SecurityException ? "Android permission missing"
                : "unexpected error";
        Log.e(TAG, what, t);
        String detail = t.getMessage() != null ? t.getMessage() : t.toString();
        return new NativeBridge.Result(NativeBridge.RESULT_PROTOCOL_FAILED, what + ": " + detail, 0, false, "");
    }

    private void finish(NativeBridge.Result result, String what) {
        boolean wasCancelled = result.code == NativeBridge.RESULT_CANCELLED;
        String message = result.isSuccess()
                ? result.filesCompleted + " file(s) " + what + ", checksums verified"
                : (wasCancelled ? "Cancelled" : "Failed: " + result.message);
        Log.i(TAG, "Session finished: " + result);
        busy = false;
        main.post(() -> listener.onFinished(result.isSuccess(), wasCancelled, message));
    }

    private void post(String status) {
        main.post(() -> listener.onStatus(status));
    }

    /** Moves a verified file into the public Downloads/AnyShare folder (Android 10+). */
    private String publishToDownloads(File file, String name) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return file.getAbsolutePath();
        ContentResolver resolver = context.getContentResolver();
        String ext = MimeTypeMap.getFileExtensionFromUrl(Uri.fromFile(file).toString());
        String mime = ext != null ? MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext.toLowerCase(Locale.US)) : null;
        ContentValues values = new ContentValues();
        values.put(MediaStore.Downloads.DISPLAY_NAME, name);
        values.put(MediaStore.Downloads.MIME_TYPE, mime != null ? mime : "application/octet-stream");
        values.put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS + "/AnyShare");
        values.put(MediaStore.Downloads.IS_PENDING, 1);
        Uri uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);   // never overwrites
        if (uri == null) return file.getAbsolutePath();
        try (InputStream in = new FileInputStream(file); OutputStream out = resolver.openOutputStream(uri)) {
            if (out == null) throw new IOException("no output stream");
            byte[] buf = new byte[256 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (IOException e) {
            resolver.delete(uri, null, null);
            Log.w(TAG, "Could not publish " + name + " to Downloads; kept at " + file, e);
            return file.getAbsolutePath();
        }
        values.clear();
        values.put(MediaStore.Downloads.IS_PENDING, 0);
        resolver.update(uri, values, null, null);
        if (!file.delete()) Log.w(TAG, "Could not remove app copy " + file);
        return "Downloads/AnyShare/" + name;
    }

    /**
     * This phone's address on the local network that a laptop or phone nearby can reach:
     * Wi-Fi first (wlan*), then this phone's own hotspot (ap*, swlan*, softap*), then Ethernet.
     * Mobile data (rmnet*, ccmni*, ...) is never offered: other devices cannot reach it.
     */
    private static String localIpv4() {
        String[] preferred = {"wlan", "ap", "swlan", "softap", "eth"};
        try {
            List<NetworkInterface> all = Collections.list(NetworkInterface.getNetworkInterfaces());
            for (String prefix : preferred) {
                for (NetworkInterface nif : all) {
                    if (!nif.isUp() || nif.isLoopback() || !nif.getName().startsWith(prefix)) continue;
                    for (InetAddress addr : Collections.list(nif.getInetAddresses())) {
                        if (addr instanceof Inet4Address && addr.isSiteLocalAddress()) return addr.getHostAddress();
                    }
                }
            }
        } catch (IOException | RuntimeException ignored) {
            // getNetworkInterfaces() may return null or throw on some devices
        }
        return null;   // not on Wi-Fi: senders must use Bluetooth
    }

    private static String randomToken() {
        byte[] bytes = new byte[8];
        new SecureRandom().nextBytes(bytes);
        StringBuilder sb = new StringBuilder();
        for (byte b : bytes) sb.append(String.format(Locale.US, "%02x", b));
        return sb.toString();
    }

    private static void closeQuietly(Closeable c) {
        if (c == null) return;
        try {
            c.close();
        } catch (IOException ignored) {
        }
    }

    private static final class Link implements Closeable {
        final Closeable socket;
        final InputStream in;
        final OutputStream out;
        final String description;

        Link(Closeable socket, InputStream in, OutputStream out, String description) {
            this.socket = socket;
            this.in = in;
            this.out = out;
            this.description = description;
        }

        @Override public void close() throws IOException {
            socket.close();
        }
    }
}
