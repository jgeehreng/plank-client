#pragma once

#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Official Wacom user agents hold USB HID exclusively. Pause them only after
// an ExactRawHid tablet is found for a live stream, then start the agents
// this Client paused. Restore even when bootout failed, because leftover
// driver processes may already have been terminated.
namespace MacWacomVendorDriver {
inline constexpr std::array<std::string_view, 3> kUserAgentPlists{
    "/Library/LaunchAgents/com.wacom.wacomtablet.plist",
    "/Library/LaunchAgents/com.wacom.IOManager.plist",
    "/Library/LaunchAgents/com.wacom.DataStoreMgr.plist",
};
inline constexpr std::array<std::string_view, 3> kProcessNames{
    "WacomTabletDriver",
    "WacomTouchDriver",
    "TabletDriver",
};

inline std::string_view labelFromPlist(std::string_view path)
{
    const auto slash = path.rfind('/');
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    constexpr std::string_view suffix = ".plist";
    if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix)
        return name.substr(0, name.size() - suffix.size());
    return name;
}

inline std::string_view plistForLabel(std::string_view label)
{
    for (const auto plist : kUserAgentPlists) {
        if (labelFromPlist(plist) == label) return plist;
    }
    return {};
}

inline bool isTrackedLabel(std::string_view label)
{
    return !plistForLabel(label).empty();
}

struct Tools {
    std::function<std::string()> domain;
    std::function<bool(std::string_view plist)> plistExists;
    std::function<int(const std::vector<std::string>& args)> launchctl;
    std::function<void(const std::vector<std::string>& names)> terminateProcesses;
};

class Hold {
public:
    explicit Hold(Tools tools) : m_Tools(std::move(tools)) {}

    bool held() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Held;
    }

    std::vector<std::string> stoppedPlists() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Stopped;
    }

    void pause()
    {
        std::vector<std::string> names;
        names.reserve(kProcessNames.size());
        for (const auto name : kProcessNames) names.emplace_back(name);
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_Held) {
                const auto domain = m_Tools.domain ? m_Tools.domain() : std::string();
                for (const auto plist : kUserAgentPlists) {
                    if (m_Tools.plistExists && !m_Tools.plistExists(plist)) continue;
                    if (m_Tools.launchctl)
                        m_Tools.launchctl({"bootout", domain, std::string(plist)});
                    m_Stopped.emplace_back(plist);
                }
                m_Held = true;
            }
        }
        if (m_Tools.terminateProcesses) m_Tools.terminateProcesses(names);
    }

    void restore()
    {
        std::vector<std::string> stopped;
        std::string domain;
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            if (!m_Held) return;
            stopped.swap(m_Stopped);
            domain = m_Tools.domain ? m_Tools.domain() : std::string();
            m_Held = false;
        }
        if (!m_Tools.launchctl) return;
        for (const auto& plist : stopped)
            m_Tools.launchctl({"bootstrap", domain, plist});
    }

private:
    Tools m_Tools;
    mutable std::mutex m_Mutex;
    std::vector<std::string> m_Stopped;
    bool m_Held = false;
};
}
