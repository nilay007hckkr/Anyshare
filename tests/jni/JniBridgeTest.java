import com.example.myapplication.nfc.ApduProtocol;
import com.example.myapplication.transfer.NativeBridge;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Desktop test of the real JNI bridge used by the Android app: the same
 * NativeBridge.java and anyshare_jni.cpp, compiled for the host JVM.
 * Run with -Xcheck:jni so the JVM validates every JNI call.
 */
public class JniBridgeTest {
    static int checks = 0, failures = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("  FAIL " + what);
        }
    }

    // ---- helpers -----------------------------------------------------------

    static File tempDir(String prefix) throws IOException {
        return Files.createTempDirectory(prefix).toFile();
    }

    static File randomFile(File dir, String name, int size, long seed) throws IOException {
        byte[] data = new byte[size];
        new Random(seed).nextBytes(data);
        File f = new File(dir, name);
        Files.write(f.toPath(), data);
        return f;
    }

    static class Sender extends NativeBridge.BaseCallbacks {
        final File[] files;
        final AtomicBoolean cancel = new AtomicBoolean();
        long cancelAfterBytes = Long.MAX_VALUE;
        boolean failOpen = false;
        final List<String> completed = new ArrayList<>();
        final List<String> errors = new ArrayList<>();

        Sender(File... files) { this.files = files; }

        @Override public boolean isCancelled() { return cancel.get(); }
        @Override public InputStream openSource(int index) throws IOException {
            if (failOpen) throw new IOException("permission denied (test)");
            return new FileInputStream(files[index]);
        }
        @Override public void onBytes(long done, long total) {
            if (done >= cancelAfterBytes) cancel.set(true);
        }
        @Override public void onFileCompleted(int index, String name, String savedPath) { completed.add(name); }
        @Override public void onError(String message) { errors.add(message); }
    }

    static class Receiver extends NativeBridge.BaseCallbacks {
        boolean accept = false;
        String[] offeredNames;
        final List<String> saved = new ArrayList<>();
        Socket socketToCloseOnBytes;
        long closeAfterBytes = Long.MAX_VALUE;
        final AtomicBoolean cancel = new AtomicBoolean();

        @Override public boolean isCancelled() { return cancel.get(); }
        @Override public boolean onOffer(String sender, String[] names, long[] sizes) {
            offeredNames = names;
            return accept;
        }
        @Override public void onBytes(long done, long total) {
            if (socketToCloseOnBytes != null && done >= closeAfterBytes) {
                cancel.set(true);
                try { socketToCloseOnBytes.close(); } catch (IOException ignored) {}
            }
        }
        @Override public void onFileCompleted(int index, String name, String savedPath) { saved.add(savedPath); }
    }

    static NativeBridge.Result[] session(File dest, String expectedToken, Receiver rx, String token, Sender tx)
            throws Exception {
        NativeBridge.Result[] results = new NativeBridge.Result[2];
        try (ServerSocket server = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            AtomicReference<Throwable> rxError = new AtomicReference<>();
            Thread rxThread = new Thread(() -> {
                try (Socket s = server.accept()) {
                    if (rx.socketToCloseOnBytes == null && rx.closeAfterBytes != Long.MAX_VALUE) rx.socketToCloseOnBytes = s;
                    results[1] = NativeBridge.receiveFiles(s.getInputStream(), s.getOutputStream(), "tcp test",
                            dest.getPath(), expectedToken, 0, rx);
                } catch (Throwable t) {
                    rxError.set(t);
                }
            });
            rxThread.start();
            try (Socket s = new Socket(InetAddress.getLoopbackAddress(), server.getLocalPort())) {
                s.setTcpNoDelay(true);
                String[] names = new String[tx.files.length];
                String[] mimes = new String[tx.files.length];
                long[] sizes = new long[tx.files.length];
                for (int i = 0; i < tx.files.length; i++) {
                    names[i] = tx.files[i].getName();
                    mimes[i] = "application/octet-stream";
                    sizes[i] = tx.files[i].length();
                }
                results[0] = NativeBridge.sendFiles(s.getInputStream(), s.getOutputStream(), "tcp test",
                        "jni-test", token, names, mimes, sizes, tx);
            }
            rxThread.join(30000);
            if (rxError.get() != null) throw new RuntimeException(rxError.get());
        }
        return results;
    }

    // ---- tests ---------------------------------------------------------------

    static void testConstantsAndHelpers() {
        String info = NativeBridge.protocolInfo();
        check(info.contains("\"aid\":\"" + ApduProtocol.AID_HEX + "\""), "C++ and Java agree on HCE AID: " + info);
        check(info.contains("\"protocol\":1"), "protocol version 1");

        String emoji = "😀";   // U+1F600, 4-byte UTF-8 / surrogate pair in Java
        check(NativeBridge.sanitizeFilename("../../party" + emoji + ".txt").equals("party" + emoji + ".txt"),
                "emoji survives UTF-16 <-> UTF-8 conversion");
        check(NativeBridge.sanitizeFilename("..").equals("file"), "'..' sanitised");

        String canonical = NativeBridge.parseHandover("{\"transport\":\"rfcomm\",\"name\":\"Pixel\",\"token\":\"ab\"}");
        check(canonical.contains("\"name\":\"Pixel\""), "parseHandover canonical JSON");
        try {
            NativeBridge.parseHandover("{\"port\":1}");
            check(false, "invalid handover must throw");
        } catch (IllegalArgumentException e) {
            check(e.getMessage().contains("Missing required field: ip"), "clear handover error: " + e.getMessage());
        }

        byte[] select = ApduProtocol.selectAidCommand();
        check(toHex(select).equals("00A4040007F0414E5953485200"), "SELECT APDU bytes " + toHex(select));
        check(ApduProtocol.isSelectAid(select), "HCE recognises its SELECT");
        String json = "{\"v\":1,\"transport\":\"rfcomm\",\"name\":\"Pixel 7\",\"token\":\"0123456789abcdef\"}";
        check(ApduProtocol.parseSelectResponse(ApduProtocol.buildSelectResponse(json)).equals(json), "APDU payload round trip");
        try {
            ApduProtocol.parseSelectResponse(ApduProtocol.SW_FILE_NOT_FOUND);
            check(false, "6A82 must throw");
        } catch (IllegalArgumentException expected) {
            check(expected.getMessage().contains("6A82"), "status word reported");
        }
    }

    static void testTokenTransfer() throws Exception {
        File src = tempDir("as-src"), dst = tempDir("as-dst");
        String emoji = "😀";
        File a = randomFile(src, "photo " + emoji + ".jpg", 3 * 1024 * 1024 + 7, 1);
        File b = randomFile(src, "empty.txt", 0, 2);
        File c = randomFile(src, "doc.pdf", 70000, 3);
        Receiver rx = new Receiver();          // rejects anything without the token
        Sender tx = new Sender(a, b, c);
        NativeBridge.Result[] r = session(dst, "tok123", rx, "tok123", tx);
        check(r[0].isSuccess(), "sender success: " + r[0]);
        check(r[1].isSuccess(), "receiver success: " + r[1]);
        check(r[1].tokenMatched, "receiver auto-accepted by token");
        check(rx.offeredNames == null, "no user prompt for tokened offer");
        check(r[1].senderName.equals("jni-test"), "sender name passed through");
        check(tx.completed.size() == 3 && rx.saved.size() == 3, "3 files acknowledged and saved");
        for (File f : new File[]{a, b, c}) {
            File out = new File(dst, f.getName());
            check(out.exists() && Arrays.equals(Files.readAllBytes(f.toPath()), Files.readAllBytes(out.toPath())),
                    "identical: " + f.getName());
        }
    }

    static void testPromptAndReject() throws Exception {
        File src = tempDir("as-src"), dst = tempDir("as-dst");
        File a = randomFile(src, "a.bin", 1000, 4);
        Receiver rx = new Receiver();
        rx.accept = false;
        NativeBridge.Result[] r = session(dst, "tok", rx, "", new Sender(a));
        check(r[0].code == NativeBridge.RESULT_REJECTED, "sender sees rejection: " + r[0]);
        check(rx.offeredNames != null && rx.offeredNames[0].equals("a.bin"), "user was asked with sanitised name");
        check(dst.list().length == 0, "nothing written after rejection");

        Receiver yes = new Receiver();
        yes.accept = true;
        NativeBridge.Result[] ok = session(dst, "tok", yes, "wrong-token", new Sender(a));
        check(ok[0].isSuccess() && !ok[1].tokenMatched, "manual accept works without token");
    }

    static void testSenderCancel() throws Exception {
        File src = tempDir("as-src"), dst = tempDir("as-dst");
        File big = randomFile(src, "big.bin", 8 * 1024 * 1024, 5);
        Sender tx = new Sender(big);
        tx.cancelAfterBytes = 1024 * 1024;
        NativeBridge.Result[] r = session(dst, "t", new Receiver(), "t", tx);
        check(r[0].code == NativeBridge.RESULT_CANCELLED, "sender cancelled: " + r[0]);
        check(!r[1].isSuccess(), "receiver not successful: " + r[1]);
        check(dst.list().length == 0, "receiver removed partial file: " + Arrays.toString(dst.list()));
    }

    static void testReceiverCancel() throws Exception {
        File src = tempDir("as-src"), dst = tempDir("as-dst");
        File big = randomFile(src, "big.bin", 8 * 1024 * 1024, 6);
        Receiver rx = new Receiver();
        rx.closeAfterBytes = 1024 * 1024;     // receiver's user taps Cancel: socket closed
        NativeBridge.Result[] r = session(dst, "t", rx, "t", new Sender(big));
        check(r[1].code == NativeBridge.RESULT_CANCELLED, "receiver cancelled: " + r[1]);
        check(r[0].code == NativeBridge.RESULT_TRANSPORT_FAILED, "sender sees lost link: " + r[0]);
        check(dst.list().length == 0, "no partial file");
    }

    static void testSourceError() throws Exception {
        File src = tempDir("as-src"), dst = tempDir("as-dst");
        Sender tx = new Sender(randomFile(src, "x.bin", 10, 7));
        tx.failOpen = true;
        NativeBridge.Result[] r = session(dst, "t", new Receiver(), "t", tx);
        check(r[0].code == NativeBridge.RESULT_LOCAL_FILE_ERROR, "unreadable source reported: " + r[0]);
        check(r[0].message.contains("permission denied"), "Java exception text propagated");
        check(dst.list().length == 0, "nothing written");
    }

    static void testManyFiles() throws Exception {
        File src = tempDir("as-src"), dst = tempDir("as-dst");
        File[] files = new File[600];   // exceeds JNI's default local-reference budget if refs leak
        for (int i = 0; i < files.length; i++) files[i] = randomFile(src, "f" + i + ".txt", i, i);
        NativeBridge.Result[] r = session(dst, "t", new Receiver(), "t", new Sender(files));
        check(r[0].isSuccess() && r[1].filesCompleted == 600, "600-file session: " + r[0]);
    }

    /** PULL frame written by hand: "ASPP" u8 version u16 len JSON (Protocol.hpp). */
    static byte[] pullFrame(String token, String receiver) {
        byte[] json = ("{\"receiver\":\"" + receiver + "\",\"token\":\"" + token + "\"}").getBytes(StandardCharsets.UTF_8);
        byte[] frame = new byte[7 + json.length];
        System.arraycopy("ASPP".getBytes(StandardCharsets.US_ASCII), 0, frame, 0, 4);
        frame[4] = 1;
        frame[5] = (byte) (json.length >> 8);
        frame[6] = (byte) json.length;
        System.arraycopy(json, 0, frame, 7, json.length);
        return frame;
    }

    /** The phone serves its files (sendFilesOnPull); the "laptop" connects, sends PULL, then receives. */
    static NativeBridge.Result[] pullSession(File dest, String phoneToken, String presentedToken, Sender tx)
            throws Exception {
        NativeBridge.Result[] results = new NativeBridge.Result[2];
        try (ServerSocket phone = new ServerSocket(0, 1, InetAddress.getLoopbackAddress())) {
            AtomicReference<Throwable> phoneError = new AtomicReference<>();
            Thread phoneThread = new Thread(() -> {
                try (Socket s = phone.accept()) {
                    String[] names = new String[tx.files.length];
                    String[] mimes = new String[tx.files.length];
                    long[] sizes = new long[tx.files.length];
                    for (int i = 0; i < tx.files.length; i++) {
                        names[i] = tx.files[i].getName();
                        mimes[i] = "application/octet-stream";
                        sizes[i] = tx.files[i].length();
                    }
                    results[0] = NativeBridge.sendFilesOnPull(s.getInputStream(), s.getOutputStream(), "tcp test",
                            "Lakshya's M34", phoneToken, names, mimes, sizes, tx);
                } catch (Throwable t) {
                    phoneError.set(t);
                }
            });
            phoneThread.start();
            try (Socket s = new Socket(InetAddress.getLoopbackAddress(), phone.getLocalPort())) {
                s.getOutputStream().write(pullFrame(presentedToken, "NILAY"));
                s.getOutputStream().flush();
                // The "laptop" accepts only the token it read from the tap: no prompt (accept = false).
                results[1] = NativeBridge.receiveFiles(s.getInputStream(), s.getOutputStream(), "tcp test",
                        dest.getPath(), presentedToken, 0, new Receiver());
            }
            phoneThread.join(30000);
            if (phoneError.get() != null) throw new RuntimeException(phoneError.get());
        }
        return results;
    }

    static void testPull() throws Exception {
        File src = tempDir("as-src"), dst = tempDir("as-dst");
        File a = randomFile(src, "holiday.jpg", 2 * 1024 * 1024 + 3, 8);
        File b = randomFile(src, "note.txt", 120, 9);

        NativeBridge.Result[] wrong = pullSession(dst, "0123456789abcdef", "ffffffffffffffff", new Sender(a, b));
        check(wrong[0].code == NativeBridge.RESULT_PULL_REFUSED, "wrong token refused by the phone: " + wrong[0]);
        check(!wrong[1].isSuccess(), "stranger receives nothing: " + wrong[1]);
        check(dst.list().length == 0, "nothing written for a stranger");

        Sender tx = new Sender(a, b);
        NativeBridge.Result[] ok = pullSession(dst, "0123456789abcdef", "0123456789abcdef", tx);
        check(ok[0].isSuccess(), "phone sent on pull: " + ok[0]);
        check(ok[0].senderName.equals("NILAY"), "phone learns who fetched: " + ok[0].senderName);
        check(ok[1].isSuccess() && ok[1].tokenMatched, "laptop accepted by token: " + ok[1]);
        check(ok[1].senderName.equals("Lakshya's M34"), "laptop sees the phone's name");
        for (File f : new File[]{a, b}) {
            File out = new File(dst, f.getName());
            check(out.exists() && Arrays.equals(Files.readAllBytes(f.toPath()), Files.readAllBytes(out.toPath())),
                    "identical after pull: " + f.getName());
        }
    }

    static String toHex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02X", x));
        return sb.toString();
    }

    public static void main(String[] args) throws Exception {
        System.out.println("native library: " + (NativeBridge.isAvailable() ? "loaded" : NativeBridge.loadError()));
        if (!NativeBridge.isAvailable()) System.exit(1);
        String[][] names = {{"constants/helpers"}, {"token transfer"}, {"prompt/reject"}, {"sender cancel"},
                {"receiver cancel"}, {"source error"}, {"600 files"}, {"pull (phone serves, laptop fetches)"}};
        Runnable[] tests = {
                JniBridgeTest::testConstantsAndHelpers,
                () -> run(JniBridgeTest::testTokenTransfer),
                () -> run(JniBridgeTest::testPromptAndReject),
                () -> run(JniBridgeTest::testSenderCancel),
                () -> run(JniBridgeTest::testReceiverCancel),
                () -> run(JniBridgeTest::testSourceError),
                () -> run(JniBridgeTest::testManyFiles),
                () -> run(JniBridgeTest::testPull),
        };
        for (int i = 0; i < tests.length; i++) {
            int before = failures;
            tests[i].run();
            System.out.println((failures == before ? "[  OK  ] " : "[ FAIL ] ") + names[i][0]);
        }
        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }

    interface ThrowingTest { void run() throws Exception; }

    static void run(ThrowingTest t) {
        try {
            t.run();
        } catch (Exception e) {
            failures++;
            System.out.println("  EXCEPTION " + e);
        }
    }
}
