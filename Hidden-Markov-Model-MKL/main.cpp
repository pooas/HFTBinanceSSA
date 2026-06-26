/**
 * HFT Live Engine: Binance WSS -> 1s Realized Variance -> HMM -> ZeroMQ
 * Architecture strictly follows "HMM-driven Volatility-Regime Framework"
 */

 #include <iostream>
 #include <vector>
 #include <string>
 #include <thread>
 #include <mutex>
 #include <chrono>
 #include <cmath>
 #include <atomic>
 
 #define ASIO_STANDALONE
 #define _WEBSOCKETPP_CPP11_STRICT_
 
 #include <websocketpp/config/asio_client.hpp>
 #include <websocketpp/client.hpp>
 #include <nlohmann/json.hpp>
 #include <zmq.hpp>
 #include "hmm_mkl.hpp"
 
 using namespace eemd;
 using json = nlohmann::json;
 
 typedef websocketpp::client<websocketpp::config::asio_tls_client> wss_client;
 typedef websocketpp::lib::shared_ptr<asio::ssl::context> context_ptr;
 
 std::vector<double> rv_history; 
 std::mutex rv_mutex;
 
 std::atomic<int> current_hmm_regime(0);     
 std::atomic<bool> is_model_trained(false);
 std::atomic<double> prob_state[3] = {1.0, 0.0, 0.0}; // 0: Calm, 1: Trend, 2: Crisis
 
 HMMResult current_model;
 
 void initialize_warm_start_model() {
     current_model.mu = {-27.0, -21.0, -18.0};    // Volatility levels (LogRV)
     current_model.sigma = {0.05, 1.0, 3.5};      
     current_model.trans = {
         {0.95, 0.04, 0.01}, 
         {0.05, 0.90, 0.05}, 
         {0.02, 0.08, 0.90}  
     };
     current_model.pi = {0.8, 0.15, 0.05}; 
     is_model_trained = true;
     std::cout << "[WARM START] Loaded default Crypto Volatility priors based on Paper standards." << std::endl;
 }
 
 void hmm_trainer_thread() {
     while (true) {
         std::this_thread::sleep_for(std::chrono::seconds(10));
 
         std::vector<double> local_rv;
         {
             std::lock_guard<std::mutex> lock(rv_mutex);
             local_rv = rv_history;
         }
 
         if (local_rv.size() < 300) continue;
 
         std::cout << "\n[TRAINER] Starting Baum-Welch EM calibration on " << local_rv.size() << " samples..." << std::endl;
 
         HMMConfig config;
         config.n_states = 3;            
         config.max_iter = 100;
         config.verbose = false;         
         config.n_threads = 0;           
         config.min_variance = 1e-4;     
         
         GaussianHMM hmm(config);
         HMMResult result;
 
         auto start = std::chrono::high_resolution_clock::now();
         bool success = hmm.fit(local_rv.data(), local_rv.size(), result);
         auto end = std::chrono::high_resolution_clock::now();
         
         if (success) {
             sort_hmm_states_by_volatility(result);
 
             {
                 std::lock_guard<std::mutex> lock(rv_mutex);
                 current_model = result; 
                 is_model_trained = true;
             }
 
             double ms = std::chrono::duration<double, std::milli>(end - start).count();
             std::cout << "[TRAINER] Calibration Success in " << ms << " ms." << std::endl;
             std::cout << "  -> Calm(0)  LogVar: " << result.mu[0] << std::endl;
             std::cout << "  -> Trend(1) LogVar: " << result.mu[1] << std::endl;
             std::cout << "  -> Crisis(2)LogVar: " << result.mu[2] << std::endl;
             
             std::this_thread::sleep_for(std::chrono::minutes(5));
         } else {
             std::cout << "[TRAINER] 🔴 Calibration Failed! Retrying in 10s..." << std::endl;
         }
     }
 }
 
 // 🌟 Real-time State Filter (Forward Bayesian Update with Sticky HMM)
 void decode_current_regime(double latest_log_rv) {
     if (!is_model_trained) return;
     std::lock_guard<std::mutex> lock(rv_mutex);
     
     double log_emission[3];
     double max_log_e = -1e9;
     for (int k = 0; k < 3; k++) {
         double mu = current_model.mu[k];
         double sig = current_model.sigma[k];
         double z = (latest_log_rv - mu) / sig;
         log_emission[k] = -0.5 * z * z - std::log(sig);
         if (log_emission[k] > max_log_e) max_log_e = log_emission[k];
     }
 
     double p_emission[3];
     for (int k=0; k<3; k++) p_emission[k] = std::exp(log_emission[k] - max_log_e);
 
     // ========================================================================
     // 🌟 راه‌حل دوم: اجبار به چسبندگی (Sticky HMM Transition Regularization)
     // ایجاد یک ماتریس انتقال که شانس ماندگاری در رژیم فعلی را روی ۹۹٪ قفل می‌کند
     // ========================================================================
     double sticky_trans[3][3];
     for(int i=0; i<3; i++) {
         double off_diagonal_sum = 0.0;
         for(int j=0; j<3; j++) {
             if(i != j) off_diagonal_sum += current_model.trans[i][j];
         }
         for(int j=0; j<3; j++) {
             if (i == j) {
                 sticky_trans[i][j] = 0.99; // 99% ماندگاری در رژیم فعلی
             } else {
                 if (off_diagonal_sum > 0) {
                     sticky_trans[i][j] = 0.01 * (current_model.trans[i][j] / off_diagonal_sum);
                 } else {
                     sticky_trans[i][j] = 0.005;
                 }
             }
         }
     }
 
     double alpha_new[3];
     double sum_alpha = 0.0;
     for (int j = 0; j < 3; j++) {
         double prior_j = 0.0;
         for (int i = 0; i < 3; i++) {
             // استفاده از ماتریس چسبنده به جای ماتریس خام
             prior_j += sticky_trans[i][j] * prob_state[i].load();
         }
         alpha_new[j] = p_emission[j] * prior_j;
         sum_alpha += alpha_new[j];
     }
 
     if (sum_alpha < 1e-12) {
         for(int j=0; j<3; j++) alpha_new[j] = p_emission[j];
         sum_alpha = p_emission[0] + p_emission[1] + p_emission[2];
     }
 
     int best_k = 0;
     double max_p = -1.0;
     for(int j=0; j<3; j++) {
         double final_p = alpha_new[j] / sum_alpha;
         prob_state[j].store(final_p); 
         if(final_p > max_p) {
             max_p = final_p;
             best_k = j;
         }
     }
     current_hmm_regime.store(best_k);
 }
 
 int main() {
     std::cout << "🚀 HFT Volatility-Regime Engine Starting..." << std::endl;
 
     initialize_warm_start_model();
 
     zmq::context_t zmq_ctx(1);
     zmq::socket_t zmq_pub(zmq_ctx, zmq::socket_type::pub);
     
     std::string zmq_port = "5555";
     if (const char* env_p = std::getenv("ZMQ_PORT")) zmq_port = env_p;
     zmq_pub.bind("tcp://*:" + zmq_port);
 
     std::thread trainer(hmm_trainer_thread);
     trainer.detach();
 
     wss_client c;
     c.init_asio();
     c.set_access_channels(websocketpp::log::alevel::none); 
     c.clear_access_channels(websocketpp::log::alevel::all);
 
     c.set_tls_init_handler([](websocketpp::connection_hdl) {
         auto ctx = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::tlsv12);
         ctx->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3);
         return ctx;
     });
 
     long last_sec = 0;
     double current_sec_rv = 0.0;
     double last_price = -1.0;
     long tick_count = 0;
     
     // متغیرهای مربوط به راه‌حل اول (هموارسازی)
     double current_ema_log_rv = 0.0;
     bool is_ema_initialized = false;
 
     c.set_message_handler([&](websocketpp::connection_hdl hdl, wss_client::message_ptr msg) {
         try {
             auto j = json::parse(msg->get_payload());
             if (!j.contains("p")) return;
 
             double p = std::stod(j["p"].get<std::string>());
             long ts = j["T"].get<long>();
             long current_sec = ts / 1000;
 
             if (last_sec == 0) last_sec = current_sec;
 
             if (last_price > 0) {
                 double log_ret = std::log(p / last_price);
                 current_sec_rv += (log_ret * log_ret);
             }
             last_price = p;
 
             if (current_sec > last_sec) {
                 double log_rv = std::log(current_sec_rv + 1e-12);
 
                 // ========================================================================
                 // 🌟 راه‌حل اول: هموارسازی ورودی‌ها (Smoothed RV / Input Feature Engineering)
                 // خنثی‌سازی اسپایک‌های فریب‌دهنده‌ی ۱ ثانیه‌ای
                 // ========================================================================
                 if (!is_ema_initialized) {
                     current_ema_log_rv = log_rv;
                     is_ema_initialized = true;
                 } else {
                     current_ema_log_rv = 0.1 * log_rv + 0.9 * current_ema_log_rv;
                 }
 
                 {
                     std::lock_guard<std::mutex> lock(rv_mutex);
                     // مدل روی نوسانات هموار شده (واقعی) آموزش می‌بیند
                     rv_history.push_back(current_ema_log_rv);
                     if (rv_history.size() > 14400) rv_history.erase(rv_history.begin());
                 }
 
                 // ارسال دیتای هموار شده برای تشخیص رژیم
                 decode_current_regime(current_ema_log_rv);
                 int active_regime = current_hmm_regime.load();
 
                 // 🌟 Single-Frame Protocol
                 std::string payload_str = "REGIME|" + std::to_string(active_regime) + "," +
                                           std::to_string(prob_state[0].load()) + "," +
                                           std::to_string(prob_state[1].load()) + "," +
                                           std::to_string(prob_state[2].load());
 
                 zmq::message_t payload(payload_str.data(), payload_str.size());
                 zmq_pub.send(payload, zmq::send_flags::none);
 
                 tick_count++;
                 if (tick_count % 10 == 0) {
                     std::string status_tag = (rv_history.size() < 300) ? "(PRIOR)" : "(LIVE)";
                     std::cout << "[LIVE] Sec: " << current_sec << " | RawLogRV: " << log_rv 
                               << " | EmaLogRV: " << current_ema_log_rv
                               << " | Regime: " << active_regime << " " << status_tag
                               << " | TrendProb: " << (prob_state[1].load() * 100.0) << "%" << std::endl;
                 }
 
                 current_sec_rv = 0.0;
                 last_sec = current_sec;
             }
         } catch (...) {}
     });
 
     c.set_close_handler([&](websocketpp::connection_hdl hdl) {
         std::cout << "🔴 WebSocket Closed!" << std::endl;
     });
 
     std::string uri = "wss://stream.binance.com:9443/ws/btcusdt@aggTrade";
     websocketpp::lib::error_code ec;
     wss_client::connection_ptr con = c.get_connection(uri, ec);
     c.connect(con);
     std::cout << "🌐 Connecting to Binance..." << std::endl;
     c.run(); 
 
     return 0;
 }