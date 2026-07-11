package works.earendil.voicepad;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.speech.RecognitionListener;
import android.speech.RecognizerIntent;
import android.speech.SpeechRecognizer;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.net.SocketTimeoutException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

public final class MainActivity extends Activity {
    private static final int MIC_REQUEST = 40;
    private static final int PORT = 7777;
    private static final String DISCOVER = "VOICEPAD_DISCOVER_V1";
    private static final String HERE = "VOICEPAD_HERE_V1";

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService network = Executors.newSingleThreadExecutor();
    private final ExecutorService keyboardNetwork = Executors.newSingleThreadExecutor();
    private final ExecutorService drawingNetwork = new ThreadPoolExecutor(
            1, 1, 0L, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(8),
            new ThreadPoolExecutor.DiscardOldestPolicy());

    private EditText endpointField;
    private EditText keyboardField;
    private TextView status;
    private TextView log;
    private Button listenButton;
    private CheckBox liveKeyboardBox;
    private CheckBox eraserBox;
    private SharedPreferences preferences;

    private SpeechRecognizer speech;
    private Intent speechIntent;
    private boolean wantsSpeech;
    private boolean speechStarting;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        preferences = getSharedPreferences("voicepad", MODE_PRIVATE);
        buildUi();
        applyPairIntent(getIntent());
        if (normalizeEndpoint(endpointField.getText().toString()).isEmpty())
            main.postDelayed(this::discover, 500);
    }

    @Override protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        applyPairIntent(intent);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        if (liveKeyboardBox == null || !liveKeyboardBox.isChecked() || event.isSystem())
            return super.dispatchKeyEvent(event);

        int key = event.getKeyCode();
        if (key == KeyEvent.KEYCODE_ESCAPE) {
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                liveKeyboardBox.setChecked(false);
                setStatus("Live keyboard capture paused. Tap the checkbox to resume.");
            }
            return true;
        }
        if (event.getAction() == KeyEvent.ACTION_DOWN) {
            int unicode = event.getUnicodeChar();
            if (key == KeyEvent.KEYCODE_DEL) sendKeyCommand("B");
            else if (key == KeyEvent.KEYCODE_FORWARD_DEL) sendKeyCommand("X");
            else if (key == KeyEvent.KEYCODE_ENTER || key == KeyEvent.KEYCODE_NUMPAD_ENTER) sendKeyCommand("N");
            else if (key == KeyEvent.KEYCODE_TAB) sendKeyCommand("T");
            else if (key == KeyEvent.KEYCODE_DPAD_LEFT) sendKeyCommand("L");
            else if (key == KeyEvent.KEYCODE_DPAD_RIGHT) sendKeyCommand("R");
            else if (key == KeyEvent.KEYCODE_DPAD_UP) sendKeyCommand("U");
            else if (key == KeyEvent.KEYCODE_DPAD_DOWN) sendKeyCommand("D");
            else if (key == KeyEvent.KEYCODE_MOVE_HOME) sendKeyCommand("H");
            else if (key == KeyEvent.KEYCODE_MOVE_END) sendKeyCommand("E");
            else if (unicode >= 32 && unicode != 127) sendKeyCommand("C " + unicode);
        }
        // Capture every non-system hardware key in live mode, including key-up
        // and navigation/modifier events, so Android controls never steal focus.
        return true;
    }

    @Override protected void onDestroy() {
        wantsSpeech = false;
        if (speech != null) speech.destroy();
        network.shutdownNow();
        keyboardNetwork.shutdownNow();
        drawingNetwork.shutdownNow();
        super.onDestroy();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private TextView label(String value, int sp) {
        TextView v = new TextView(this);
        v.setText(value);
        v.setTextSize(sp);
        v.setTextColor(Color.rgb(30, 30, 30));
        v.setPadding(0, dp(5), 0, dp(5));
        return v;
    }

    private Button button(String value) {
        Button b = new Button(this);
        b.setText(value);
        b.setAllCaps(false);
        return b;
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(20), dp(20), dp(28));
        root.setBackgroundColor(Color.WHITE);
        root.setFocusableInTouchMode(true);
        scroll.addView(root, new ScrollView.LayoutParams(-1, -2));

        TextView title = label("Voicepad 0.4.0", 30);
        title.setGravity(Gravity.CENTER_HORIZONTAL);
        root.addView(title);

        status = label("Scan the tablet QR or find it on Wi-Fi.", 16);
        status.setGravity(Gravity.CENTER_HORIZONTAL);
        status.setBackgroundColor(Color.rgb(238, 243, 247));
        status.setPadding(dp(10), dp(12), dp(10), dp(12));
        root.addView(status, new LinearLayout.LayoutParams(-1, -2));

        root.addView(label("Tablet address", 14));
        endpointField = new EditText(this);
        endpointField.setSingleLine(true);
        endpointField.setHint("192.168.x.x:7777");
        endpointField.setInputType(android.text.InputType.TYPE_CLASS_TEXT |
                android.text.InputType.TYPE_TEXT_VARIATION_URI);
        endpointField.setText(preferences.getString("endpoint", ""));
        root.addView(endpointField, new LinearLayout.LayoutParams(-1, -2));

        LinearLayout pairRow = new LinearLayout(this);
        pairRow.setOrientation(LinearLayout.HORIZONTAL);
        Button connect = button("Connect / test");
        Button find = button("Find tablet");
        pairRow.addView(connect, new LinearLayout.LayoutParams(0, -2, 1));
        pairRow.addView(find, new LinearLayout.LayoutParams(0, -2, 1));
        root.addView(pairRow, new LinearLayout.LayoutParams(-1, -2));
        connect.setOnClickListener(v -> testConnection());
        find.setOnClickListener(v -> discover());

        listenButton = button("Start microphone");
        listenButton.setTextSize(20);
        LinearLayout.LayoutParams listenParams = new LinearLayout.LayoutParams(-1, dp(66));
        listenParams.topMargin = dp(14);
        root.addView(listenButton, listenParams);
        listenButton.setOnClickListener(v -> toggleSpeech());

        root.addView(label("Bluetooth or phone keyboard", 17));
        TextView keyboardHelp = label("Bluetooth keys go straight to the shared tablet document. Arrow keys move its cursor; Backspace/Delete edit; Enter starts a line. Speech is inserted at that same cursor. Press Escape or uncheck capture to use Android controls.", 13);
        keyboardHelp.setTextColor(Color.DKGRAY);
        root.addView(keyboardHelp);
        liveKeyboardBox = new CheckBox(this);
        liveKeyboardBox.setText("Capture all Bluetooth keyboard input");
        liveKeyboardBox.setChecked(true);
        root.addView(liveKeyboardBox);
        liveKeyboardBox.setOnCheckedChangeListener((buttonView, checked) -> {
            if (checked) {
                root.requestFocus();
                setStatus("Bluetooth keyboard captured — type and watch the tablet.");
            } else {
                setStatus("Keyboard capture paused. Android controls are available.");
            }
        });
        LinearLayout keyboardRow = new LinearLayout(this);
        keyboardRow.setOrientation(LinearLayout.HORIZONTAL);
        keyboardField = new EditText(this);
        keyboardField.setSingleLine(true);
        keyboardField.setHint("Tap here, then type on the keyboard");
        keyboardField.setImeOptions(EditorInfo.IME_ACTION_SEND);
        Button send = button("Send line");
        keyboardRow.addView(keyboardField, new LinearLayout.LayoutParams(0, -2, 1));
        keyboardRow.addView(send, new LinearLayout.LayoutParams(-2, -2));
        root.addView(keyboardRow, new LinearLayout.LayoutParams(-1, -2));
        send.setOnClickListener(v -> sendKeyboardText());
        keyboardField.setOnFocusChangeListener((v, focused) -> {
            if (focused && liveKeyboardBox.isChecked())
                setStatus("Live keyboard ready — type and watch the tablet.");
        });
        keyboardField.setOnEditorActionListener((v, action, event) -> {
            boolean enter = event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER &&
                    event.getAction() == KeyEvent.ACTION_DOWN;
            if (action == EditorInfo.IME_ACTION_SEND || enter) {
                sendKeyboardText();
                return true;
            }
            return false;
        });

        root.addView(label("Remote ink pad", 17));
        TextView padHelp = label("Draw here with a finger. The marks appear on Voicepad over Wi-Fi.", 13);
        padHelp.setTextColor(Color.DKGRAY);
        root.addView(padHelp);
        eraserBox = new CheckBox(this);
        eraserBox.setText("Eraser");
        root.addView(eraserBox);
        InkPad pad = new InkPad(this);
        LinearLayout.LayoutParams padParams = new LinearLayout.LayoutParams(-1, dp(230));
        padParams.bottomMargin = dp(12);
        root.addView(pad, padParams);

        root.addView(label("Transcript history", 17));
        log = label("", 14);
        log.setTextIsSelectable(true);
        root.addView(log, new LinearLayout.LayoutParams(-1, -2));

        setContentView(scroll);
        root.requestFocus();
    }

    private String normalizeEndpoint(String raw) {
        if (raw == null) return "";
        String value = raw.trim();
        if (value.startsWith("http://")) value = value.substring(7);
        if (value.startsWith("https://")) value = value.substring(8);
        int slash = value.indexOf('/');
        if (slash >= 0) value = value.substring(0, slash);
        if (!value.isEmpty() && value.indexOf(':') < 0) value += ":" + PORT;
        return value;
    }

    private String endpoint() {
        String raw = endpointField.getText().toString();
        String value = normalizeEndpoint(raw);
        if (!value.isEmpty()) {
            if (!value.equals(raw)) {
                endpointField.setText(value);
                endpointField.setSelection(value.length());
            }
            if (!value.equals(preferences.getString("endpoint", "")))
                preferences.edit().putString("endpoint", value).apply();
        }
        return value;
    }

    private void applyPairIntent(Intent intent) {
        Uri data = intent == null ? null : intent.getData();
        if (data == null || !"voicepad".equals(data.getScheme())) return;
        String host = normalizeEndpoint(data.getQueryParameter("host"));
        if (host.isEmpty()) return;
        endpointField.setText(host);
        endpointField.setSelection(host.length());
        preferences.edit().putString("endpoint", host).apply();
        setStatus("Paired with " + host + ". Testing…");
        testConnection();
    }

    private void setStatus(String value) {
        if (Looper.myLooper() == Looper.getMainLooper()) status.setText(value);
        else main.post(() -> status.setText(value));
    }

    private void appendLog(String value) {
        main.post(() -> {
            String old = log.getText().toString();
            log.setText(value + (old.isEmpty() ? "" : "\n\n" + old));
        });
    }

    private void testConnection() {
        final String host = endpoint();
        if (host.isEmpty()) {
            setStatus("Enter an address, scan the QR, or tap Find tablet.");
            return;
        }
        setStatus("Connecting to " + host + "…");
        network.execute(() -> {
            HttpURLConnection c = null;
            try {
                c = (HttpURLConnection)new URL("http://" + host + "/health").openConnection();
                c.setConnectTimeout(2500);
                c.setReadTimeout(2500);
                c.setRequestProperty("Connection", "close");
                int code = c.getResponseCode();
                if (code == 200) setStatus("Connected to Voicepad at " + host);
                else setStatus("Tablet replied with HTTP " + code);
            } catch (Exception e) {
                setStatus("Cannot reach " + host + ". Check Wi-Fi and start Voicepad.");
            } finally {
                if (c != null) c.disconnect();
            }
        });
    }

    private void discover() {
        setStatus("Looking for Voicepad on this Wi-Fi…");
        network.execute(() -> {
            DatagramSocket socket = null;
            try {
                socket = new DatagramSocket();
                socket.setBroadcast(true);
                socket.setSoTimeout(2200);
                byte[] request = DISCOVER.getBytes(StandardCharsets.US_ASCII);
                Set<InetAddress> broadcasts = new LinkedHashSet<>();
                broadcasts.add(InetAddress.getByName("255.255.255.255"));
                Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
                while (interfaces != null && interfaces.hasMoreElements()) {
                    NetworkInterface ni = interfaces.nextElement();
                    if (!ni.isUp() || ni.isLoopback()) continue;
                    for (InterfaceAddress ia : ni.getInterfaceAddresses())
                        if (ia.getBroadcast() != null) broadcasts.add(ia.getBroadcast());
                }
                for (InetAddress address : broadcasts)
                    socket.send(new DatagramPacket(request, request.length, address, PORT));

                byte[] reply = new byte[256];
                DatagramPacket packet = new DatagramPacket(reply, reply.length);
                socket.receive(packet);
                String text = new String(reply, 0, packet.getLength(), StandardCharsets.US_ASCII);
                if (!text.startsWith(HERE)) throw new Exception("unexpected discovery reply");
                String found = packet.getAddress().getHostAddress() + ":" + PORT;
                final String host = found;
                main.post(() -> {
                    endpointField.setText(host);
                    endpointField.setSelection(host.length());
                    preferences.edit().putString("endpoint", host).apply();
                    setStatus("Found Voicepad at " + host);
                    testConnection();
                });
            } catch (SocketTimeoutException e) {
                setStatus("No tablet found. Scan its QR or enter the displayed address.");
            } catch (Exception e) {
                setStatus("Discovery failed. Enter the tablet address manually.");
            } finally {
                if (socket != null) socket.close();
            }
        });
    }

    private void sendKeyboardText() {
        String value = keyboardField.getText().toString().trim();
        if (value.isEmpty()) return;
        keyboardField.setText("");
        sendTranscript(value);
    }

    private void sendKeyCommand(String command) {
        if (normalizeEndpoint(endpointField.getText().toString()).isEmpty()) {
            setStatus("Pair with the tablet before using the live keyboard.");
            return;
        }
        postBody(keyboardNetwork, "/key", command, false);
    }

    private void sendTranscript(String value) {
        appendLog(value);
        // Speech, line sends, cursor moves, and live keys share one executor so
        // they reach the tablet in exactly the order Android observed them.
        postBody(keyboardNetwork, "/say", value, true);
    }

    private void postBody(ExecutorService executor, String path, String value, boolean report) {
        final String host = endpoint();
        if (host.isEmpty()) {
            if (report) setStatus("Pair with the tablet first.");
            return;
        }
        final byte[] body = value.getBytes(StandardCharsets.UTF_8);
        executor.execute(() -> {
            HttpURLConnection c = null;
            try {
                c = (HttpURLConnection)new URL("http://" + host + path).openConnection();
                int timeout = report ? 2500 : 700;
                c.setConnectTimeout(timeout);
                c.setReadTimeout(timeout);
                c.setRequestMethod("POST");
                c.setDoOutput(true);
                c.setFixedLengthStreamingMode(body.length);
                c.setRequestProperty("Content-Type", "text/plain; charset=utf-8");
                c.setRequestProperty("Connection", "close");
                try (OutputStream out = c.getOutputStream()) { out.write(body); }
                int code = c.getResponseCode();
                if (report && code != 204) setStatus("Tablet rejected the message (HTTP " + code + ").");
            } catch (Exception e) {
                if (report) setStatus("Send failed. Is Voicepad running on the same Wi-Fi?");
            } finally {
                if (c != null) c.disconnect();
            }
        });
    }

    private void toggleSpeech() {
        if (wantsSpeech) {
            wantsSpeech = false;
            speechStarting = false;
            listenButton.setText("Start microphone");
            setStatus("Microphone stopped.");
            if (speech != null) speech.cancel();
            return;
        }
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.RECORD_AUDIO}, MIC_REQUEST);
            return;
        }
        startSpeechSession();
    }

    @Override public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != MIC_REQUEST) return;
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED)
            startSpeechSession();
        else
            setStatus("Microphone permission was denied. Enable it in Android Settings → Apps → Voicepad.");
    }

    private void startSpeechSession() {
        if (!SpeechRecognizer.isRecognitionAvailable(this)) {
            setStatus("Android has no speech recognition service enabled.");
            return;
        }
        ensureSpeechRecognizer();
        wantsSpeech = true;
        listenButton.setText("Stop microphone");
        startRecognition();
    }

    private void ensureSpeechRecognizer() {
        if (speech != null) return;
        speech = SpeechRecognizer.createSpeechRecognizer(this);
        speechIntent = new Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH);
        speechIntent.putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM);
        speechIntent.putExtra(RecognizerIntent.EXTRA_LANGUAGE, Locale.getDefault().toLanguageTag());
        speechIntent.putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true);
        speechIntent.putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 3);
        speech.setRecognitionListener(new RecognitionListener() {
            @Override public void onReadyForSpeech(Bundle params) {
                speechStarting = false;
                setStatus("Listening — speak into the phone or connected Bluetooth microphone.");
            }
            @Override public void onBeginningOfSpeech() { }
            @Override public void onRmsChanged(float rmsdB) { }
            @Override public void onBufferReceived(byte[] buffer) { }
            @Override public void onEndOfSpeech() { speechStarting = false; }
            @Override public void onError(int error) {
                speechStarting = false;
                if (!wantsSpeech) return;
                if (error == SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS) {
                    wantsSpeech = false;
                    listenButton.setText("Start microphone");
                    setStatus("Android denied microphone access. Enable it in app settings.");
                    return;
                }
                if (error == SpeechRecognizer.ERROR_NETWORK || error == SpeechRecognizer.ERROR_NETWORK_TIMEOUT)
                    setStatus("Speech service network error; retrying…");
                restartRecognition(error == SpeechRecognizer.ERROR_RECOGNIZER_BUSY ? 800 : 250);
            }
            @Override public void onResults(Bundle results) {
                speechStarting = false;
                ArrayList<String> values = results.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION);
                if (values != null && !values.isEmpty()) {
                    String text = values.get(0).trim();
                    if (!text.isEmpty()) sendTranscript(text);
                }
                restartRecognition(180);
            }
            @Override public void onPartialResults(Bundle partialResults) { }
            @Override public void onEvent(int eventType, Bundle params) { }
        });
    }

    private void startRecognition() {
        if (!wantsSpeech || speechStarting || speech == null) return;
        speechStarting = true;
        try {
            speech.startListening(speechIntent);
            setStatus("Starting microphone…");
        } catch (Exception e) {
            speechStarting = false;
            restartRecognition(700);
        }
    }

    private void restartRecognition(long delayMs) {
        if (!wantsSpeech) return;
        main.postDelayed(this::startRecognition, delayMs);
    }

    private final class InkPad extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float lastX = -1;
        private float lastY = -1;
        private long lastSent;

        InkPad(Context context) {
            super(context);
            setBackgroundColor(Color.rgb(242, 242, 238));
            paint.setColor(Color.rgb(80, 80, 80));
            paint.setTextSize(dp(15));
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            canvas.drawText("Phone drawing surface", dp(14), dp(25), paint);
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            getParent().requestDisallowInterceptTouchEvent(true);
            int action = event.getActionMasked();
            float x = Math.max(0, Math.min(getWidth() - 1, event.getX()));
            float y = Math.max(0, Math.min(getHeight() - 1, event.getY()));
            long now = System.currentTimeMillis();
            if (action == MotionEvent.ACTION_DOWN) {
                lastX = x; lastY = y; lastSent = now;
                sendPoint('D', x, y);
                return true;
            }
            if (action == MotionEvent.ACTION_MOVE) {
                float dx = x - lastX, dy = y - lastY;
                if (now - lastSent >= 28 || dx * dx + dy * dy >= 100) {
                    lastX = x; lastY = y; lastSent = now;
                    sendPoint('M', x, y);
                }
                return true;
            }
            if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
                sendPoint('U', x, y);
                lastX = lastY = -1;
                getParent().requestDisallowInterceptTouchEvent(false);
                return true;
            }
            return true;
        }

        private void sendPoint(char action, float x, float y) {
            int nx = getWidth() > 1 ? Math.round(x * 10000f / (getWidth() - 1)) : 0;
            int ny = getHeight() > 1 ? Math.round(y * 10000f / (getHeight() - 1)) : 0;
            String body = action + " " + nx + " " + ny + " " + (eraserBox.isChecked() ? 1 : 0);
            postBody(drawingNetwork, "/draw", body, false);
        }
    }
}
