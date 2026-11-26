package io.github.ksugnss.btbridge;

import android.app.AlarmManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.os.SystemClock;
import android.util.Log;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.UUID;

public class BridgeService extends Service {
    private static final UUID SPP = UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    private static final long SELF_CHECK_MS = 90000L;
    private Thread worker;

    @Override public void onCreate() {
        super.onCreate();
        scheduleSelfCheck();
        NotificationManager nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26)
            nm.createNotificationChannel(new NotificationChannel("bt", "Bridge", NotificationManager.IMPORTANCE_LOW));
        Notification.Builder b = new Notification.Builder(this);
        if (Build.VERSION.SDK_INT >= 26) b.setChannelId("bt");
        startForeground(1, b.setContentTitle("GNSS BT Bridge")
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth).setOngoing(true).build());
    }

    @Override public int onStartCommand(Intent it, int f, int id) {
        String mac = it != null ? it.getStringExtra("mac") : null;
        if (mac == null) {
            // START_STICKY restart after a system kill delivers a null intent:
            // fall back to the remembered device so the bridge self-heals.
            mac = getSharedPreferences("state", MODE_PRIVATE).getString("bt_mac", "");
        }
        if (mac != null && mac.length() > 0 && worker == null) {
            worker = new Thread(new BtLoop(this, mac), "bt");
            worker.start();
        }
        return START_STICKY;
    }

    @Override public IBinder onBind(Intent i) { return null; }

    /**
     * HyperOS does not honor START_STICKY restarts after a hard kill, so keep a
     * wakeup alarm that re-starts this service (null intent -> prefs fallback
     * path) whenever it vanished. No-op when already running. Battery-exempt
     * apps are allowed to start a foreground service from the alarm.
     */
    private void scheduleSelfCheck() {
        AlarmManager am = (AlarmManager) getSystemService(Context.ALARM_SERVICE);
        Intent i = new Intent(this, BridgeService.class);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23) flags |= PendingIntent.FLAG_IMMUTABLE;
        PendingIntent pi = PendingIntent.getService(this, 1, i, flags);
        am.setInexactRepeating(AlarmManager.ELAPSED_REALTIME_WAKEUP,
                SystemClock.elapsedRealtime() + SELF_CHECK_MS, SELF_CHECK_MS, pi);
    }

    @Override public void onDestroy() {
        if (worker != null) worker.interrupt();
        super.onDestroy();
    }

    // main loop: keep BT connected, serve TCP clients on :5000
    private static class BtLoop implements Runnable {
        private final BridgeService svc;
        private final String mac;
        BtLoop(BridgeService s, String m) { svc = s; mac = m; }

        public void run() {
            BluetoothAdapter bt = ((BluetoothManager) svc.getSystemService(Context.BLUETOOTH_SERVICE)).getAdapter();
            if (bt == null) return;
            while (!Thread.currentThread().isInterrupted()) {
                BluetoothSocket sock = null;
                try {
                    try { bt.cancelDiscovery(); } catch (SecurityException se) {
                        Log.w("BtBridge", "cancelDiscovery denied (no BLUETOOTH_SCAN), continuing");
                    }
                    BluetoothDevice dev = bt.getRemoteDevice(mac);
                    sock = dev.createRfcommSocketToServiceRecord(SPP);
                    sock.connect();
                    serve(sock);
                } catch (Exception e) {
                    Log.e("BtBridge", "bridge cycle failed: " + e);
                    if (sock != null) { try { sock.close(); } catch (Exception ignored) {} }
                    try { Thread.sleep(3000); } catch (InterruptedException x) { return; }
                }
            }
        }

        private void serve(BluetoothSocket bt) throws Exception {
            // bind IPv4 loopback explicitly: getLoopbackAddress() yields ::1 here,
            // which refuses HAL's tcp://127.0.0.1:5000 connects
            ServerSocket srv = new ServerSocket(5000, 4, InetAddress.getByName("127.0.0.1"));
            try {
                while (bt.isConnected()) {
                    Socket cli = srv.accept();
                    try { pump(bt, cli); } catch (Exception ignored) {}
                    try { cli.close(); } catch (Exception ignored) {}
                }
            } finally {
                try { srv.close(); } catch (Exception ignored) {}
            }
        }

        private void pump(BluetoothSocket bt, Socket tcp) throws Exception {
            InputStream btIn = bt.getInputStream();
            OutputStream btOut = bt.getOutputStream();
            InputStream tcpIn = tcp.getInputStream();
            OutputStream tcpOut = tcp.getOutputStream();
            Thread back = new Thread(new TcpToBt(tcpIn, btOut), "back");
            back.start();
            byte[] b = new byte[1024];
            int n;
            while ((n = btIn.read(b)) > 0) { tcpOut.write(b, 0, n); tcpOut.flush(); }
            back.interrupt();
        }
    }

    // TCP -> BT direction
    private static class TcpToBt implements Runnable {
        private final InputStream in;
        private final OutputStream out;

        TcpToBt(InputStream i, OutputStream o) { in = i; out = o; }

        public void run() {
            byte[] b = new byte[256];
            try {
                int n;
                while ((n = in.read(b)) > 0) { out.write(b, 0, n); out.flush(); }
            } catch (Exception ignored) {}
        }
    }
}
