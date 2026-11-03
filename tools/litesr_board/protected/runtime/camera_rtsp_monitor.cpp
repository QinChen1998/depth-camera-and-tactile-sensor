// Observe the owned VENC channel. No capture, decoding or synthetic frame clock.
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace rtsp_camera {
static std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
static bool number(const std::string& s, uint64_t& value) {
    if (s.empty() || s.size() > 19 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    value = strtoull(s.c_str(), nullptr, 10);
    return true;
}
static std::vector<std::string> columns(const std::string& s) {
    std::vector<std::string> result;
    std::istringstream input(s); std::string part;
    while (std::getline(input, part, '|')) result.push_back(trim(part));
    return result;
}
static int index(const std::vector<std::string>& v, const char* key) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == key) return static_cast<int>(i);
    return -1;
}
struct Counters { uint64_t encoded = 0, retrieved = 0; };
static bool parse(const std::string& text, Counters& result) {
    std::istringstream input(text); std::string line;
    std::vector<std::string> header;
    bool counters = false, shape = false, running = false;
    while (std::getline(input, line)) {
        auto row = columns(line);
        if (row.empty()) continue;
        if (row[0] == "ID") { header = row; continue; }
        uint64_t channel = 0;
        if (!number(row[0], channel) || row.size() != header.size()) continue;
        const int encoded = index(header, "seq_encoded"), got = index(header, "seq_user_get");
        const int width = index(header, "Width"), height = index(header, "Height"), type = index(header, "Type");
        const int run = index(header, "runing");
        if (encoded >= 0 || type >= 0 || run >= 0) {
            // The managed mode enables exactly one encoder; an extra channel is not ours.
            if (channel != 0) return false;
        }
        if (encoded >= 0 && got >= 0) {
            if (counters || !number(row[encoded], result.encoded) || !number(row[got], result.retrieved)) return false;
            counters = true;
        }
        if (width >= 0 && height >= 0 && type >= 0) {
            const int by_frame = index(header, "ByFrame");
            if (shape || row[width] != "2304" || row[height] != "1296" || row[type] != "h265"
                || by_frame < 0 || row[by_frame] != "y") return false;
            shape = true;
        }
        if (run >= 0) {
            if (running || row[run] != "1") return false;
            running = true;
        }
        for (const char* key : {"enc_err_cnt", "wrap_ovfl", "bs_ovfl", "wrap_src_mis"}) {
            const int field = index(header, key);
            if (field >= 0 && row[field] != "0") return false;
        }
    }
    return counters && shape && running && result.retrieved <= result.encoded;
}
struct Progress {
    Counters previous{};
    Counters last_observed{};
    uint64_t observed_ns = 0;
    bool baseline = false, ready = false;
    // A duplicate observation never refreshes freshness. Reset is an identity failure.
    int observe(const Counters& value, uint64_t now) {
        if (!baseline) { previous = last_observed = value; baseline = true; return 0; }
        if (value.encoded < last_observed.encoded || value.retrieved < last_observed.retrieved) return -1;
        last_observed = value;
        if (value.encoded > previous.encoded && value.retrieved > previous.retrieved) {
            previous = value; observed_ns = now; ready = true; return 1;
        }
        return 0;
    }
};
static bool read(const std::string& path, std::string& text, size_t maximum = 131072) {
    std::ifstream file(path); if (!file) return false;
    text.clear(); char block[4096];
    while (file) { file.read(block, sizeof(block)); text.append(block, static_cast<size_t>(file.gcount())); if (text.size() > maximum) return false; }
    return !file.bad();
}
static uint64_t monotonic() {
    timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000000000ULL + uint64_t(t.tv_nsec);
}
static bool rtsp_client(const std::string& sockets) {
    std::istringstream lines(sockets); std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line); std::string slot, local, remote, state;
        if (!(fields >> slot >> local >> remote >> state)) continue;
        const size_t colon = local.rfind(':');
        if (colon != std::string::npos && local.substr(colon + 1) == "022A" && state == "01") return true;
    }
    return false;
}
static bool socket_clients(const std::string& tcp_path, const std::string& tcp6_path) {
    std::string tcp, tcp6;
    // Failure to inspect sockets must not be reported as an idle service.
    if (!read(tcp_path, tcp)) return true;
    if (rtsp_client(tcp)) return true;
    struct stat info{};
    // This firmware has no IPv6 socket table. Only confirmed ENOENT means absent;
    // permission/I/O errors still mean that connection state is unknown/busy.
    if (stat(tcp6_path.c_str(), &info)) return errno != ENOENT;
    if (!read(tcp6_path, tcp6)) return true;
    return rtsp_client(tcp6);
}
static bool client_connected() {
    return socket_clients("/proc/net/tcp", "/proc/net/tcp6");
}
static bool owned(pid_t pid, const std::string& start) {
    const std::string base = "/proc/" + std::to_string(pid);
    std::string stat;
    if (!read(base + "/stat", stat, 4096)) return false;
    size_t close = stat.rfind(") "); if (close == std::string::npos) return false;
    std::istringstream fields(stat.substr(close + 2)); std::string value;
    for (int i = 0; i <= 19; ++i) { if (!(fields >> value)) return false; if (i == 0 && value == "Z") return false; }
    if (value != start) return false;
    char executable[256]; const ssize_t n = readlink((base + "/exe").c_str(), executable, sizeof(executable));
    return n > 0 && std::string(executable, static_cast<size_t>(n)) == "/oem/usr/bin/rkipc";
}
static volatile sig_atomic_t active = 1;
static void stop(int) { active = 0; }
} // namespace rtsp_camera

