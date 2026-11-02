package com.example.myapplication;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import com.example.myapplication.nfc.ApduProtocol;

import org.junit.Test;

/** JVM unit tests for the NFC APDU format (no device needed): ./gradlew test */
public class ApduProtocolTest {

    @Test
    public void selectCommandMatchesLinuxReader() {
        // Same bytes the PN532 reader sends (linux/src/Pn532NfcHandover.cpp selectAidApdu()).
        byte[] expected = {0x00, (byte) 0xA4, 0x04, 0x00, 0x07,
                (byte) 0xF0, 0x41, 0x4E, 0x59, 0x53, 0x48, 0x52, 0x00};
        assertArrayEquals(expected, ApduProtocol.selectAidCommand());
        assertTrue(ApduProtocol.isSelectAid(expected));
    }

    @Test
    public void rejectsOtherCommands() {
        assertFalse(ApduProtocol.isSelectAid(new byte[]{0x00, (byte) 0xA4, 0x04, 0x00, 0x07, 1, 2, 3, 4, 5, 6, 7}));
        assertFalse(ApduProtocol.isSelectAid(new byte[]{0x00}));
        assertFalse(ApduProtocol.isSelectAid(null));
    }

    @Test
    public void payloadRoundTrip() {
        String json = "{\"v\":1,\"transport\":\"rfcomm\",\"name\":\"Pixel 7\",\"token\":\"0123456789abcdef\"}";
        assertEquals(json, ApduProtocol.parseSelectResponse(ApduProtocol.buildSelectResponse(json)));
    }

    @Test(expected = IllegalArgumentException.class)
    public void notInReceiveMode() {
        ApduProtocol.parseSelectResponse(ApduProtocol.buildSelectResponse(null));
    }
}
