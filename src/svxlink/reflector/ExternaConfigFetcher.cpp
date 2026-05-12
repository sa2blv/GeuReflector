/**
@file	 ExternaConfigFetcher.cpp
@brief   The Trunk service class
@author  Peter Lundberg / SA2BLV
@date	 2026-01-11

\verbatim
SvxReflector - An Trunk for  svxreflector for connecting SvxLink Servers
Copyright (C) 2025-2026 Peter Lundberg / SA2BLV

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\endverbatim
*/





#include "ExternaConfigFetcher.h"
#include <curl/curl.h>
#include <iostream>
#include <sstream>


// Statisk pointer för singleton instans
static ExternaConfigFetcher* g_instance = nullptr;

// Callback för libcurl för att samla svar
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* s) {
    s->append((char*)contents, size * nmemb);
    return size * nmemb;
}

ExternaConfigFetcher* ExternaConfigFetcher::initialize(const std::string& url, 
                                                       const std::string& api_key,
                                                       const std::string& node_id,
                                                       int interval_seconds) {
    if (g_instance == nullptr) {
        g_instance = new ExternaConfigFetcher(url, api_key, node_id, interval_seconds);
        std::cout << "ConfigFetcher:  initialized" << std::endl;
    } else {
        std::cerr << "WARNING: ConfigFetcher already initialized!" << std::endl;
    }
    return g_instance;
}

ExternaConfigFetcher* ExternaConfigFetcher::instance() {
    if (g_instance == nullptr) {
        std::cerr << "ERROR: ConfigFetcher not initialized! Call initialize() first!" << std::endl;
    }
    return g_instance;
}

ExternaConfigFetcher::ExternaConfigFetcher(const std::string& url,
                                           const std::string& api_key,
                                           const std::string& node_id,
                                           int interval_seconds)
    : m_url(url), m_api_key(api_key), m_node_id(node_id), 
      m_interval_seconds(interval_seconds) {
    // Initiera tom config
    m_config = Json::Value(Json::objectValue);
}

ExternaConfigFetcher::~ExternaConfigFetcher() {
    stop();
}

void ExternaConfigFetcher::start() {
    if (m_running.exchange(true)) {
        return; // Already started
    }
    
    std::cout << "ConfigFetcher: Starting thread collicting (intervall: " 
              << m_interval_seconds << "s, URL: " << m_url << ")" << std::endl;
    
    m_timer_thread = std::thread(&ExternaConfigFetcher::timerLoop, this);
}

void ExternaConfigFetcher::stop() {
    if (!m_running.exchange(false)) {
        return; // Not started
    }
    
    std::cout << "ConfigFetcher: Stops thread" << std::endl;
    
    if (m_timer_thread.joinable()) {
        m_timer_thread.join();
    }
}

json ExternaConfigFetcher::getConfig() const {
    std::lock_guard<std::mutex> lock(m_config_mutex);
    return m_config;
}

void ExternaConfigFetcher::setCallback(ConfigCallback callback) {
    m_callback = callback;
}

void ExternaConfigFetcher::fetchNow() {
    //  attached separately to the internal blocker
    std::thread([this]() {
        json result;
        if (fetchConfigFromServer(result)) {
            {
                std::lock_guard<std::mutex> lock(m_config_mutex);
                m_config = result;
            }
            
            if (m_callback) {
                m_callback(result);
            }
            
        } else {
            std::cerr << "ConfigFetcher << Fail to get config from server" << std::endl;
        }
    }).detach();
}

void ExternaConfigFetcher::timerLoop() {
    std::cout << "ConfigFetcher: Started at thread  (PID: " << std::this_thread::get_id() << ")" << std::endl;
    
    while (m_running) {
        try {
            // wait for stop 
            for (int i = 0; i < m_interval_seconds && m_running; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            
            if (!m_running) break;
            
            
            json result;
            if (fetchConfigFromServer(result)) {
                {
                    std::lock_guard<std::mutex> lock(m_config_mutex);
                    m_config = result;
                }
                
                if (m_callback) {
                    m_callback(result);
                }
                
        ///        std::cout << "ExternaConfigFetcher: New config" << std::endl;
            } else {
                std::cerr << "ConfigFetcher: Fail coud not get config" << std::endl;
            }
            
        } catch (const std::exception& e) {
            std::cerr << "ConfigFetcher: Exception in timmer-thread: " << e.what() << std::endl;
        }
    }
    
    std::cout << "ConfigFetcher: Timer-thread stopped" << std::endl;
}

bool ExternaConfigFetcher::fetchConfigFromServer(json& result) {
    try {
        std::string response = httpPost();
        
        if (response.empty()) {
            std::cerr << "ConfigFetcher: No data from server" << std::endl;
            return false;
        }
        
        // Parsa JSON
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream s(response);

        if (!Json::parseFromStream(builder, s, &result, &errs)) {
            std::cerr << "JSON parse error: " << errs << std::endl;
            return false;
        }
        return true;
        

    } catch (const std::exception& e) {
        std::cerr << "ExternaConfigFetcher: Error: " << e.what() << std::endl;
        return false;
    }
}

std::string ExternaConfigFetcher::httpPost(const std::string& data)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "CURL init failed" << std::endl;
        return "";
    }

    std::string response;

    // --- Build JSON body ---
    json post_body(Json::objectValue);
    post_body["api_key"] = m_api_key;
    post_body["node_id"] = m_node_id;

    if (!data.empty()) {
        Json::Value extra_data;
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream s(data);

        if (Json::parseFromStream(builder, s, &extra_data, &errs)) {
            for (const auto& key : extra_data.getMemberNames()) {
                post_body[key] = extra_data[key];
            } 
        }
    }

    // Serialize JSON
    Json::StreamWriterBuilder writer;
    std::string postdata = Json::writeString(writer, post_body);

    // --- CURL setup ---
    curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // ADD THIS
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);       // ADD THIS
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postdata.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    // --- Perform request ---
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
        response.clear();
    }
    else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code != 200) {
            std::cerr << "HTTP status: " << http_code << std::endl;
            response.clear();
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return response;
}
