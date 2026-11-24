package io.github.ksugnss.btbridge;

import android.Manifest;
import android.app.Activity;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.ArrayList;
import java.util.Set;

public class MainActivity extends Activity
        implements View.OnClickListener, AdapterView.OnItemSelectedListener,
                   TextView.OnEditorActionListener, Runnable {

    private static final int REQ_BT = 1;
    private Handler ui;
    private TextView tvStatus;
    private TextView tvLog;
    private EditText etCmd;
    private Spinner spBt;
    private Button btnAtgm;
    private Button btnBk;
    private int module = 1;
    private String btMac = "";
    private StringBuffer logBuf = new StringBuffer();
    private volatile boolean running = true;
    private ArrayList<String> btMacs = new ArrayList<String>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);
        ui = new Handler(Looper.getMainLooper());
        tvStatus = (TextView) findViewById(R.id.tvStatus);
        tvLog = (TextView) findViewById(R.id.tvLog);
        spBt = (Spinner) findViewById(R.id.spBt);
        btnAtgm = (Button) findViewById(R.id.btnAtgm);
        btnBk = (Button) findViewById(R.id.btnBk);
        etCmd = (EditText) findViewById(R.id.etCmd);
        int[] ids = {R.id.btnAtgm, R.id.btnBk, R.id.btnRate1, R.id.btnRate2,
                R.id.btnRate5, R.id.btnRate10, R.id.btnSysGpsBd, R.id.btnSysAll,
                R.id.btnHot, R.id.btnCold, R.id.btnVer, R.id.btnSave, R.id.btnBtConn,
                R.id.btnSend};
        for (int i = 0; i < ids.length; i++) {
            findViewById(ids[i]).setOnClickListener(this);
        }
        spBt.setOnItemSelectedListener(this);
        etCmd.setOnEditorActionListener(this);
        checkPerm();
        mark();
        Thread t = new Thread(this, "poll");
        t.start();
    }

    @Override
    protected void onDestroy() {
        running = false;
        super.onDestroy();
    }

    // View.OnClickListener
    @Override
    public void onClick(View v) {
        int id = v.getId();
        if (id == R.id.btnAtgm) { module = 1; mark(); lg("mod: ATGM"); return; }
        if (id == R.id.btnBk) { module = 2; mark(); lg("mod: BK"); return; }
        if (id == R.id.btnBtConn) { connBt(); return; }
        if (id == R.id.btnSend) { sendManual(); return; }
        String cmd = cmdFor(id);
        if (cmd != null) send(cmd);
    }

    // TextView.OnEditorActionListener (IME send on etCmd)
    @Override
    public boolean onEditorAction(TextView v, int actionId, android.view.KeyEvent event) {
        if (actionId == EditorInfo.IME_ACTION_SEND) {
            sendManual();
            return true;
        }
        return false;
    }

    // AdapterView.OnItemSelectedListener
    @Override
    public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
        if (pos >= 0 && pos < btMacs.size()) btMac = btMacs.get(pos);
    }

    @Override
    public void onNothingSelected(AdapterView<?> p) {
        btMac = "";
    }

    // Runnable (status poller)
    @Override
    public void run() {
        while (running) {
            String r = hal("STATUS");
            if (r != null && r.startsWith("{")) {
                String st = fmt(r);
                ui.post(new SetText(tvStatus, st));
            }
            try { Thread.sleep(2000); } catch (InterruptedException e) { return; }
        }
    }

    private static class SetText implements Runnable {
        private final TextView tv;
        private final String s;
        SetText(TextView t, String v) { tv = t; s = v; }
        public void run() { tv.setText(s); }
    }

    private static class AddLog implements Runnable {
        private final MainActivity a;
        private final String s;
        AddLog(MainActivity act, String v) { a = act; s = v; }
        public void run() {
            StringBuffer lb = a.logBuf;
            if (lb.length() > 6000) lb.setLength(0);
            lb.append(s).append('\n');
            a.tvLog.setText(lb.toString());
        }
    }

    private static class CmdRun implements Runnable {
        private final MainActivity a;
        private final String cmd;
        CmdRun(MainActivity act, String c) { a = act; cmd = c; }
        public void run() {
            String r = a.hal("SEND " + cmd);
            if (r != null && r.length() > 0) a.lg(r);
        }
    }

    private String cmdFor(int id) {
        if (id == R.id.btnRate1) return module == 1 ? "$PCAS02,1000" : "$POLCFGNAV,1,1";
        if (id == R.id.btnRate2) return module == 1 ? "$PCAS02,500" : "$POLCFGNAV,5,5";
        if (id == R.id.btnRate5) return "$PCAS02,200";
        if (id == R.id.btnRate10) return module == 1 ? "$PCAS02,100" : "$POLCFGNAV,10,10";
        if (id == R.id.btnSysGpsBd) return "$PCAS04,3";
        if (id == R.id.btnSysAll) return module == 1 ? "$PCAS04,7F" : "$POLCFGSYS,3FFH";
        if (id == R.id.btnHot) return module == 1 ? "$PCAS10,0" : "$POLCFGRESET,0";
        if (id == R.id.btnCold) return module == 1 ? "$PCAS10,2" : "$POLCFGRESET,1";
        if (id == R.id.btnVer) return module == 1 ? "$PCAS06,0" : "$POLCFGPTVER";
        if (id == R.id.btnSave) return module == 1 ? "$PCAS00" : "$POLCFGSAVE";
        return null;
    }

    private void sendManual() {
        String c = etCmd.getText().toString().trim();
        if (c.length() == 0) {
            Toast.makeText(this, "empty", Toast.LENGTH_SHORT).show();
            return;
        }
        send(c);
        etCmd.setText("");
    }

    private void mark() {
        btnAtgm.setEnabled(module != 1);
        btnBk.setEnabled(module != 2);
    }

    private void connBt() {
        if (btMac.length() == 0) {
            Toast.makeText(this, "select device", Toast.LENGTH_SHORT).show();
            return;
        }
        Intent it = new Intent(this, BridgeService.class);
        it.putExtra("mac", btMac);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(it);
        else startService(it);
        lg("BT bridge: " + btMac);
    }

    private void checkPerm() {
        if (Build.VERSION.SDK_INT >= 31 &&
                checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.BLUETOOTH_CONNECT}, REQ_BT);
        } else {
            fillBt();
        }
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] p, int[] r) {
        if (req == REQ_BT && r.length > 0 && r[0] == PackageManager.PERMISSION_GRANTED) fillBt();
    }

    private void fillBt() {
        try {
            BluetoothAdapter bt = ((BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE)).getAdapter();
            if (bt == null) return;
            Set<BluetoothDevice> bonded = bt.getBondedDevices();
            ArrayList<String> names = new ArrayList<String>();
            btMacs = new ArrayList<String>();
            if (bonded != null) {
                for (BluetoothDevice d : bonded) {
                    String n;
                    try { n = d.getName(); } catch (SecurityException e) { n = "?"; }
                    names.add(n);
                    btMacs.add(d.getAddress());
                }
            }
            ArrayAdapter<String> ad = new ArrayAdapter<String>(this,
                    android.R.layout.simple_spinner_item, names);
            ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            spBt.setAdapter(ad);
        } catch (Exception ignored) { }
    }

    // ---- HAL ----
    private void send(final String cmd) {
        if (cmd == null) return;
        String c = cmd;
        if (module == 1 && c.contains("PCAS") && !c.contains("*")) {
            int x = 0;
            for (int i = 1; i < c.length(); i++) x ^= c.charAt(i);
            c = c + String.format("*%02X", x & 0xFF);
        }
        lg(">> " + c);
        Thread t = new Thread(new CmdRun(this, c), "cmd");
        t.start();
    }

    String hal(String req) {
        try {
            Socket s = new Socket();
            s.connect(new InetSocketAddress("127.0.0.1", 5001), 2000);
            s.setSoTimeout(3000);
            OutputStream o = s.getOutputStream();
            o.write((req + "\n").getBytes());
            o.flush();
            BufferedReader br = new BufferedReader(new InputStreamReader(s.getInputStream()));
            String r = br.readLine();
            s.close();
            return r;
        } catch (Exception e) {
            return "";
        }
    }

    private static String fmt(String json) {
        String fix = jget(json, "fix");
        return "link: " + jget(json, "transport")
             + "\nsats: " + jget(json, "sats") + " (used " + jget(json, "used") + ")"
             + "  fix: " + ("3".equals(fix) ? "3D" : "none")
             + "  acc: " + jget(json, "acc") + "m"
             + "\nlat/lon: " + jget(json, "lat") + ", " + jget(json, "lon");
    }

    private static String jget(String json, String key) {
        int i = json.indexOf("\"" + key + "\":");
        if (i < 0) return "?";
        int s = json.indexOf(':', i + key.length() + 3) + 1;
        int e = json.indexOf(',', s);
        if (e < 0) e = json.indexOf('}', s);
        return json.substring(s, e).trim();
    }

    void lg(String msg) {
        ui.post(new AddLog(this, msg));
    }
}
