package io.github.ksugnss.btbridge;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothSocket;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.UUID;

/**
 * Foreground service bridging a Bluetooth SPP GNSS receiver to a local TCP
 * server socket. The gnss_usb HAL connects as a TCP client
 * (persist.vendor.gnss_usb.source=tcp, socket_addr=tcp://127.0.0.1:5000) and
 * receives the NMEA stream; writes from the HAL (e.g. UBX rate commands) are
 * relayed back over SPP.
 */
public class BridgeService extends Service {
    private static final UUID SPP_UUID =
            UUID.fromString("00001101-0000-1000-8000-00805F9B34FB");
    public static final int TCP_PORT = 5000;

    private volatile boolean running;
    private Thread worker;
    private NotificationManager nm;

    @Override
    public void onCreate() {
        super.onCreate();
        nm = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26) {
            nm.createNotificationChannel(new NotificationChannel(
                    "bridge", "GNSS BT Bridge", NotificationManager.IMPORTANCE_LOW));
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        String mac = intent != null ? intent.getStringExtra("mac") : null;
        startForegroundCompat("connecting " + (mac == null ? "?" : mac));
        if (running) {
            notify("already running");
            return START_STICKY;
        }
        if (mac == null) {
            notify("no device selected");
            stopSelf();
            return START_NOT_STICKY;
        }
        running = true;
        final String deviceMac = mac;
        worker = new Thread(() -> runBridge(deviceMac), "bt-bridge");
        worker.start();
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        running = false;
        if (worker != null) worker.interrupt();
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void runBridge(String mac) {
        BluetoothAdapter bt = ((BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE))
                .getAdapter();
        if (bt == null) {
            notify("no bluetooth adapter");
            stopSelf();
            return;
        }

        // 1. Keep the SPP link up (auto-reconnect).
        // 2. For each connected SPP session, accept HAL clients on the TCP
        //    server socket and pump bytes both ways.
        while (running) {
            try {
                BluetoothDevice dev = bt.getRemoteDevice(mac);
                bt.cancelDiscovery();
                notify("connecting " + mac);
                BluetoothSocket sock = dev.createRfcommSocketToServiceRecord(SPP_UUID);
                sock.connect();
                notify("SPP connected, listening tcp:" + TCP_PORT);
                serve(sock);
                safeClose(sock);
            } catch (Exception e) {
                notify("error: " + e.getMessage() + " (retry in 3s)");
                sleepQuiet(3000);
            }
        }
    }

    private void serve(BluetoothSocket sock) throws IOException {
        ServerSocket server = new ServerSocket(TCP_PORT, 4, InetAddress.getByName("127.0.0.1"));
        try {
            while (running) {
                Socket client = server.accept();
                notify("HAL connected");
                try {
                    pump(sock, client);
                } catch (IOException e) {
                    // client went away; wait for the next one
                } finally {
                    notify("SPP connected, listening tcp:" + TCP_PORT);
                    safeClose(client);
                }
            }
        } finally {
            server.close();
        }
    }

    private void pump(BluetoothSocket bt, Socket tcp) throws IOException {
        InputStream btIn = bt.getInputStream();
        OutputStream btOut = bt.getOutputStream();
        InputStream tcpIn = tcp.getInputStream();
        OutputStream tcpOut = tcp.getOutputStream();

        Thread back = new Thread(() -> {
            byte[] buf = new byte[256];
            try {
                int n;
                while (running && (n = tcpIn.read(buf)) > 0) {
                    btOut.write(buf, 0, n);
                    btOut.flush();
                }
            } catch (IOException ignored) {
            }
        }, "tcp->bt");
        back.start();

        byte[] buf = new byte[1024];
        int n;
        while (running && (n = btIn.read(buf)) > 0) {
            tcpOut.write(buf, 0, n);
            tcpOut.flush();
        }
        back.interrupt();
    }

    private void startForegroundCompat(String text) {
        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, "bridge")
                : new Notification.Builder(this);
        Notification n = b.setContentTitle("GNSS BT Bridge")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setOngoing(true)
                .build();
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(1, n, android.content.pm.ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        } else {
            startForeground(1, n);
        }
    }

    private void notify(String text) {
        Notification.Builder b = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, "bridge")
                : new Notification.Builder(this);
        nm.notify(1, b.setContentTitle("GNSS BT Bridge")
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setOngoing(true)
                .build());
    }

    private static void safeClose(java.io.Closeable c) {
        try {
            if (c != null) c.close();
        } catch (IOException ignored) {
        }
    }

    private static void sleepQuiet(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
