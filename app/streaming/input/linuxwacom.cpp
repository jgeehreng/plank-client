#include "linuxwacom.h"

#include <Limelight.h>
#include <SDL3/SDL.h>
#include <libinput.h>
#include <libudev.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

const unsigned short kUnknownRotation = LI_ROT_UNKNOWN;
const unsigned char kUnknownTilt = LI_TILT_UNKNOWN;
const unsigned int kWacomVendorId = 0x056a;

bool propertyIsSet(udev_device* device, const char* name)
{
    const char* value = udev_device_get_property_value(device, name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

void encodeTilt(double tiltX, double tiltY, unsigned short& rotation,
                unsigned char& tilt)
{
    const double pi = std::acos(-1.0);
    const double x = std::tan(tiltX * pi / 180.0);
    const double y = std::tan(tiltY * pi / 180.0);
    const double magnitude = std::atan(std::hypot(x, y)) * 180.0 / pi;
    double direction = -std::atan2(x, y) * 180.0 / pi;
    if (direction < 0.0) {
        direction += 360.0;
    }

    tilt = static_cast<unsigned char>(std::lround(
        std::max(0.0, std::min(90.0, magnitude))));
    rotation = static_cast<unsigned short>(std::lround(direction)) % 360;
}

unsigned char toolType(libinput_tablet_tool* tool)
{
    if (tool != nullptr &&
            libinput_tablet_tool_get_type(tool) == LIBINPUT_TABLET_TOOL_TYPE_ERASER) {
        return LI_TOOL_TYPE_ERASER;
    }
    return LI_TOOL_TYPE_PEN;
}

} // namespace

LinuxWacomInput::LinuxWacomInput(std::function<void()> tabletActivity)
    : m_Active(false),
      m_Stopping(false),
      m_TipDown(false),
      m_ToolType(LI_TOOL_TYPE_PEN),
      m_Buttons(0),
      m_X(0.0f),
      m_Y(0.0f),
      m_Pressure(0.0f),
      m_Distance(0.0f),
      m_Rotation(kUnknownRotation),
      m_Tilt(kUnknownTilt),
      m_TabletActivity(std::move(tabletActivity))
{
    m_Thread = std::thread(&LinuxWacomInput::run, this);
}

LinuxWacomInput::~LinuxWacomInput()
{
    setActive(false);
    m_Stopping.store(true);
    if (m_Thread.joinable()) {
        m_Thread.join();
    }
}

void LinuxWacomInput::setActive(bool active)
{
    const bool changed = m_Active.exchange(active) != active;
    if (!changed) {
        return;
    }

    if (!active) {
        std::lock_guard<std::mutex> stateLock(m_StateMutex);
        cancelRemotePen();
    }

    std::lock_guard<std::mutex> lock(m_DeviceMutex);
    for (int fd : m_PenFds) {
        updateGrab(fd, active);
    }
}

int LinuxWacomInput::openRestricted(const char* path, int flags, void* userData)
{
    LinuxWacomInput* self = static_cast<LinuxWacomInput*>(userData);
    const int fd = open(path, flags | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return -errno;
    }

    if (self->isWacomPenNode(fd)) {
        std::lock_guard<std::mutex> lock(self->m_DeviceMutex);
        self->m_PenFds.push_back(fd);
        self->updateGrab(fd, self->m_Active.load());
    }
    return fd;
}

void LinuxWacomInput::closeRestricted(int fd, void* userData)
{
    LinuxWacomInput* self = static_cast<LinuxWacomInput*>(userData);
    {
        std::lock_guard<std::mutex> lock(self->m_DeviceMutex);
        std::vector<int>::iterator it =
            std::find(self->m_PenFds.begin(), self->m_PenFds.end(), fd);
        if (it != self->m_PenFds.end()) {
            self->updateGrab(fd, false);
            self->m_PenFds.erase(it);
        }
    }
    close(fd);
}

bool LinuxWacomInput::isWacomPenNode(int fd) const
{
    struct stat nodeStat;
    if (fstat(fd, &nodeStat) != 0) {
        return false;
    }

    udev* udevContext = udev_new();
    if (udevContext == nullptr) {
        return false;
    }
    udev_device* device =
        udev_device_new_from_devnum(udevContext, 'c', nodeStat.st_rdev);
    if (device == nullptr) {
        udev_unref(udevContext);
        return false;
    }

    const char* vendor = udev_device_get_property_value(device, "ID_VENDOR_ID");
    char* end = nullptr;
    const unsigned long vendorId = vendor != nullptr ? std::strtoul(vendor, &end, 16) : 0;
    const bool isWacom = vendor != nullptr && end != vendor && *end == '\0' &&
                         vendorId == kWacomVendorId;
    const bool isPen = propertyIsSet(device, "ID_INPUT_TABLET") &&
                       !propertyIsSet(device, "ID_INPUT_TABLET_PAD") &&
                       !propertyIsSet(device, "ID_INPUT_TOUCHPAD");

    udev_device_unref(device);
    udev_unref(udevContext);
    return isWacom && isPen;
}

void LinuxWacomInput::updateGrab(int fd, bool grabbed)
{
    const int value = grabbed ? 1 : 0;
    if (ioctl(fd, EVIOCGRAB, value) < 0 && errno != ENODEV) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Unable to %s Wacom tablet node: %s",
                    grabbed ? "grab" : "release", std::strerror(errno));
    }
}

