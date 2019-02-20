#include <bb/registry.hpp>
#include <iostream>

#include <vector>
#include <boost/asio.hpp>

#include <fbs/req_generated.h>

#include <boost/process.hpp>

#include <spdlog/spdlog.h>
#include "spdlog/sinks/stdout_color_sinks.h"

#include <thread>
#include <bb/handler.hpp>
#include <bb/dynamic.hpp>
#include <nlohmann/json.hpp>
#include <zap/json_auth.hpp>
#include "iauth.hpp"
#include "module_info.hpp"
#include "rem_auth.hpp"

using boost::asio::ip::udp;

auto auth = zap::make_null_auth();

class server
{
public:
    server(boost::asio::io_context& io_context)
            : socket_(io_context, udp::endpoint(udp::v4(), 0))
    {
        auto req_log = spdlog::stderr_color_mt("req-log");
        do_receive();
    }

    void load_module(std::string_view ns, boost::dll::shared_library&& lib)
    {
        auto [it, ins] = mods.emplace(ns, std::move(lib));
        auto log = spdlog::get("zap-system");
        if (ins)
        {
            log->info("Namespace \"{}\" loaded", ns);
            for (auto& handler : it->second.reg->get_handlers())
            {
                if (handler != "moddef")
                {
                    log->info("Handler added: \"{}.{}\"", std::string(ns), handler);
                } else {
                    log->info("Handler added: \"{}\"", std::string(ns));
                }
            }
        }
        else
        {
            log->warn("Namespace {} failed to load!", ns);
        }
    }

    uint16_t get_port() const
    {
        return socket_.local_endpoint().port();
    }

private:
    void handle_packet(tos::span<const uint8_t> packet, const udp::endpoint& from)
    {
        auto ver = flatbuffers::Verifier(packet.data(), packet.size());
        bool ok = zap::cloud::VerifyRequestBuffer(ver);

        auto req = flatbuffers::GetRoot<zap::cloud::Request>(packet.data());

        auto remote_addr = from.address().to_string();
        auto remote_port = from.port();

        auto req_log = spdlog::get("req-log");
        if (!ok)
        {
            req_log->error("Received garbage from {}:{}", remote_addr, remote_port);
            return;
        }

        auto body = tos::span<const uint8_t>(req->body()->data(), req->body()->size());

        auto token = req->token()->str();

        auto handler = req->handler()->str();

        req_log->info(
                "Got request on \"{}\" with body size {} from {}:{}",
                handler, body.size(), remote_addr, remote_port);

        try
        {
            auto id = auth->authenticate(token);

            auto fun_log = spdlog::get(handler);

            zap::call_info ci;
            ci.client = id;
            ci.log = fun_log ? fun_log : spdlog::stdout_color_mt(handler);

            auto dot_pos = handler.find('.');

            auto ns = handler;
            if (dot_pos != handler.npos)
            {
                ns = handler.substr(0, dot_pos);
                handler = handler.substr(dot_pos + 1);
            }
            else
            {
                handler.clear();
            }

            auto mod_it = mods.find(ns);

            if (mod_it == mods.end())
            {
                req_log->info("No module {} was found for the last request", ns);
                std::string res = "no such module failed";
                socket_.async_send_to(boost::asio::buffer(res), ep, [res = std::move(res)](auto&, auto){});
                return;
            }

            auto reg = mod_it->second.reg;
            auto res = handler.empty() ? reg->post(body, ci) : reg->post(handler.c_str(), body, ci);

            if (!res)
            {
                req_log->info("No handler was found for the last request");
            }

            if (ci.res)
            {
                ci.log->info("Response: {}", ci.res.get());
                socket_.async_send_to(boost::asio::buffer(ci.res.get()), ep,
                        [](boost::system::error_code, std::size_t){});
            }

            req_log->info("Request handled successfully");
        }
        catch (std::exception& err)
        {
            req_log->error("Handling failed: {}", err.what());
            std::string res = "request failed";
            socket_.async_send_to(boost::asio::buffer(res), ep, [res = std::move(res)](auto&, auto){});
        }
    }

    void do_receive()
    {
        socket_.async_receive_from(boost::asio::buffer(data_, max_length), ep,
                [this](boost::system::error_code ec, std::size_t bytes_recvd)
                {
                    if (!ec && bytes_recvd > 0)
                    {
                        handle_packet(tos::span<const uint8_t>(data_, bytes_recvd), ep);
                    }

                    do_receive();
                });
    }

    std::unordered_map<std::string, zap::module_info> mods;

    udp::socket socket_;
    enum { max_length = 1024 };
    udp::endpoint ep;
    uint8_t data_[max_length];
};

int main(int argc, char** argv)
{
    using namespace boost;

    auto log = spdlog::stderr_color_mt("zap-system");

    asio::io_context io;

    server s(io);

    auto env = boost::this_process::environment();

    auto conf_path = argc > 1 ? argv[1] : env["ZAP_ENTRY"].to_string().c_str();
    std::ifstream conf_file{conf_path};

    log->info("Zap Serverless Dynamic Runtime {}", "0.1.1");
    log->info("Loading Configuration From \"{}\"", conf_path);

    auto conf = nlohmann::json::parse(conf_file);

    for (nlohmann::json& mod : conf)
    {
        auto object_path = mod["module"].get<std::string>();
        auto ns = mod["name"].get<std::string>();
        log->info("Loading module from \"{}\" to namespace \"{}\"", object_path, ns);
        dll::shared_library lib(object_path);
        s.load_module(ns, std::move(lib));
    }

    if (conf.find("auth") != conf.end())
    {
        auto addr = boost::asio::ip::make_address(conf["auth"]["host"].get<std::string>());
        auto port = conf["auth"]["port"].get<uint16_t>();
        log->info("Configuration has authentication on {}:{}", addr.to_string(), port);
        auth = zap::make_remote_auth(io, udp::endpoint(addr, port));
    }

    std::vector<std::thread> threads(std::thread::hardware_concurrency());

    for (auto& t : threads)
    {
        t = std::thread([&]{
            io.run();
        });
    }

    log->info("Zap running in port {} with {} threads", s.get_port(), threads.size());

    for (auto& t : threads)
    {
        t.join();
    }
}
