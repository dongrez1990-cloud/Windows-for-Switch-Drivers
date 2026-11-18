package com.paopao.switchkey;

import java.util.ArrayList;
import java.util.List;

/**
 * PointerPump 的 JVM 单元测试（假时钟，不依赖 Android）：
 *   javac -encoding UTF-8 -d out PointerPump.java PointerPumpTest.java
 *   java -cp out com.paopao.switchkey.PointerPumpTest
 */
public final class PointerPumpTest {

    private static int passed;
    private static int failed;

    // ---- 假时钟 ----
    private static final class FakeClock implements PointerPump.Clock {
        long now;
        final List<Object[]> queue = new ArrayList<>(); // {Long delayDue, Runnable}

        @Override
        public long now() {
            return now;
        }

        @Override
        public void postDelayed(Runnable r, long delayMs) {
            queue.add(new Object[] { now + delayMs, r });
        }

        @Override
        public void cancel(Runnable r) {
            for (int i = queue.size() - 1; i >= 0; i--) {
                if (queue.get(i)[1] == r) {
                    queue.remove(i);
                }
            }
        }

        /** 推进到 t，并执行所有到期的任务 */
        void advanceTo(long t) {
            now = t;
            boolean ran = true;
            while (ran) {
                ran = false;
                for (int i = 0; i < queue.size(); i++) {
                    if ((Long) queue.get(i)[0] <= now) {
                        Runnable r = (Runnable) queue.remove(i)[1];
                        r.run();
                        ran = true;
                        break;
                    }
                }
            }
        }

        int liveTimers() {
            return queue.size();
        }
    }

    private static final class Recorder implements PointerPump.Sink {
        final List<String> out = new ArrayList<>();

        @Override
        public void onMouse(int buttons, int dx, int dy, int wheel) {
            out.add(buttons + "," + dx + "," + dy + "," + wheel);
        }

        String last() {
            return out.isEmpty() ? "-" : out.get(out.size() - 1);
        }
    }

    private static FakeClock clock;
    private static Recorder rec;
    private static PointerPump pump;

    private static void setup(int intervalMs) {
        clock = new FakeClock();
        rec = new Recorder();
        pump = new PointerPump(rec, clock);
        pump.setIntervalMs(intervalMs);
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

    public static void main(String[] args) {
        System.out.println("PointerPumpTest");

        // 1. 第一帧立刻发（不能有启动延迟）
        setup(PointerPump.RATE_60HZ);
        pump.onEvent(0, 3, -2, 0);
        check("首帧立即发送", rec.out.size() == 1 && rec.last().equals("0,3,-2,0"), rec.last());

        // 2. 时间窗内的多次移动合并成一帧
        setup(PointerPump.RATE_60HZ);
        pump.onEvent(0, 2, 0, 0);      // t=0 立即发
        clock.advanceTo(5);
        pump.onEvent(0, 3, 1, 0);      // 排队
        pump.onEvent(0, 4, 1, 0);      // 继续累加，不重排定时器
        check("合并期间不额外发送", rec.out.size() == 1, "size=" + rec.out.size());
        check("只挂一个定时器", clock.liveTimers() == 1, "timers=" + clock.liveTimers());
        clock.advanceTo(16);           // 到期
        check("到期一次发出累加位移",
                rec.out.size() == 2 && rec.last().equals("0,7,2,0"), rec.last());

        // 3. 按键变化立刻发，不等时间窗
        setup(PointerPump.RATE_60HZ);
        pump.onEvent(0, 5, 0, 0);      // t=0
        clock.advanceTo(3);
        pump.onEvent(1, 0, 0, 0);      // 按下左键
        check("按键变化立即发送", rec.out.size() == 2 && rec.last().equals("1,0,0,0"), rec.last());

        // 4. 滚轮立刻发
        setup(PointerPump.RATE_60HZ);
        pump.onEvent(0, 0, 0, 0);      // 无位移无按键 -> 不发
        pump.onEvent(0, 1, 0, 0);      // t=0 发
        clock.advanceTo(2);
        pump.onEvent(0, 0, 0, -1);     // 滚轮
        check("滚轮立即发送", rec.out.size() == 2 && rec.last().equals("0,0,0,-1"), rec.last());

        // 5. 不限速：每个事件一帧
        setup(PointerPump.RATE_OFF);
        pump.onEvent(0, 1, 1, 0);
        pump.onEvent(0, 1, 1, 0);
        pump.onEvent(0, 1, 1, 0);
        check("RATE_OFF 每事件一帧", rec.out.size() == 3, "size=" + rec.out.size());

        // 6. int8 截断
        setup(PointerPump.RATE_OFF);
        pump.onEvent(0, 500, -900, 0);
        check("位移截断到 ±127", rec.last().equals("0,127,-127,0"), rec.last());

        // 7. flush() 把待发位移立刻送出
        setup(PointerPump.RATE_60HZ);
        pump.onEvent(0, 6, 6, 0);      // t=0 发
        clock.advanceTo(4);
        pump.onEvent(0, 2, 2, 0);      // 排队
        pump.flush();
        check("flush 立即送出排队位移", rec.out.size() == 2 && rec.last().equals("0,2,2,0"), rec.last());

        // 8. reset() 丢弃排队位移
        setup(PointerPump.RATE_60HZ);
        pump.onEvent(0, 6, 6, 0);
        clock.advanceTo(4);
        pump.onEvent(0, 9, 9, 0);
        pump.reset();
        clock.advanceTo(100);
        check("reset 后不再发送", rec.out.size() == 1, "size=" + rec.out.size());

        // 9. 30Hz 档的窗口更宽
        setup(PointerPump.RATE_30HZ);
        pump.onEvent(0, 1, 0, 0);      // t=0
        clock.advanceTo(20);
        pump.onEvent(0, 1, 0, 0);      // 20 < 33 -> 排队
        clock.advanceTo(33);
        check("30Hz：33ms 一帧", rec.out.size() == 2 && rec.last().equals("0,1,0,0"), rec.last());

        // 10. 没有变化不乱发帧（避免空报告占链路）
        setup(PointerPump.RATE_OFF);
        pump.onEvent(0, 0, 0, 0);
        pump.onEvent(0, 0, 0, 0);
        check("空事件不发帧", rec.out.size() == 0, "size=" + rec.out.size());

        // 11. 松手回到全零也会发（避免按键卡住）
        setup(PointerPump.RATE_60HZ);
        pump.onEvent(1, 0, 0, 0);
        clock.advanceTo(5);
        pump.onEvent(0, 0, 0, 0);
        check("松开按键立即上报", rec.out.size() == 2 && rec.last().equals("0,0,0,0"), rec.last());

        System.out.println(failed == 0
                ? "  ALL PASSED  (" + passed + ")"
                : "  FAILED " + failed + " / " + (passed + failed));
        if (failed != 0) {
            System.exit(1);
        }
    }
}
