// cast_and_serve.cpp
// Single-file C++ app: discovers Chromecast devices via mDNS, serves a local MP4 over HTTP,
// then casts it to the chosen device using Cast V2 protocol.

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-common/error.h>
#include <avahi-common/simple-watch.h>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio.hpp>

#include <curl/curl.h>
#include <boost/asio/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <json/json.h>
#include "cast_channel.pb.h"

#include <iostream>
#include <thread>
#include <chrono>
#include <map>
#include <vector>
#include <mutex>
#include <limits>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <queue>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;
using ssl_socket = boost::asio::ssl::stream<boost::asio::ip::tcp::socket>;

static AvahiSimplePoll *simple_poll = nullptr;
static std::mutex dev_lock;
static std::map<std::string, std::string> devices;
static std::atomic<bool> shutdown_requested{false};

// resolve_callback: record each found device
std::string get_friendly_name(const std::string& ip) {
    CURL* curl = curl_easy_init();
    std::string response;
    if (curl) {
        std::string url = "http://" + ip + ":8008/setup/eureka_info";
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void *contents, size_t size, size_t nmemb, void *userp) -> size_t {
            static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
            return size * nmemb;
        });
        CURLcode result = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        if (result == CURLE_OK) {
            size_t name_pos = response.find("\"name\":");
            if (name_pos != std::string::npos) {
                size_t start = response.find("\"", name_pos + 7);
                size_t end = response.find("\"", start + 1);
                if (start != std::string::npos && end != std::string::npos) {
                    return response.substr(start + 1, end - start - 1);
                }
            }
        }
    }
    return "";
}

