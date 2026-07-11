/**
 * HFT Live Engine: Hyperliquid REST (History) + WSS (Candles) -> Realized Variance -> HMM -> ZeroMQ
 * Upgraded to Professional K-Line Architecture with Zero Cold-Start Time
 */

 #include <iostream>
 #include <vector>
 #include <string>
 #include <thread>
 #include <mutex>
 #include <chrono>
 #include <cmath>
 #include <atomic>
 #include <cstdlib>
 #include <array>
 #include <memory>
 #include <stdexcept>
 
 #define ASIO_STANDALONE
 #define _WEBSOCKETPP_CPP11_STRICT_
 
 #define index index_
 #include <websocketpp/config/asio_client.hpp>
 #include <websocketpp/client.hpp>
 #undef index
 
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
 std::atomic<double> prob_state[3] = {1.0, 0.0, 0.0}; 
 std::atomic<double> live_last_close(-1.0); // نگهداری آخرین قیمت بسته شده
 
 HMMResult current_model;
 
 // =========================================================
 // 🌟 Hyperliquid JSON parsing helpers (string-or-number safe)
 // =========================================================
 static double parse_hl_price(const json& j) {
     if (j.is_string()) return std::stod(j.get<std::string>());
     return j.get<double>();
 }
 
 static long parse_hl_time(const json& j) {
     if (j.is_string()) return std::stol(j.get<std::string>());
     return j.get<long>();
 }
 
 std::string exec_cmd(const char* cmd) {
     std::array<char, 128> buffer;
     std::string result;
     std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
     if (!pipe) {
         throw std::runtime_error("popen() failed!");
     }
     while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
         result += buffer.data();
     }
     return result;
 }
 
 std::string get_interval(long seconds) {
     if (seconds == 60) return "1m";
     if (seconds == 300) return "5m";
     if (seconds == 900) return "15m";
     if (seconds == 3600) return "1h";
     return "1m"; // پیش‌فرض
 }
 
 void fetch_historical_klines(long timeframe_seconds) {
     std::string interval = get_interval(timeframe_seconds);
 
     long long end_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
     long long start_ms = end_ms - static_cast<long long>(500) * timeframe_seconds * 1000LL;
 
     std::string body = "{\"type\":\"candleSnapshot\",\"req\":{\"coin\":\"BTC\",\"interval\":\""
                        + interval + "\",\"startTime\":" + std::to_string(start_ms)
                        + ",\"endTime\":" + std::to_string(end_ms) + "}}";
     std::string cmd = "wget -qO- --header='Content-Type: application/json' --post-data='"
                       + body + "' 'https://api.hyperliquid-testnet.xyz/info'";
     std::cout << "[INIT] Fetching 500 historical candles (" << interval << ") from Hyperliquid REST API..." << std::endl;
     
     try {
         std::string json_str = exec_cmd(cmd.c_str());
         auto j = json::parse(json_str);
         
         if (!j.is_array()) {
             std::cerr << "[ERROR] Hyperliquid candleSnapshot returned non-array response" << std::endl;
             return;
         }
 
         double prev_close = -1.0;
         
         for (const auto& item : j) {
             double close_price = parse_hl_price(item["c"]);
             if (prev_close > 0.0) {
                 // محاسبه Squared Return (معادل LogRV در تایم فریم کندلی)
                 double log_ret = std::log(close_price / prev_close);
                 double log_rv = std::log(log_ret * log_ret + 1e-12);
                 rv_history.push_back(log_rv);
             }
             prev_close = close_price;
         }
         
         // آخرین قیمت بسته شده‌ی تاریخچه را برای محاسبات لایو ذخیره می‌کنیم
         live_last_close.store(prev_close);
         std::cout << "[INIT] Successfully loaded " << rv_history.size() << " historical RV samples. Last Close: " << prev_close << std::endl;
         
     } catch (const std::exception& e) {
         std::cerr << "[ERROR] Failed to fetch historical klines: " << e.what() << std::endl;
     }
 }
 
 // =========================================================
 
 void run_hmm_training(bool is_initial = false) {
     std::vector<double> local_rv;
     {
         std::lock_guard<std::mutex> lock(rv_mutex);
         local_rv = rv_history;
     }
 
     if (local_rv.size() < 60) return;
 
     if (!is_initial) {
         std::cout << "\n[TRAINER] Starting Background Baum-Welch EM calibration on " << local_rv.size() << " samples..." << std::endl;
     }
 
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
     } else {
         std::cout << "[TRAINER] 🔴 Calibration Failed!" << std::endl;
     }
 }
 
 void hmm_background_trainer_thread() {
     while (true) {
         // هر 15 دقیقه مدل را در پس‌زمینه دوباره آپدیت می‌کند
         std::this_thread::sleep_for(std::chrono::minutes(15));
         run_hmm_training(false);
     }
 }
 
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
 
     double alpha_new[3];
     double sum_alpha = 0.0;
     for (int j = 0; j < 3; j++) {
         double prior_j = 0.0;
         for (int i = 0; i < 3; i++) {
             prior_j += current_model.trans[i][j] * prob_state[i].load();
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
     std::cout << "🚀 HFT Kline-Based Volatility-Regime Engine Starting..." << std::endl;
 
     zmq::context_t zmq_ctx(1);
     zmq::socket_t zmq_pub(zmq_ctx, zmq::socket_type::pub);
     
     std::string zmq_port = "5555";
     if (const char* env_p = std::getenv("ZMQ_PORT")) zmq_port = env_p;
     zmq_pub.bind("tcp://*:" + zmq_port);
 
     long timeframe_seconds = 60;
     if (const char* env_tf = std::getenv("TIMEFRAME_SEC")) {
         timeframe_seconds = std::stol(env_tf);
     }
     std::cout << "[INIT] HMM Timeframe set to: " << timeframe_seconds << " seconds (" << get_interval(timeframe_seconds) << ")." << std::endl;
 
     // 🌟 دانلود تاریخچه 500 کندل قبلی
     fetch_historical_klines(timeframe_seconds);
 
     // 🌟 اجرای آموزش فوری قبل از شروع لایو
     std::cout << "[INIT] Running Initial HMM Calibration on Historical Data..." << std::endl;
     run_hmm_training(true);
 
     // راه‌اندازی ترد آپدیت بک‌گراند (هر ۱۵ دقیقه)
     std::thread trainer(hmm_background_trainer_thread);
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
 
     long tick_count = 0;
     long current_candle_t = 0;
     double current_candle_close = 0.0;
 
     std::string stream_interval = get_interval(timeframe_seconds);
 
     c.set_open_handler([&](websocketpp::connection_hdl hdl) {
         std::string sub = "{\"method\":\"subscribe\",\"subscription\":{\"type\":\"candle\",\"coin\":\"BTC\",\"interval\":\""
                           + stream_interval + "\"}}";
         websocketpp::lib::error_code ec;
         c.send(hdl, sub, websocketpp::frame::opcode::text, ec);
         if (!ec) {
             std::cout << "🌐 Subscribed to Hyperliquid candle stream (" << stream_interval << ")" << std::endl;
         } else {
             std::cerr << "🔴 Subscription send failed: " << ec.message() << std::endl;
         }
     });
 
     c.set_message_handler([&](websocketpp::connection_hdl hdl, wss_client::message_ptr msg) {
         try {
             auto j = json::parse(msg->get_payload());
             if (!j.contains("channel") || j["channel"] != "candle") return;
 
             auto k = j["data"];
             long t = parse_hl_time(k["t"]);
             double current_close = parse_hl_price(k["c"]);
 
             if (t == current_candle_t) {
                 // Hyperliquid streams partial candle updates; keep the latest close but do not
                 // finalize the HMM signal until the next candle starts.
                 current_candle_close = current_close;
                 tick_count++;
                 if (tick_count % 50 == 0) {
                     double ref_price = live_last_close.load();
                     double temp_log_rv = 0.0;
                     if (ref_price > 0.0) {
                         double log_ret = std::log(current_candle_close / ref_price);
                         temp_log_rv = std::log(log_ret * log_ret + 1e-12);
                     }
                     std::cout << "[LIVE|OPEN] Price: " << current_candle_close << " | Temp LogRV: " << temp_log_rv
                               << " | Waiting for candle close..." << std::endl;
                 }
             } else if (t > current_candle_t) {
                 // New candle open means the previous candle is finalized.
                 if (current_candle_t != 0) {
                     double ref_price = live_last_close.load();
                     if (ref_price > 0.0) {
                         double log_ret = std::log(current_candle_close / ref_price);
                         double log_rv = std::log(log_ret * log_ret + 1e-12);
 
                         decode_current_regime(log_rv);
                         int active_regime = current_hmm_regime.load();
 
                         // ارسال قطعی یک سیگنال معتبر و ثابت برای کل تایم‌فریم بعدی
                         std::string payload_str = "REGIME|" + std::to_string(active_regime) + "," +
                                                   std::to_string(prob_state[0].load()) + "," +
                                                   std::to_string(prob_state[1].load()) + "," +
                                                   std::to_string(prob_state[2].load());
 
                         zmq::message_t payload(payload_str.data(), payload_str.size());
                         zmq_pub.send(payload, zmq::send_flags::none);
 
                         // آپدیت تاریخچه مدل برای آموزش‌های بعدی
                         {
                             std::lock_guard<std::mutex> lock(rv_mutex);
                             rv_history.push_back(log_rv);
                             if (rv_history.size() > 14400) rv_history.erase(rv_history.begin());
                         }
                         live_last_close.store(current_candle_close);
                         
                         std::cout << "[LIVE|CLOSED] Candle Finalized. LogRV: " << log_rv 
                                   << " | Regime: " << active_regime 
                                   << " | TrendProb: " << (prob_state[1].load() * 100.0) << "%" << std::endl;
                     }
                 }
                 current_candle_t = t;
                 current_candle_close = current_close;
             }
         } catch (...) {}
     });
 
     c.set_close_handler([&](websocketpp::connection_hdl hdl) {
         std::cout << "🔴 WebSocket Closed!" << std::endl;
     });
 
     // 🌟 اتصال به استریم کندل هایپرلی کوید
     std::string uri = "wss://api.hyperliquid-testnet.xyz/ws";
     
     websocketpp::lib::error_code ec;
     wss_client::connection_ptr con = c.get_connection(uri, ec);
     c.connect(con);
     std::cout << "🌐 Connecting to Hyperliquid Candle Stream (" << stream_interval << ")..." << std::endl;
     c.run(); 
 
     return 0;
 }