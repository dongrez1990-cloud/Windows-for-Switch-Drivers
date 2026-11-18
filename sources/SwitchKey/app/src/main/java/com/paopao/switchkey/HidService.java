package com.paopao.switchkey;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * 一个极简前台服务，唯一作用是让进程在键盘使用时不被系统回收
 * （切到别的界面、锁屏后蓝牙连接还能维持）。
 */
public class HidService extends Service {

    private static final String CHANNEL_ID = "switchkey_status";
    private static final int NOTIFICATION_ID = 4211;

    public static void ensureRunning(Context context) {
        Intent i = new Intent(context, HidService.class);
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(i);
            } else {
                context.startService(i);
            }
        } catch (Throwable t) {
            // 后台启动被限制也无所谓，键盘仍然能用
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        createChannel();
        Notification n = buildNotification();
        try {
            if (Build.VERSION.SDK_INT >= 34) {
                startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            } else {
                startForeground(NOTIFICATION_ID, n);
            }
        } catch (Throwable t) {
            stopSelf();
        }
        return START_STICKY;
    }

    private void createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm == null || nm.getNotificationChannel(CHANNEL_ID) != null) {
            return;
        }
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "键盘连接状态", NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
    }

    private Notification buildNotification() {
        Notification.Builder b;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            b = new Notification.Builder(this, CHANNEL_ID);
        } else {
            b = new Notification.Builder(this);
        }
        b.setContentTitle("SwitchKey 正在工作")
                .setContentText("手机作为蓝牙键盘连接中")
                .setSmallIcon(android.R.drawable.ic_menu_edit)
                .setOngoing(true);
        return b.build();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
