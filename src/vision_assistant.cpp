#include <opencv2/opencv.hpp>
#include <curl/curl.h>
#include <linux/input.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <fstream>
#include <termios.h>
#include <stdint.h>
#include <sstream>

// ── Config ────────────────────────────────────────────────────────────────────
#define INPUT_DEVICE          "/dev/input/event0"
#define GEMINI_KEY            "AIzaSyCmaq0vaKmDujCFQZjC93TGv9ag0HB9-LE"
#define GEMINI_VISION_URL     "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-lite:generateContent"
#define GEMINI_TTS_URL        "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-preview-tts:generateContent"
#define WARMUP_FRAMES         15
#define WIFI_CONFIG_FILE      "/etc/camera-wifi.conf"
#define WIFI_IFACE            "wlan0"
#define WPA_CONF              "/tmp/camera-wpa.conf"
#define IDLE_TIMEOUT_SEC      30
#define AUDIO_DEVICE_CAPTURE  "hw:0,0"
#define AUDIO_DEVICE_PLAYBACK "hw:1,0"
#define RECORD_SECONDS        5
#define BOARD_SAMPLE_RATE     16000
#define TTS_SAMPLE_RATE       24000
#define INPUT_AUDIO_FILE      "/tmp/input_audio.wav"
#define OUTPUT_RAW_FILE       "/tmp/response.raw"

// ── JSON value extractor (handles "key":"val" and "key": "val") ───────────────
static std::string json_extract(const std::string &json, const char *key)
{
    std::string k1 = std::string("\"") + key + "\":\"";
    std::string k2 = std::string("\"") + key + "\": \"";
    size_t pos = json.find(k1);
    size_t klen = k1.size();
    if (pos == std::string::npos) {
        pos = json.find(k2);
        klen = k2.size();
    }
    if (pos == std::string::npos) return "";
    pos += klen;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            char esc = json[++pos];
            if      (esc == 'n')  result += '\n';
            else if (esc == 't')  result += '\t';
            else if (esc == '"')  result += '"';
            else if (esc == '\\') result += '\\';
            else                  result += esc;
        } else {
            result += json[pos];
        }
        pos++;
    }
    return result;
}

// ── JSON value extractor (raw, no escape processing — for base64) ─────────────
static std::string json_extract_raw(const std::string &json, const char *key)
{
    std::string k1 = std::string("\"") + key + "\":\"";
    std::string k2 = std::string("\"") + key + "\": \"";
    size_t pos = json.find(k1);
    size_t klen = k1.size();
    if (pos == std::string::npos) {
        pos = json.find(k2);
        klen = k2.size();
    }
    if (pos == std::string::npos) return "";
    pos += klen;
    std::string result;
    while (pos < json.size() && json[pos] != '"')
        result += json[pos++];
    return result;
}

// ── Terminal helpers ──────────────────────────────────────────────────────────
static std::string read_line_silent(const char *prompt)
{
    printf("%s", prompt);
    fflush(stdout);
    struct termios old, noecho;
    tcgetattr(STDIN_FILENO, &old);
    noecho = old;
    noecho.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &noecho);
    std::string result;
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        result += (char)c;
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    printf("\n");
    return result;
}

static std::string read_line(const char *prompt)
{
    printf("%s", prompt);
    fflush(stdout);
    std::string result;
    int c;
    while ((c = getchar()) != '\n' && c != EOF)
        result += (char)c;
    return result;
}

// ── WiFi config ───────────────────────────────────────────────────────────────
struct WifiConfig { std::string ssid, password; };

static bool load_wifi_config(WifiConfig &cfg)
{
    std::ifstream f(WIFI_CONFIG_FILE);
    if (!f.is_open()) return false;
    std::getline(f, cfg.ssid);
    std::getline(f, cfg.password);
    return !cfg.ssid.empty();
}

static void save_wifi_config(const WifiConfig &cfg)
{
    std::ofstream f(WIFI_CONFIG_FILE);
    f << cfg.ssid << "\n" << cfg.password << "\n";
    printf("WiFi credentials saved to %s\n", WIFI_CONFIG_FILE);
}

