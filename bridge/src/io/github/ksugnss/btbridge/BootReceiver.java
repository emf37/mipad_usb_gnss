package io.github.ksugnss.btbridge;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Build;

/**
 * Boot auto-connect: if the user last ran the Bluetooth bridge (auto_bt flag)
 * and a device address is remembered, restart BridgeService unattended.
 */
public class BootReceiver extends BroadcastReceiver {

    @Override
    public void onReceive(Context context, Intent intent) {
        if (!Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        SharedPreferences p = context.getSharedPreferences("state", Context.MODE_PRIVATE);
        String mac = p.getString("bt_mac", "");
        boolean auto = p.getBoolean("auto_bt", false);
        if (mac.length() == 0 || !auto) return;
        Intent it = new Intent(context, BridgeService.class);
        it.putExtra("mac", mac);
        if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(it);
        else context.startService(it);
    }
}
