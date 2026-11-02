package com.example.myapplication.nfc;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/**
 * ISO 7816-4 APDUs exchanged during an NFC tap. Pure Java (desktop-testable).
 *
 * The receiving phone emulates a card (HCE) under the proprietary AID
 * F0 41 4E 59 53 48 52 ("ANYSHR"); the sender (another phone in reader mode,
 * or a PN532 on Linux) SELECTs it and gets the receiver's handover JSON back.
 * Must match protocol::kHceAidHex in core-engine/include/Protocol.hpp.
 */
public final class ApduProtocol {
    public static final String AID_HEX = "F0414E59534852";
    public static final byte[] AID = hexToBytes(AID_HEX);
    public static final byte[] SW_OK = {(byte) 0x90, 0x00};
    public static final byte[] SW_FILE_NOT_FOUND = {0x6A, (byte) 0x82};     // not in receive mode
    public static final byte[] SW_INS_NOT_SUPPORTED = {0x6D, 0x00};
    /** Response data must fit a short APDU (max 256 bytes incl. status word). */
    public static final int MAX_PAYLOAD_BYTES = 250;

    private ApduProtocol() {}

    public static byte[] selectAidCommand() {
        byte[] cmd = new byte[6 + AID.length];
        cmd[0] = 0x00;          // CLA
        cmd[1] = (byte) 0xA4;   // INS: SELECT
        cmd[2] = 0x04;          // P1: by DF name (AID)
        cmd[3] = 0x00;          // P2
        cmd[4] = (byte) AID.length;
        System.arraycopy(AID, 0, cmd, 5, AID.length);
        cmd[cmd.length - 1] = 0x00;   // Le
        return cmd;
    }

    public static boolean isSelectAid(byte[] apdu) {
        if (apdu == null || apdu.length < 5 + AID.length) return false;
        if (apdu[0] != 0x00 || apdu[1] != (byte) 0xA4 || apdu[2] != 0x04) return false;
        if ((apdu[4] & 0xFF) != AID.length) return false;
        return Arrays.equals(Arrays.copyOfRange(apdu, 5, 5 + AID.length), AID);
    }

    /** Card side: payload + 90 00, or 6A 82 when there is nothing to offer. */
    public static byte[] buildSelectResponse(String payloadJson) {
        if (payloadJson == null) return SW_FILE_NOT_FOUND;
        byte[] data = payloadJson.getBytes(StandardCharsets.UTF_8);
        if (data.length > MAX_PAYLOAD_BYTES) throw new IllegalArgumentException("handover payload too long");
        byte[] out = Arrays.copyOf(data, data.length + 2);
        out[data.length] = SW_OK[0];
        out[data.length + 1] = SW_OK[1];
        return out;
    }

    /** Reader side: returns the JSON or throws with the status word. */
    public static String parseSelectResponse(byte[] response) {
        if (response == null || response.length < 2) throw new IllegalArgumentException("empty APDU response");
        int sw = ((response[response.length - 2] & 0xFF) << 8) | (response[response.length - 1] & 0xFF);
        if (sw != 0x9000) {
            throw new IllegalArgumentException(String.format("receiver refused (SW %04X): is it in receive mode?", sw));
        }
        return new String(response, 0, response.length - 2, StandardCharsets.UTF_8);
    }

    static byte[] hexToBytes(String hex) {
        byte[] out = new byte[hex.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(hex.substring(2 * i, 2 * i + 2), 16);
        }
        return out;
    }
}