static WifiConfig prompt_wifi_config()
{
    printf("\n=== First Time WiFi Setup ===\n");
    WifiConfig cfg;
    cfg.ssid     = read_line("Enter WiFi SSID: ");
    cfg.password = read_line_silent("Enter WiFi Password: ");
    save_wifi_config(cfg);
    return cfg;
}

// ── WiFi connect/disconnect ───────────────────────────────────────────────────
static bool wifi_connect(const WifiConfig &cfg)
{
    printf("Connecting to WiFi '%s'...\n", cfg.ssid.c_str());
    FILE *f = fopen(WPA_CONF, "w");
    if (!f) { perror("fopen wpa conf"); return false; }
    fprintf(f, "network={\n  ssid=\"%s\"\n  psk=\"%s\"\n}\n",
            cfg.ssid.c_str(), cfg.password.c_str());
    fclose(f);
    system("killall wpa_supplicant 2>/dev/null; sleep 1");
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "wpa_supplicant -B -i " WIFI_IFACE " -c %s 2>/dev/null", WPA_CONF);
    if (system(cmd) != 0) { fprintf(stderr, "ERROR: wpa_supplicant failed\n"); return false; }
    sleep(3);
    snprintf(cmd, sizeof(cmd), "udhcpc -i " WIFI_IFACE " -q 2>/dev/null");
    if (system(cmd) != 0) { fprintf(stderr, "ERROR: DHCP failed\n"); return false; }
    system("echo 'nameserver 8.8.8.8' > /etc/resolv.conf");
    if (system("ping -c 1 -W 3 8.8.8.8 >/dev/null 2>&1") != 0) {
        fprintf(stderr, "ERROR: no internet after connect\n"); return false;
    }
    printf("WiFi connected.\n");
    return true;
}

static void wifi_disconnect()
{
    printf("Disconnecting WiFi...\n");
    system("killall wpa_supplicant 2>/dev/null");
    system("ip addr flush dev " WIFI_IFACE " 2>/dev/null");
    printf("WiFi disconnected.\n");
}

// ── Base64 encode ─────────────────────────────────────────────────────────────
static const char B64_ENC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char *data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int b = (data[i] << 16)
                       | (i+1 < len ? data[i+1] << 8 : 0)
                       | (i+2 < len ? data[i+2]      : 0);
        out += B64_ENC[(b >> 18) & 0x3f];
        out += B64_ENC[(b >> 12) & 0x3f];
        out += (i+1 < len) ? B64_ENC[(b >> 6) & 0x3f] : '=';
        out += (i+2 < len) ? B64_ENC[b        & 0x3f] : '=';
    }
    return out;
}

// ── Base64 decode ─────────────────────────────────────────────────────────────
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<uint8_t> base64_decode(const std::string &in)
{
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, valb = -8;
    for (char c : in) {
        if (c == '=') break;
        int v = b64_val(c);
        if (v < 0) continue;
        val = (val << 6) + v;
        valb += 6;
        if (valb >= 0) {
            out.push_back((uint8_t)((val >> valb) & 0xff));
            valb -= 8;
        }
    }
    return out;
}

// ── JSON string escaper ───────────────────────────────────────────────────────
static std::string json_escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// ── Decimate 24kHz → 16kHz (3:2 ratio, linear interpolation) ─────────────────
static std::vector<int16_t> decimate_24k_to_16k(const uint8_t *raw, size_t byte_len)
{
    size_t in_samples = byte_len / 2;
    const int16_t *in = reinterpret_cast<const int16_t *>(raw);
    size_t aligned = (in_samples / 3) * 3;
    std::vector<int16_t> out;
    out.reserve(aligned * 2 / 3);
    for (size_t i = 0; i < aligned; i += 3) {
        out.push_back(in[i]);
        out.push_back((int16_t)(((int32_t)in[i+1] + in[i+2]) / 2));
    }
    return out;
}

// ── CURL POST helper ──────────────────────────────────────────────────────────
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    ((std::string *)userdata)->append((char *)ptr, size * nmemb);
    return size * nmemb;
}

