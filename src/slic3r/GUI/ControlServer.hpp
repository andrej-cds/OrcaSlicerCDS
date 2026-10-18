#ifndef slic3r_ControlServer_hpp_
#define slic3r_ControlServer_hpp_

// OrcaSlicerCDS: local control server.
//
// A small JSON-over-HTTP server that lets a local tool (for example an MCP bridge used by Claude)
// drive the running slicer: query state, open projects, change presets and settings, slice and
// export. It listens on 127.0.0.1 only, every request needs the bearer token that is written to
// <data_dir>/control_server.json at start, and the Host header must be a loopback name (this blocks
// DNS-rebinding from web pages). Sending a print to a printer is NEVER done silently: the command
// only opens the normal "Send" dialog and the user has to confirm it.

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace Slic3r { namespace GUI {

class ControlServer
{
public:
    static constexpr unsigned short DEFAULT_PORT = 13620;

    ControlServer()  = default;
    ~ControlServer() { stop(); }

    // Starts listening (tries DEFAULT_PORT and the next few ports). Returns false on failure.
    bool start();
    void stop();
    bool is_running() const { return m_running; }
    unsigned short port() const { return m_port; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::thread           m_thread;
    std::atomic<bool>     m_running{false};
    unsigned short        m_port{0};
    std::string           m_token;
    std::string           m_info_path;
};

}} // namespace Slic3r::GUI

#endif