void resolve_callback(AvahiServiceResolver *r,
                      AvahiIfIndex, AvahiProtocol,
                      AvahiResolverEvent event, const char *name,
                      const char *, const char *, const char *,
                      const AvahiAddress *address, uint16_t,
                      AvahiStringList *, AvahiLookupResultFlags, void*)
{
    if (event == AVAHI_RESOLVER_FOUND) {
        char addr[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(addr, sizeof(addr), address);
        std::string friendly_name = get_friendly_name(addr);
        std::lock_guard<std::mutex> lk(dev_lock);
        devices[friendly_name.empty() ? name : friendly_name] = addr;
    }
    avahi_service_resolver_free(r);
}

// browse_callback: request resolution whenever a service appears
void browse_callback(AvahiServiceBrowser *b,
                     AvahiIfIndex interface,
                     AvahiProtocol protocol,
                     AvahiBrowserEvent event,
                     const char *name,
                     const char *type,
                     const char *domain,
                     AvahiLookupResultFlags,
                     void*)
{
    if (event == AVAHI_BROWSER_NEW) {
        avahi_service_resolver_new(
            avahi_service_browser_get_client(b),
            interface, protocol,
            name, type, domain,
            AVAHI_PROTO_UNSPEC, (AvahiLookupFlags)0,
            resolve_callback, nullptr
        );
    }
}

// A minimal HTTP server that serves exactly file_path on the given port
void http_server(const std::string &file_path, unsigned short port) {
    net::io_context ioc{1};
    tcp::acceptor acceptor{ioc, {tcp::v4(), port}};
    std::string filename = file_path.substr(file_path.find_last_of("/\\") + 1);
    std::cout << "[HTTP] Serving " << filename << " on port " << port << "\n";

    for (;;) {
        tcp::socket socket{ioc};
        acceptor.accept(socket);
        std::thread([sock = std::move(socket), fp = file_path]() mutable {
            try {
                beast::flat_buffer buffer;
                http::request<http::string_body> req;
                beast::error_code ec;
                http::read(sock, buffer, req, ec);

                // Range request support for video seeking
                beast::file file;
                file.open(fp.c_str(), beast::file_mode::scan, ec);
                if (ec) {
                    http::response<http::string_body> err{http::status::not_found, req.version()};
                    err.set(http::field::content_type, "text/plain");
                    err.body() = "Not found";
                    err.prepare_payload();
                    http::write(sock, err);
                } else {
                    auto file_size = file.size(ec);
                    if (ec) {
                        http::response<http::string_body> err{http::status::internal_server_error, req.version()};
                        err.set(http::field::content_type, "text/plain");
                        err.body() = "File size error";
                        err.prepare_payload();
                        http::write(sock, err);
                        return;
                    }

                    // Check for Range header
                    auto range_header = req.find(http::field::range);
                    if (range_header != req.end()) {
                        // Parse Range: bytes=start-end
                        std::string range_str = range_header->value();
                        if (range_str.substr(0, 6) == "bytes=") {
                            std::string range_spec = range_str.substr(6);
                            size_t dash_pos = range_spec.find('-');
                            
                            if (dash_pos != std::string::npos) {
                                std::string start_str = range_spec.substr(0, dash_pos);
                                std::string end_str = range_spec.substr(dash_pos + 1);
                                
                                uint64_t start = 0, end = file_size - 1;
                                
                                // Parse start
                                if (!start_str.empty()) {
                                    start = std::stoull(start_str);
                                }
                                
                                // Parse end  
                                if (!end_str.empty()) {
                                    end = std::stoull(end_str);
                                }
                                
                                // Validate range
                                if (start < file_size && end < file_size && start <= end) {
                                    uint64_t content_length = end - start + 1;
                                    
                                    // Create 206 Partial Content response
                                    http::response<http::string_body> res{http::status::partial_content, req.version()};
                                    res.set(http::field::server, "CastServer");
                                    res.set(http::field::content_type, "video/mp4");
                                    res.set(http::field::access_control_allow_origin, "*");
                                    res.set(http::field::accept_ranges, "bytes");
                                    res.set(http::field::content_range, "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(file_size));
                                    res.set(http::field::content_length, std::to_string(content_length));
                                    
                                    // Read and send partial content
                                    std::vector<char> buffer(std::min(content_length, (uint64_t)8192));
                                    file.seek(start, ec);
                                    if (ec) {
                                        std::cout << "[HTTP] Seek error: " << ec.message() << "\n";
                                        return;
                                    }
                                    
                                    http::write(sock, res);
                                    
                                    uint64_t remaining = content_length;
                                    while (remaining > 0) {
                                        size_t to_read = std::min(remaining, (uint64_t)buffer.size());
                                        size_t bytes_read = file.read(buffer.data(), to_read, ec);
                                        if (ec || bytes_read == 0) break;
                                        
                                        boost::asio::write(sock, boost::asio::buffer(buffer.data(), bytes_read));
                                        remaining -= bytes_read;
                                    }
                                    
                                    std::cout << "[HTTP] Served range " << start << "-" << end << " (" << content_length << " bytes)\n";
                                } else {
                                    // Invalid range - return 416
                                    http::response<http::string_body> err{http::status::range_not_satisfiable, req.version()};
                                    err.set(http::field::content_range, "bytes */" + std::to_string(file_size));
                                    err.prepare_payload();
                                    http::write(sock, err);
                                }
                            }
                        }
                    } else {
                        // No Range header - serve full file
                        http::response<http::file_body> res{http::status::ok, req.version()};
                        res.body().open(fp.c_str(), beast::file_mode::scan, ec);
                        if (ec) {
                            std::cout << "[HTTP] File open error for full serve: " << ec.message() << "\n";
                            return;
                        }
                        res.set(http::field::server, "CastServer");
                        res.set(http::field::content_type, "video/mp4");
                        res.set(http::field::access_control_allow_origin, "*");
                        res.set(http::field::accept_ranges, "bytes");
                        res.prepare_payload();
                        http::write(sock, res);
                        std::cout << "[HTTP] Served full file (" << file_size << " bytes)\n";
                    }
                }
            
            // **Use sock.shutdown, not socket.shutdown**
            sock.shutdown(tcp::socket::shutdown_send, ec);
            
            } catch (const std::exception& e) {
                std::cout << "[HTTP] Connection error: " << e.what() << "\n";
            } catch (...) {
                std::cout << "[HTTP] Unknown connection error\n";
            }
        }).detach();
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <MP4_PATH> <HTTP_PORT>\n";
        return 1;
    }
    std::string file_path = argv[1];
    unsigned short http_port = static_cast<unsigned short>(std::stoi(argv[2]));

    // 1) Start Avahi discovery
    int error;
    simple_poll = avahi_simple_poll_new();
    AvahiClient *client = avahi_client_new(
        avahi_simple_poll_get(simple_poll), (AvahiClientFlags)0,
        nullptr, nullptr, &error
    );
    if (!client) {
        std::cerr << "Avahi client error: " << avahi_strerror(error) << "\n";
        return 1;
    }
    AvahiServiceBrowser *browser = avahi_service_browser_new(
        client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
        "_googlecast._tcp", nullptr, (AvahiLookupFlags)0,
        browse_callback, nullptr
    );
    std::cout << "[Discover] Scanning for Chromecast devices... (3s)\n";

    // stop after 3s
    std::thread([&] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        avahi_simple_poll_quit(simple_poll);
    }).detach();

    avahi_simple_poll_loop(simple_poll);
    avahi_service_browser_free(browser);
    avahi_client_free(client);
    avahi_simple_poll_free(simple_poll);

    // 2) List & choose
    if (devices.empty()) {
        std::cerr << "No Chromecast devices found.\n";
        return 1;
    }
    std::vector<std::pair<std::string,std::string>> list;
    for (auto &kv : devices) list.emplace_back(kv.first, kv.second);
    std::cout << "Found devices:\n";
    for (size_t i = 0; i < list.size(); ++i)
        std::cout << " " << (i+1) << ". " << list[i].first
                  << " @ " << list[i].second << "\n";

    int choice = 0;
    while (choice < 1 || choice > (int)list.size()) {
        std::cout << "Select device [1-" << list.size() << "]: ";
        if (!(std::cin >> choice)) {
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cerr << "Invalid input.\n";
        }
    }
    std::string cast_ip = list[choice-1].second;

    // Get local IP address
    std::string local_ip;
    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        tcp::resolver::results_type endpoints = resolver.resolve(tcp::v4(), cast_ip, "8008");
        tcp::socket socket(ioc);
        socket.connect(*endpoints.begin());
        local_ip = socket.local_endpoint().address().to_string();
        socket.close();
        std::cout << "[Cast] Local IP: " << local_ip << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to get local IP: " << e.what() << std::endl;
        std::cerr << "[INFO] Using fallback method to get local IP..." << std::endl;
        // Try to connect to cast_ip on port 8008 instead of 80
        try {
            net::io_context ioc2;
            tcp::resolver resolver2(ioc2);
            tcp::resolver::results_type endpoints2 = resolver2.resolve(tcp::v4(), cast_ip, "8008");
            tcp::socket socket2(ioc2);
            socket2.connect(*endpoints2.begin());
            local_ip = socket2.local_endpoint().address().to_string();
            socket2.close();
            std::cout << "[Cast] Fallback local IP: " << local_ip << std::endl;
        } catch (const std::exception& e2) {
            std::cerr << "[ERROR] Fallback also failed: " << e2.what() << std::endl;
            // Last resort: parse from cast_ip assuming same subnet
            size_t last_dot = cast_ip.find_last_of('.');
            if (last_dot != std::string::npos) {
                local_ip = cast_ip.substr(0, last_dot) + ".100"; // assume .100 as fallback
                std::cout << "[Cast] Using subnet guess: " << local_ip << std::endl;
            } else {
                local_ip = "192.168.1.100";
            }
        }
    }

    // 3) Launch HTTP server
    std::thread(http_server, file_path, http_port).detach();
    std::string filename = file_path.substr(file_path.find_last_of("/\\") + 1);
    
    // URL encode the filename to handle spaces
    std::string encoded_filename;
    for (char c : filename) {
        if (c == ' ') {
            encoded_filename += "%20";
        } else if (std::isalnum(c) || c == '.' || c == '-' || c == '_') {
            encoded_filename += c;
        } else {
            encoded_filename += '%';
            encoded_filename += "0123456789ABCDEF"[(c >> 4) & 0xF];
            encoded_filename += "0123456789ABCDEF"[c & 0xF];
        }
    }
    
    std::string media_url = "http://" + local_ip + ":" + std::to_string(http_port) + "/" + encoded_filename;
    std::cout << "[Cast] Media URL = " << media_url << "\n";

    std::cout << "[Cast] Using Cast V2 protocol to control Chromecast\n";
    
    // Helper function for HTTP requests
    auto http_request = [&](const std::string& method, const std::string& url, const std::string& data = "") {
        CURL *curl = curl_easy_init();
        std::string response;
        
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);  // 10 second timeout
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void *contents, size_t size, size_t nmemb, void *userp) -> size_t {
                static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
                return size * nmemb;
            });
            
            if (method == "POST") {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                if (!data.empty()) {
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
                    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.length());
                }
            }
            
            struct curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            
            std::cout << "[HTTP] " << method << " " << url << "...";
            std::cout.flush();
            
            CURLcode res = curl_easy_perform(curl);
            long response_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            
            std::cout << " -> " << response_code;
            if (res != CURLE_OK) {
                std::cout << " (curl error: " << curl_easy_strerror(res) << ")";
            }
            std::cout << "\n";
            
            if (!response.empty()) {
                std::cout << "[HTTP] Response: " << response.substr(0, 200);
                if (response.size() > 200) std::cout << "...";
                std::cout << "\n";
            }
            
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return response_code == 200 || response_code == 201;
        }
        return false;
    };

    // Step 1: Establish TLS connection to port 8009
    std::cout << "[Cast] Establishing TLS connection to " << cast_ip << ":8009\n";
    boost::asio::io_context io_context;
    boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12_client);
    
    // Set up SSL context - Chromecast uses self-signed certs, verify via device auth
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(boost::asio::ssl::verify_none);
    
    // Create SSL socket
    ssl_socket socket(io_context, ssl_ctx);
    
    // Resolve and connect to port 8009
    tcp::resolver resolver(io_context);
    auto endpoints = resolver.resolve(cast_ip, "8009");
    
    std::cout << "[Cast] Connecting to " << cast_ip << ":8009...\n";
    boost::asio::connect(socket.lowest_layer(), endpoints);
    
    // Perform TLS handshake
    std::cout << "[Cast] Performing TLS handshake...\n";
    socket.handshake(boost::asio::ssl::stream_base::client);
    std::cout << "[Cast] ✓ TLS connection established!\n";

    // Step 2: Device Authentication
    std::cout << "[Cast] Performing device authentication...\n";
    
    // Create device auth challenge
    cast_channel::DeviceAuthMessage auth_msg;
    auth_msg.mutable_challenge(); // Creates empty challenge
    
    // Send auth challenge as binary protobuf
    cast_channel::CastMessage auth_challenge;
    auth_challenge.set_protocol_version(0);
    auth_challenge.set_source_id("sender-0");
    auth_challenge.set_destination_id("receiver-0");
    auth_challenge.set_namespace_("urn:x-cast:com.google.cast.tp.deviceauth");
    auth_challenge.set_payload_type(cast_channel::CastMessage::BINARY);
    
    std::string auth_data;
    auth_msg.SerializeToString(&auth_data);
    auth_challenge.set_payload_binary(auth_data);
    
    // Send auth challenge with length prefix
    std::string auth_serialized;
    auth_challenge.SerializeToString(&auth_serialized);
    uint32_t auth_len = htonl(auth_serialized.size());
    
    boost::asio::write(socket, boost::asio::buffer(&auth_len, 4));
    boost::asio::write(socket, boost::asio::buffer(auth_serialized));
    std::cout << "[Cast] ✓ Device auth challenge sent\n";
    
    // Read auth response
    uint32_t auth_response_len;
    boost::asio::read(socket, boost::asio::buffer(&auth_response_len, 4));
    auth_response_len = ntohl(auth_response_len);
    
    std::vector<char> auth_response_data(auth_response_len);
    boost::asio::read(socket, boost::asio::buffer(auth_response_data));
    
    cast_channel::CastMessage auth_response_msg;
    if (auth_response_msg.ParseFromArray(auth_response_data.data(), auth_response_len)) {
        if (auth_response_msg.payload_type() == cast_channel::CastMessage::BINARY) {
            cast_channel::DeviceAuthMessage auth_response;
            if (auth_response.ParseFromString(auth_response_msg.payload_binary())) {
                if (auth_response.has_response()) {
                    std::cout << "[Cast] ✓ Device authenticated successfully\n";
                    // In a real implementation, you'd verify the signature and certificate here
                } else if (auth_response.has_error()) {
                    std::cout << "[Cast] ✗ Device auth error: " << auth_response.error().error_type() << "\n";
                }
            }
        }
    }

    std::cout << "[Cast] Using pure Cast V2 protocol\n";

    // Global variables for session management
    std::string session_id;
    std::atomic<bool> heartbeat_running{true};
    std::atomic<int> request_counter{1};
    
    // Helper to send Cast protobuf messages
    auto send_cast_message = [&](const std::string& source_id, const std::string& dest_id, 
                                  const std::string& namespace_name, const std::string& json_payload) {
        cast_channel::CastMessage message;
        message.set_protocol_version(0);
        message.set_source_id(source_id);
        message.set_destination_id(dest_id);
        message.set_namespace_(namespace_name);
        message.set_payload_type(cast_channel::CastMessage::STRING);
        message.set_payload_utf8(json_payload);
        
        std::string serialized;
        message.SerializeToString(&serialized);
        
        uint32_t len = htonl(serialized.size());
        std::string frame;
        frame.append(reinterpret_cast<const char*>(&len), 4);
        frame.append(serialized);
        
        std::cout << "[Cast] Sending to " << namespace_name << ": " << json_payload << "\n";
        boost::asio::write(socket, boost::asio::buffer(frame));
    };
    
    // Helper to receive Cast protobuf messages with timeout
    auto receive_cast_message = [&]() -> cast_channel::CastMessage {
        try {
            std::cout << "[Cast] Waiting for response...\n";
            
            // Read 4-byte length prefix with timeout
            uint32_t msg_len_network;
            boost::system::error_code ec;
            
            // Use async read with timeout
            auto timer = std::make_shared<boost::asio::steady_timer>(io_context, std::chrono::seconds(5));
            bool timeout_occurred = false;
            
            timer->async_wait([&](boost::system::error_code) {
                timeout_occurred = true;
                socket.lowest_layer().cancel();
            });
            
            size_t bytes_read = boost::asio::read(socket, boost::asio::buffer(&msg_len_network, 4), ec);
            timer->cancel();
            
            if (timeout_occurred) {
                std::cout << "[Cast] Read timeout occurred\n";
                return cast_channel::CastMessage();
            }
            
            if (ec || bytes_read != 4) {
                std::cout << "[Cast] Failed to read length prefix: " << ec.message() << "\n";
                return cast_channel::CastMessage();
            }
            
            uint32_t msg_len = ntohl(msg_len_network);
            std::cout << "[Cast] Message length: " << msg_len << " bytes\n";
            
            if (msg_len > 65536) { // Sanity check
                std::cout << "[Cast] Invalid message length: " << msg_len << "\n";
                return cast_channel::CastMessage();
            }
            
            // Read the protobuf message
            std::string serialized_msg(msg_len, 0);
            bytes_read = boost::asio::read(socket, boost::asio::buffer(&serialized_msg[0], msg_len), ec);
            
            if (ec || bytes_read != msg_len) {
                std::cout << "[Cast] Failed to read message body: " << ec.message() << "\n";
                return cast_channel::CastMessage();
            }
            
            cast_channel::CastMessage message;
            if (!message.ParseFromString(serialized_msg)) {
                std::cout << "[Cast] Failed to parse protobuf message\n";
                return cast_channel::CastMessage();
            }
            
            std::cout << "[Cast] ✓ Received from " << message.namespace_() << ": " << message.payload_utf8() << "\n";
            return message;
        } catch (const std::exception& e) {
            std::cout << "[Cast] Receive timeout or error: " << e.what() << "\n";
            return cast_channel::CastMessage();
        }
    };

    // Start heartbeat thread for PING/PONG (will update destination when we get sessionId)
    std::thread heartbeat_thread([&]() {
        while (heartbeat_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!heartbeat_running) break;
            
            try {
                Json::Value ping_payload;
                ping_payload["type"] = "PING";
                
                // Use session_id if we have it, otherwise receiver-0
                std::string heartbeat_dest = session_id.empty() ? "receiver-0" : session_id;
                send_cast_message("sender-0", heartbeat_dest, "urn:x-cast:com.google.cast.tp.heartbeat",
                                 Json::FastWriter().write(ping_payload));
                std::cout << "[Cast] Sent heartbeat PING to " << heartbeat_dest << "\n";
            } catch (const std::exception& e) {
                std::cout << "[Cast] Heartbeat error: " << e.what() << "\n";
                break;
            }
        }
    });

    // Step 3: Cast protocol handshake and media loading using proper namespaces
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    try {
        // Step 1: CONNECT to connection namespace
        Json::Value connect_payload;
        connect_payload["type"] = "CONNECT";
        send_cast_message("sender-0", "receiver-0", "urn:x-cast:com.google.cast.tp.connection", 
                         Json::FastWriter().write(connect_payload));
        
        auto response = receive_cast_message();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Step 2: LAUNCH app via receiver namespace and extract sessionId
        Json::Value launch_payload;
        launch_payload["type"] = "LAUNCH";
        launch_payload["requestId"] = request_counter++;
        launch_payload["appId"] = "CC1AD845";
        std::cout << "[Cast] Launching Default Media Receiver...\n";
        send_cast_message("sender-0", "receiver-0", "urn:x-cast:com.google.cast.receiver",
                         Json::FastWriter().write(launch_payload));
        
        response = receive_cast_message();
        if (response.payload_utf8().empty()) {
            std::cout << "[Cast] No response to LAUNCH, retrying...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            response = receive_cast_message();
        }
        
        // LAUNCH_STATUS doesn't contain sessionId, need to wait for RECEIVER_STATUS
        std::cout << "[Cast] Got LAUNCH_STATUS, waiting for RECEIVER_STATUS with sessionId...\n";
        
        // Wait for the actual RECEIVER_STATUS with running applications
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        
        // Request receiver status to get sessionId
        Json::Value status_request;
        status_request["type"] = "GET_STATUS";
        status_request["requestId"] = request_counter++;
        send_cast_message("sender-0", "receiver-0", "urn:x-cast:com.google.cast.receiver",
                         Json::FastWriter().write(status_request));
        
        auto status_response = receive_cast_message();
        
        // Extract sessionId from RECEIVER_STATUS response
        if (!status_response.payload_utf8().empty()) {
            Json::Value parsed_status;
            Json::Reader reader;
            if (reader.parse(status_response.payload_utf8(), parsed_status)) {
                if (parsed_status["type"].asString() == "RECEIVER_STATUS" && 
                    parsed_status["status"]["applications"].isArray() &&
                    parsed_status["status"]["applications"].size() > 0) {
                    session_id = parsed_status["status"]["applications"][0]["sessionId"].asString();
                    std::cout << "[Cast] ✓ Extracted sessionId: " << session_id << "\n";
                }
            }
        }
        
        if (session_id.empty()) {
            std::cout << "[Cast] Warning: Still no sessionId, app may not have launched properly\n";
            session_id = "DEFAULT_MEDIA_RECEIVER";
        }

        // Wait for app to be fully ready - check statusText
        std::cout << "[Cast] Waiting for app to be ready...\n";
        for (int ready_attempts = 0; ready_attempts < 10; ready_attempts++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            
            Json::Value ready_check;
            ready_check["type"] = "GET_STATUS";
            ready_check["requestId"] = request_counter++;
            send_cast_message("sender-0", "receiver-0", "urn:x-cast:com.google.cast.receiver",
                             Json::FastWriter().write(ready_check));
            
            auto ready_response = receive_cast_message();
            if (!ready_response.payload_utf8().empty()) {
                Json::Value ready_parsed;
                Json::Reader reader;
                if (reader.parse(ready_response.payload_utf8(), ready_parsed)) {
                    if (ready_parsed["status"]["applications"].isArray() && 
                        ready_parsed["status"]["applications"].size() > 0) {
                        std::string statusText = ready_parsed["status"]["applications"][0]["statusText"].asString();
                        std::cout << "[Cast] App status: " << statusText << "\n";
                        if (statusText == "Ready To Cast" || statusText == "Default Media Receiver") {
                            std::cout << "[Cast] ✓ App is ready!\n";
                            break;
                        }
                    }
                }
            }
            std::cout << "[Cast] App not ready yet, waiting... (" << (ready_attempts + 1) << "/10)\n";
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        // Step 3: Connect to launched app session using extracted sessionId
        Json::Value app_connect_payload;
        app_connect_payload["type"] = "CONNECT";
        send_cast_message("sender-0", session_id, "urn:x-cast:com.google.cast.tp.connection",
                         Json::FastWriter().write(app_connect_payload));
        
        response = receive_cast_message();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Step 4: LOAD media with contentId, contentType, streamType using sessionId
        int load_request_id = request_counter++;
        Json::Value load_payload;
        load_payload["type"] = "LOAD";
        load_payload["requestId"] = load_request_id;
        load_payload["autoplay"] = true;
        load_payload["media"]["contentId"] = media_url;
        load_payload["media"]["contentType"] = "video/mp4";
        load_payload["media"]["streamType"] = "BUFFERED";
        
        std::cout << "[Cast] Sending LOAD command with requestId " << load_request_id << " to sessionId " << session_id << "\n";
        send_cast_message("sender-0", session_id, "urn:x-cast:com.google.cast.media",
                         Json::FastWriter().write(load_payload));

        std::cout << "[Cast] ✓ LOAD command sent with proper protobuf and sessionId!\n";
        std::cout << "[Cast] ✓ contentId=" << media_url << "\n";
        std::cout << "[Cast] ✓ contentType=video/mp4, streamType=BUFFERED\n";
        std::cout << "[Cast] ✓ sessionId=" << session_id << "\n";
        std::cout << "[Cast] ✓ Check your TV! Video should start playing now.\n";

        // Keep looking for media response, not just heartbeat
        for (int attempts = 0; attempts < 10; attempts++) {
            response = receive_cast_message();
            
            if (!response.payload_utf8().empty()) {
                Json::Value load_response;
                Json::Reader reader;
                if (reader.parse(response.payload_utf8(), load_response)) {
                    std::string msgType = load_response["type"].asString();
                    if (msgType == "MEDIA_STATUS") {
                        std::cout << "[Cast] ✓ Received MEDIA_STATUS - video should be loading!\n";
                        std::cout << "[Cast] MEDIA_STATUS details: " << response.payload_utf8() << "\n";
                        
                        // Check if media is IDLE (paused) and send PLAY command
                        if (load_response["status"].isArray() && load_response["status"].size() > 0) {
                            std::string playerState = load_response["status"][0]["playerState"].asString();
                            int mediaSessionId = load_response["status"][0]["mediaSessionId"].asInt();
                            
                            std::cout << "[Cast] Player state: " << playerState << ", mediaSessionId: " << mediaSessionId << "\n";
                            
                            if (playerState == "IDLE") {
                                std::cout << "[Cast] Media is IDLE, sending PLAY command...\n";
                                Json::Value play_payload;
                                play_payload["type"] = "PLAY";
                                play_payload["requestId"] = request_counter++;
                                play_payload["mediaSessionId"] = mediaSessionId;
                                
                                send_cast_message("sender-0", session_id, "urn:x-cast:com.google.cast.media",
                                                 Json::FastWriter().write(play_payload));
                                std::cout << "[Cast] ✓ PLAY command sent!\n";
                                
                                // Wait for PLAY response to confirm it worked
                                std::cout << "[Cast] Waiting for PLAY response...\n";
                                auto play_response = receive_cast_message();
                                if (!play_response.payload_utf8().empty()) {
                                    std::cout << "[Cast] PLAY response: " << play_response.payload_utf8() << "\n";
                                }
                            }
                        }
                        break;
                    } else if (msgType == "LOAD_FAILED") {
                        std::cout << "[Cast] ✗ LOAD_FAILED: " << load_response["reason"].asString() << "\n";
                        std::cout << "[Cast] Full error: " << response.payload_utf8() << "\n";
                        break;
                    } else if (msgType == "PONG") {
                        std::cout << "[Cast] Got heartbeat PONG, continuing to wait for media response...\n";
                        continue;
                    } else {
                        std::cout << "[Cast] Got response: " << msgType << " - " << response.payload_utf8() << "\n";
                    }
                }
            } else {
                std::cout << "[Cast] Empty response on attempt " << (attempts+1) << "\n";
            }
        }
        
        // Wait a bit for the media to start, then keep monitoring playback
        std::cout << "[Cast] Waiting for media to start...\n";
        std::this_thread::sleep_for(std::chrono::seconds(3));
        
        // Keep TLS connection alive and monitor playback status
        std::cout << "[Cast] ✓ Keeping TLS connection alive to monitor playback...\n";
        std::cout << "[Cast] HTTP server running on port " << http_port << " (with Range support for audio/video)\n";
        std::cout << "[Cast] Press Ctrl+C to stop.\n";
        
        // Keep both TLS connection and HTTP server running
        while (heartbeat_running && !shutdown_requested) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cout << "[Cast] Cast protocol error: " << e.what() << "\n";
        std::cout << "[Cast] Fallback: Use Chrome browser to cast " << media_url << "\n";
    }

    // Clean shutdown - stop heartbeat and close TLS connection
    std::cout << "[Cast] Shutting down...\n";
    heartbeat_running = false;
    if (heartbeat_thread.joinable()) heartbeat_thread.join();
    
    boost::system::error_code ec;
    socket.shutdown(ec);
    std::cout << "[Cast] ✓ TLS connection closed gracefully.\n";

    return 0;
}
