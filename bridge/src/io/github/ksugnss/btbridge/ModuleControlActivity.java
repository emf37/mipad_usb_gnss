package io.github.ksugnss.btbridge;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * GNSS 模组控制页：双模组切换（中科微ATGM/CASIC、博通BK166X）+ 私有指令面板 +
 * 实时状态（连接HAL控制口 127.0.0.1:5001，经守护进程转发到模组）。
 */
public class ModuleControlActivity extends Activity implements ControlLink.Listener {
    private ControlLink link;
    private TextView statusView;
    private TextView logView;
    private LinearLayout cmdArea;
    private final StringBuilder log = new StringBuilder();
    private final SimpleDateFormat ts =
            new SimpleDateFormat("HH:mm:ss", Locale.US);

    private static final int MOD_ATGM = 0;
    private static final int MOD_BK = 1;
    private int module = MOD_ATGM;

    @SuppressLint("SetTextI18n")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (10 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        // 蓝牙桥接入口（原主界面）
        android.widget.Button btBtn = new android.widget.Button(this);
        btBtn.setAllCaps(false);
        btBtn.setText("📶 蓝牙GNSS桥接 »");
        btBtn.setOnClickListener(v ->
                startActivity(new android.content.Intent(this, MainActivity.class)));
        root.addView(btBtn);

        // 状态行
        statusView = new TextView(this);
        statusView.setText("连接守护进程中…");
        statusView.setPadding(0, 0, 0, pad);
        root.addView(statusView);

        // 模组切换
        LinearLayout sw = new LinearLayout(this);
        sw.setOrientation(LinearLayout.HORIZONTAL);
        Button bAtgm = styledBtn("中科微 ATGM");
        Button bBk = styledBtn("博通 BK");
        bAtgm.setOnClickListener(v -> { module = MOD_ATGM; buildCommands(); toast("ATGM/CASIC 指令集"); });
        bBk.setOnClickListener(v -> { module = MOD_BK; buildCommands(); toast("BK166X 指令集"); });
        sw.addView(bAtgm);
        sw.addView(bBk);
        root.addView(sw);

        // 指令区
        cmdArea = new LinearLayout(this);
        cmdArea.setOrientation(LinearLayout.VERTICAL);
        root.addView(cmdArea);
        buildCommands();

        // 日志
        logView = new TextView(this);
        logView.setTypeface(android.graphics.Typeface.MONOSPACE);
        logView.setTextSize(11);
        logView.setPadding(0, pad, 0, 0);
        ScrollView sc = new ScrollView(this);
        sc.addView(logView);
        root.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
        connect();
    }

