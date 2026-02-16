//cl.exe /MD /EHsc /I"C:\vcpkg\installed\x64-windows\include" YetAnotherScraper.cpp /link /LIBPATH:"C:\vcpkg\installed\x64-windows\lib" ixwebsocket.lib mbedtls.lib mbedx509.lib mbedcrypto.lib bcrypt.lib ws2_32.lib crypt32.lib gdi32.lib user32.lib advapi32.lib zlib.lib
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <future>
#include <map>
#include <mutex>
#include <iomanip>
#include <algorithm>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>


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

float ParseCleanFloat(std::string raw)
{
    if (raw == "N/A" || raw.empty()) return 0.0f;

    // Remove anything that is NOT a digit or a dot
    // This strips '$', ',', '%', and spaces
    std::string clean = "";
    for(char c : raw)
    {
        if(isdigit(c) || c == '.')
        {
            clean += c;
        }
    }
    
    try
    {
        return std::stof(clean);
    }
    catch (...)
    {
        return 0.0f;
    }
}

bool launch_chrome(std::string chromePath, std::string proxy = "")
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // Critical flags for research and stealth
    // --remote-debugging-port: Opens the CDP bridge
    // --disable-blink-features=AutomationControlled: Removes 'navigator.webdriver'
    // --user-data-dir: Persists cookies/sessions
    std::string cmd = "\"" + chromePath + "\" " +
                        "--remote-debugging-port=9222 " +
                        "--remote-allow-origins=* " + 
                        "--disable-blink-features=AutomationControlled " +
                        "--no-first-run " +
                        "--no-default-browser-check " +
                        "--user-data-dir=\"C:\\temp\\cs_research_profile\"";

    
    // --- PROXY LOGIC ---
    if(!proxy.empty())
    {
        // Standard Chrome flag for proxies
        cmd += " --proxy-server=\"" + proxy + "\"";
        std::cout << "[+] Launching with Proxy: " << proxy << std::endl;
    }
    else std::cout << "[+] Launching without Proxy (Direct)" << std::endl;
    // -------------------
    
    
    BOOL success = CreateProcessA(NULL, (LPSTR)cmd.c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if(!success)
    {
        DWORD error = GetLastError();
        std::cout << "Error opening the process -> :" << error << std::endl;
        return 0;
    }

    // Close handles to the process and thread to avoid memory leaks
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return true;
}

std::string GetWebSocketUrl()
{
    ix::HttpClient httpClient;
    ix::HttpRequestArgsPtr args = httpClient.createRequest();
    
    // IMPORTANT: Disable certificate validation just in case (though http shouldn't need it)
    // and ensure no proxy interference.
    args->connectTimeout = 5; 
    
    int maxAttempts = 10;
    
    for(int i = 0; i < maxAttempts; i++)
    {
        // Query the JSON list endpoint
        ix::HttpResponsePtr response = httpClient.get("http://127.0.0.1:9222/json/list", args);

        if(response->statusCode == 200)
        {
            try
            {
                auto targets = json::parse(response->body);
                
                for(auto& target : targets)
                {
                    // Chrome returns multiple targets (background pages, extensions, etc.)
                    // We specifically want a "page" that has a WebSocket URL.
                    if(target.contains("type") && target["type"] == "page" && target.contains("webSocketDebuggerUrl")) return target["webSocketDebuggerUrl"];
                }
            }
            catch(const json::exception& e)
            {
                 std::cerr << "[-] JSON Parse Error: " << e.what() << std::endl;
            }
        }
        else std::cout << "[.] Connection attempt " << i + 1 << " failed. Status: " << response->statusCode << std::endl;

        Sleep(1000);
    }
    
    return "";
}

namespace Core
{
    // Global/Static state for the connection
    ix::WebSocket _ws;
    std::mutex mapMutex;
    std::map<int, std::promise<json>> _pendingRequests;
    int currentId = 0;

    // This function handles incoming messages from Chrome
    void OnMessage(const ix::WebSocketMessagePtr& msg)
    {
        if(msg->type == ix::WebSocketMessageType::Message)
        {
            json response = json::parse(msg->str);
            
            // If the JSON has an 'id', it's a response to a command we sent
            if(response.contains("id"))
            {
                int id = response["id"];
                std::lock_guard<std::mutex> lock(mapMutex);
                
                if(_pendingRequests.count(id))
                {
                    _pendingRequests[id].set_value(response);
                    _pendingRequests.erase(id);
                }
            }
        }
    }

