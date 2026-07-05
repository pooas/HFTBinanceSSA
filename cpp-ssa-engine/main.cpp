/**
 * Phase-2 DSP Engine — ClickHouse Tick Reader → Phase-1 dsp_math / dsp_pipeline
 *                     → ZeroMQ Binary Publisher
 *
 * Pipeline:
 *   1. Bootstrap: fetch last N ticks from ClickHouse HTTP API to warm FilterPipeline
 *   2. Poll loop: query ClickHouse for new ticks since last_sequence_
 *   3. For each new tick: FilterPipeline::step(price) → O(1) trend update
 *   4. Publish 68-byte SsaFrame binary via ZMQ PUB :5558
 *
 * Phase-3 wiring (sensor fusion):
 *   • This engine emits dsp_macro_trend   on port 5558
 *   • The existing cpp-sg-dsp engine emits sg_slope on port 5557
 *   • The Java consumer (Phase-3) fuses them: zero_lag_trend = macro_trend + K·sg_slope
 *
 * Structurally mirrors cpp-sg-dsp/main.cpp — same env-var configuration,
 * same ClickHouse retry logic, identical ZMQ send pattern. Compiles
 * identically in the consolidated Dockerfile.cpp-consolidated builder stage.
 *
 * Hot-path: zero heap allocations, zero std::cout spam (5 000-tick cadence).
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
#include "dsp_pipeline.hpp"

// ============================================================================
// CURL response accumulator — tiny owning buffer for libcurl writes
// ============================================================================

struct CurlBuffer {
    std::string data;
};

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* buf = static_cast<CurlBuffer*>(userdata);
    buf->data.append(static_cast<char*>(ptr), total);
    return total;
}

// ============================================================================
// ClickHouseReader — polling client for hft_market_data
//
// We deliberately read three columns only (sequence, timestamp_ms, price):
// Phase-2 FilterPipeline owns its own noise/cycle math — no need to depend on
// dom_cycle / vress / hmm_prob_crisis columns previously written by the
// Java app for the SG-DSP node. Self-contained cold-start is one less
// chicken-and-egg dependency at boot.
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
        const std::string query =
            "SELECT sequence, toUnixTimestamp64Milli(timestamp), price "
            "FROM hft_market_data "
            "ORDER BY sequence DESC LIMIT " + std::to_string(limit) +
            " FORMAT TabSeparated";

        std::string response;
        if (!execute_query(query, response)) return false;

        parse_rows(response, out);
        std::reverse(out.begin(), out.end());

        if (!out.empty()) last_sequence_ = out.back().sequence;
        return true;
    }

    bool poll_new(std::vector<TickRow>& out) {
        const std::string query =
            "SELECT sequence, toUnixTimestamp64Milli(timestamp), price "
            "FROM hft_market_data "
            "WHERE sequence > " + std::to_string(last_sequence_) +
            " ORDER BY sequence ASC LIMIT 5000"
            " FORMAT TabSeparated";

        std::string response;
        if (!execute_query(query, response)) return false;

        parse_rows(response, out);

        if (!out.empty()) last_sequence_ = out.back().sequence;
        return true;
    }

    uint64_t last_sequence() const { return last_sequence_; }

private:
    std::string base_url_;
    std::string user_;
    std::string password_;
    uint64_t    last_sequence_;

    bool execute_query(const std::string& query, std::string& response) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        CurlBuffer buf;
        const std::string url = base_url_ + "?user=" + user_ + "&password=" + password_;

        curl_easy_setopt(curl, CURLOPT_URL,           url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    query.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(query.size()));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA,    &buf);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT,       10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

        const CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) return false;

        response = std::move(buf.data);
        return true;
    }

    static void parse_rows(const std::string& tsv, std::vector<TickRow>& out) {
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
        row.price        = std::strtod(p,    &end);

        return true;
    }
};

// ============================================================================
// Main engine
// ============================================================================
int main() {
    std::cout << "[DSP] Phase-2 Ehlers DSP Engine Starting..." << std::endl;

    // --- Environment configuration ---
    auto env_or = [](const char* name, const char* def) -> std::string {
        const char* v = std::getenv(name);
        return (v && v[0]) ? v : def;
    };

    const std::string ch_host     = env_or("CLICKHOUSE_HOST",     "clickhouse");
    const int         ch_port     = std::stoi(env_or("CLICKHOUSE_PORT",     "8123"));
    const std::string ch_user     = env_or("CLICKHOUSE_USER",      "default");
    const std::string ch_password = env_or("CLICKHOUSE_PASSWORD", "");
    const std::string ssa_port    = env_or("SSA_ZMQ_PORT",         "5558");
    const int         poll_ms     = std::stoi(env_or("SSA_POLL_INTERVAL_MS", "50"));
    const int         warmup_rows = std::stoi(env_or("SSA_WARMUP_ROWS",      "500"));

    const double lower_bound = std::stod(env_or("SSA_MEE_LOWER",   "8.0"));
    const double upper_bound = std::stod(env_or("SSA_MEE_UPPER", "330.0"));

    // --- ZeroMQ PUB socket ---
    zmq::context_t zmq_ctx(1);
    zmq::socket_t  zmq_pub(zmq_ctx, zmq::socket_type::pub);
    zmq_pub.set(zmq::sockopt::sndhwm, 10000);
    zmq_pub.set(zmq::sockopt::linger, 0);
    zmq_pub.bind("tcp://*:" + ssa_port);
    std::cout << "[DSP] ZMQ PUB bound on tcp://*:" << ssa_port << std::endl;

    // --- FilterPipeline (Phase-1 math wired together in Phase-2) ---
    dsp::FilterPipeline pipeline(lower_bound, upper_bound);

    // --- ClickHouse reader with retry ---
    ClickHouseReader ch_reader(ch_host, ch_port, ch_user, ch_password);

    std::cout << "[DSP] Waiting for ClickHouse at " << ch_host << ":" << ch_port
              << "..." << std::endl;

    std::vector<ClickHouseReader::TickRow> history;
    history.reserve(warmup_rows);

    int retries = 30;
    while (retries > 0) {
        if (ch_reader.fetch_history(history, warmup_rows) && !history.empty()) break;
        --retries;
        std::cerr << "[DSP] ClickHouse not ready, retries left: " << retries << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    if (history.empty()) {
        std::cerr << "[DSP] No historical data after retries. Starting cold." << std::endl;
    } else {
        std::cout << "[DSP] Warming up with " << history.size() << " historical ticks..." << std::endl;
        for (const auto& row : history) {
            // FilterPipeline::step handles its own warm-up bookkeeping.
            pipeline.step(row.price);
        }
        std::cout << "[DSP] Warm-up complete. dom_cycle=" << pipeline.dom_cycle()
                  << " micro_period=" << pipeline.fast_period()
                  << " macro_period=" << pipeline.slow_period()
                  << " warmed=" << (pipeline.is_warmed() ? "yes" : "no")
                  << std::endl;
    }
    history.clear();
    history.shrink_to_fit();

    // --- Pre-allocate batch + frame (no allocation in the hot loop) ---
    std::vector<ClickHouseReader::TickRow> batch;
    batch.reserve(5000);

    SsaFrame frame;
    frame.magic = SSA_FRAME_MAGIC;

    std::cout << "[DSP] Entering live polling loop (interval=" << poll_ms << "ms)..." << std::endl;
    uint64_t total_ticks      = 0;
    uint64_t total_published  = 0;

    // ========================================================================
    // Hot polling loop — zero heap allocations, zero stdout spam.
    // ========================================================================
    while (true) {
        batch.clear();
        if (!ch_reader.poll_new(batch) || batch.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
            continue;
        }

        for (const auto& tick : batch) {
            ++total_ticks;
            const dsp::PipelineResult r = pipeline.step(tick.price);

            // --- Pack binary frame ---
            frame.timestamp_ns     = tick.timestamp_ms * 1000000ULL;
            frame.ssa_smoothed     = r.micro_trend;       // wire-compatible alias
            frame.ssa_slope        = r.slope;
            frame.ssa_accel        = r.accel;
            frame.ssa_macro_trend  = r.macro_trend;
            frame.L_fast           = pipeline.fast_period();
            frame.L_slow           = pipeline.slow_period();
            frame.blend_weight     = 0.0f;                // reserved (Phase-1 legacy)
            frame.evr_fast         = 0.0f;                // reserved
            frame.evr_slow         = static_cast<float>(r.spectral_conc); // diagnostic
            frame.eigen_gap        = 0.0f;                 // reserved

            zmq::message_t msg(&frame, SSA_FRAME_SIZE);
            zmq_pub.send(msg, zmq::send_flags::dontwait);
            ++total_published;

            // --- Throttled diagnostic log (every 5 000 ticks) ---
            if (total_published % 5000 == 0) {
                std::cout << "[DSP] pub=" << total_published
                          << " | dom_cycle=" << r.dom_cycle
                          << " | micro="     << r.micro_trend
                          << " | macro="     << r.macro_trend
                          << " | slope="     << r.slope
                          << " | spec_conc=" << r.spectral_conc
                          << std::endl;
            }
        }
    }

    return 0;
}