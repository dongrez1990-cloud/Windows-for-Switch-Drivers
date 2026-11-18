package com.switchpad;

/**
 * 相对指针移动模型：把触摸屏上的浮点位移换算成 HID 鼠标报告可用的 int8 增量。
 *
 * 关键点是<b>亚像素（sub-count）累加</b>：手指一次只移动 0.4 个单位时，
 * 若直接四舍五入成 0 就会永远不动；这里把余量保留到下一次，
 * 于是慢速细调不会丢失，快速划动也不会因为超过 127 被截断而“跳变大”。
 */
public final class PointerModel {

    /** 单次报告允许的最大位移（int8 有符号范围） */
    public static final int MAX_STEP = 127;

    private float residueX;
    private float residueY;

    private float sensitivity = 1.0f;
    private boolean acceleration = true;

    /** 防止主机不消费报告时残差无限堆积 */
    private static final float RESIDUE_LIMIT = 4096f;

    public void setSensitivity(float s) {
        if (s < 0.05f) s = 0.05f;
        if (s > 4f) s = 4f;
        this.sensitivity = s;
    }

    public float sensitivity() {
        return sensitivity;
    }

    public void setAcceleration(boolean on) {
        this.acceleration = on;
    }

    public boolean acceleration() {
        return acceleration;
    }

    public void reset() {
        residueX = 0f;
        residueY = 0f;
    }

    /**
     * 吃进一次原始位移，返回本次应发送的整数增量。
     *
     * @param dx   手指横向位移（屏幕像素，右为正）
     * @param dy   手指纵向位移（屏幕像素，下为正）
     * @param dtMs 距上次事件的毫秒数；<=0 表示未知（按无加速处理）
     * @return {ix, iy}，已限制在 [-127, 127]，未消费的零头留在内部
     */
    public int[] offer(float dx, float dy, float dtMs) {
        float gain = sensitivity;
        if (acceleration) {
            float speed = 0f;
            if (dtMs > 0.5f) {
                // 像素/秒
                speed = (float) (Math.sqrt(dx * dx + dy * dy) / dtMs * 1000.0);
            }
            // 慢速 1.0x，快速最多 2.0x：手机上"整屏滑动"约 540*灵敏度 个 counts 才合手
            float boost = Math.min(speed / 3000f, 1.0f);
            gain *= (1.0f + boost);
        }

        residueX += dx * gain;
        residueY += dy * gain;

        if (residueX > RESIDUE_LIMIT) residueX = RESIDUE_LIMIT;
        if (residueX < -RESIDUE_LIMIT) residueX = -RESIDUE_LIMIT;
        if (residueY > RESIDUE_LIMIT) residueY = RESIDUE_LIMIT;
        if (residueY < -RESIDUE_LIMIT) residueY = -RESIDUE_LIMIT;

        // 取整数部分（向零截断，而不是四舍五入）：0.8 的零头必须留到下一次，
        // 否则手指的细微抖动会被放大成 ±1 的指针跳动。
        int ix = clamp((int) residueX);
        int iy = clamp((int) residueY);
        residueX -= ix;
        residueY -= iy;
        return new int[]{ix, iy};
    }

    private static int clamp(int v) {
        if (v > MAX_STEP) return MAX_STEP;
        if (v < -MAX_STEP) return -MAX_STEP;
        return v;
    }
}
