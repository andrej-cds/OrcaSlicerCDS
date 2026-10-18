#include "ControlServer.hpp"

#include <chrono>
#include <exception>
#include <fstream>
#include <future>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <wx/app.h>
#include <wx/string.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"

#include "Event.hpp"
#include "GLToolbar.hpp"
#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = boost::beast::http;
using tcp       = boost::asio::ip::tcp;
using json      = nlohmann::json;
namespace fs    = boost::filesystem;

namespace Slic3r { namespace GUI {

namespace {

struct ApiError : std::runtime_error
{
    int status;
    ApiError(int status, const std::string& msg) : std::runtime_error(msg), status(status) {}
};

std::string make_token()
{
    std::random_device rd;
    std::mt19937_64    gen((uint64_t(rd()) << 32) ^ rd());
    static const char* hex = "0123456789abcdef";
    std::string        out;
    for (int i = 0; i < 32; ++i)
        out.push_back(hex[gen() & 0xF]);
    return out;
}

bool const_time_equal(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

bool is_loopback_host(const std::string& host)
{
    return host.rfind("127.0.0.1", 0) == 0 || host.rfind("localhost", 0) == 0 || host.rfind("[::1]", 0) == 0;
}

std::string url_decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else if (s[i] == '+')
            out.push_back(' ');
        else
            out.push_back(s[i]);
    }
    return out;
}

std::string query_param(const std::string& query, const std::string& key)
{
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos)
            end = query.size();
        std::string kv = query.substr(pos, end - pos);
        size_t      eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key)
            return url_decode(kv.substr(eq + 1));
        pos = end + 1;
    }
    return {};
}

std::atomic<bool>* g_stopping = nullptr;

// Runs f on the GUI thread and waits for the result.
template<class F> auto run_ui(F f) -> decltype(f())
{
    using R   = decltype(f());
    auto prom = std::make_shared<std::promise<R>>();
    auto fut  = prom->get_future();
    wxTheApp->CallAfter([prom, f]() mutable {
        try {
            prom->set_value(f());
        } catch (...) {
            prom->set_exception(std::current_exception());
        }
    });
    for (int i = 0; i < 600; ++i) { // 120 s
        if (fut.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready)
            return fut.get();
        if (g_stopping && g_stopping->load())
            throw ApiError(503, "server is stopping");
    }
    throw ApiError(504, "the slicer did not answer in time (a dialog may be open)");
}

Preset::Type type_from_string(const std::string& t)
{
    if (t == "print" || t == "process")
        return Preset::TYPE_PRINT;
    if (t == "filament")
        return Preset::TYPE_FILAMENT;
    if (t == "printer")
        return Preset::TYPE_PRINTER;
    throw ApiError(400, "type must be print, filament or printer");
}

PresetCollection& collection_for(Preset::Type type)
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    switch (type) {
    case Preset::TYPE_PRINT: return pb->prints;
    case Preset::TYPE_FILAMENT: return pb->filaments;
    default: return pb->printers;
    }
}

json preset_list(Preset::Type type)
{
    PresetCollection& coll = collection_for(type);
    json              names = json::array();
    for (const Preset& p : coll)
        if (p.is_visible)
            names.push_back(p.name);
    return json{{"selected", coll.get_selected_preset_name()}, {"dirty", coll.get_edited_preset().is_dirty}, {"names", names}};
}

json cmd_status()
{
    Plater*    plater = wxGetApp().plater();
    json       out;
    out["app"]     = "OrcaSlicerCDS";
    out["version"] = SLIC3R_VERSION;
    if (!plater) {
        out["ready"] = false;
        return out;
    }
    PartPlateList& plates = plater->get_partplate_list();
    PartPlate*     cur    = plates.get_curr_plate();
    out["ready"]               = true;
    out["project"]             = plater->get_project_name().ToUTF8().data();
    out["project_dirty"]       = plater->is_project_dirty();
    out["plate_count"]         = plates.get_plate_count();
    out["current_plate"]       = plates.get_curr_plate_index();
    out["slice_valid"]         = cur ? cur->is_slice_result_valid() : false;
    out["gcode_file"]          = cur ? cur->get_gcode_filename() : std::string();
    out["objects_in_project"]  = plater->model().objects.size();
    out["printer_preset"]      = collection_for(Preset::TYPE_PRINTER).get_selected_preset_name();
    out["print_preset"]        = collection_for(Preset::TYPE_PRINT).get_selected_preset_name();
    out["filament_preset"]     = collection_for(Preset::TYPE_FILAMENT).get_selected_preset_name();
    return out;
}

