//cl.exe /MD /std:c++17 /EHsc /I"C:\vcpkg\installed\x64-windows\include" YetAnotherScraper.cpp /link /LIBPATH:"C:\vcpkg\installed\x64-windows\lib" ixwebsocket.lib mbedtls.lib mbedx509.lib mbedcrypto.lib bcrypt.lib ws2_32.lib crypt32.lib gdi32.lib user32.lib advapi32.lib zlib.lib
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <future>
#include <filesystem>
#include <mutex>
#include <thread>
#include <iomanip>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

// -------------------------------------------------------------------------------------------------

std::mutex printMutex;

// -------------------------------------------------------------------------------------------------

struct _MARKET_OFFERS
{
    std::string marketName;
    int numOffers;
    float minPrice;

    std::string marketLink;
};

struct _MARKET_INFO
{
    float volume_24h;
    float vol_by_cap;

    std::vector<_MARKET_OFFERS> market_offers;
};

struct _SKINS
{
    int id;
    std::string name;
    
    std::string condition_string;
    float condition_float;

    _MARKET_INFO market_info;
};

// -------------------------------------------------------------------------------------------------

using json = nlohmann::json;

PROCESS_INFORMATION LaunchInstance(int port, std::string proxy, std::string userDataDir)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::string chromePath = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
    
    // Construct command line
    std::string cmd = "\"" + chromePath + "\" " +
                      "--remote-debugging-port=" + std::to_string(port) + " " +
                      "--user-data-dir=\"" + userDataDir + "\" " +
                      "--remote-allow-origins=* " + 
                      "--disable-blink-features=AutomationControlled " +
                      "--no-first-run ";
                    //   "--headless=new "; // OPTIONAL: Run headless so you don't see 10 windows popping up

    if(!proxy.empty()) cmd += " --proxy-server=\"" + proxy + "\"";

    if(!CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        std::cerr << "[-] Failed to launch Chrome on port " << port << std::endl;
        return {0};
    }

    return pi;
}

std::string GetWebSocketUrl(int port)
{
    ix::HttpClient httpClient;
    ix::HttpRequestArgsPtr args = httpClient.createRequest();
    
    args->connectTimeout = 3; 
    
    std::string endpoint = "http://127.0.0.1:" + std::to_string(port) + "/json/list";
    int maxAttempts = 5;
    
    for(int i = 0; i < maxAttempts; i++)
    {
        ix::HttpResponsePtr response = httpClient.get(endpoint, args);

        if(response->statusCode == 200)
        {
            try
            {
                auto targets = json::parse(response->body);
                for(auto& target : targets)
                {
                    // Filter for "page" type to avoid connecting to extensions/background workers
                    if(target.contains("type") && target["type"] == "page" && target.contains("webSocketDebuggerUrl")) 
                    {
                        return target["webSocketDebuggerUrl"];
                    }
                }
            }
            catch(const json::exception& e)
            {
                // JSON might be incomplete if Chrome is still starting up
            }
        }
        
        // Wait 500ms before retrying
        Sleep(500);
    }
    
    return "";
}

class ChromeClient
{
private:
    ix::WebSocket _ws;
    std::mutex _queueMutex;
    std::map<int, std::promise<json>> _pendingRequests;
    int _currentId = 0;
    bool _connected = false;

    // Internal: Handles incoming messages from the WebSocket thread
    void OnMessage(const ix::WebSocketMessagePtr& msg)
    {
        if(msg->type == ix::WebSocketMessageType::Message)
        {
            try {
                json response = json::parse(msg->str);
                
                // If the JSON has an 'id', it is a response to a command we sent
                if(response.contains("id"))
                {
                    int id = response["id"];
                    std::lock_guard<std::mutex> lock(_queueMutex);
                    
                    // Check if we are waiting for this ID
                    if(_pendingRequests.count(id))
                    {
                        _pendingRequests[id].set_value(response);
                        _pendingRequests.erase(id);
                    }
                }
            } catch(...) { /* Ignore malformed JSON */ }
        }
    }

public:
    // Constructor
    ChromeClient() {}

    // Destructor: Clean shutdown
    ~ChromeClient() {
        if(_connected) _ws.stop();
    }

