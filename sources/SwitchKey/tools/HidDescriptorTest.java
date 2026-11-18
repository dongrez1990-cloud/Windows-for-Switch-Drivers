package com.paopao.switchkey;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.TreeMap;

/**
 * HID 报告描述符解析测试：确认复合设备里键盘/鼠标各自的报告长度正确。
 *   javac -encoding UTF-8 -d out HidDescriptors.java HidDescriptorTest.java
 *   java -cp out com.paopao.switchkey.HidDescriptorTest
 */
public final class HidDescriptorTest {

    private static int passed;
    private static int failed;

    private static void check(String name, boolean ok, String detail) {
        if (ok) {
            passed++;
            System.out.println("  PASS  " + name);
        } else {
            failed++;
            System.out.println("  FAIL  " + name + "  (" + detail + ")");
        }
    }

    public static void main(String[] args) {
        byte[] map = HidDescriptors.REPORT_MAP;

        int reportId = 0;
        int reportSize = 0;
        int reportCount = 0;
        int depth = 0;
        int maxDepth = 0;
        boolean longItem = false;

        // reportId -> [inputBits, outputBits]
        Map<Integer, int[]> bits = new TreeMap<>();
        Map<Integer, Integer> collectionsPerId = new LinkedHashMap<>();

        int i = 0;
        while (i < map.length) {
            int b = map[i] & 0xFF;
            int tag = (b >> 4) & 0x0F;
            int type = (b >> 2) & 0x03;
            int sizeCode = b & 0x03;
            if (sizeCode == 3) {
                longItem = true; // 0xFE / 0xFF 长条目，本描述符不该出现
                break;
            }
            int size = sizeCode;
            long data = 0;
            for (int k = 0; k < size; k++) {
                data |= ((long) (map[i + 1 + k] & 0xFF)) << (8 * k);
            }
            i += 1 + size;

            if (type == 1) { // global
                if (tag == 7) {
                    reportSize = (int) data;
                } else if (tag == 8) {
                    reportId = (int) data;
                } else if (tag == 9) {
                    reportCount = (int) data;
                }
            } else if (type == 0) { // main
                if (tag == 10) { // Collection
                    depth++;
                    maxDepth = Math.max(maxDepth, depth);
                    Integer c = collectionsPerId.get(reportId);
                    collectionsPerId.put(reportId, c == null ? 1 : c + 1);
                } else if (tag == 12) { // End Collection
                    depth--;
                } else if (tag == 8 || tag == 9 || tag == 11) { // Input / Output / Feature
                    int[] slot = bits.get(reportId);
                    if (slot == null) {
                        slot = new int[3];
                        bits.put(reportId, slot);
                    }
                    int idx = tag == 8 ? 0 : (tag == 9 ? 1 : 2);
                    slot[idx] += reportSize * reportCount;
                }
            }
        }

        check("no long items", !longItem, "0xFE/0xFF item found");
        check("collections balanced", depth == 0, "depth=" + depth);
        check("two top-level collections", maxDepth == 2, "maxDepth=" + maxDepth);

        int[] kb = bits.get(HidDescriptors.REPORT_ID_KEYBOARD);
        int[] ms = bits.get(HidDescriptors.REPORT_ID_MOUSE);
        check("report id 1 present", kb != null, "missing");
        check("report id 2 present", ms != null, "missing");
        check("only report ids 1 and 2", bits.size() == 2, "ids=" + bits.keySet());

        if (kb != null) {
            check("keyboard input = " + HidDescriptors.KEYBOARD_PAYLOAD + " bytes",
                    kb[0] == HidDescriptors.KEYBOARD_PAYLOAD * 8,
                    "bits=" + kb[0]);
            check("keyboard output (LED) = 1 byte", kb[1] == 8, "bits=" + kb[1]);
            check("keyboard has no feature report", kb[2] == 0, "bits=" + kb[2]);
        }
        if (ms != null) {
            check("mouse input = " + HidDescriptors.MOUSE_PAYLOAD + " bytes",
                    ms[0] == HidDescriptors.MOUSE_PAYLOAD * 8,
                    "bits=" + ms[0]);
        }

        System.out.println();
        System.out.println("descriptor bytes = " + map.length);
        for (Map.Entry<Integer, int[]> e : bits.entrySet()) {
            System.out.println("  report id " + e.getKey()
                    + ": input " + (e.getValue()[0] / 8) + "B"
                    + ", output " + (e.getValue()[1] / 8) + "B"
                    + ", feature " + (e.getValue()[2] / 8) + "B");
        }
        System.out.println("passed=" + passed + " failed=" + failed);
        if (failed > 0) {
            System.exit(1);
        }
    }
}
