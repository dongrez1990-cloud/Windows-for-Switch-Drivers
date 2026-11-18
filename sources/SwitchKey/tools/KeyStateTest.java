package com.paopao.switchkey;

/**
 * KeyState 的 JVM 单元测试（不依赖 Android，直接 java 跑）：
 *   javac -encoding UTF-8 -d out KeyState.java KeyStateTest.java && java -cp out com.paopao.switchkey.KeyStateTest
 */
public final class KeyStateTest {

    private static int passed;
    private static int failed;

    // 最近一次报告
    private static int lastMods;
    private static int[] lastUsages = new int[6];
    private static int lastCount;

    private static KeyState newState() {
        KeyState s = new KeyState();
        s.setSink(new KeyState.Sink() {
            @Override
            public void onReport(int modifiers, int[] usages, int count) {
                lastMods = modifiers;
                lastCount = count;
                lastUsages = usages.clone();
            }
        });
        lastMods = 0;
        lastCount = 0;
        lastUsages = new int[6];
        return s;
    }

    private static void check(String name, boolean ok, String detail) {
        if (ok) {
            passed++;
            System.out.println("  PASS  " + name);
        } else {
            failed++;
            System.out.println("  FAIL  " + name + "  (" + detail + ")");
        }
    }

    private static void expectReport(String name, int mods, int[] usages) {
        StringBuilder got = new StringBuilder();
        got.append("mods=0x").append(Integer.toHexString(lastMods)).append(" keys=");
        for (int i = 0; i < lastCount; i++) {
            got.append("0x").append(Integer.toHexString(lastUsages[i])).append(" ");
        }
        StringBuilder want = new StringBuilder();
        want.append("mods=0x").append(Integer.toHexString(mods)).append(" keys=");
        for (int u : usages) {
            want.append("0x").append(Integer.toHexString(u)).append(" ");
        }
        boolean ok = lastMods == mods && lastCount == usages.length;
        for (int i = 0; ok && i < usages.length; i++) {
            ok = lastUsages[i] == usages[i];
        }
        check(name, ok, "want " + want + " / got " + got);
    }

    public static void main(String[] args) {
        final int A = 0x04;
        final int B = 0x05;
        final int C = 0x06;
        final int SHIFT = 0x02; // MOD_LSHIFT
        final int CTRL = 0x01;  // MOD_LCTRL

        // 1. 普通打字
        System.out.println("1) plain key");
        KeyState s = newState();
        s.keyDown(A, 0);
        expectReport("a down", 0, new int[] { A });
        s.keyUp(A, 0);
        expectReport("a up", 0, new int[] {});

        // 2. 按住 Shift + A（组合键）
        System.out.println("2) hold shift + a");
        s = newState();
        s.keyDown(0, SHIFT);
        expectReport("shift down", SHIFT, new int[] {});
        s.keyDown(A, 0);
        expectReport("shift+a down", SHIFT, new int[] { A });
        s.keyUp(A, 0);
        expectReport("a up", SHIFT, new int[] {});
        s.keyUp(0, SHIFT);
        expectReport("shift up (no sticky)", 0, new int[] {});
        check("held combo did not become sticky", s.activeMods() == 0, "mods=" + s.activeMods());

        // 3. 轻点 Shift → 一次性
        System.out.println("3) tap shift then a");
        s = newState();
        s.setNow(1000);
        s.keyDown(0, SHIFT);
        s.keyUp(0, SHIFT);
        expectReport("shift tapped", SHIFT, new int[] {});
        s.keyDown(A, 0);
        expectReport("sticky shift + a", SHIFT, new int[] { A });
        s.keyUp(A, 0);
        expectReport("sticky cleared after key", 0, new int[] {});

        // 4. 双击 Shift → 锁定，再点解除
        System.out.println("4) double tap shift = lock");
        s = newState();
        s.setNow(1000);
        s.keyDown(0, SHIFT);
        s.keyUp(0, SHIFT);
        s.setNow(1100);
        s.keyDown(0, SHIFT);
        s.keyUp(0, SHIFT);
        expectReport("locked", SHIFT, new int[] {});
        s.setNow(2000);
        s.keyDown(A, 0);
        expectReport("locked shift + a", SHIFT, new int[] { A });
        s.keyUp(A, 0);
        expectReport("lock survives key release", SHIFT, new int[] {});
        s.setNow(3000);
        s.keyDown(0, SHIFT);
        expectReport("tap unlocks immediately", 0, new int[] {});

        // 5. 多指同时按
        System.out.println("5) multi touch");
        s = newState();
        s.keyDown(A, 0);
        s.keyDown(B, 0);
        s.keyDown(C, 0);
        expectReport("a+b+c", 0, new int[] { A, B, C });
        s.keyUp(B, 0);
        expectReport("b released", 0, new int[] { A, C });

        // 6. 最多 6 键
        System.out.println("6) 6KRO limit");
        s = newState();
        for (int i = 0; i < 9; i++) {
            s.keyDown(0x04 + i, 0);
        }
        check("caps at 6 keys", lastCount == 6 && s.heldCount() == 6, "held=" + s.heldCount());

        // 7. Ctrl+Shift+C 三键组合
        System.out.println("7) ctrl+shift+c");
        s = newState();
        s.keyDown(0, CTRL);
        s.keyDown(0, SHIFT);
        s.keyDown(C, 0);
        expectReport("ctrl+shift+c", CTRL | SHIFT, new int[] { C });

        // 8. releaseAll
        System.out.println("8) release all");
        s = newState();
        s.keyDown(0, SHIFT);
        s.keyDown(A, 0);
        s.releaseAll();
        expectReport("all up", 0, new int[] {});
        check("no sticky left", s.activeMods() == 0, "mods=" + s.activeMods());

        System.out.println();
        System.out.println("passed=" + passed + " failed=" + failed);
        if (failed > 0) {
            System.exit(1);
        }
    }
}