    bool Connect(const std::string& url)
    {
        _ws.setUrl(url);
        _ws.disableAutomaticReconnection(); // We handle lifecycle manually

        // Important: Bind the callback to THIS instance
        _ws.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
            this->OnMessage(msg);
        });

        _ws.start();

        // Wait for connection (max 5 seconds)
        int timeout = 0;
        while(_ws.getReadyState() != ix::ReadyState::Open)
        {
            if(timeout > 50) return false; // 50 * 100ms = 5 sec
            Sleep(100);
            timeout++;
        }

        _connected = true;
        return true;
    }

    void Disconnect()
    {
        if(_connected) {
            _ws.stop();
            _connected = false;
        }
    }

    // Thread-safe command sender
    json SendCommand(std::string method, json params = json::object())
    {
        if(!_connected) return {{"error", "Not connected"}};

        int id;
        std::future<json> future;

        // 1. Register the promise
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            id = ++_currentId;
            // Create a promise and get its future
            std::promise<json> p;
            future = p.get_future();
            _pendingRequests[id] = std::move(p);
        }

        // 2. Send the request
        json request = {
            {"id", id},
            {"method", method},
            {"params", params}
        };
        _ws.send(request.dump());

        // 3. Wait for the result (Blocking)
        // You might want to add a timeout here (wait_for) in production code
        if(future.wait_for(std::chrono::seconds(30)) == std::future_status::timeout)
        {
             return {{"error", "Timeout waiting for Chrome response"}};
        }

        return future.get();
    }

    // Instance-based Selector Waiter
    bool WaitForSelector(std::string selector, int timeoutMs = 10000)
    {
        std::string jsCode = R"(
            (function(selector, timeout) {
                return new Promise((resolve, reject) => {
                    const startTime = Date.now();
                    const check = () => {
                        if(document.querySelector(selector)) return resolve(true);
                        if(Date.now() - startTime > timeout) return resolve(false); // Resolve false instead of reject
                        setTimeout(check, 100);
                    };
                    check();
                });
            })
        )";

        std::string fullExpression = jsCode + "('" + selector + "', " + std::to_string(timeoutMs) + ")";

        json evalParams = {
            {"expression", fullExpression},
            {"awaitPromise", true},
            {"returnByValue", true}
        };

        json response = SendCommand("Runtime.evaluate", evalParams);

        if(response.contains("result") && 
           response["result"].contains("result") && 
           response["result"]["result"].contains("value"))
        {
            return response["result"]["result"]["value"].get<bool>();
        }
        return false;
    }
};

class ProxyManager
{
private:
    std::vector<std::string> proxies;
    int currentIndex = 0;

public:
    ProxyManager()
    {
        // Add your proxies here. 
        // If you are using Tor, you might only have one, but let's pretend we have a list.
        proxies.push_back("socks5://127.0.0.1:9150"); // Tor 1
        // proxies.push_back("http://192.168.1.50:8080"); // Mobile 1
        // proxies.push_back("http://10.0.0.1:3128");    // Data Center 1
    }

    std::string GetNextProxy()
    {
        if(proxies.empty()) return "";
        
        // Round Robin rotation
        std::string p = proxies[currentIndex];
        currentIndex = (currentIndex + 1) % proxies.size();
        return p;
    }


};

json inject_extractionJs(ChromeClient& client)
{

    std::string extractionJs = R"(
        (() => {
            try {
                // --- HELPER: Find value by Label Text ---
                function getStat(searchText) {
                    // 1. Find all potential labels (gray text)
                    const labels = document.querySelectorAll('.text-gray-400');
                    
                    for(const label of labels) {
                        // 2. Check if this element contains our target text
                        if(label.innerText.includes(searchText)) {
                            // 3. The value is the "Next Sibling" in the HTML tree
                            const valueEl = label.nextElementSibling;
                            return valueEl ? valueEl.innerText.trim() : "N/A";
                        }
                    }
                    return "N/A";
                }

                // --- A. Extract Global Stats ---
                const volume24h = getStat("24h Trading Volume");
                const volToCap  = getStat("Volume / Market Cap");

                // --- B. Extract Offers (Your existing code) ---
                const rows = document.querySelectorAll('.active-offer');
                const debugLog = "Found " + rows.length + " rows.";

                const offerData = Array.from(rows).map(row => {
                    try {
                        if(row.innerText.includes('Promoted')) return null;

                        const marketEl = row.querySelector('a.custom-underline');
                        const marketName = marketEl ? marketEl.innerText.trim() : "Unknown";

                        // Complex selector for the '45' offers count
                        const offersCol = row.querySelector('.hidden.sm\\:block');
                        const offersEl = offersCol ? offersCol.lastElementChild : null;
                        const numOffers = offersEl ? offersEl.innerText.trim() : "0";

                        const priceEl = row.querySelector('.font-bold.text-lg');
                        const priceValue = priceEl ? priceEl.innerText.trim() : "N/A";

                        const redirectEl = row.querySelector('a[href*="/redirects/"]');
                        const listingUrl = redirectEl ? redirectEl.href : "N/A";

                        return {
                            market: marketName,
                            count: numOffers,
                            price: priceValue,
                            link: listingUrl
                        };
                    } catch (e) { return null; }
                }).filter(item => item !== null);

                // --- RETURN EVERYTHING ---
                return {
                    status: "success",
                    debug: debugLog,
                    stats: {
                        volume: volume24h,
                        vol_cap: volToCap
                    },
                    offers: offerData
                };

            } catch (err) {
                return { status: "error", message: err.toString() };
            }
        })()
    )";

    // std::cout << "[+] Running Extraction Script..." << std::endl;
    
    json evalParams = { 
        {"expression", extractionJs}, 
        {"returnByValue", true} 
    };
    
    return client.SendCommand("Runtime.evaluate", evalParams);

}

