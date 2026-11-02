package com.example.myapplication.transfer;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Java side of the JNI bridge to the shared C++ transfer core (ASP/1).
 *
 * Deliberately free of android.* imports so the bridge can be compiled and
 * tested on a desktop JVM (see tests/jni). All methods block and must be
 * called from a worker thread; callbacks arrive on that same thread.
 */
public final class NativeBridge {

    public static final int RESULT_SUCCESS = 0;
    public static final int RESULT_REJECTED = 1;
    public static final int RESULT_CANCELLED = 2;
    public static final int RESULT_TRANSPORT_FAILED = 3;
    public static final int RESULT_PROTOCOL_FAILED = 4;
    public static final int RESULT_LOCAL_FILE_ERROR = 5;
    public static final int RESULT_INTEGRITY_FAILED = 6;
    /** sendFilesOnPull: the connection did not present our tap token; nothing was sent. Keep waiting. */
    public static final int RESULT_PULL_REFUSED = 7;

    private static final String LOAD_ERROR;

    static {
        String error = null;
        try {
            System.loadLibrary("anyshare_jni");
        } catch (UnsatisfiedLinkError e) {
            error = e.getMessage();
        }
        LOAD_ERROR = error;
    }

    private NativeBridge() {}

    /** True if libanyshare_jni was loaded. Transfers are impossible otherwise (no simulation). */
    public static boolean isAvailable() {
        return LOAD_ERROR == null;
    }

    public static String loadError() {
        return LOAD_ERROR;
    }

    /** Callbacks from native code, invoked synchronously on the transfer thread. */
    public interface Callbacks {
        /** Polled after every chunk; return true to stop the transfer. */
        boolean isCancelled();

        /** Sender: open the data of file {@code index}. Native code closes it. */
        InputStream openSource(int index) throws IOException;

        /**
         * Receiver: an offer arrived without this session's tap token. May block
         * (e.g. waiting for the user); return true to accept.
         * Names are already sanitised exactly as they will be saved.
         */
        boolean onOffer(String senderName, String[] names, long[] sizes);

        void onSessionStarted(int fileCount, long totalBytes);

        void onFileStarted(int index, String name, long size);

        /** Called after every chunk. Throttle UI work. */
        void onBytes(long done, long total);

        /** Sender: the receiver acknowledged the checksum. Receiver: file saved at savedPath. */
        void onFileCompleted(int index, String name, String savedPath);

        void onError(String message);
    }

    /** Adapter with no-op defaults. */
    public abstract static class BaseCallbacks implements Callbacks {
        @Override public InputStream openSource(int index) throws IOException {
            throw new IOException("not a sender");
        }
        @Override public boolean onOffer(String senderName, String[] names, long[] sizes) { return false; }
        @Override public void onSessionStarted(int fileCount, long totalBytes) {}
        @Override public void onFileStarted(int index, String name, long size) {}
        @Override public void onBytes(long done, long total) {}
        @Override public void onFileCompleted(int index, String name, String savedPath) {}
        @Override public void onError(String message) {}
    }

    /** Outcome of a session. Only RESULT_SUCCESS means every file was checksum-verified. */
    public static final class Result {
        public final int code;
        public final String message;
        public final int filesCompleted;
        public final boolean tokenMatched;
        public final String senderName;   // receiveFiles: who sent; sendFilesOnPull: who fetched

        public Result(int code, String message, int filesCompleted, boolean tokenMatched, String senderName) {
            this.code = code;
            this.message = message;
            this.filesCompleted = filesCompleted;
            this.tokenMatched = tokenMatched;
            this.senderName = senderName;
        }

        public boolean isSuccess() {
            return code == RESULT_SUCCESS;
        }

        @Override public String toString() {
            return "Result{code=" + code + ", files=" + filesCompleted + ", message=" + message + "}";
        }
    }

    /** {"protocol":1,"uuid":"...","aid":"...","serviceName":"AnyShare"} from the C++ core. */
    public static native String protocolInfo();

    public static native String sanitizeFilename(String name);

    /** Validates a tap payload; returns canonical JSON or throws IllegalArgumentException. */
    public static native String parseHandover(String json);

    public static native Result sendFiles(InputStream in, OutputStream out, String linkDescription,
                                          String senderName, String token,
                                          String[] names, String[] mimeTypes, long[] sizes,
                                          Callbacks callbacks);

    /**
     * Pull: on a connection this phone accepted after publishing {"role":"send",...,"token"} by NFC.
     * Sends only if the peer presents {@code token}; otherwise returns RESULT_PULL_REFUSED without
     * revealing anything. On success {@link Result#senderName} is the name of the device that fetched.
     */
    public static native Result sendFilesOnPull(InputStream in, OutputStream out, String linkDescription,
                                                String senderName, String token,
                                                String[] names, String[] mimeTypes, long[] sizes,
                                                Callbacks callbacks);

    /** @param maxTotalBytes 0 = core default limit */
    public static native Result receiveFiles(InputStream in, OutputStream out, String linkDescription,
                                             String destinationDir, String expectedToken,
                                             long maxTotalBytes, Callbacks callbacks);
}
