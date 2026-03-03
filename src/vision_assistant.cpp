#include <opencv2/opencv.hpp>
#include <curl/curl.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <fstream>
#include <termios.h>

// ── Config ────────────────────────────────────────────────────────────────────
#define INPUT_DEVICE      "/dev/input/event0"
#define OPENROUTER_URL    "https://openrouter.ai/api/v1/chat/completions"
#define OPENROUTER_KEY    "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"
#define MODEL             "anthropic/claude-opus-4.6"
#define PROMPT            "Describe what you see in this image in 10 words."
#define WARMUP_FRAMES     15
#define WIFI_CONFIG_FILE  "/etc/camera-wifi.conf"
#define WIFI_IFACE        "wlan0"
#define WPA_CONF          "/tmp/camera-wpa.conf"
#define IDLE_TIMEOUT_SEC  30

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
struct WifiConfig {
    std::string ssid;
    std::string password;
};

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
             "wpa_supplicant -B -i " WIFI_IFACE " -c %s >/dev/null 2>&1", WPA_CONF);
    if (system(cmd) != 0) {
        fprintf(stderr, "ERROR: wpa_supplicant failed\n");
        return false;
    }
    sleep(3);

    snprintf(cmd, sizeof(cmd), "udhcpc -i " WIFI_IFACE " -q 2>/dev/null");
    if (system(cmd) != 0) {
        fprintf(stderr, "ERROR: DHCP failed\n");
        return false;
    }

    system("echo 'nameserver 8.8.8.8' > /etc/resolv.conf");

    if (system("ping -c 1 -W 3 8.8.8.8 >/dev/null 2>&1") != 0) {
        fprintf(stderr, "ERROR: no internet after connect\n");
        return false;
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

// ── Base64 ────────────────────────────────────────────────────────────────────
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const unsigned char *data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int b = (data[i] << 16)
                       | (i+1 < len ? data[i+1] << 8 : 0)
                       | (i+2 < len ? data[i+2]      : 0);
        out += B64[(b >> 18) & 0x3f];
        out += B64[(b >> 12) & 0x3f];
        out += (i+1 < len) ? B64[(b >> 6) & 0x3f] : '=';
        out += (i+2 < len) ? B64[b        & 0x3f] : '=';
    }
    return out;
}

// ── CURL ─────────────────────────────────────────────────────────────────────
static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    std::string *buf = (std::string *)userdata;
    buf->append((char *)ptr, size * nmemb);
    return size * nmemb;
}

static std::string parse_content(const std::string &json)
{
    const std::string key = "\"content\":\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) return json;
    pos += key.size();
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos+1 < json.size()) {
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

// ── Camera ────────────────────────────────────────────────────────────────────
static bool capture_frame(std::vector<uchar> &jpeg_buf)
{
    cv::VideoCapture cap;
    cap.open(0);
    if (!cap.isOpened()) {
        fprintf(stderr, "ERROR: cannot open camera\n");
        return false;
    }
    cv::Mat frame;
    for (int i = 0; i < WARMUP_FRAMES; i++) cap >> frame;
    cap >> frame;
    cap.release();
    if (frame.empty()) {
        fprintf(stderr, "ERROR: empty frame\n");
        return false;
    }
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 85};
    cv::imencode(".jpg", frame, jpeg_buf, params);
    return true;
}

// ── OpenRouter ────────────────────────────────────────────────────────────────
static std::string query_openrouter(const std::string &b64_image)
{
    CURL *curl = curl_easy_init();
    if (!curl) return "ERROR: curl init failed";

    std::string response;
    std::string body =
        "{"
        "\"model\":\"" MODEL "\","
        "\"messages\":[{"
            "\"role\":\"user\","
            "\"content\":["
                "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64," + b64_image + "\"}},"
                "{\"type\":\"text\",\"text\":\"" PROMPT "\"}"
            "]"
        "}]"
        "}";

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Authorization: Bearer " OPENROUTER_KEY);

    curl_easy_setopt(curl, CURLOPT_URL, OPENROUTER_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return std::string("ERROR: curl - ") + curl_easy_strerror(res);

    return parse_content(response);
}

// ── Button: wait with optional timeout (-1 = forever) ────────────────────────
static bool wait_for_button(int timeout_sec)
{
    int fd = open(INPUT_DEVICE, O_RDONLY | O_NONBLOCK);
    if (fd < 0) { perror("ERROR: open input device"); exit(1); }

    time_t start = time(NULL);
    struct input_event ev;

    while (true) {
        ssize_t n = read(fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) {
                close(fd);
                return true;
            }
        }
        if (timeout_sec > 0 && (time(NULL) - start) >= timeout_sec) {
            close(fd);
            return false;
        }
        usleep(10000);
    }
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main()
{
    printf("=== Vision Assistant ===\n");
    printf("Press USER button to capture and analyze.\n");
    printf("Press Ctrl+C to exit.\n\n");

    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Load or prompt WiFi config
    WifiConfig wifi;
    if (!load_wifi_config(wifi)) {
        wifi = prompt_wifi_config();
    } else {
        printf("WiFi config loaded: SSID='%s'\n", wifi.ssid.c_str());
    }

    while (true) {
        printf("\nWaiting for button press...\n");
        wait_for_button(-1);

        // Connect WiFi
        if (!wifi_connect(wifi)) {
            printf("WiFi failed. Delete %s to reconfigure.\n", WIFI_CONFIG_FILE);
            continue;
        }

        // Session loop — stay connected for IDLE_TIMEOUT_SEC after each capture
        while (true) {
            printf("Capturing image...\n");
            std::vector<uchar> jpeg_buf;
            if (!capture_frame(jpeg_buf)) {
                printf("Capture failed.\n");
            } else {
                printf("Captured %zu bytes. Sending to AI...\n", jpeg_buf.size());
                std::string b64 = base64_encode(jpeg_buf.data(), jpeg_buf.size());
                std::string result = query_openrouter(b64);
                printf("\n--- AI Response ---\n%s\n-------------------\n", result.c_str());
            }

            printf("\nPress button within %ds for another capture, or wait to disconnect...\n",
                   IDLE_TIMEOUT_SEC);

            bool pressed = wait_for_button(IDLE_TIMEOUT_SEC);
            if (!pressed) {
                wifi_disconnect();
                break;
            }
        }
    }

    curl_global_cleanup();
    return 0;
}
