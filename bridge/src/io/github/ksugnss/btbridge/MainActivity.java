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
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.Set;

public class MainActivity extends Activity {
    private static final int REQ_PERMS = 1;
    private TextView status;
    private ArrayAdapter<String> adapter;
    private final ArrayList<String> rows = new ArrayList<>();
    private final ArrayList<BluetoothDevice> devices = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.main);  // XML布局，平台原生渲染

        TextView ctl = findViewById(R.id.btnModuleControl);
        ctl.setOnClickListener(v ->
                startActivity(new Intent(this, ModuleControlActivity.class)));

        status = findViewById(R.id.statusText);
        ListView list = findViewById(R.id.deviceList);
        adapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, rows);
        list.setAdapter(adapter);
        list.setOnItemClickListener((parent, view, position, id) -> {
            BluetoothDevice d = devices.get(position);
            if (d != null) startBridge(d.getAddress());
        });

        if (Build.VERSION.SDK_INT >= 31
                && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                        != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.BLUETOOTH_CONNECT}, REQ_PERMS);
        } else {
            refreshDevices();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        if (requestCode == REQ_PERMS && results.length > 0
                && results[0] == PackageManager.PERMISSION_GRANTED) {
            refreshDevices();
        } else {
            status.setText("缺少「附近的设备」权限");
        }
    }

    private void refreshDevices() {
        BluetoothAdapter bt = ((BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE))
                .getAdapter();
        if (bt == null) { status.setText("本机无蓝牙"); return; }
        Set<BluetoothDevice> bonded = bt.getBondedDevices();
        rows.clear(); devices.clear();
        if (bonded != null) {
            for (BluetoothDevice d : bonded) {
                String name;
                try { name = d.getName(); } catch (SecurityException e) { name = "?"; }
                rows.add(name + "\n" + d.getAddress());
                devices.add(d);
            }
        }
        adapter.notifyDataSetChanged();
        status.setText(rows.isEmpty() ? "没有已配对设备" : "选择设备开始桥接");
    }

    private void startBridge(String mac) {
        Intent it = new Intent(this, BridgeService.class);
        it.putExtra("mac", mac);
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(it);
        else startService(it);
        Toast.makeText(this, "桥接已启动：" + mac, Toast.LENGTH_SHORT).show();
    }
}