void LinuxWacomInput::cancelRemotePen()
{
    LiSendPenEvent(LI_TOUCH_EVENT_CANCEL_ALL, LI_TOOL_TYPE_UNKNOWN, 0,
                   0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                   LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
}

void LinuxWacomInput::run()
{
    const libinput_interface interface = {
        &LinuxWacomInput::openRestricted,
        &LinuxWacomInput::closeRestricted,
    };
    udev* udevContext = udev_new();
    if (udevContext == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_INPUT, "Unable to create udev context for Wacom input");
        return;
    }

    libinput* input = libinput_udev_create_context(&interface, this, udevContext);
    if (input == nullptr || libinput_udev_assign_seat(input, "seat0") != 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "Wacom input unavailable; check PLANK input-device permissions");
        if (input != nullptr) {
            libinput_unref(input);
        }
        udev_unref(udevContext);
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "Linux Wacom tablet capture initialized");
    libinput_dispatch(input);
    while (!m_Stopping.load()) {
        pollfd descriptor = {libinput_get_fd(input), POLLIN, 0};
        const int result = poll(&descriptor, 1, 100);
        if (result < 0 && errno != EINTR) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Wacom input poll failed: %s",
                        std::strerror(errno));
            break;
        }
        if (result <= 0 || (descriptor.revents & POLLIN) == 0) {
            continue;
        }
        if (libinput_dispatch(input) != 0) {
            break;
        }

        libinput_event* event = nullptr;
        while ((event = libinput_get_event(input)) != nullptr) {
            const libinput_event_type type = libinput_event_get_type(event);
            if (type >= LIBINPUT_EVENT_TABLET_TOOL_AXIS &&
                    type <= LIBINPUT_EVENT_TABLET_TOOL_BUTTON) {
                libinput_event_tablet_tool* tabletEvent =
                    libinput_event_get_tablet_tool_event(event);
                libinput_device* device = libinput_event_get_device(event);
                if (libinput_device_get_id_vendor(device) == kWacomVendorId) {
                    handleTabletEvent(tabletEvent, static_cast<unsigned int>(type));
                }
            }
            libinput_event_destroy(event);
        }
    }

    libinput_unref(input);
    udev_unref(udevContext);
}