    // Connects the "Pipe" to the browser
    void ConnectToBrowser(const std::string& url)
    {
        std::cout << "[.] Attempting WebSocket handshake..." << std::endl;

            // WORKAROUND: Since your library version lacks setProxy(), 
            // we clear these environment variables to prevent auto-detection 
            // of proxies like Fiddler/Charles for this process.
            #ifdef _WIN32
                _putenv("HTTP_PROXY=");
                _putenv("HTTPS_PROXY=");
                _putenv("ALL_PROXY=");
            #else
                unsetenv("HTTP_PROXY");
                unsetenv("HTTPS_PROXY");
                unsetenv("ALL_PROXY");
            #endif

            _ws.setUrl(url);
            
            ix::WebSocketHttpHeaders headers;
            _ws.setExtraHeaders(headers);
            
            // REMOVED: _ws.setProxy("", 0); -> Your version does not support this.

            _ws.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg)
            {
                if(msg->type == ix::WebSocketMessageType::Message)
                {
                    OnMessage(msg);
                }
                else if(msg->type == ix::WebSocketMessageType::Error)
                {
                    std::cerr << "\n[-] WebSocket Error: " << msg->errorInfo.reason << std::endl;
                    std::cerr << "[-] HTTP Status: " << msg->errorInfo.http_status << std::endl;
                }
                else if(msg->type == ix::WebSocketMessageType::Close)
                {
                    std::cerr << "\n[-] Connection closed by Chrome." << std::endl;
                }
            });


        _ws.start();

        int timeout = 0;
        while(_ws.getReadyState() != ix::ReadyState::Open)
        {
            if(timeout > 100) // 5 second timeout (100 * 50ms)
            {
                std::cerr << "\n[-] Connection timed out. ReadyState: " << (int)_ws.getReadyState() << std::endl;
                return;
            }

            std::cout << "." << std::flush;
            Sleep(50);
            timeout++;
        }

        std::cout << "\n[+] WebSocket Bridge Open." << std::endl;
    }

    // Sends a command and WAITS for the result (Synchronous wrapper)
    json SendCommand(std::string method, json params = json::object())
    {
        int id = ++currentId;
        
        json request = 
        {
            {"id", id},
            {"method", method},
            {"params", params}
        };

        std::promise<json> promise;
        std::future<json> future = promise.get_future();

        {
            std::lock_guard<std::mutex> lock(mapMutex);
            _pendingRequests[id] = std::move(promise);
        }

        _ws.send(request.dump());

        // This blocks the C++ thread until Chrome sends the response back
        return future.get();
    }

    // Helper: Blocks execution until the CSS selector exists on the page
    bool WaitForSelector(std::string selector, int timeoutMs = 10000)
    {
        // A robust Polling-based waiter. 
        // It survives page transitions better than MutationObserver.
        std::string jsCode = R"(
            (function(selector, timeout) {
                return new Promise((resolve, reject) => {
                    const startTime = Date.now();

                    const check = () => {
                        // 1. Found it?
                        if(document.querySelector(selector)) {
                            return resolve(true);
                        }

                        // 2. Timeout?
                        if(Date.now() - startTime > timeout) {
                            return reject(new Error("Timeout waiting for " + selector));
                        }

                        // 3. Keep looking (Check again in 100ms)
                        setTimeout(check, 100);
                    };

                    check();
                });
            })
        )";

        // Inject arguments
        std::string fullExpression = jsCode + "('" + selector + "', " + std::to_string(timeoutMs) + ")";

        json evalParams = {
            {"expression", fullExpression},
            {"awaitPromise", true},
            {"returnByValue", true}
        };

        // Send command
        json response = Core::SendCommand("Runtime.evaluate", evalParams);

        // --- DEBUGGING BLOCK ---
        // If there is an error(Exception), print it!
        if(response.contains("result") && response["result"].contains("exceptionDetails")) 
        {
            auto details = response["result"]["exceptionDetails"];
            std::cerr << "\n[!] WaitForSelector Error: " << details["text"] << std::endl;
            
            if(details.contains("exception") && details["exception"].contains("description"))
            {
                std::cerr << "[!] Details: " << details["exception"]["description"] << std::endl;
            }
            return false;
        }
        // -----------------------

        return true;
    }

}

json inject_extractionJs()
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

    std::cout << "[+] Running Extraction Script..." << std::endl;
    
    json evalParams = { 
        {"expression", extractionJs}, 
        {"returnByValue", true} 
    };
    
    return Core::SendCommand("Runtime.evaluate", evalParams);

}

void print(const _SKINS& skin)
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

    for(const _MARKET_OFFERS& offer : skin.market_info.market_offers)
    {
        std::cout << "Market: " << std::left << std::setw(15) << offer.marketName
                  << "| Offers: " << std::left << std::setw(5) << offer.numOffers
                  << "| Price: $" << std::left << std::setw(10) << offer.minPrice
                  << "| Link: " << offer.marketLink << std::endl;
    }
    std::cout << std::endl;
}

