/**
 * SG-DSP Engine: ClickHouse Tick Reader → Adaptive Savitzky-Golay → ZeroMQ Binary Publisher
 *
 * Pipeline:
 *   1. Bootstrap: fetch last N ticks from ClickHouse HTTP API to warm the SG filter
 *   2. Poll loop: query ClickHouse for new ticks since last_sequence
 *   3. For each new tick: ingest into SG circular buffer, compute filtered output
 *   4. Publish 52-byte binary frame via ZMQ PUB socket
 *
 * Binary frame layout (little-endian, 52 bytes):
 *   [0..3]   uint32   magic = 0x53474450 ("SGDP")
 *   [4..11]  uint64   timestamp_ns
 *   [12..19] double   sg_smoothed   (SG-filtered price)
 *   [20..27] double   sg_slope      (1st derivative — replaces emaTrendSlope)
 *   [28..35] double   sg_accel      (2nd derivative)
 *   [36..43] double   sg_sideway    (residual-based sideway score)
 *   [44..47] int32    adaptive_window
 *   [48..51] int32    poly_order
 *
 * Zero-GC on the Java subscriber: ByteBuffer.getDouble() reads directly.
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>

#include <zmq.hpp>
#include <curl/curl.h>

#include "adaptive_sg_filter.hpp"

// ============================================================================
// Binary frame constants
// ============================================================================

static constexpr uint32_t SG_FRAME_MAGIC = 0x53474450; // "SGDP"
static constexpr size_t   SG_FRAME_SIZE  = 52;

#pragma pack(push, 1)
struct SgFrame {
    uint32_t magic;
    uint64_t timestamp_ns;
    double   sg_smoothed;
    double   sg_slope;
    double   sg_accel;
    double   sg_sideway;
    int32_t  adaptive_window;
    int32_t  poly_order;
};
#pragma pack(pop)

static_assert(sizeof(SgFrame) == SG_FRAME_SIZE, "Frame packing mismatch");

// ============================================================================
// CURL response accumulator
// ============================================================================

struct CurlBuffer {
    std::string data;
};

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    auto* buf = static_cast<CurlBuffer*>(userdata);
    buf->data.append(static_cast<char*>(ptr), total);
    return total;
}

// ============================================================================
// ClickHouse HTTP client — minimal, no external JSON dependency
// ============================================================================

class ClickHouseReader {
public:
    ClickHouseReader(const std::string& host, int port,
                     const std::string& user, const std::string& password)
        : base_url_("http://" + host + ":" + std::to_string(port) + "/"),
          user_(user), password_(password), last_sequence_(0)
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~ClickHouseReader() {
        curl_global_cleanup();
    }

    struct TickRow {
        uint64_t sequence;
        uint64_t timestamp_ms;
        double   price;
        double   volume;
        double   dom_cycle;
        double   vress;
        double   hmm_prob_crisis;
        double   sideway_score;
    };

    // Fetch historical ticks for SG warm-up (most recent `limit` rows)
    bool fetch_history(std::vector<TickRow>& out, int limit = 500) {
        std::string query =
            "SELECT sequence, toUnixTimestamp64Milli(timestamp), price, volume, "
            "dom_cycle, vress, hmm_prob_crisis, 0 AS sideway_score "
            "FROM hft_market_data "
            "ORDER BY sequence DESC LIMIT " + std::to_string(limit) +
            " FORMAT TabSeparated";

        std::string response;
        if (!execute_query(query, response)) return false;

        parse_rows(response, out);
        std::reverse(out.begin(), out.end());

        if (!out.empty()) {
            last_sequence_ = out.back().sequence;
        }
        return true;
    }

    // Poll for new ticks since last_sequence_
    bool poll_new(std::vector<TickRow>& out) {
        std::string query =
            "SELECT sequence, toUnixTimestamp64Milli(timestamp), price, volume, "
            "dom_cycle, vress, hmm_prob_crisis, 0 AS sideway_score "
            "FROM hft_market_data "
            "WHERE sequence > " + std::to_string(last_sequence_) +
            " ORDER BY sequence ASC LIMIT 5000"
            " FORMAT TabSeparated";

        std::string response;
        if (!execute_query(query, response)) return false;

        parse_rows(response, out);

        if (!out.empty()) {
            last_sequence_ = out.back().sequence;
        }
        return true;
    }

    uint64_t last_sequence() const { return last_sequence_; }

private:
    std::string base_url_;
    std::string user_;
    std::string password_;
    uint64_t last_sequence_;

    bool execute_query(const std::string& query, std::string& response) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        CurlBuffer buf;
        std::string url = base_url_ + "?user=" + user_ + "&password=" + password_;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(query.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) return false;

        response = std::move(buf.data);
        return true;
    }

    void parse_rows(const std::string& tsv, std::vector<TickRow>& out) {
        std::istringstream stream(tsv);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            TickRow row{};
            if (parse_tab_line(line, row)) {
                out.push_back(row);
            }
        }
    }

    static bool parse_tab_line(const std::string& line, TickRow& row) {
        const char* p = line.c_str();
        char* end;

        row.sequence       = std::strtoull(p, &end, 10); if (*end != '\t') return false; p = end + 1;
        row.timestamp_ms   = std::strtoull(p, &end, 10); if (*end != '\t') return false; p = end + 1;
        row.price          = std::strtod(p, &end);       if (*end != '\t') return false; p = end + 1;
        row.volume         = std::strtod(p, &end);       if (*end != '\t') return false; p = end + 1;
        row.dom_cycle      = std::strtod(p, &end);       if (*end != '\t') return false; p = end + 1;
        row.vress          = std::strtod(p, &end);       if (*end != '\t') return false; p = end + 1;
        row.hmm_prob_crisis= std::strtod(p, &end);       if (*end != '\t') return false; p = end + 1;
        row.sideway_score  = std::strtod(p, &end);

        return true;
    }
};

// ============================================================================
// Main engine
// ============================================================================

int main() {
    std::cout << "[SG-DSP] Adaptive Savitzky-Golay DSP Engine Starting..." << std::endl;

    // --- Environment configuration ---
    auto env_or = [](const char* name, const char* def) -> std::string {
        const char* v = std::getenv(name);
        return (v && v[0]) ? v : def;
    };

    std::string ch_host     = env_or("CLICKHOUSE_HOST", "clickhouse");
    int         ch_port     = std::stoi(env_or("CLICKHOUSE_PORT", "8123"));
    std::string ch_user     = env_or("CLICKHOUSE_USER", "default");
    std::string ch_password = env_or("CLICKHOUSE_PASSWORD", "");
    std::string zmq_port    = env_or("SG_ZMQ_PORT", "5557");
    int         poll_ms     = std::stoi(env_or("SG_POLL_INTERVAL_MS", "50"));
    int         warmup      = std::stoi(env_or("SG_WARMUP_ROWS", "500"));

    // --- ZeroMQ PUB socket ---
    zmq::context_t zmq_ctx(1);
    zmq::socket_t zmq_pub(zmq_ctx, zmq::socket_type::pub);
    zmq_pub.set(zmq::sockopt::sndhwm, 10000);
    zmq_pub.set(zmq::sockopt::linger, 0);
    zmq_pub.bind("tcp://*:" + zmq_port);
    std::cout << "[SG-DSP] ZMQ PUB bound on tcp://*:" << zmq_port << std::endl;

    // --- SG filter initialization ---
    sg::SgCoefficientCache coeff_cache;
    sg::AdaptiveSgFilter sg_filter(coeff_cache);
    std::cout << "[SG-DSP] SG coefficient cache built (windows "
              << sg::SG_MIN_WINDOW << ".." << sg::SG_MAX_WINDOW << ")" << std::endl;

    // --- ClickHouse reader with retry ---
    ClickHouseReader ch_reader(ch_host, ch_port, ch_user, ch_password);

    std::cout << "[SG-DSP] Waiting for ClickHouse at " << ch_host << ":" << ch_port << "..." << std::endl;
    std::vector<ClickHouseReader::TickRow> history;
    int retries = 30;
    while (retries > 0) {
        if (ch_reader.fetch_history(history, warmup) && !history.empty()) break;
        --retries;
        std::cerr << "[SG-DSP] ClickHouse not ready, retries left: " << retries << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    if (history.empty()) {
        std::cerr << "[SG-DSP] No historical data after retries. Starting cold." << std::endl;
    } else {
        std::cout << "[SG-DSP] Warming up with " << history.size() << " historical ticks..." << std::endl;
        for (const auto& row : history) {
            sg_filter.ingest(row.price);
        }
        if (!history.empty()) {
            const auto& last = history.back();
            double price_scale = std::max(std::abs(last.price) * 1e-4, 1e-9);
            double noise_ratio = std::min(1.0, last.vress / price_scale);
            sg_filter.adapt(last.dom_cycle, noise_ratio, last.hmm_prob_crisis, last.sideway_score);
        }
        std::cout << "[SG-DSP] Warm-up complete. Window=" << sg_filter.window()
                  << " Poly=" << sg_filter.poly_order() << std::endl;
    }
    history.clear();

    // --- Main polling loop ---
    std::cout << "[SG-DSP] Entering live polling loop (interval=" << poll_ms << "ms)..." << std::endl;
    uint64_t total_ticks = 0;
    uint64_t total_published = 0;
    SgFrame frame;
    frame.magic = SG_FRAME_MAGIC;

    std::vector<ClickHouseReader::TickRow> batch;
    batch.reserve(5000);

    while (true) {
        batch.clear();
        if (!ch_reader.poll_new(batch) || batch.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
            continue;
        }

        for (const auto& tick : batch) {
            sg_filter.ingest(tick.price);
            ++total_ticks;

            // Adapt window periodically (every 100 ticks to avoid overhead)
            if (total_ticks % 100 == 0) {
                double price_scale = std::max(std::abs(tick.price) * 1e-4, 1e-9);
                double noise_ratio = std::min(1.0, tick.vress / price_scale);
                sg_filter.adapt(tick.dom_cycle, noise_ratio,
                               tick.hmm_prob_crisis, tick.sideway_score);
            }

            sg::SgResult result;
            if (!sg_filter.compute(result)) continue;

            double sideway = sg_filter.compute_sideway_score();

            // Pack binary frame
            frame.timestamp_ns    = tick.timestamp_ms * 1000000ULL;
            frame.sg_smoothed     = result.smoothed;
            frame.sg_slope        = result.slope;
            frame.sg_accel        = result.acceleration;
            frame.sg_sideway      = sideway;
            frame.adaptive_window = result.window;
            frame.poly_order      = result.poly_order;

            zmq::message_t msg(SG_FRAME_SIZE);
            std::memcpy(msg.data(), &frame, SG_FRAME_SIZE);
            zmq_pub.send(msg, zmq::send_flags::dontwait);
            ++total_published;

            if (total_published % 5000 == 0) {
                std::cout << "[SG-DSP] Published " << total_published
                          << " frames | Window=" << result.window
                          << " Poly=" << result.poly_order
                          << " Smoothed=" << result.smoothed
                          << " Slope=" << result.slope
                          << " Sideway=" << sideway << std::endl;
            }
        }
    }

    return 0;
}
