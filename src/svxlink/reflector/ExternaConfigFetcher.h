#ifndef EXTERNA_CONFIG_FETCHER_H
#define EXTERNA_CONFIG_FETCHER_H

#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <memory>
#include <functional>


#include <json/json.h>



using json = Json::Value;

class ExternaConfigFetcher {
public:
    // Singleton instance - anslut till redan initialiserad fetcher
    /*
    static ExternaConfigFetcher* instance() {
        static ExternaConfigFetcher* inst = nullptr;
        return inst;
    }*/
    static ExternaConfigFetcher* instance();

    
    // Initiera singleton (anropas en gång i main)
    static ExternaConfigFetcher* initialize(const std::string& url, 
                                            const std::string& api_key,
                                            const std::string& node_id,
                                            int interval_seconds = 300);
    
    // Publik konstruktor (används av initialize)
    ExternaConfigFetcher(const std::string& url, 
                        const std::string& api_key,
                        const std::string& node_id,
                        int interval_seconds = 300);
    
    ~ExternaConfigFetcher();

    // Starta timer-tråd
    void start();
    
    // Stoppa timer-tråd
    void stop();
    
    // Hämta senaste config
    json getConfig() const;
    
    // Sätt callback för när ny config är hämtad
    using ConfigCallback = std::function<void(const json&)>;
    void setCallback(ConfigCallback callback);
    

    // Manuell hämtning (körs i egen tråd)
    void fetchNow();

private:
    std::string m_url;
    std::string m_api_key;
    std::string m_node_id;
    int m_interval_seconds;
    
    std::thread m_timer_thread;
    std::atomic<bool> m_running{false};
    
    mutable std::mutex m_config_mutex;
    json m_config;
    
    ConfigCallback m_callback;
    
    // Worker-trådens main loop
    void timerLoop();
    
    // Faktisk HTTP POST och JSON parsing
    bool fetchConfigFromServer(json& result);
    
    // HTTP request med libcurl
    std::string httpPost(const std::string& data = "");
};

#endif // EXTERNA_CONFIG_FETCHER_H