int main()
{
    // Initialize Networking
    ix::initNetSystem();

    std::cout << "[+] Launching Browser..." << std::endl;
    std::string path = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";

    // TOR CONFIGURATION
    // Tor Browser uses port 9150 by default.
    // Tor Expert Bundle uses port 9050.
    std::string torProxy = "socks5://127.0.0.1:9150";

    if(!launch_chrome(path, torProxy))
    {
        ix::uninitNetSystem();
        std::cerr << "[-] Failed to launch Chrome." << std::endl;
        return 1;
    }

    Sleep(1000);

    std::cout << "[+] Discovering CDP Endpoint..." << std::endl;
    std::string wsUrl = GetWebSocketUrl();
    if(wsUrl.empty())
    {
        ix::uninitNetSystem();
        std::cerr << "[-] Could not find an active tab." << std::endl;
        return 1;
    } std::cout << "[+] Target Found: " << wsUrl << std::endl;
    

    // Connect the Bridge
    Core::ConnectToBrowser(wsUrl);

    // ----------------------------------------------------------------------------------------------------------------
    std::cout << "[+] Checking IP Address via Tor..." << std::endl;
    
    Core::SendCommand("Page.navigate", { {"url", "https://api.ipify.org/"} });

    if(Core::WaitForSelector("pre", 30000))
    {
        json ipCheck = Core::SendCommand("Runtime.evaluate", { 
            {"expression", "document.body.innerText"}, 
            {"returnByValue", true} 
        });

        if(ipCheck.contains("result") && ipCheck["result"].contains("result") && ipCheck["result"]["result"].contains("value"))
        {
            std::string ip = ipCheck["result"]["result"]["value"];
            std::cout << "[*] Current IP: " << ip << std::endl;
        }
        else std::cerr << "[-] Error: Failed to extract IP string." << std::endl;
    }
    else std::cerr << "[-] Timeout: Tor network is too slow or connection died." << std::endl;
    // ----------------------------------------------------------------------------------------------------------------

    _SKINS awp_fn, usps_fn;

    awp_fn.condition_string = "factory-new";
    awp_fn.name = "awp-printstream";

    awp_fn.condition_string = "factory-new";
    awp_fn.name = "usp-s-printstream";

    std::cout << "[+] Navigating to Aggregator..." << std::endl;
    Core::SendCommand("Page.navigate", { {"url", "https://csgoskins.gg/items/" + awp_fn.name + "/" + awp_fn.condition_string} });

    std::cout << "[+] Waiting for 'active-offer' elements..." << std::endl;
    
    // We wait for the specific class that holds the data we want
    if(!Core::WaitForSelector(".active-offer", 5000))
    {
        ix::uninitNetSystem();   
        std::cerr << "[-] Timed out. Page took too long to load." << std::endl;
        return 0;
    }

    std::cout << "[+] Allowing data to hydrate (200ms)..." << std::endl;
    Sleep(200); 

    json response = inject_extractionJs();

    // -------------------------------------------------------------------------
    // PROCESS RESULTS
    // -------------------------------------------------------------------------
    
    if( !response.contains("result") || !response["result"].contains("result") || !response["result"]["result"].contains("value") )
    {
        std::cerr << "[-] Critical: Failed to receive valid JSON." << std::endl;
        ix::uninitNetSystem();
        return 0;
    }

    auto resultObj = response["result"]["result"]["value"];

    if(resultObj["status"] == "error") 
    {
        std::cerr << "[-] JS CRASHED: " << resultObj["message"] << std::endl;
        ix::uninitNetSystem();
        return 0;
    }
    
    std::string rawVol = resultObj["stats"]["volume"].get<std::string>();
    std::string rawCap = resultObj["stats"]["vol_cap"].get<std::string>();

    awp_fn.market_info.volume_24h = ParseCleanFloat(rawVol);
    awp_fn.market_info.vol_by_cap = ParseCleanFloat(rawCap);

    // Parse Offers List
    auto items = resultObj["offers"];
    
    // Clear vector to avoid duplicates if running in a loop
    awp_fn.market_info.market_offers.clear();

    for(const auto& item : items)
    {
        _MARKET_OFFERS tempOffer;

        tempOffer.marketName = item["market"].get<std::string>();
        tempOffer.marketLink = item["link"].get<std::string>();

        std::string rawCount = item["count"].get<std::string>();
        std::string rawPrice = item["price"].get<std::string>();

        tempOffer.numOffers = (int)ParseCleanFloat(rawCount);
        tempOffer.minPrice  = ParseCleanFloat(rawPrice);

        awp_fn.market_info.market_offers.push_back(tempOffer);
    }
    
    std::cout << "[+] Successfully parsed " << awp_fn.market_info.market_offers.size() << " offers." << std::endl;
    
    print(awp_fn);



    ix::uninitNetSystem();
    return 0;
}