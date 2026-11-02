package com.example.myapplication.transfer;

import java.io.Closeable;
import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Finds AnyShare receivers on the local network by name, so nobody types an
 * IP address. Plain UDP broadcast (no mDNS); same format as the Linux side
 * (linux/include/LanDiscovery.hpp):
 *
 * <pre>
 *   sender   -> broadcast "ANYSHARE?1"                              to UDP 48080
 *   receiver -> unicast   "ANYSHARE!1|&lt;tcp port&gt;|tcp|&lt;os&gt;|&lt;name&gt;"
 * </pre>
 *
 * The receiver's IP is the reply's source address. Replies carry no session
 * token, so the receiver still asks the user to accept. Pure Java (desktop-testable).
 */
public final class LanDiscovery {
    public static final int PORT = 48080;
    public static final String REQUEST = "ANYSHARE?1";
    public static final String REPLY_PREFIX = "ANYSHARE!1|";
    private static final int MAX_NAME_BYTES = 64;

    private LanDiscovery() {}

    public static final class Peer {
        public final String name;
        public final String host;
        public final int port;
        public final String os;

        Peer(String name, String host, int port, String os) {
            this.name = name;
            this.host = host;
            this.port = port;
            this.os = os;
        }

        /** e.g. "Nilay Laptop  (Linux · 10.213.46.14)" */
        public String label() {
            String kind = os.isEmpty() ? "" : Character.toUpperCase(os.charAt(0)) + os.substring(1) + " · ";
            return name + "  (" + kind + host + ")";
        }
    }

    /** Called on the discovery thread for every new receiver. */
    public interface Listener {
        void onPeer(Peer peer);
    }

    /** Control characters removed, at most 64 UTF-8 bytes, never cut inside a character. */
    public static String cleanName(String name) {
        StringBuilder sb = new StringBuilder();
        int bytes = 0;
        for (int i = 0; i < name.length(); ) {
            int cp = name.codePointAt(i);
            i += Character.charCount(cp);
            if (cp < 0x20 || cp == 0x7F) continue;
            int size = new String(Character.toChars(cp)).getBytes(StandardCharsets.UTF_8).length;
            if (bytes + size > MAX_NAME_BYTES) break;
            bytes += size;
            sb.appendCodePoint(cp);
        }
        return sb.length() == 0 ? "AnyShare device" : sb.toString();
    }

    public static String encodeReply(String name, int tcpPort, String os) {
        return REPLY_PREFIX + tcpPort + "|tcp|" + os + "|" + cleanName(name);
    }

    /** Returns null for anything that is not a valid reply. */
    public static Peer parseReply(String datagram, String host) {
        if (!datagram.startsWith(REPLY_PREFIX)) return null;
        String rest = datagram.substring(REPLY_PREFIX.length());
        String[] head = rest.split("\\|", 4);   // port, transport, os, name (the name may contain '|')
        if (head.length != 4 || !"tcp".equals(head[1])) return null;
        if (!head[0].matches("\\d{1,5}")) return null;
        int port = Integer.parseInt(head[0]);
        if (port < 1 || port > 65535) return null;
        return new Peer(cleanName(head[3]), host, port, head[2]);
    }

    /** Every interface's broadcast address, plus 255.255.255.255. */
    public static List<InetAddress> broadcastTargets() {
        List<InetAddress> out = new ArrayList<>();
        try {
            out.add(InetAddress.getByName("255.255.255.255"));
            for (NetworkInterface nif : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nif.isUp() || nif.isLoopback()) continue;
                for (InterfaceAddress a : nif.getInterfaceAddresses()) {
                    InetAddress b = a.getBroadcast();
                    if (b != null && !out.contains(b)) out.add(b);
                }
            }
        } catch (IOException e) {
            // keep what we have: the global broadcast address still works on most networks
        }
        return out;
    }

    /**
     * Blocking. Sends the request three times over {@code timeoutMs} (Wi-Fi
     * drops broadcasts), reports each new receiver as it answers, and returns
     * all of them, one per name and port.
     */
    public static List<Peer> discover(int timeoutMs, int udpPort, List<InetAddress> targets, Listener listener) {
        List<Peer> peers = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        byte[] request = REQUEST.getBytes(StandardCharsets.US_ASCII);
        try (DatagramSocket socket = new DatagramSocket()) {
            socket.setBroadcast(true);
            long start = System.currentTimeMillis();
            long deadline = start + timeoutMs;
            int sent = 0;
            byte[] buf = new byte[512];
            while (true) {
                long now = System.currentTimeMillis();
                if (now >= deadline) break;
                if (sent < 3 && now >= start + (long) timeoutMs * sent / 3) {
                    for (InetAddress t : targets) {
                        try {
                            socket.send(new DatagramPacket(request, request.length, t, udpPort));
                        } catch (IOException ignored) {
                            // some interfaces refuse broadcast; the others still work
                        }
                    }
                    sent++;
                }
                socket.setSoTimeout((int) Math.max(1, Math.min(100, deadline - now)));
                DatagramPacket packet = new DatagramPacket(buf, buf.length);
                try {
                    socket.receive(packet);
                } catch (SocketTimeoutException e) {
                    continue;
                }
                String text = new String(packet.getData(), 0, packet.getLength(), StandardCharsets.UTF_8);
                Peer peer = parseReply(text, packet.getAddress().getHostAddress());
                if (peer == null || !seen.add(peer.name + "|" + peer.port)) continue;
                peers.add(peer);
                if (listener != null) listener.onPeer(peer);
            }
        } catch (IOException e) {
            // no network: nothing found
        }
        return peers;
    }

    /** Answers discovery requests until closed. */
    public static final class Responder implements Closeable {
        private final DatagramSocket socket;
        private final byte[] reply;
        private final Thread thread;
        private volatile boolean open = true;

        public Responder(String name, int tcpPort, String os, int udpPort) throws SocketException {
            reply = encodeReply(name, tcpPort, os).getBytes(StandardCharsets.UTF_8);
            socket = new DatagramSocket(null);
            socket.setReuseAddress(true);
            socket.bind(new InetSocketAddress(udpPort));
            thread = new Thread(this::run, "anyshare-discovery");
            thread.setDaemon(true);
            thread.start();
        }

        public int udpPort() {
            return socket.getLocalPort();
        }

        private void run() {
            byte[] buf = new byte[512];
            while (open) {
                DatagramPacket packet = new DatagramPacket(buf, buf.length);
                try {
                    socket.receive(packet);
                    String text = new String(packet.getData(), 0, packet.getLength(), StandardCharsets.US_ASCII).trim();
                    if (REQUEST.equals(text)) {
                        socket.send(new DatagramPacket(reply, reply.length, packet.getSocketAddress()));
                    }
                } catch (IOException e) {
                    if (!open) return;   // closed
                }
            }
        }

        @Override
        public void close() {
            open = false;
            socket.close();
        }
    }
}