void print(const _SKINS& skin, size_t no_of_markets)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << " ITEM: " << skin.name << " (" << skin.condition_string << ")" << std::endl;
    std::cout << "========================================" << std::endl;
    
    // Print Global Stats with 2 decimal precision
    std::cout << std::fixed << std::setprecision(2);
    std::cout << " 24h Volume  : $" << skin.market_info.volume_24h << std::endl;
    std::cout << " Vol / Cap   : "  << (skin.market_info.vol_by_cap * 100.0f) << "%" << std::endl; // Assuming this is a percentage ratio
    std::cout << "========================================\n" << std::endl;

    std::cout << "--- FOUND " << skin.market_info.market_offers.size() << " OFFERS ---\n";

    if(no_of_markets > skin.market_info.market_offers.size()) no_of_markets = skin.market_info.market_offers.size();
    for(size_t i = 0; i < no_of_markets; i++)
    {
        const _MARKET_OFFERS& offer = skin.market_info.market_offers[i];

        std::cout << " [" << (i + 1) << "] Market: " << std::left << std::setw(15) << offer.marketName
                << "| Offers: " << std::left << std::setw(5) << offer.numOffers
                << "| Price: $" << std::left << std::setw(10) << offer.minPrice
                << "| Link: " << offer.marketLink << std::endl;
    }
}

bool ScrapeSkin(ChromeClient& client, _SKINS& skin)
{
    // ---------------------------------------------------------
    // Construct URL & Navigate
    // ---------------------------------------------------------
    std::string url = "https://csgoskins.gg/items/" + skin.name + "/" + skin.condition_string;
    
    // std::cout << "\n------------------------------------------------" << std::endl;
    // std::cout << "[+] Processing: " << skin.name << " (" << skin.condition_string << ")" << std::endl;
    // std::cout << "[+] Navigating: " << url << std::endl;

    client.SendCommand("Page.navigate", { {"url", url} });

    // ---------------------------------------------------------
    // Wait for Load
    // ---------------------------------------------------------

    if(!client.WaitForSelector(".active-offer", 8000))
    {
        std::cerr << "[-] Timeout: Offers not found for " << skin.name << std::endl;
        return false;
    }

    Sleep(200);

    // ---------------------------------------------------------
    // Extract Data
    // ---------------------------------------------------------
    json response = inject_extractionJs(client);

    // Helper Lambda for Float Parsing
    auto ParseCleanFloat = [](std::string raw) -> float {
        if (raw == "N/A" || raw.empty()) return 0.0f;
        std::string clean = "";
        for (char c : raw) {
            if (isdigit(c) || c == '.') clean += c;
        }
        try { return std::stof(clean); } catch (...) { return 0.0f; }
    };

    // ---------------------------------------------------------
    // Parse JSON
    // ---------------------------------------------------------
    if( !response.contains("result") || !response["result"].contains("result") || !response["result"]["result"].contains("value") )
    {
        std::cerr << "[-] Error: Invalid JSON response." << std::endl;
        return false;
    }

    auto resultObj = response["result"]["result"]["value"];

    if(resultObj["status"] == "error") 
    {
        std::cerr << "[-] JS Error: " << resultObj["message"] << std::endl;
        return false;
    }

    // Global Stats
    if(resultObj.contains("stats"))
    {
        skin.market_info.volume_24h = ParseCleanFloat(resultObj["stats"]["volume"].get<std::string>());
        skin.market_info.vol_by_cap = ParseCleanFloat(resultObj["stats"]["vol_cap"].get<std::string>());
    }

    // Offers
    if(resultObj.contains("offers"))
    {
        skin.market_info.market_offers.clear();
        
        for(const auto& item : resultObj["offers"])
        {
            _MARKET_OFFERS tempOffer;
            
            tempOffer.marketName = item["market"].get<std::string>();
            tempOffer.marketLink = item["link"].get<std::string>();
            tempOffer.numOffers  = (int)ParseCleanFloat(item["count"].get<std::string>());
            tempOffer.minPrice   = ParseCleanFloat(item["price"].get<std::string>());

            skin.market_info.market_offers.push_back(tempOffer);
        }
    }

    std::cout << "[+] Success: Scraped " << skin.market_info.market_offers.size() << " offers for " << skin.name << std::endl;
    return true;
}

