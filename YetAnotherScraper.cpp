//cl.exe /MD /EHsc /I"C:\vcpkg\installed\x64-windows\include" YetAnotherScraper.cpp /link /LIBPATH:"C:\vcpkg\installed\x64-windows\lib" ixwebsocket.lib mbedtls.lib mbedx509.lib mbedcrypto.lib bcrypt.lib ws2_32.lib crypt32.lib gdi32.lib user32.lib advapi32.lib zlib.lib
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXNetSystem.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;



bool Launch(std::string chromePath)
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

int main()
{

    ix::initNetSystem();

    std::string path = "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";

    std::cout << "[+] Launching Browser..." << std::endl;
    if(!Launch(path))
    {
        std::cerr << "[-] Failed to launch Chrome." << std::endl;
        return 1;
    }

    // Give Chrome time to initialize the debugging server
    Sleep(2000);

    std::cout << "[+] Discovering CDP Endpoint..." << std::endl;
    std::string wsUrl = GetWebSocketUrl();

    if(wsUrl.empty())
    {
        std::cerr << "[-] Could not find an active tab." << std::endl;
        return 1;
    } std::cout << "[+] Target Found: " << wsUrl << std::endl;

    ix::uninitNetSystem();
    return 0;
}