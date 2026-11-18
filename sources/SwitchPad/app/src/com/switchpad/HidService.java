package com.switchpad;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;

/**
 * 只做一件事：把进程抬到前台，让蓝牙 HID 连接在息屏 / 切后台后继续存活。
 * 真正的 HID 逻辑在 {@link HidCore} 里。
 */
public final class HidService extends Service {

    private static final String TAG = "SwitchPad";
    private static final String CHANNEL_ID = "switchpad_hid";
    private static final int NOTIF_ID = 0x5EED;

    private static boolean running;

    public static boolean isRunning() {
        return running;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        running = true;
        createChannel();
        startForeground(NOTIF_ID, buildNotification("正在初始化蓝牙 HID…"));
        HidCore.get(this).start();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && intent.hasExtra("text")) {
            String text = intent.getStringExtra("text");
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (nm != null) nm.notify(NOTIF_ID, buildNotification(text));
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        try {
            HidCore core = HidCore.peek();
            if (core != null) core.stop();
        } catch (Exception e) {
            Log.w(TAG, "stop on destroy", e);
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    public static void updateNotification(android.content.Context ctx, String text) {
        try {
            Intent i = new Intent(ctx, HidService.class);
            i.putExtra("text", text);
            ctx.startService(i);
        } catch (Exception e) {
            Log.w(TAG, "updateNotification", e);
        }
    }

    private void createChannel() {
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null) return;
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "蓝牙键鼠连接", NotificationManager.IMPORTANCE_LOW);
        ch.setDescription("保持手机与电脑之间的 HID 连接");
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
    }

    private Notification buildNotification(String text) {
        Intent open = new Intent(this, PadActivity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(
                this, 0, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Notification.Builder b = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("SwitchPad 键鼠")
                .setContentText(text)
                .setSmallIcon(R.drawable.ic_launcher)
                .setContentIntent(pi)
                .setOngoing(true)
                .setShowWhen(false);
        return b.build();
    }
}