#ifndef DEPTH_RTSP_MONITOR_NO_MAIN
int main(int argc, char** argv) {
    using namespace rtsp_camera;
    // PID, process start ticks, generation, model, private runtime directory, status directory.
    if (argc != 7) return 2;
    uint64_t pid_value = 0, start_value = 0, generation = 0;
    if (!number(argv[1], pid_value) || !pid_value || pid_value > 2147483647
        || !number(argv[2], start_value) || !number(argv[3], generation)) return 2;
    const std::string model = argv[4], runtime = argv[5], state = argv[6];
    if ((model != "B0" && model != "F_constant" && model != "F_LNCA")
        || runtime.empty() || runtime[0] != '/' || state.empty() || state[0] != '/') return 2;
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGHUP, stop);
    Progress progress; const uint64_t started = monotonic();
    while (active) {
        const uint64_t now = monotonic();
        if (!owned(static_cast<pid_t>(pid_value), argv[2])) {
            fprintf(stderr, "RTSP_MONITOR encoder identity/exit failure\n"); return 3;
        }
        std::string marker, raw;
        Counters counters;
        const bool guard = read(runtime + "/route_guard.active", marker, 64) && trim(marker) == argv[1];
        const bool valid = guard && read("/proc/vcodec/enc/venc_info", raw) && parse(raw, counters);
        if (valid && progress.observe(counters, now) < 0) {
            fprintf(stderr, "RTSP_MONITOR encoder sequence reset\n"); return 4;
        }
        const uint64_t freshness = progress.ready ? progress.observed_ns : started;
        const uint64_t limit = progress.ready ? 5000000000ULL : 10000000000ULL;
        if (now - freshness > limit) {
            fprintf(stderr, "RTSP_MONITOR %s\n", guard ? "encoded frame progress timeout" : "network guard not active"); return 5;
        }
        if (progress.ready) {
            std::ostringstream json;
            json << "{\"model\":\"" << model << "\",\"generation\":\"" << argv[3]
                 << "\",\"transport\":\"rtsp\",\"pixel_format\":\"H265\",\"work_mode\":\"camera_only\",\"depth_present\":false"
                 << ",\"rgb_width\":2304,\"rgb_height\":1296,\"frame_id\":" << progress.previous.encoded
                 << ",\"encoded_frame_id\":" << progress.previous.encoded
                 << ",\"encoder_retrieved_frame_id\":" << progress.previous.retrieved
                 << ",\"progress_source\":\"venc_info:chn0\",\"progress_timestamp_kind\":\"observation_monotonic\""
                 << ",\"updated_monotonic_ns\":" << progress.observed_ns
                 << ",\"health_checked_monotonic_ns\":" << now << ",\"encoder_pid\":" << pid_value
                 << ",\"client_connected\":" << (client_connected() ? "true" : "false")
                 << ",\"route_guard_active\":true,\"rtsp_path\":\"/live/0\"}\n";
            const std::string temporary = state + "/telemetry.tmp";
            std::ofstream file(temporary, std::ios::trunc); file << json.str(); file.close();
            if (!file || rename(temporary.c_str(), (state + "/telemetry.json").c_str())) {
                fprintf(stderr, "RTSP_MONITOR telemetry publication failure\n"); return 6;
            }
        }
        usleep(250000);
    }
    return 0;
}
#endif
