**
 * HFT Live Engine: Binance WSS -> 1s Realized Variance -> HMM -> ZeroMQ
 * * Architecture:
 * - Thread 1: WebSocket Client (Receives ticks, computes 1s RV, Decodes Regime)
 * - Thread 2: HMM Trainer (Runs Baum-Welch EM in background when enough data is gathered)
 * - ZeroMQ: Publishes "REGIME <0|1|2>" to Port 5555 for Java execution.
 */

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>
#include <atomic>

// WebSocket & JSON
#include <websocketpp/config/asio_client.hpp>
#include <websocketpp/client.hpp>
#include <nlohmann/json.hpp>

// ZeroMQ for IPC
#include <zmq.hpp>

// HMM MKL Engine
#include "hmm_mkl.hpp"

using namespace eemd;
using json = nlohmann::json;

typedef websocketpp::client<websocketpp::config::asio_tls_client> wss_client;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context> context_ptr;

// ============================================================================
// Global States & Memory
// ============================================================================

std::vector<double> rv_history; // History of 1-second Log-Realized Variances
std::mutex rv_mutex;

std::atomic<int> current_hmm_regime(0);     // 0=Calm, 1=Trend, 2=Crisis
std::atomic<bool> is_model_trained(false);

HMMResult current_model;

// ============================================================================
// 🌟 Warm Start Initialization (Cold-Start Fix)
// ============================================================================
// این تابع پارامترهای پیش‌فرض کریپتو را در لحظه استارت در سیستم بارگذاری می‌کند
// تا ربات در 5 دقیقه اول (قبل از جمع‌آوری دیتای لایو) کور نباشد.
void initialize_warm_start_model() {
    current_model.mu = {-17.0, -14.0, -11.0};    // تخمین Log-RV برای: آرام، ترند، بحران
    current_model.sigma = {1.0, 1.0, 1.5};       // واریانسِ هر رژیم
    
    current_model.trans = {
        {0.95, 0.04, 0.01}, // Calm transitions
        {0.05, 0.90, 0.05}, // Trend transitions
        {0.02, 0.08, 0.90}  // Crisis transitions
    };
    
    current_model.pi = {0.8, 0.15, 0.05}; // احتمال حضور در شروع
    
    is_model_trained = true;
    std::cout << "[WARM START] Loaded default Crypto Volatility priors. Bot is live immediately!" << std::endl;
}

// ============================================================================
// Background Thread: HMM Trainer
// ============================================================================

void hmm_trainer_thread() {
    while (true) {
        // به جای 5 دقیقه خواب ممتد، هر 10 ثانیه چک می‌کنیم 
        // تا به محض رسیدن دیتا به حد نصاب (300 ثانیه)، آموزش را استارت بزنیم
        std::this_thread::sleep_for(std::chrono::seconds(10));

        std::vector<double> local_rv;
        {
            std::lock_guard<std::mutex> lock(rv_mutex);
            local_rv = rv_history;
        }

        // We need at least 300 points (5 minutes) to train meaningfully
        if (local_rv.size() < 300) {
            continue;
        }

        // We also want to train every ~5 minutes, not every 10 seconds!
        // So after a successful train, we will sleep for 5 minutes.
        std::cout << "\n[TRAINER] Starting Baum-Welch EM calibration on " << local_rv.size() << " samples..." << std::endl;

        HMMConfig config;
        config.n_states = 3;            // 3 Regimes: Calm, Trend, Crisis
        config.max_iter = 100;
        config.verbose = false;         
        config.n_threads = 0;           // Use all available P-Cores
        config.min_variance = 1e-4;     // جلوگیری از خطای ریاضی در توزیع‌های بسته
        
        GaussianHMM hmm(config);
        HMMResult result;

        auto start = std::chrono::high_resolution_clock::now();
        bool success = hmm.fit(local_rv.data(), local_rv.size(), result);
        auto end = std::chrono::high_resolution_clock::now();
        
        if (success) {
            // Sort states by Volatility (sigma) 
            // Ensures State 0 = Calm, State 1 = Trend, State 2 = Crisis
            sort_hmm_states_by_volatility(result);

            {
                std::lock_guard<std::mutex> lock(rv_mutex);
                current_model = result; // جایگزینیِ مدل پیش‌فرض با مدل لایو
                is_model_trained = true;
            }

            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            std::cout << "[TRAINER] Calibration Success in " << ms << " ms." << std::endl;
            std::cout << "  -> Calm(0)  mu: " << result.mu[0] << ", sig: " << result.sigma[0] << std::endl;
            std::cout << "  -> Trend(1) mu: " << result.mu[1] << ", sig: " << result.sigma[1] << std::endl;
            std::cout << "  -> Crisis(2)mu: " << result.mu[2] << ", sig: " << result.sigma[2] << std::endl;
            
            // خواب 5 دقیقه‌ای بعد از یک آموزش موفق
            std::this_thread::sleep_for(std::chrono::minutes(5));
        } else {
            std::cout << "[TRAINER] 🔴 Calibration Failed! Retrying in 10s..." << std::endl;
        }
    }
}

// ============================================================================
// Ultra-Low Latency Inference
// ============================================================================