json cmd_presets()
{
    return json{{"printer", preset_list(Preset::TYPE_PRINTER)},
                {"print", preset_list(Preset::TYPE_PRINT)},
                {"filament", preset_list(Preset::TYPE_FILAMENT)}};
}

json cmd_get_config(const std::string& keys_csv)
{
    if (keys_csv.empty())
        throw ApiError(400, "query parameter 'key' is required (comma separated option keys)");
    DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config();
    json               out = json::object();
    std::stringstream  ss(keys_csv);
    std::string        key;
    while (std::getline(ss, key, ',')) {
        if (key.empty())
            continue;
        out[key] = cfg.has(key) ? json(cfg.opt_serialize(key)) : json(nullptr);
    }
    return out;
}

json cmd_set_config(const json& body)
{
    if (!body.contains("changes") || !body["changes"].is_object())
        throw ApiError(400, "body must be {\"changes\": {\"option_key\": \"value\", ...}}");

    const Preset::Type types[] = {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER};
    json               applied = json::object();
    json               failed  = json::object();

    for (auto it = body["changes"].begin(); it != body["changes"].end(); ++it) {
        const std::string key   = it.key();
        const std::string value = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
        bool              done  = false;
        for (Preset::Type type : types) {
            PresetCollection& coll = collection_for(type);
            DynamicPrintConfig cfg  = coll.get_edited_preset().config;
            if (!cfg.has(key))
                continue;
            ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
            if (!cfg.set_deserialize_nothrow(key, value, ctx)) {
                failed[key] = "invalid value";
                done        = true;
                break;
            }
            Tab* tab = wxGetApp().get_tab(type);
            if (!tab) {
                failed[key] = "settings tab not available";
                done        = true;
                break;
            }
            tab->load_config(cfg);
            applied[key] = coll.get_edited_preset().config.opt_serialize(key);
            done         = true;
            break;
        }
        if (!done)
            failed[key] = "unknown option";
    }
    return json{{"applied", applied}, {"failed", failed}};
}

json cmd_select_preset(const json& body)
{
    if (!body.contains("type") || !body.contains("name"))
        throw ApiError(400, "body must be {\"type\": \"print|filament|printer\", \"name\": \"...\"}");
    Preset::Type type = type_from_string(body["type"].get<std::string>());
    std::string  name = body["name"].get<std::string>();
    Tab*         tab  = wxGetApp().get_tab(type);
    if (!tab)
        throw ApiError(500, "settings tab not available");
    if (!collection_for(type).find_preset(name, false))
        throw ApiError(404, "preset not found: " + name);
    bool ok = tab->select_preset(name);
    return json{{"ok", ok}, {"selected", collection_for(type).get_selected_preset_name()}};
}

json cmd_open(const json& body)
{
    if (!body.contains("path"))
        throw ApiError(400, "body must be {\"path\": \"C:\\\\...\\\\file.3mf\"}");
    const std::string path = body["path"].get<std::string>();
    if (!fs::exists(fs::path(path)))
        throw ApiError(404, "file not found: " + path);
    Plater* plater = wxGetApp().plater();
    if (!plater)
        throw ApiError(503, "slicer is not ready");
    std::string ext = fs::path(path).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".3mf") {
        plater->load_project(wxString::FromUTF8(path.c_str()));
        return json{{"ok", true}, {"opened_as", "project"}};
    }
    wxArrayString files;
    files.Add(wxString::FromUTF8(path.c_str()));
    bool ok = plater->load_files(files);
    return json{{"ok", ok}, {"opened_as", "model"}};
}

json cmd_select_plate(const json& body)
{
    Plater* plater = wxGetApp().plater();
    if (!plater || !body.contains("index"))
        throw ApiError(400, "body must be {\"index\": 0}");
    int index = body["index"].get<int>();
    if (index < 0 || index >= plater->get_partplate_list().get_plate_count())
        throw ApiError(400, "plate index out of range");
    plater->select_plate(index);
    return json{{"ok", true}, {"current_plate", plater->get_partplate_list().get_curr_plate_index()}};
}

