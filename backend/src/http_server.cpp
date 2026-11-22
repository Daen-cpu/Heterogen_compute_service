#include "hetero/http_server.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <iostream>
namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
namespace hetero
{
static http::response<http::string_body> handle(Controller& c,
                                                const http::request<http::string_body>& req)
{
    http::response<http::string_body> res{http::status::ok, req.version()};
    res.set(http::field::server, "HeteroCompute/0.2");
    res.set(http::field::content_type, "application/json");
    res.set(http::field::access_control_allow_origin, "*");
    res.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    res.set(http::field::access_control_allow_headers, "Content-Type");
    res.keep_alive(req.keep_alive());
    try
    {
        if (req.method() == http::verb::options)
        {
            res.result(http::status::no_content);
            return res;
        }
        nlohmann::json body =
            req.body().empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body());
        const std::string path(req.target());
        if (req.method() == http::verb::get && path == "/api/state")
            res.body() = c.state().dump();
        else if (req.method() == http::verb::post && path == "/api/configure")
        {
            c.configure(body);
            res.body() = c.state().dump();
        }
        else if (req.method() == http::verb::post && path == "/api/start")
        {
            c.start();
            res.body() = c.state().dump();
        }
        else if (req.method() == http::verb::post && path == "/api/pause")
        {
            c.pause();
            res.body() = c.state().dump();
        }
        else if (req.method() == http::verb::post && path == "/api/resume")
        {
            c.resume();
            res.body() = c.state().dump();
        }
        else if (req.method() == http::verb::post && path == "/api/reset")
        {
            c.reset();
            res.body() = c.state().dump();
        }
        else if (req.method() == http::verb::post && path == "/api/policy")
        {
            c.set_policy(body);
            res.body() = c.state().dump();
        }
        else if (req.method() == http::verb::post && path == "/api/worker")
        {
            c.set_worker(body);
            res.body() = c.state().dump();
        }
        else
        {
            res.result(http::status::not_found);
            res.body() = R"({"error":"not found"})";
        }
    }
    catch (const std::exception& e)
    {
        res.result(http::status::bad_request);
        res.body() = nlohmann::json{{"error", e.what()}}.dump();
    }
    res.prepare_payload();
    return res;
}
void run_http_server(Controller& c, unsigned short port)
{
    net::io_context ioc{1};
    tcp::acceptor acceptor{ioc, {tcp::v4(), port}};
    std::cout << "HeteroCompute API listening on http://127.0.0.1:" << port << "\n";
    for (;;)
    {
        tcp::socket socket{ioc};
        acceptor.accept(socket);
        std::thread(
            [&c, s = std::move(socket)]() mutable
            {
                beast::flat_buffer buffer;
                beast::error_code ec;
                while (!ec)
                {
                    http::request<http::string_body> req;
                    http::read(s, buffer, req, ec);
                    if (ec)
                        break;
                    auto res = handle(c, req);
                    http::write(s, res, ec);
                    if (ec || !res.keep_alive())
                        break;
                }
                s.shutdown(tcp::socket::shutdown_send, ec);
            })
            .detach();
    }
}
} // namespace hetero