void decode_current_regime(double latest_log_rv) {
    if (!is_model_trained) return;

    std::lock_guard<std::mutex> lock(rv_mutex);
    
    // O(1) Fast Likelihood Estimation:
    // Which Gaussian state best explains the latest log_rv?
    double max_log_p = -1e9;
    int best_k = current_hmm_regime.load();

    for (int k = 0; k < 3; k++) {
        double mu = current_model.mu[k];
        double sig = current_model.sigma[k];
        
        // Log of Gaussian PDF: -0.5 * z^2 - log(sigma)
        double z = (latest_log_rv - mu) / sig;
        double log_p = -0.5 * z * z - std::log(sig);
        
        if (log_p > max_log_p) {
            max_log_p = log_p;
            best_k = k;
        }
    }

    current_hmm_regime.store(best_k);
}

// ============================================================================
// Main WebSocket & ZMQ Loop
// ============================================================================

int main() {
    std::cout << "🚀 HFT C++ Engine (HMM-MKL + ZMQ) Starting..." << std::endl;

    // 🌟 0. اجرای سیستم Warm Start برای حل مشکل استارت سرد
    initialize_warm_start_model();

    // 1. Setup ZeroMQ Publisher
    zmq::context_t zmq_ctx(1);
    zmq::socket_t zmq_pub(zmq_ctx, zmq::socket_type::pub);
    
    std::string zmq_port = "5555";
    if (const char* env_p = std::getenv("ZMQ_PORT")) {
        zmq_port = env_p;
    }
    zmq_pub.bind("tcp://*:" + zmq_port);
    std::cout << "📡 ZeroMQ Publisher bound to port " << zmq_port << std::endl;

    // 2. Start Background Trainer Thread
    std::thread trainer(hmm_trainer_thread);
    trainer.detach();

    // 3. Setup WebSocket Client
    wss_client c;
    c.init_asio();
    c.set_access_channels(websocketpp::log::alevel::none); // Disable verbose logging
    c.clear_access_channels(websocketpp::log::alevel::all);

    // TLS initialization for wss://
    c.set_tls_init_handler([](websocketpp::connection_hdl) {
        auto ctx = websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12);
        ctx->set_options(boost::asio::ssl::context::default_workarounds |
                         boost::asio::ssl::context::no_sslv2 |
                         boost::asio::ssl::context::no_sslv3 |
                         boost::asio::ssl::context::single_dh_use);
        return ctx;
    });

    // Trading State
    long last_sec = 0;
    double current_sec_rv = 0.0;
    double last_price = -1.0;
    long tick_count = 0;

    c.set_message_handler([&](websocketpp::connection_hdl hdl, wss_client::message_ptr msg) {
        try {
            auto j = json::parse(msg->get_payload());
            if (!j.contains("p")) return;

            double p = std::stod(j["p"].get<std::string>());
            long ts = j["T"].get<long>();
            long current_sec = ts / 1000;

            if (last_sec == 0) last_sec = current_sec;

            // Compute Log Return and add to current second's RV
            if (last_price > 0) {
                double log_ret = std::log(p / last_price);
                current_sec_rv += (log_ret * log_ret);
            }
            last_price = p;

            // When second changes, finalize RV and emit Regime
            if (current_sec > last_sec) {
                
                // If RV is exactly 0 (no movement), add epsilon to prevent -inf log
                double log_rv = std::log(current_sec_rv + 1e-12);

                {
                    std::lock_guard<std::mutex> lock(rv_mutex);
                    rv_history.push_back(log_rv);
                    // Keep maximum 4 hours of 1s RV history (14400 points)
                    if (rv_history.size() > 14400) {
                        rv_history.erase(rv_history.begin());
                    }
                }

                // Decode regime for this new second
                decode_current_regime(log_rv);
                int active_regime = current_hmm_regime.load();

                // Publish to ZeroMQ
                // Format Topic: "REGIME", Message: "1"
                zmq::message_t topic("REGIME", 6);
                zmq::message_t payload(std::to_string(active_regime).data(), 1);
                
                zmq_pub.send(topic, zmq::send_flags::sndmore);
                zmq_pub.send(payload, zmq::send_flags::none);

                // Terminal output every 10 seconds
                tick_count++;
                if (tick_count % 10 == 0) {
                    std::cout << "[LIVE] Sec: " << current_sec 
                              << " | LogRV: " << log_rv 
                              << " | Regime: " << active_regime 
                              << (rv_history.size() < 300 ? " (PRIOR)" : " (LIVE)") << std::endl;
                }

                // Reset for next second
                current_sec_rv = 0.0;
                last_sec = current_sec;
            }

        } catch (...) {
            // Ignore parse errors silently in hot path
        }
    });

    c.set_close_handler([&](websocketpp::connection_hdl hdl) {
        std::cout << "🔴 WebSocket Closed! You should implement auto-reconnect here." << std::endl;
    });

    // 4. Connect and Run
    std::string uri = "wss://stream.binance.com:9443/ws/btcusdt@aggTrade";
    websocketpp::lib::error_code ec;
    wss_client::connection_ptr con = c.get_connection(uri, ec);
    if (ec) {
        std::cout << "Connection Init Error: " << ec.message() << std::endl;
        return -1;
    }
    
    c.connect(con);
    std::cout << "🌐 Connecting to Binance " << uri << " ..." << std::endl;
    c.run(); // Blocking call

    return 0;
}