json cmd_slice(const json& body)
{
    Plater* plater = wxGetApp().plater();
    if (!plater)
        throw ApiError(503, "slicer is not ready");
    const bool all = body.value("all", false);
    wxPostEvent(plater, SimpleEvent(all ? EVT_GLTOOLBAR_SLICE_ALL : EVT_GLTOOLBAR_SLICE_PLATE));
    return json{{"ok", true}, {"note", "slicing started; poll /status until slice_valid is true"}};
}

json cmd_export_gcode(const json& body)
{
    if (!body.contains("path"))
        throw ApiError(400, "body must be {\"path\": \"...\\\\out.gcode\"}");
    Plater* plater = wxGetApp().plater();
    if (!plater)
        throw ApiError(503, "slicer is not ready");
    PartPlate* cur = plater->get_partplate_list().get_curr_plate();
    if (!cur || !cur->is_slice_result_valid())
        throw ApiError(409, "current plate is not sliced");
    const std::string src = cur->get_gcode_filename();
    if (src.empty() || !fs::exists(fs::path(src)))
        throw ApiError(409, "sliced G-code file is not available");
    const fs::path dst(body["path"].get<std::string>());
    if (dst.has_parent_path())
        fs::create_directories(dst.parent_path());
    fs::copy_file(fs::path(src), dst, fs::copy_options::overwrite_existing);
    return json{{"ok", true}, {"path", dst.string()}, {"size", static_cast<uint64_t>(fs::file_size(dst))}};
}

json cmd_export_3mf(const json& body)
{
    if (!body.contains("path"))
        throw ApiError(400, "body must be {\"path\": \"...\\\\out.3mf\"}");
    Plater* plater = wxGetApp().plater();
    if (!plater)
        throw ApiError(503, "slicer is not ready");
    const fs::path dst(body["path"].get<std::string>());
    if (dst.has_parent_path())
        fs::create_directories(dst.parent_path());
    int rc = plater->export_3mf(dst);
    return json{{"ok", rc >= 0}, {"path", dst.string()}};
}

json cmd_send_to_printer()
{
    Plater* plater = wxGetApp().plater();
    if (!plater)
        throw ApiError(503, "slicer is not ready");
    // Never sends silently: this opens the regular "Send" dialog and the user must confirm it there.
    plater->send_to_printer(false);
    return json{{"ok", true}, {"note", "the Send dialog was opened; the user has to confirm the print in the slicer"}};
}

json dispatch(const std::string& method, const std::string& path, const std::string& query, const std::string& body_text)
{
    json body = json::object();
    if (method == "POST" && !body_text.empty()) {
        body = json::parse(body_text, nullptr, false);
        if (body.is_discarded() || !body.is_object())
            throw ApiError(400, "body is not a valid JSON object");
    }

    if (method == "GET" && path == "/status")
        return run_ui([] { return cmd_status(); });
    if (method == "GET" && path == "/presets")
        return run_ui([] { return cmd_presets(); });
    if (method == "GET" && path == "/config") {
        const std::string keys = query_param(query, "key");
        return run_ui([keys] { return cmd_get_config(keys); });
    }
    if (method == "POST") {
        if (path == "/config")
            return run_ui([body] { return cmd_set_config(body); });
        if (path == "/select_preset")
            return run_ui([body] { return cmd_select_preset(body); });
        if (path == "/open")
            return run_ui([body] { return cmd_open(body); });
        if (path == "/select_plate")
            return run_ui([body] { return cmd_select_plate(body); });
        if (path == "/slice")
            return run_ui([body] { return cmd_slice(body); });
        if (path == "/export_gcode")
            return run_ui([body] { return cmd_export_gcode(body); });
        if (path == "/export_3mf")
            return run_ui([body] { return cmd_export_3mf(body); });
        if (path == "/send_to_printer")
            return run_ui([] { return cmd_send_to_printer(); });
    }
    throw ApiError(404, "unknown endpoint: " + method + " " + path);
}

} // namespace

struct ControlServer::Impl
{
    asio::io_context io;
    tcp::acceptor    acceptor{io};
};

ControlServer::ControlServer() = default;

ControlServer::~ControlServer()
{
    stop();
}