static std::string curl_post_json(const char *url, const std::string &body)
{
    CURL *curl = curl_easy_init();
    if (!curl) { fprintf(stderr, "ERROR: curl init\n"); return ""; }
    std::string response;
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "x-goog-api-key: " GEMINI_KEY);
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       60L);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "ERROR: curl - %s\n", curl_easy_strerror(res));
        return "";
    }
    return response;
}

// ── Camera capture ────────────────────────────────────────────────────────────
static bool capture_frame(std::vector<uchar> &jpeg_buf)
{
    cv::VideoCapture cap;
    cap.open(0);
    if (!cap.isOpened()) { fprintf(stderr, "ERROR: cannot open camera\n"); return false; }
    cv::Mat frame;
    for (int i = 0; i < WARMUP_FRAMES; i++) cap >> frame;
    cap >> frame;
    cap.release();
    if (frame.empty()) { fprintf(stderr, "ERROR: empty frame\n"); return false; }
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
    cv::imencode(".jpg", frame, jpeg_buf, params);
    return true;
}

// ── Microphone recording ──────────────────────────────────────────────────────
static bool record_audio(const char *path, int seconds)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "arecord -D " AUDIO_DEVICE_CAPTURE " -r %d -f S16_LE -c 1 -d %d %s 2>/dev/null",
        BOARD_SAMPLE_RATE, seconds, path);
    return system(cmd) == 0;
}

// ── ALSA playback init ────────────────────────────────────────────────────────
// Restores system-wide audio state saved at boot via /etc/init.d/S05audio
static void alsa_init_playback()
{
    system("alsactl restore 2>/dev/null");
}

// ── Audio playback via fork+exec ──────────────────────────────────────────────
static void play_audio(const char *path)
{
    char rate_str[16];
    snprintf(rate_str, sizeof(rate_str), "%d", BOARD_SAMPLE_RATE);
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/aplay", "aplay",
              "-D", AUDIO_DEVICE_PLAYBACK,
              "-t", "raw",
              "-f", "S16_LE",
              "-r", rate_str,
              "-c", "1",
              path, NULL);
        _exit(1);
    } else if (pid > 0) {
        waitpid(pid, NULL, 0);
    } else {
        perror("fork");
    }
}

// ── Gemini Vision + Audio → text ──────────────────────────────────────────────
static std::string query_gemini_vision(const std::string &b64_image,
                                       const std::string &b64_audio)
{
    std::string parts;
    parts += "{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"" + b64_image + "\"}}";
    if (!b64_audio.empty()) {
        parts += ",{\"inline_data\":{\"mime_type\":\"audio/wav\",\"data\":\"" + b64_audio + "\"}}";
        parts += ",{\"text\":\"You are a helpful wearable assistant. "
                 "Answer the question asked in the audio concisely, in 1-3 sentences.\"}";
    } else {
        parts += ",{\"text\":\"You are a helpful wearable assistant. "
                 "Describe what you see in this image concisely, in 1-3 sentences.\"}";
    }
    std::string body = "{\"contents\":[{\"parts\":[" + parts + "]}]}";
    printf("Querying Gemini Vision...\n");
    std::string response = curl_post_json(GEMINI_VISION_URL, body);
    if (response.empty()) return "";
    std::string text = json_extract(response, "text");
    if (text.empty())
        fprintf(stderr, "WARN: no text in response:\n%.300s\n", response.c_str());
    return text;
}

// ── Gemini TTS → decode → 24k→16k → write raw PCM ───────────────────────────
static uint32_t query_gemini_tts(const std::string &text, const char *output_path)
{
    std::string body =
        "{\"contents\":[{\"parts\":[{\"text\":\"" + json_escape(text) + "\"}]}],"
        "\"generationConfig\":{"
            "\"responseModalities\":[\"AUDIO\"],"
            "\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":"
                "{\"voiceName\":\"charon\"}}}"
        "}}";
    printf("Generating TTS...\n");
    std::string response = curl_post_json(GEMINI_TTS_URL, body);
    if (response.empty()) return 0;

    std::string b64 = json_extract_raw(response, "data");
    if (b64.empty()) {
        fprintf(stderr, "WARN: no audio data in TTS response:\n%.300s\n", response.c_str());
        return 0;
    }

    std::vector<uint8_t> raw = base64_decode(b64);
    if (raw.empty()) { fprintf(stderr, "ERROR: b64 decode empty\n"); return 0; }

    std::vector<int16_t> samples = decimate_24k_to_16k(raw.data(), raw.size());
    if (samples.empty()) { fprintf(stderr, "ERROR: decimate empty\n"); return 0; }

    FILE *f = fopen(output_path, "wb");
    if (!f) { perror("fopen output raw"); return 0; }
    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);

    printf("TTS ready: %zu samples @ %dHz\n", samples.size(), BOARD_SAMPLE_RATE);
    return (uint32_t)samples.size();
}

