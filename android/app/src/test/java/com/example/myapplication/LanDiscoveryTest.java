package com.example.myapplication;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import com.example.myapplication.transfer.LanDiscovery;

import org.junit.Test;

import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.List;

/**
 * JVM unit tests for finding receivers by name (no device needed): ./gradlew test
 * Set ANYSHARE_LINUX_RESPONDER=host:udpPort to also find a running Linux receiver.
 */
public class LanDiscoveryTest {

    @Test
    public void replyFormatMatchesLinux() {
        // Same text as linux/src/LanDiscovery.cpp encodeReply("nilay-laptop", 8080, "linux").
        assertEquals("ANYSHARE!1|8080|tcp|linux|nilay-laptop", LanDiscovery.encodeReply("nilay-laptop", 8080, "linux"));
        LanDiscovery.Peer p = LanDiscovery.parseReply("ANYSHARE!1|8080|tcp|android|Almas's Galaxy | A52", "10.0.0.7");
        assertNotNull(p);
        assertEquals("Almas's Galaxy | A52", p.name);
        assertEquals(8080, p.port);
        assertEquals("Almas's Galaxy | A52  (Android · 10.0.0.7)", p.label());
    }

    @Test
    public void rejectsBadReplies() {
        assertNull(LanDiscovery.parseReply("ANYSHARE!1|99999|tcp|linux|x", "h"));
        assertNull(LanDiscovery.parseReply("ANYSHARE!1|8080|udp|linux|x", "h"));
        assertNull(LanDiscovery.parseReply("ANYSHARE!1|8080|tcp", "h"));
        assertNull(LanDiscovery.parseReply("HELLO", "h"));
    }

    @Test
    public void cleansNames() {
        assertEquals("badname!", LanDiscovery.cleanName("bad\nname\t!"));
        assertEquals("AnyShare device", LanDiscovery.cleanName(""));
        StringBuilder longName = new StringBuilder();
        for (int i = 0; i < 40; i++) longName.append('é');   // 80 UTF-8 bytes
        String cut = LanDiscovery.cleanName(longName.toString());
        assertEquals(64, cut.getBytes(StandardCharsets.UTF_8).length);
        assertEquals(32, cut.length());
    }

    @Test
    public void findsResponderOnLoopback() throws Exception {
        try (LanDiscovery.Responder r = new LanDiscovery.Responder("test-phone", 9123, "android", 0)) {
            List<InetAddress> targets = Collections.singletonList(InetAddress.getByName("127.0.0.1"));
            long start = System.currentTimeMillis();
            List<LanDiscovery.Peer> peers = LanDiscovery.discover(600, r.udpPort(), targets, null);
            assertTrue(System.currentTimeMillis() - start < 1500);
            assertEquals(1, peers.size());   // three requests, one entry
            assertEquals("test-phone", peers.get(0).name);
            assertEquals(9123, peers.get(0).port);
            assertEquals("android", peers.get(0).os);
        }
    }

    @Test
    public void findsLinuxReceiverWhenConfigured() throws Exception {
        String target = System.getenv("ANYSHARE_LINUX_RESPONDER");
        if (target == null) return;   // optional cross-language check
        String host = target.substring(0, target.lastIndexOf(':'));
        int port = Integer.parseInt(target.substring(target.lastIndexOf(':') + 1));
        List<LanDiscovery.Peer> peers = LanDiscovery.discover(1500, port,
                Collections.singletonList(InetAddress.getByName(host)), null);
        assertEquals(1, peers.size());
        assertEquals("linux", peers.get(0).os);
        System.out.println("Found Linux receiver: " + peers.get(0).label() + " port " + peers.get(0).port);
    }
}