bool ControlServer::start()
{
    if (m_running)
        return true;

    m_impl = std::make_unique<Impl>();
    m_token = make_token();

    boost::system::error_code ec;
    for (unsigned short p = DEFAULT_PORT; p < DEFAULT_PORT + 10; ++p) {
        tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), p);
        m_impl->acceptor.open(ep.protocol(), ec);
        if (ec)
            break;
        m_impl->acceptor.set_option(asio::socket_base::reuse_address(false), ec);
        m_impl->acceptor.bind(ep, ec);
        if (!ec) {
            m_impl->acceptor.listen(asio::socket_base::max_listen_connections, ec);
            if (!ec) {
                m_port = p;
                break;
            }
        }
        m_impl->acceptor.close(ec);
    }
    if (m_port == 0) {
        BOOST_LOG_TRIVIAL(error) << "ControlServer: could not bind a local port";
        m_impl.reset();
        return false;
    }

    // Publish port and token for local tools.
    m_info_path = (fs::path(data_dir()) / "control_server.json").string();
    try {
        std::ofstream f(m_info_path, std::ios::trunc);
        f << json{{"port", m_port}, {"token", m_token}}.dump(2);
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "ControlServer: could not write " << m_info_path;
    }

    static std::atomic<bool> stopping;
    stopping    = false;
    g_stopping  = &stopping;
    m_running   = true;

    Impl*             impl  = m_impl.get();
    const std::string token = m_token;

    m_thread = std::thread([impl, token, this]() {
        while (m_running) {
            boost::system::error_code ec;
            tcp::socket               socket(impl->io);
            impl->acceptor.accept(socket, ec);
            if (ec) {
                if (!m_running)
                    break;
                continue;
            }
            try {
                beast::tcp_stream stream(std::move(socket));
                stream.expires_after(std::chrono::seconds(15));
                beast::flat_buffer                  buffer;
                http::request_parser<http::string_body> parser;
                parser.body_limit(1024 * 1024);
                http::read(stream, buffer, parser, ec);
                if (ec)
                    continue;
                http::request<http::string_body>& req = parser.get();

                http::response<http::string_body> res;
                res.version(req.version());
                res.set(http::field::content_type, "application/json; charset=utf-8");
                res.set(http::field::cache_control, "no-store");
                res.keep_alive(false);

                json out;
                int  status = 200;
                try {
                    const std::string host(req.base()["Host"]);
                    if (!is_loopback_host(host) || req.base().find(http::field::origin) != req.base().end())
                        throw ApiError(403, "forbidden");
                    std::string auth(req.base()["Authorization"]);
                    if (auth.rfind("Bearer ", 0) != 0 || !const_time_equal(auth.substr(7), token))
                        throw ApiError(401, "missing or wrong token");

                    std::string target(req.target());
                    std::string path  = target;
                    std::string query;
                    size_t      q = target.find('?');
                    if (q != std::string::npos) {
                        path  = target.substr(0, q);
                        query = target.substr(q + 1);
                    }
                    out = dispatch(std::string(req.method_string()), path, query, req.body());
                } catch (const ApiError& e) {
                    status = e.status;
                    out    = json{{"error", e.what()}};
                } catch (const std::exception& e) {
                    status = 500;
                    out    = json{{"error", e.what()}};
                }
                res.result(static_cast<http::status>(status));
                res.body() = out.dump();
                res.prepare_payload();
                http::write(stream, res, ec);
                stream.socket().shutdown(tcp::socket::shutdown_send, ec);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "ControlServer: request failed: " << e.what();
            }
        }
    });

    BOOST_LOG_TRIVIAL(info) << "ControlServer: listening on 127.0.0.1:" << m_port;
    return true;
}

void ControlServer::stop()
{
    if (!m_running && !m_thread.joinable())
        return;
    m_running = false;
    if (g_stopping)
        g_stopping->store(true);
    if (m_impl) {
        boost::system::error_code ec;
        m_impl->acceptor.close(ec);
    }
    if (m_thread.joinable())
        m_thread.join();
    m_impl.reset();
    if (!m_info_path.empty()) {
        boost::system::error_code ec;
        fs::remove(fs::path(m_info_path), ec);
    }
}

}} // namespace Slic3r::GUI