// ── Phone relay (Phase 2 stub) ────────────────────────────────────────────────
static bool phone_relay_available() { return false; }

// ── Core query pipeline ───────────────────────────────────────────────────────
static void process_query_wifi()
{
    printf("Capturing image...\n");
    std::vector<uchar> jpeg_buf;
    if (!capture_frame(jpeg_buf)) { printf("Image capture failed.\n"); return; }
    printf("Image: %zu bytes\n", jpeg_buf.size());

    printf("Listening... speak now (%ds)\n", RECORD_SECONDS);
    bool has_audio = record_audio(INPUT_AUDIO_FILE, RECORD_SECONDS);
    if (!has_audio) printf("WARN: mic record failed — sending image only\n");

    std::string b64_image = base64_encode(jpeg_buf.data(), jpeg_buf.size());
    std::string b64_audio;
    if (has_audio) {
        std::ifstream af(INPUT_AUDIO_FILE, std::ios::binary);
        if (af.is_open()) {
            std::vector<uint8_t> ab((std::istreambuf_iterator<char>(af)), {});
            b64_audio = base64_encode(ab.data(), ab.size());
        }
    }

    std::string text = query_gemini_vision(b64_image, b64_audio);
    if (text.empty()) { printf("No response received.\n"); return; }
    printf("\n--- Response ---\n%s\n----------------\n\n", text.c_str());

    uint32_t num_samples = query_gemini_tts(text, OUTPUT_RAW_FILE);
    if (num_samples == 0) { printf("TTS failed — text shown above.\n"); return; }

    // Restore DAC state — camera pipeline and arecord corrupt audio hardware registers
    system("alsactl restore 2>/dev/null");
    sleep(1);
    printf("Playing...\n");
    play_audio(OUTPUT_RAW_FILE);
}

// ── Button wait ───────────────────────────────────────────────────────────────
static bool wait_for_button(int timeout_sec)
{
    int fd = open(INPUT_DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror("ERROR: open input device"); exit(1); }
    time_t start = time(NULL);
    struct input_event ev;
    while (true) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev) && ev.type == EV_KEY && ev.value == 1) {
            close(fd); return true;
        }
        if (timeout_sec > 0 && (time(NULL) - start) >= timeout_sec) {
            close(fd); return false;
        }
        usleep(10000);
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main()
{
    printf("=== Vision Assistant ===\n");
    printf("Press USER button to capture + listen + respond.\n");
    printf("Press Ctrl+C to exit.\n\n");

    curl_global_init(CURL_GLOBAL_DEFAULT);
    alsa_init_playback();

    WifiConfig wifi;
    if (!load_wifi_config(wifi))
        wifi = prompt_wifi_config();
    else
        printf("WiFi config loaded: SSID='%s'\n", wifi.ssid.c_str());

    bool wifi_connected = false;

    while (true) {
        printf("\nWaiting for button press...\n");
        wait_for_button(-1);

        if (phone_relay_available())
            printf("[Phone relay not yet implemented — falling back to WiFi]\n");

        if (!wifi_connected) {
            wifi_connected = wifi_connect(wifi);
            if (!wifi_connected) {
                printf("WiFi failed. Delete %s to reconfigure.\n", WIFI_CONFIG_FILE);
                continue;
            }
        }

        process_query_wifi();

        printf("\nPress button within %ds for another query...\n", IDLE_TIMEOUT_SEC);
        if (!wait_for_button(IDLE_TIMEOUT_SEC)) {
            wifi_disconnect();
            wifi_connected = false;
        }
    }

    curl_global_cleanup();
    return 0;
}
