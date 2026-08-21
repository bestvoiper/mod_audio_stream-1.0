#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include "WebSocketClient.h"

constexpr int totalTestCases = 516;

int main() {
    for (int i = 1; i <= totalTestCases; ++i) {
        std::cout << "\n--- Running test case " << i << " ---" << std::endl;

        auto client = std::make_shared<WebSocketClient>();

        client->setMessageCallback([&](const std::string& message) {
            client->sendMessage(message);
        });

        client->setBinaryCallback([&](const void* data, size_t length) {
            client->sendBinary(data, length);
        });

        std::mutex              done_mutex;
        std::condition_variable done_cv;
        bool                    closed = false;

        // On close from server, mirror it then signal
        client->setCloseCallback([&](int code, const std::string& reason) {
            std::cout << "Closed by server: \"" << reason
                      << "\" (code=" << code << ")\n";
            {
                std::lock_guard<std::mutex> lock(done_mutex);
                closed = true;
            }
            done_cv.notify_one();
        });

        // On error, just signal (don’t early-disconnect)
        client->setErrorCallback([&](int error_code, const std::string& error_message) {
            std::cout << "Error: " << error_message << std::endl;
            {
                std::lock_guard<std::mutex> lock(done_mutex);
                closed = true;
            }
            done_cv.notify_one();
        });

        client->setOpenCallback([]() {
            std::cout << "Connected\n";
        });

        // Start the test case
        std::string url =
          "ws://192.168.0.26:9001/runCase?case="
          + std::to_string(i) + "&agent=libwsc";

        client->setUrl(url);
        client->connect();

        {
            std::unique_lock<std::mutex> lock(done_mutex);
            if (!done_cv.wait_for(lock, std::chrono::seconds(20),
                                  [&]() { return closed; })) {
                // no CLOSE from server → we must initiate a clean close
                std::cout << "⏳ Timeout: sending NORMAL close\n";

                client->close(
                  static_cast<int>(WebSocketClient::CloseCode::NORMAL),
                  ""
                );

            }
            // give libevent a moment to flush *our* Close frame
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        client->disconnect();
    }

    // Final report
    
    {
        std::cout << "\n--- Reporting results ---\n";
        auto reportClient = std::make_shared<WebSocketClient>();
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done = false;

        reportClient->setOpenCallback([](){
            std::cout << "Connected to report endpoint\n";
        });
        reportClient->setErrorCallback(
            [&](int, const std::string& err) {
                std::cout << "Report error: " << err << "\n";
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    done = true;
                }
                cv.notify_one();
            }
        );
        reportClient->setCloseCallback(
            [&](int code, const std::string& reason) {
                std::cout << "Report closed: " << code
                          << " \"" << reason << "\"\n";
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    done = true;
                }
                cv.notify_one();
            }
        );

        reportClient->setUrl(
          "ws://192.168.0.26:9001/updateReports?agent=libwsc"
        );
        reportClient->connect();

        // wait up to 2s for the report-close
        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait_for(lk, std::chrono::seconds(2),
                        [&]{ return done; });
        }
        reportClient->disconnect();
    }
    
    std::cout << "All tests + report complete.\n";
    return 0;
}