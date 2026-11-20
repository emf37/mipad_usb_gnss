package io.github.ksugnss.btbridge;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Client for the gnss_usb_hal control server (127.0.0.1:5001).
 * Line protocol: STATUS / SEND <cmd> / SUB (live "NMEA <line>" push).
 */
public class ControlLink {
    public interface Listener {
        void onStatus(String json);
        void onLine(String line);      // raw pushed lines ("NMEA ...", module replies)
        void onDisconnected();
    }

    private final Listener listener;
    private Socket socket;
    private OutputStream out;
    private Thread reader;
    private Thread poller;
    private final AtomicBoolean running = new AtomicBoolean(false);

    public ControlLink(Listener l) {
        this.listener = l;
    }

    public boolean connect() {
        try {
            socket = new Socket();
            socket.connect(new InetSocketAddress("127.0.0.1", 5001), 1500);
            socket.setTcpNoDelay(true);
            out = socket.getOutputStream();
            out.write("SUB\n".getBytes());
            out.flush();
            running.set(true);

            reader = new Thread(() -> {
                try {
                    BufferedReader br = new BufferedReader(
                            new InputStreamReader(socket.getInputStream()));
                    String line;
                    while (running.get() && (line = br.readLine()) != null) {
                        if (line.startsWith("{")) {
                            listener.onStatus(line);
                        } else {
                            listener.onLine(line);
                        }
                    }
                } catch (Exception ignored) {
                }
                running.set(false);
                listener.onDisconnected();
            }, "ctl-reader");
            reader.start();

            poller = new Thread(() -> {
                while (running.get()) {
                    send("STATUS");
                    try { Thread.sleep(2000); } catch (InterruptedException e) { return; }
                }
            }, "ctl-poller");
            poller.start();
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    public boolean send(String cmd) {
        try {
            if (out == null) return false;
            out.write((cmd + "\n").getBytes());
            out.flush();
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /** Send a module command through the HAL transport (CRLF appended by daemon). */
    public boolean sendModule(String cmd) {
        return send("SEND " + cmd);
    }

    public void close() {
        running.set(false);
        try { if (socket != null) socket.close(); } catch (Exception ignored) {}
    }

    public boolean isConnected() {
        return running.get();
    }
}