void RunWorker(_SKINS& skin, std::string proxy, int port)
{
    // GENERATE UNIQUE PORT & PROFILE
    // int port = 10000 + (rand() % 5000); 
    std::string tempProfile = "C:\\temp\\chrome_worker_" + std::to_string(port);

    {
        std::lock_guard<std::mutex> lock(printMutex);
        std::cout << "\n[>>>] WORKER STARTED: " << skin.name << " on Port " << port;
    }

    // LAUNCH CHROME INSTANCE
    PROCESS_INFORMATION pi = LaunchInstance(port, proxy, tempProfile);
    if(pi.hProcess == NULL) return;

    Sleep(1000);

    // CONNECT WEB SOCKET
    std::string wsUrl = GetWebSocketUrl(port);
    
    if(!wsUrl.empty())
    {
        ChromeClient client;
        if(client.Connect(wsUrl))
        {
            // Verify IP inside this specific instance
            client.SendCommand("Page.navigate", {{"url", "https://api.ipify.org"}});

            if(client.WaitForSelector("pre", 30000))
            {
                json ipCheck = client.SendCommand("Runtime.evaluate", {
                    {"expression", "document.body.innerText"},
                    {"returnByValue", true}
                });

                if(ipCheck.contains("result") && ipCheck["result"].contains("result") && ipCheck["result"]["result"].contains("value"))
                {

                    std::string ip = ipCheck["result"]["result"]["value"];
                    std::cout << "[*] Current IP: " << ip << std::endl;
                } else std::cerr << "[-] Error: Failed to extract IP string." << std::endl;
            } else std::cerr << "[-] Timeout: Tor network is too slow or connection died." << std::endl;


            // PERFORM THE SCRAPE
            if(!ScrapeSkin(client, skin)) std::cerr << "[-] Scrape failed for " << skin.name << std::endl;

            client.Disconnect();
        }
    }
    else std::cerr << "[-] Failed to connect to Worker on port " << port << std::endl;

    // 5. CLEANUP
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    Sleep(1000); // Wait for file locks to release
    try {
        std::filesystem::remove_all(tempProfile); // Delete the temp user folder
    } catch(...) {}

}

int main()
{
    // Initialize Networking
    ix::initNetSystem();
    srand(time(0));

    // ---------------------------------------------------------
    // CONFIGURATION
    // ---------------------------------------------------------
    std::string currentProxy = "socks5://127.0.0.1:9150";
    std::vector<_SKINS> targets;

    _SKINS item1;
    item1.name = "awp-printstream";
    item1.condition_string = "factory-new";
    targets.push_back(item1);

    _SKINS item2;
    item2.name = "usp-s-printstream";
    item2.condition_string = "factory-new";
    targets.push_back(item2);

    _SKINS item3;
    item3.name = "ak-47-vulcan";
    item3.condition_string = "field-tested";
    targets.push_back(item3);

    // ---------------------------------------------------------
    // EXECUTION LOOP
    // ---------------------------------------------------------
    std::cout << "[+] Starting PARALLEL Batch of " << targets.size() << " items..." << std::endl;
    
    // A vector to hold our active threads
    std::vector<std::thread> workerThreads;
    int basePort = 10000;
    
    // Launch all workers simultaneously
    for(int i = 0; i < targets.size(); i++)
    {
        int assignedPort = basePort + i; // 10000, 10001, 10002...
        workerThreads.emplace_back(RunWorker, std::ref(targets[i]), currentProxy, assignedPort);
        
        // Slight stagger
        Sleep(200);
    }

    std::cout << "\n[+] All workers launched! Waiting for completion..." << std::endl;

    // ---------------------------------------------------------
    // SYNCHRONIZATION
    // ---------------------------------------------------------

    for(auto& t : workerThreads) if(t.joinable()) t.join();
    std::cout << "[+] All Parallel Jobs Complete." << std::endl;

    // ---------------------------------------------------------
    // REPORTING
    // ---------------------------------------------------------
    std::cout << "\n=== FINAL REPORT ===" << std::endl;
    for(const auto& skin : targets) print(skin, 2);

    ix::uninitNetSystem();
    return 0;
}