    private Button styledBtn(String text) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        int m = (int) (4 * getResources().getDisplayMetrics().density);
        lp.setMargins(m, m, m, m);
        b.setLayoutParams(lp);
        return b;
    }

    private void connect() {
        link = new ControlLink(this);
        if (!link.connect()) {
            statusView.setText("守护进程未连接（先激活GNSS，确认HAL在运行）");
        }
    }

    @Override
    protected void onDestroy() {
        if (link != null) link.close();
        super.onDestroy();
    }

    // ---- ControlLink.Listener --------------------------------------------------

    @SuppressLint("SetTextI18n")
    @Override
    public void onStatus(String json) {
        // {"transport":"...","session":1,"sats":30,"used":12,"fix":3,"lat":..,"lon":..,"acc":..}
        runOnUiThread(() -> statusView.setText(fmtStatus(json)));
    }

    @Override
    public void onLine(String line) {
        runOnUiThread(() -> {
            if (log.length() > 8000) log.setLength(0);
            log.append(ts.format(new Date())).append(' ')
               .append(line.replaceFirst("^NMEA ", "")).append('\n');
            logView.setText(log.toString());
        });
    }

    @Override
    public void onDisconnected() {
        runOnUiThread(() -> statusView.setText("守护进程连接断开"));
    }

    private String fmtStatus(String j) {
        try {
            return "传输:" + j.replaceAll(".*\"transport\":\"([^\"]*)\".*", "$1")
                    + "  会话:" + field(j, "session")
                    + "  卫星:" + field(j, "sats") + "(用" + field(j, "used") + ")"
                    + "  fix:" + ("3".equals(field(j, "fix")) ? "3D" : "无")
                    + "  精度:" + field(j, "acc") + "m"
                    + "\n" + field(j, "lat") + ", " + field(j, "lon");
        } catch (Exception e) {
            return j;
        }
    }

    /** 从简单JSON行取数值字段（statusJson只含数字与一个字符串字段，够用） */
    static String field(String json, String key) {
        java.util.regex.Matcher m = java.util.regex.Pattern
                .compile("\"" + key + "\":(-?[0-9.]+)").matcher(json);
        return m.find() ? m.group(1) : "?";
    }

    private void toast(String s) {
        Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
    }

    /** 发送命令；ATGM(CASIC) 自动补 NMEA 异或校验和，BK(POLCFG) 原样。 */
    private void sendCmd(String cmd) {
        if (link == null || !link.isConnected()) {
            toast("未连接守护进程"); return;
        }
        String full = (module == MOD_ATGM) ? withChecksum(cmd) : cmd;
        link.sendModule(full);
        onLine(">> " + full);
    }

    static String withChecksum(String body) {
        int ck = 0;
        for (int i = 1; i < body.length(); i++) ck ^= body.charAt(i);  // skip '$'
        return String.format(Locale.US, "%s*%02X", body, ck);
    }

    // ---- command panels ---------------------------------------------------------

    private void buildCommands() {
        cmdArea.removeAllViews();
        if (module == MOD_ATGM) buildAtgm();
        else buildBk();
    }

    private void row(String label, String cmd) {
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setText(label);
        b.setOnClickListener(v -> sendCmd(cmd));
        b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        int m = (int) (2 * getResources().getDisplayMetrics().density);
        lp.setMargins(m, m, m, m);
        b.setLayoutParams(lp);
        cmdArea.addView(b);
    }

    private void pair(LinearLayout parent, String l1, String c1, String l2, String c2) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        Button a = styledBtn(l1), b = styledBtn(l2);
        a.setOnClickListener(v -> sendCmd(c1));
        b.setOnClickListener(v -> sendCmd(c2));
        r.addView(a); r.addView(b);
        parent.addView(r);
    }

    // ---- AGNSS: framework channel (自动，无需手动操作) ----
    // 框架会在 GNSS 会话开始时自动下载 PSDS 数据并调 injectPsdsData
    // 状态和日志通过控制口 SUB 流在 onLine 中实时显示

    private void buildAtgm() {
        pair(cmdArea, "1Hz", "$PCAS02,1000", "2Hz", "$PCAS02,500");
        pair(cmdArea, "5Hz", "$PCAS02,200", "10Hz", "$PCAS02,100");
        pair(cmdArea, "星座:GPS+北斗", "$PCAS04,3", "星座:全系统", "$PCAS04,7F");
        pair(cmdArea, "热启动", "$PCAS10,0", "冷启动", "$PCAS10,2");
        pair(cmdArea, "查固件版本", "$PCAS06,0", "查型号序列号", "$PCAS06,1");
        row("保存配置到FLASH", "$PCAS00");
        row("出厂复位(慎用)", "$PCAS10,3");
    }

    private void buildBk() {
        pair(cmdArea, "1Hz", "$POLCFGNAV,1,1", "5Hz", "$POLCFGNAV,5,5");
        pair(cmdArea, "10Hz", "$POLCFGNAV,10,10", "20Hz", "$POLCFGNAV,20,20");
        // 双频全系统: bits0-9 = 0x3FF (GPS L1/L5 + BDS B1I/B2A/B2B/B1C + GLO G1 + GAL E1/E5A/E5B)
        // 单频: GPS_L1|BDS_B1I|GLO_G1|GAL_E1 = 0x95
        pair(cmdArea, "频点:双频全系统", "$POLCFGSYS,3FFH", "频点:单频L1", "$POLCFGSYS,95H");
        pair(cmdArea, "热启动", "$POLCFGRESET,0", "冷启动", "$POLCFGRESET,1");
        pair(cmdArea, "查星座配置", "$POLCFGPTSYS", "查版本", "$POLCFGPTVER");
        row("保存配置", "$POLCFGSAVE");
        row("波特率→230400(慎用)", "$POLCFGPRT,230400,0");
    }
}