void LinuxWacomInput::handleTabletEvent(libinput_event_tablet_tool* event,
                                        unsigned int rawType)
{
    std::lock_guard<std::mutex> stateLock(m_StateMutex);
    const libinput_event_type type = static_cast<libinput_event_type>(rawType);
    libinput_tablet_tool* tool = libinput_event_tablet_tool_get_tool(event);
    m_ToolType = toolType(tool);

    if (libinput_event_tablet_tool_x_has_changed(event) ||
            type == LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY) {
        m_X = static_cast<float>(
            libinput_event_tablet_tool_get_x_transformed(event, 65535) / 65535.0);
    }
    if (libinput_event_tablet_tool_y_has_changed(event) ||
            type == LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY) {
        m_Y = static_cast<float>(
            libinput_event_tablet_tool_get_y_transformed(event, 65535) / 65535.0);
    }
    if (libinput_event_tablet_tool_pressure_has_changed(event)) {
        m_Pressure = static_cast<float>(libinput_event_tablet_tool_get_pressure(event));
    }
    if (libinput_event_tablet_tool_distance_has_changed(event)) {
        m_Distance = static_cast<float>(libinput_event_tablet_tool_get_distance(event));
    }
    if (libinput_event_tablet_tool_tilt_x_has_changed(event) ||
            libinput_event_tablet_tool_tilt_y_has_changed(event)) {
        encodeTilt(libinput_event_tablet_tool_get_tilt_x(event),
                   libinput_event_tablet_tool_get_tilt_y(event),
                   m_Rotation, m_Tilt);
    }

    unsigned char eventType = m_TipDown ? LI_TOUCH_EVENT_MOVE : LI_TOUCH_EVENT_HOVER;
    if (type == LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY) {
        if (libinput_event_tablet_tool_get_proximity_state(event) ==
                LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_OUT) {
            eventType = LI_TOUCH_EVENT_HOVER_LEAVE;
        }
        else {
            eventType = LI_TOUCH_EVENT_HOVER;
        }
    }
    else if (type == LIBINPUT_EVENT_TABLET_TOOL_TIP) {
        m_TipDown = libinput_event_tablet_tool_get_tip_state(event) ==
                    LIBINPUT_TABLET_TOOL_TIP_DOWN;
        eventType = m_TipDown ? LI_TOUCH_EVENT_DOWN : LI_TOUCH_EVENT_UP;
    }
    else if (type == LIBINPUT_EVENT_TABLET_TOOL_BUTTON) {
        unsigned char mask = 0;
        switch (libinput_event_tablet_tool_get_button(event)) {
        case BTN_STYLUS:
            mask = LI_PEN_BUTTON_PRIMARY;
            break;
        case BTN_STYLUS2:
            mask = LI_PEN_BUTTON_SECONDARY;
            break;
        case BTN_STYLUS3:
            mask = LI_PEN_BUTTON_TERTIARY;
            break;
        default:
            return;
        }
        if (libinput_event_tablet_tool_get_button_state(event) ==
                LIBINPUT_BUTTON_STATE_PRESSED) {
            m_Buttons |= mask;
        }
        else {
            m_Buttons &= static_cast<unsigned char>(~mask);
        }
        eventType = LI_TOUCH_EVENT_BUTTON_ONLY;
    }

    if (m_Active.load()) {
        const float pressureOrDistance = m_TipDown ? m_Pressure : m_Distance;
        LiSendPenEvent(eventType, m_ToolType, m_Buttons,
                       std::max(0.0f, std::min(1.0f, m_X)),
                       std::max(0.0f, std::min(1.0f, m_Y)),
                       std::max(0.0f, std::min(1.0f, pressureOrDistance)),
                       0.0f, 0.0f, m_Rotation, m_Tilt);
        if (m_TabletActivity && eventType != LI_TOUCH_EVENT_HOVER_LEAVE) {
            m_TabletActivity();
        }
    }

    if (eventType == LI_TOUCH_EVENT_HOVER_LEAVE) {
        m_TipDown = false;
        m_Buttons = 0;
        m_Rotation = kUnknownRotation;
        m_Tilt = kUnknownTilt;
    }
}
