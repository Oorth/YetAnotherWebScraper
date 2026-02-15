//cl.exe /MD /EHsc /I"C:\vcpkg\installed\x64-windows\include" YetAnotherScraper.cpp /link /LIBPATH:"C:\vcpkg\installed\x64-windows\lib" ixwebsocket.lib mbedtls.lib mbedx509.lib mbedcrypto.lib bcrypt.lib ws2_32.lib crypt32.lib gdi32.lib user32.lib advapi32.lib zlib.lib
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <future>
#include <map>
#include <mutex>
#include <iomanip>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

bool launch_chrome(std::string chromePath)
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
}

int main()
{
    // Initialize Networking
    ix::initNetSystem();

    std::cout << "[+] Launching Browser..." << std::endl;
    std::string path = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
    if(!launch_chrome(path))
    {
        ix::uninitNetSystem();
        std::cerr << "[-] Failed to launch Chrome." << std::endl;
        return 1;
    }

    Sleep(2000);

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


    std::cout << "[+] Navigating to Aggregator..." << std::endl;
    Core::SendCommand("Page.navigate", { {"url", "https://csgoskins.gg/items/awp-printstream/factory-new"} });

    // Wait for the page to settle
    std::cout << "[+] Waiting for prices to load..." << std::endl;
    Sleep(1000); 

    std::string jsCode = R"(
        (() => {
            const rows = document.querySelectorAll('.active-offer');
            
            return Array.from(rows).map(row => {
                // Check for the "Promoted" label
                const labels = row.querySelectorAll('.font-bold');
                let isPromoted = false;
                
                for(const label of labels)
                {
                    if (label.innerText.trim() === 'Promoted')
                    {
                        isPromoted = true;
                        break;
                    }
                }

                // Skip if promoted
                if(isPromoted)
                {
                    return null;
                }

                // Extract Market Name
                const marketEl = row.querySelector('a.custom-underline');
                const marketName = marketEl ? marketEl.innerText : "Unknown Market";

                // Extract Price
                const priceEl = row.querySelector('.font-bold.text-lg');
                const priceValue = priceEl ? priceEl.innerText : "N/A";

                // Extract the Redirect Link
                const redirectEl = row.querySelector('a[href*="/redirects/"]');
                const listingUrl = redirectEl ? redirectEl.href : "N/A";

                return {
                    market: marketName.trim(),
                    price: priceValue.trim(),
                    link: listingUrl
                };
            }).filter(item => item !== null); // Remove the skipped promoted entries
        })()
    )";

    json evalParams = 
    {
        {"expression", jsCode},
        {"returnByValue", true} 
    };

    std::cout << "[+] Extracting data from active-offer elements..." << std::endl;
    json response = Core::SendCommand("Runtime.evaluate", evalParams);

    if(response.contains("result") && response["result"].contains("result"))
    {
        auto& resultData = response["result"]["result"];
        
        if(resultData.contains("value") && resultData["value"].is_array())
        {
            std::cout << "\n--- DETECTED OFFERS ---" << std::endl;
            
            for(const auto& item : resultData["value"])
            {
                std::string market = item.value("market", "Unknown");
                std::string price  = item.value("price", "N/A");
                std::string link   = item.value("link", "N/A");
                
                if(market != "Unknown Market" && price != "N/A")
                {

                    

                    std::cout << "Market: " << std::left << std::setw(15) << market 
                    << "| Price: " << std::left << std::setw(10) << price 
                    << "| URL: " << link << std::endl;
                }
            }
        }
    }

    ix::uninitNetSystem();
    return 0;
}