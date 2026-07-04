/**
 * SSA Engine: ClickHouse Tick Reader → HMM ZMQ Subscriber → Dual SSA Pipeline → ZMQ Binary Publisher
 *
 * Full pipeline (Phases 1-4):
 *   1. Bootstrap: fetch last N ticks from ClickHouse HTTP API to warm the circular buffer
 *   2. HMM Subscriber (background thread): ZMQ SUB on :5555 for REGIME|r,p0,p1,p2 messages
 *   3. AdaptiveL: maps regime probabilities → stable L_fast, L_slow via EMA + hysteresis
 *   4. Dual SSA Pipeline: fast + slow TrajectoryMatrix → RecursiveSVD → Hankelization
 *   5. SoftBlender: regime-adaptive tanh fusion of the two reconstructed signals
 *   6. Publish 68-byte SsaFrame via ZMQ PUB :5558
 *
 * Structural mirror of cpp-sg-dsp/main.cpp — same ClickHouse reader pattern,
 * same env-var configuration, same retry logic. Compiles identically in the
 * consolidated Dockerfile builder stage.
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

#include "ssa_types.hpp"
#include "ssa_pipeline.hpp"
#include "soft_blender.hpp"
#include "adaptive_l.hpp"

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
// ClickHouse HTTP client — polls hft_market_data for new ticks
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
    };

    bool fetch_history(std::vector<TickRow>& out, int limit) {
        std::string query =
            "SELECT sequence, toUnixTimestamp64Milli(timestamp), price "
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

    bool poll_new(std::vector<TickRow>& out) {
        std::string query =
            "SELECT sequence, toUnixTimestamp64Milli(timestamp), price "
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

        row.sequence     = std::strtoull(p, &end, 10); if (*end != '\t') return false; p = end + 1;
        row.timestamp_ms = std::strtoull(p, &end, 10); if (*end != '\t') return false; p = end + 1;
        row.price        = std::strtod(p, &end);

        return true;
    }
};

// ============================================================================
// HMM Regime Subscriber — background thread consuming ZMQ :5555
//
// Parses "REGIME|r,p0,p1,p2" string messages from the HMM engine.
// Stores regime probabilities in atomics (relaxed ordering — transient
// inconsistency between the three values is fine, smoothed by the EMA
// in AdaptiveL).
// ============================================================================

class HmmSubscriber {
public:
    HmmSubscriber()
        : p_calm_(0.33), p_trend_(0.33), p_crisis_(0.33),
          regime_(0), running_(false) {}

    void start(const std::string& host, int port) {
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&HmmSubscriber::run, this, host, port);
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    double p_calm()   const { return p_calm_.load(std::memory_order_relaxed); }
    double p_trend()  const { return p_trend_.load(std::memory_order_relaxed); }
    double p_crisis() const { return p_crisis_.load(std::memory_order_relaxed); }
    int    regime()   const { return regime_.load(std::memory_order_relaxed); }

private:
    std::atomic<double> p_calm_;
    std::atomic<double> p_trend_;
    std::atomic<double> p_crisis_;
    std::atomic<int>    regime_;
    std::atomic<bool>   running_;
    std::thread         thread_;

    void run(const std::string& host, int port) {
        zmq::context_t ctx(1);
        zmq::socket_t sub(ctx, zmq::socket_type::sub);
        sub.set(zmq::sockopt::subscribe, "REGIME");
        sub.set(zmq::sockopt::rcvtimeo, 100); // 100ms poll timeout
        sub.set(zmq::sockopt::linger, 0);

        std::string endpoint = "tcp://" + host + ":" + std::to_string(port);
        sub.connect(endpoint);

        std::cout << "[SSA] HMM subscriber connected to " << endpoint << std::endl;

        while (running_.load(std::memory_order_acquire)) {
            zmq::message_t msg;
            auto result = sub.recv(msg, zmq::recv_flags::none);
            if (!result.has_value()) continue;

            parse_regime_msg(static_cast<const char*>(msg.data()), msg.size());
        }
    }

    // Expected format: "REGIME|r,p0,p1,p2"
    void parse_regime_msg(const char* data, size_t len) {
        // Skip "REGIME|" prefix (7 chars)
        if (len < 8) return;
        const char* p = data + 7;
        char* end;

        int r = static_cast<int>(std::strtol(p, &end, 10));
        if (*end != ',') return; p = end + 1;

        double p0 = std::strtod(p, &end);
        if (*end != ',') return; p = end + 1;

        double p1 = std::strtod(p, &end);
        if (*end != ',') return; p = end + 1;

        double p2 = std::strtod(p, &end);

        // HMM states: 0=calm, 1=trend, 2=crisis (convention from the HMM engine)
        p_calm_.store(p0, std::memory_order_relaxed);
        p_trend_.store(p1, std::memory_order_relaxed);
        p_crisis_.store(p2, std::memory_order_relaxed);
        regime_.store(r, std::memory_order_relaxed);
    }
};

// ============================================================================
// Main Engine
// ============================================================================

int main() {
    std::cout << "[SSA] C++ SSA Engine Starting..." << std::endl;

    // --- Environment configuration ---
    auto env_or = [](const char* name, const char* def) -> std::string {
        const char* v = std::getenv(name);
        return (v && v[0]) ? v : def;
    };

    const std::string ch_host     = env_or("CLICKHOUSE_HOST", "clickhouse");
    const int         ch_port     = std::stoi(env_or("CLICKHOUSE_PORT", "8123"));
    const std::string ch_user     = env_or("CLICKHOUSE_USER", "default");
    const std::string ch_password = env_or("CLICKHOUSE_PASSWORD", "");
    const std::string hmm_host    = env_or("HMM_ZMQ_HOST", "cpp-hmm");
    const int         hmm_port    = std::stoi(env_or("HMM_ZMQ_PORT", "5555"));
    const std::string ssa_port    = env_or("SSA_ZMQ_PORT", "5558");
    const int         poll_ms     = std::stoi(env_or("SSA_POLL_INTERVAL_MS", "50"));
    const int         warmup_rows = std::stoi(env_or("SSA_WARMUP_ROWS", "500"));

    // --- ZeroMQ PUB socket for SSA output ---
    zmq::context_t zmq_ctx(1);
    zmq::socket_t zmq_pub(zmq_ctx, zmq::socket_type::pub);
    zmq_pub.set(zmq::sockopt::sndhwm, 10000);
    zmq_pub.set(zmq::sockopt::linger, 0);
    zmq_pub.bind("tcp://*:" + ssa_port);
    std::cout << "[SSA] ZMQ PUB bound on tcp://*:" << ssa_port << std::endl;

    // --- Start HMM subscriber (background thread) ---
    HmmSubscriber hmm_sub;
    hmm_sub.start(hmm_host, hmm_port);

    // --- Circular price buffer (pre-allocated, no runtime allocs) ---
    CircularPriceBuffer price_buffer;

    // --- ClickHouse reader with retry ---
    ClickHouseReader ch_reader(ch_host, ch_port, ch_user, ch_password);

    std::cout << "[SSA] Waiting for ClickHouse at " << ch_host << ":" << ch_port << "..." << std::endl;
    std::vector<ClickHouseReader::TickRow> history;
    history.reserve(warmup_rows);
    int retries = 30;
    while (retries > 0) {
        if (ch_reader.fetch_history(history, warmup_rows) && !history.empty()) break;
        --retries;
        std::cerr << "[SSA] ClickHouse not ready, retries left: " << retries << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    if (history.empty()) {
        std::cerr << "[SSA] No historical data after retries. Starting cold." << std::endl;
    } else {
        std::cout << "[SSA] Warming up with " << history.size() << " historical ticks..." << std::endl;
        for (const auto& row : history) {
            price_buffer.push(row.price);
        }
        std::cout << "[SSA] Warm-up complete. Buffer size=" << price_buffer.size() << std::endl;
    }
    history.clear();
    history.shrink_to_fit();

    // --- Initialize dual SSA pipeline ---
    // Default window lengths (Phase 4 AdaptiveL will override these dynamically)
    static constexpr int DEFAULT_L_SLOW = 61;
    static constexpr int DEFAULT_L_FAST = 25;   // ~rho(0.4) * L_slow
    static constexpr int SSA_RANK       = 2;

    PipelineController pipeline;
    pipeline.configure(&price_buffer, DEFAULT_L_FAST, DEFAULT_L_SLOW, SSA_RANK);

    // Adaptive window controller (initialized at default L_slow)
    AdaptiveL adaptive_l(DEFAULT_L_SLOW);

    if (price_buffer.size() >= 2 * DEFAULT_L_SLOW) {
        if (pipeline.warm_up()) {
            std::cout << "[SSA] Pipeline warm-up complete. L_fast="
                      << pipeline.L_fast() << " L_slow=" << pipeline.L_slow() << std::endl;
        } else {
            std::cerr << "[SSA] Pipeline warm-up failed. Will retry on sufficient data." << std::endl;
        }
    } else {
        std::cout << "[SSA] Insufficient data for pipeline warm-up (" << price_buffer.size()
                  << " < " << 2 * DEFAULT_L_SLOW << "). Will warm up when ready." << std::endl;
    }

    // --- Pre-allocate batch vector (no reallocation in hot loop) ---
    std::vector<ClickHouseReader::TickRow> batch;
    batch.reserve(5000);

    // --- Pre-allocate output frame ---
    SsaFrame frame;
    frame.magic = SSA_FRAME_MAGIC;

    // --- Main polling loop ---
    std::cout << "[SSA] Entering live polling loop (interval=" << poll_ms << "ms)..." << std::endl;
    uint64_t total_ticks = 0;
    uint64_t total_published = 0;

    while (true) {
        batch.clear();
        if (!ch_reader.poll_new(batch) || batch.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
            continue;
        }

        for (const auto& tick : batch) {
            price_buffer.push(tick.price);
            ++total_ticks;

            // Attempt deferred warm-up if pipeline not yet ready
            if (!pipeline.is_warmed()) {
                if (price_buffer.size() >= 2 * DEFAULT_L_SLOW) {
                    pipeline.warm_up();
                }
                // Publish raw price while pipeline warms up
                frame.timestamp_ns = tick.timestamp_ms * 1000000ULL;
                frame.ssa_smoothed = tick.price;
                frame.ssa_slope    = 0.0;
                frame.ssa_accel    = 0.0;
                frame.ssa_sideway  = 0.0;
                frame.L_fast       = DEFAULT_L_FAST;
                frame.L_slow       = DEFAULT_L_SLOW;
                frame.blend_weight = 0.5f;
                frame.evr_fast     = 0.0f;
                frame.evr_slow     = 0.0f;
                frame.eigen_gap    = 0.0f;

                zmq::message_t msg(&frame, SSA_FRAME_SIZE);
                zmq_pub.send(msg, zmq::send_flags::dontwait);
                ++total_published;
                continue;
            }

            // Read HMM regime probabilities (relaxed atomic — no lock)
            const double p_calm   = hmm_sub.p_calm();
            const double p_trend  = hmm_sub.p_trend();
            const double p_crisis = hmm_sub.p_crisis();

            // Adaptive L(t): EMA + hysteresis → stable window lengths
            AdaptiveLResult lr = adaptive_l.update(p_calm, p_trend, p_crisis);
            if (lr.changed) {
                pipeline.resize(lr.L_fast, lr.L_slow);
            }

            // Execute dual SSA pipeline (hot path — target < 5 µs)
            PipelineOutput pout = pipeline.step();

            if (pout.valid) {
                // Blend fast + slow signals with regime-aware weighting
                BlendedOutput blended = SoftBlender::blend(
                    pout.fast, pout.slow, p_calm, p_trend, p_crisis);

                frame.timestamp_ns = tick.timestamp_ms * 1000000ULL;
                frame.ssa_smoothed = blended.smoothed;
                frame.ssa_slope    = blended.slope;
                frame.ssa_accel    = blended.accel;
                frame.ssa_sideway  = blended.sideway;
                frame.L_fast       = pipeline.L_fast();
                frame.L_slow       = pipeline.L_slow();
                frame.blend_weight = blended.blend_weight;
                frame.evr_fast     = blended.evr_fast;
                frame.evr_slow     = blended.evr_slow;
                frame.eigen_gap    = blended.eigen_gap;
            } else {
                // Pipeline returned invalid (should not happen after warm-up)
                frame.timestamp_ns = tick.timestamp_ms * 1000000ULL;
                frame.ssa_smoothed = tick.price;
                frame.ssa_slope    = 0.0;
                frame.ssa_accel    = 0.0;
                frame.ssa_sideway  = 0.0;
                frame.L_fast       = pipeline.L_fast();
                frame.L_slow       = pipeline.L_slow();
                frame.blend_weight = 0.5f;
                frame.evr_fast     = 0.0f;
                frame.evr_slow     = 0.0f;
                frame.eigen_gap    = 0.0f;
            }

            zmq::message_t msg(&frame, SSA_FRAME_SIZE);
            zmq_pub.send(msg, zmq::send_flags::dontwait);
            ++total_published;

            if (total_published % 5000 == 0) {
                std::cout << "[SSA] Published " << total_published
                          << " frames | Buffer=" << price_buffer.size()
                          << " | Regime=" << hmm_sub.regime()
                          << " P(calm)=" << p_calm
                          << " P(trend)=" << p_trend
                          << " P(crisis)=" << p_crisis
                          << " | w=" << frame.blend_weight
                          << " EVR_f=" << frame.evr_fast
                          << " EVR_s=" << frame.evr_slow
                          << " | Smoothed=" << frame.ssa_smoothed
                          << " Slope=" << frame.ssa_slope << std::endl;
            }
        }
    }

    hmm_sub.stop();
    return 0;
}
