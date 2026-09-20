QT += core quick network quickcontrols2 svg
CONFIG += c++17

unix:contains(CONFIG, plank-transport) {
    isEmpty(PLANK_TRANSPORT_DIR) {
        PLANK_TRANSPORT_DIR = $$(PLANK_TRANSPORT_DIR)
    }
    isEmpty(PLANK_TRANSPORT_DIR) {
        PLANK_TRANSPORT_DIR = $$clean_path($$PWD/../../../protocol/plank-transport)
    }
    !exists($$PLANK_TRANSPORT_DIR/Cargo.toml) {
        error("PLANK native transport Cargo.toml is missing: $$PLANK_TRANSPORT_DIR")
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport.h) {
        error("PLANK native transport header is missing: $$PLANK_TRANSPORT_DIR")
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport_control.h) {
        error("PLANK transport control header is missing: $$PLANK_TRANSPORT_DIR")
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport_input.h) {
        error("PLANK transport input header is missing: $$PLANK_TRANSPORT_DIR")
    }
    !exists($$PLANK_TRANSPORT_DIR/include/plank_transport_event.h) {
        error("PLANK transport event header is missing: $$PLANK_TRANSPORT_DIR")
    }

    PLANK_CARGO = $$(CARGO)
    isEmpty(PLANK_CARGO): PLANK_CARGO = cargo
    PLANK_TRANSPORT_CARGO_TARGET_DIR = $$OUT_PWD/plank-transport-cargo
    PLANK_TRANSPORT_LIBRARY = $$PLANK_TRANSPORT_CARGO_TARGET_DIR/release/libplank_transport.a

    plank_transport.target = $$PLANK_TRANSPORT_LIBRARY
    plank_transport.depends = FORCE
    plank_transport.commands = \
        CARGO_TARGET_DIR=$$shell_quote($$PLANK_TRANSPORT_CARGO_TARGET_DIR) \
        $$shell_quote($$PLANK_CARGO) build --locked --offline --release \
        --manifest-path $$shell_quote($$PLANK_TRANSPORT_DIR/Cargo.toml)
    QMAKE_EXTRA_TARGETS += plank_transport
    PRE_TARGETDEPS += $$PLANK_TRANSPORT_LIBRARY
    QMAKE_CLEAN += $$PLANK_TRANSPORT_CARGO_TARGET_DIR

    INCLUDEPATH += $$PLANK_TRANSPORT_DIR/include
    LIBS += $$PLANK_TRANSPORT_LIBRARY -ldl -lpthread -lm
    !macx: LIBS += -lrt
    macx: LIBS += -framework Security -framework SystemConfiguration
    DEFINES += PLANK_TRANSPORT=1
}

TARGET = plank-client

include(../globaldefs.pri)

# Precompile QML files to avoid writing qmlcache on portable versions.
# Since this binds the app against the Qt runtime version, we will only
# do this for Windows and Mac (when disable-prebuilts is not defined),
# since they always ship with the matching build of the Qt runtime.
!disable-prebuilts {
    win32|macx {
        CONFIG(release, debug|release) {
            CONFIG += qtquickcompiler
        }
    }
}

TEMPLATE = app

# The following define makes your compiler emit warnings if you use
# any feature of Qt which has been marked as deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS
contains(CONFIG, plank-frame-flow-trace): DEFINES += PLANK_FRAME_FLOW_TRACE

# You can also make your code fail to compile if you use deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

win32 {
    !exists($$PWD/../libs/windows) {
        error("Missing dependencies. Please run 'powershell .\setup-deps.ps1' to fetch prebuilt libraries.")
    }

    contains(QT_ARCH, x86_64) {
        LIBS += -L$$PWD/../libs/windows/lib/x64
        INCLUDEPATH += $$PWD/../libs/windows/include/x64 $$PWD/../libs/windows/include/x64/SDL3
    }
    contains(QT_ARCH, arm64) {
        LIBS += -L$$PWD/../libs/windows/lib/arm64
        INCLUDEPATH += $$PWD/../libs/windows/include/arm64 $$PWD/../libs/windows/include/arm64/SDL3
    }

    INCLUDEPATH += $$PWD/../libs/windows/include
    LIBS += ws2_32.lib winmm.lib dxva2.lib ole32.lib gdi32.lib user32.lib d3d9.lib dwmapi.lib dbghelp.lib
}
macx:!disable-prebuilts {
    !exists($$PWD/../libs/mac) {
        error("Missing dependencies. Please run 'python3 setup-deps.py' to fetch prebuilt libraries.")
    }

    INCLUDEPATH += $$PWD/../libs/mac/include $$PWD/../libs/mac/include/SDL3
    LIBS += -L$$PWD/../libs/mac/lib
}

unix:if(!macx|disable-prebuilts) {
    CONFIG += link_pkgconfig
    PKGCONFIG += openssl sdl3 sdl3-ttf

    # We have our own optimized libopus.a for Steam Link
    if(!config_SL|disable-prebuilts) {
        PKGCONFIG += opus
    }

    !disable-ffmpeg {
        packagesExist(libavcodec) {
            PKGCONFIG += libavcodec libavutil libswscale libswresample
            CONFIG += ffmpeg

            !disable-libva {
                packagesExist(libva) {
                    !disable-x11 {
                        packagesExist(libva-x11) {
                            CONFIG += libva-x11
                        }
                    }
                    !disable-wayland {
                        packagesExist(libva-wayland) {
                            CONFIG += libva-wayland
                        }
                    }
                    !disable-libdrm {
                        packagesExist(libva-drm) {
                            CONFIG += libva-drm
                        }
                    }
                    CONFIG += libva
                }
            }

            !disable-libvdpau {
                packagesExist(vdpau) {
                    CONFIG += libvdpau
                }
            }

            !disable-mmal {
                packagesExist(mmal) {
                    PKGCONFIG += mmal
                    CONFIG += mmal
                }
            }

            !disable-libdrm {
                packagesExist(libdrm) {
                    PKGCONFIG += libdrm
                    CONFIG += libdrm
                }
            }

            # Disabled by default due to reliability issues. See #1314.
            # CUDA interop is superseded by VDPAU and Vulkan Video.
            enable-cuda {
                packagesExist(ffnvcodec) {
                    PKGCONFIG += ffnvcodec
                    CONFIG += cuda
                }
            }

            !disable-libplacebo {
                packagesExist(libplacebo) {
                    PKGCONFIG += libplacebo
                    CONFIG += libplacebo
                }
            }
        }

        !disable-wayland {
            packagesExist(wayland-client) {
                CONFIG += wayland
                PKGCONFIG += wayland-client
            }
        }

        !disable-x11 {
            packagesExist(x11) {
                DEFINES += HAS_X11
                PKGCONFIG += x11
            }
        }
    }
}

linux:packagesExist(libinput):packagesExist(libudev) {
    DEFINES += HAVE_LIBINPUT_TABLET
    PKGCONFIG += libinput libudev
}

win32 {
    LIBS += -llibssl -llibcrypto -lSDL3 -lSDL3_ttf -lavcodec -lavutil -lswscale -lopus -ldxgi -ld3d11 -llibplacebo
    CONFIG += ffmpeg libplacebo
}
win32:!winrt {
}
macx {
    !disable-prebuilts {
        LIBS += -lssl.3 -lcrypto.3 -lavcodec.63 -lavutil.61 -lswscale.10 -lopus.0 -lSDL3 -lSDL3_ttf -lplacebo
        CONFIG += libplacebo
    }

    LIBS += -lobjc -framework VideoToolbox -framework AVFoundation -framework CoreVideo -framework CoreGraphics -framework CoreMedia -framework AppKit -framework Metal -framework QuartzCore
    CONFIG += ffmpeg
}

SOURCES += \
    backend/nvaddress.cpp \
    backend/outputtopology.cpp \
    backend/nvapp.cpp \
    main.cpp \
    backend/computerseeker.cpp \
    backend/nvcomputer.cpp \
    backend/nvhttp.cpp \
    backend/computermanager.cpp \
    backend/relaywakeclient.cpp \
    cli/commandlineparser.cpp \
    cli/startstream.cpp \
    settings/plankclientpolicy.cpp \
    settings/streamingpreferences.cpp \
    streaming/input/input.cpp \
    streaming/input/keyboard.cpp \
    streaming/input/mouse.cpp \
    streaming/session.cpp \
    streaming/avsynccontroller.cpp \
    streaming/plankdisplaymode.cpp \
    streaming/plankpresentation.cpp \
    streaming/planktoolbar.cpp \
    streaming/plankwaylandcursor.cpp \
    streaming/plankwaylandtoolbar.cpp \
    streaming/audio/audio.cpp \
    streaming/audio/renderers/sdlaud.cpp \
    gui/computermodel.cpp \
    streaming/bandwidth.cpp \
    streaming/streamutils.cpp \
    path.cpp \
    streaming/video/overlaymanager.cpp \
    backend/systemproperties.cpp \
    wm.cpp

macx: HEADERS += macapplication.h

HEADERS += \
    streaming/video/packedbt709.h \
    backend/nvaddress.h \
    backend/outputtopology.h \
    backend/nvapp.h \
    utils.h \
    backend/computerseeker.h \
    backend/nvcomputer.h \
    backend/planknetwork.h \
    backend/nvhttp.h \
    backend/desktopstage.h \
    backend/hostrecovery.h \
    backend/computermanager.h \
    backend/relaywakeclient.h \
    cli/commandlineparser.h \
    cli/startstream.h \
    settings/streamingpreferences.h \
    settings/plankclientpolicy.h \
    streaming/avsynccontroller.h \
    streaming/input/input.h \
    streaming/input/plankpointerlogic.h \
    streaming/input/plankmousemotion.h \
    streaming/session.h \
    streaming/plankdisplaymode.h \
    streaming/plankpresentation.h \
    streaming/planktoolbar.h \
    streaming/planktoolbarlogic.h \
    streaming/planktoolbarstats.h \
    streaming/plankreconnectpolicy.h \
    streaming/audio/renderers/renderer.h \
    streaming/audio/renderers/sdl.h \
    gui/computermodel.h \
    streaming/video/decoder.h \
    streaming/bandwidth.h \
    streaming/streamutils.h \
    path.h \
    streaming/video/overlaymanager.h \
    backend/systemproperties.h

contains(DEFINES, HAVE_LIBINPUT_TABLET) {
    SOURCES += streaming/input/linuxwacom.cpp
    HEADERS += streaming/input/linuxwacom.h
    SOURCES += streaming/input/linuxrawwacom.cpp
    HEADERS += streaming/input/linuxrawwacom.h
}

# Platform-specific renderers and decoders
ffmpeg {
    message(FFmpeg decoder selected)

    DEFINES += HAVE_FFMPEG
    SOURCES += \
        streaming/video/ffmpeg.cpp \
        streaming/video/ffmpeg-renderers/genhwaccel.cpp \
        streaming/video/ffmpeg-renderers/sdlvid.cpp \
        streaming/video/ffmpeg-renderers/swframemapper.cpp \
        streaming/video/ffmpeg-renderers/pacer/pacer.cpp

    HEADERS += \
        streaming/video/ffmpeg.h \
        streaming/video/ffmpeg-renderers/renderer.h \
        streaming/video/ffmpeg-renderers/genhwaccel.h \
        streaming/video/ffmpeg-renderers/sdlvid.h \
        streaming/video/ffmpeg-renderers/swframemapper.h \
        streaming/video/ffmpeg-renderers/pacer/pacer.h
}
libva {
    message(VAAPI renderer selected)

    PKGCONFIG += libva
    DEFINES += HAVE_LIBVA
    SOURCES += streaming/video/ffmpeg-renderers/vaapi.cpp
    HEADERS += streaming/video/ffmpeg-renderers/vaapi.h
}
libva-x11 {
    message(VAAPI X11 support enabled)

    PKGCONFIG += libva-x11
    DEFINES += HAVE_LIBVA_X11
}
libva-wayland {
    message(VAAPI Wayland support enabled)

    PKGCONFIG += libva-wayland
    DEFINES += HAVE_LIBVA_WAYLAND
}
libva-drm {
    message(VAAPI DRM support enabled)

    PKGCONFIG += libva-drm
    DEFINES += HAVE_LIBVA_DRM
}
libvdpau {
    message(VDPAU renderer selected)

    DEFINES += HAVE_LIBVDPAU
    SOURCES += streaming/video/ffmpeg-renderers/vdpau.cpp
    HEADERS += streaming/video/ffmpeg-renderers/vdpau.h
}
mmal {
    message(MMAL renderer selected)

    DEFINES += HAVE_MMAL
    SOURCES += streaming/video/ffmpeg-renderers/mmal.cpp
    HEADERS += streaming/video/ffmpeg-renderers/mmal.h

    # We suppress EGL usage when MMAL is available because MMAL has
    # significantly better performance than EGL on the Pi. Setting
    # this option allows EGL usage even if built with MMAL support.
    #
    # It is highly recommended to also build with 'gpuslow' to avoid
    # EGL being preferred if direct DRM rendering is available.
    allow-egl-with-mmal {
        message(Allowing EGL usage with MMAL enabled)

        DEFINES += ALLOW_EGL_WITH_MMAL
    }
}
libdrm {
    message(DRM renderer selected)

    DEFINES += HAVE_DRM
    SOURCES += streaming/video/ffmpeg-renderers/drm.cpp
    HEADERS += streaming/video/ffmpeg-renderers/drm.h

    linux {
        !disable-masterhooks {
            message(Master hooks enabled)
            DEFINES += HAVE_DRM_MASTER_HOOKS
            SOURCES += masterhook.c masterhook_internal.c
            LIBS += -ldl -pthread
        }
    }
}
cuda {
    message(CUDA support enabled)

    DEFINES += HAVE_CUDA
    SOURCES += streaming/video/ffmpeg-renderers/cuda.cpp
    HEADERS += streaming/video/ffmpeg-renderers/cuda.h

    # ffnvcodec uses libdl in cuda_load_functions()/cuda_free_functions()
    LIBS += -ldl
}
libplacebo {
    message(Vulkan support enabled via libplacebo)

    DEFINES += HAVE_LIBPLACEBO_VULKAN
    SOURCES += \
        streaming/video/ffmpeg-renderers/plvk.cpp \
        streaming/video/ffmpeg-renderers/plvk_c.c
    HEADERS += \
        streaming/video/ffmpeg-renderers/plvk.h

    macx {
        SOURCES += streaming/video/ffmpeg-renderers/plvk_objc.mm
    }
}
config_EGL {
    message(EGL renderer selected)

    CONFIG += egl
    DEFINES += HAVE_EGL
    SOURCES += \
        streaming/video/ffmpeg-renderers/eglvid.cpp \
        streaming/video/ffmpeg-renderers/egl_extensions.cpp \
        streaming/video/ffmpeg-renderers/eglimagefactory.cpp
    HEADERS += \
        streaming/video/ffmpeg-renderers/eglvid.h \
        streaming/video/ffmpeg-renderers/eglimagefactory.h
}
config_SL {
    message(Steam Link build configuration selected)

    !disable-prebuilts {
        # Link against our NEON-optimized libopus build
        LIBS += -L$$PWD/../libs/steamlink/lib
        INCLUDEPATH += $$PWD/../libs/steamlink/include
        LIBS += -lopus -larmasm -lNE10
    }

    DEFINES += EMBEDDED_BUILD STEAM_LINK HAVE_SLVIDEO HAVE_SLAUDIO
    LIBS += -lSLVideo -lSLAudio

    SOURCES += \
        streaming/video/slvid.cpp \
        streaming/audio/renderers/slaud.cpp
    HEADERS += \
        streaming/video/slvid.h \
        streaming/audio/renderers/slaud.h
}
win32 {
    HEADERS += streaming/video/ffmpeg-renderers/dxutil.h
}
win32:!winrt {
    message(DXVA2 and D3D11VA renderers selected)

    SOURCES += \
        streaming/video/ffmpeg-renderers/dxva2.cpp \
        streaming/video/ffmpeg-renderers/d3d11va.cpp \
        streaming/video/ffmpeg-renderers/pacer/dxvsyncsource.cpp

    HEADERS += \
        streaming/video/ffmpeg-renderers/dxva2.h \
        streaming/video/ffmpeg-renderers/d3d11va.h \
        streaming/video/ffmpeg-renderers/pacer/dxvsyncsource.h
}
macx {
    message(VideoToolbox renderer selected)

    DEFINES += HAVE_MAC_RAW_WACOM
    SOURCES += streaming/input/macrawwacom.cpp streaming/input/macnormalizedpen.cpp
    HEADERS += streaming/input/macrawwacom.h streaming/input/macrawwacomlogic.h streaming/input/macrawwacomasync.h \
        streaming/input/macnormalizedpen.h streaming/input/macnormalizedpenlogic.h
    LIBS += -framework IOKit -framework CoreFoundation -framework ApplicationServices -framework Carbon

    SOURCES += \
        streaming/macquitshortcut.mm \
        streaming/mackeyboardcapture.mm \
        streaming/macwindow.mm \
        streaming/video/ffmpeg-renderers/vt_base.mm \
        streaming/video/ffmpeg-renderers/vt_metal.mm

    HEADERS += \
        streaming/macquitshortcut.h \
        streaming/mackeyboardcapture.h \
        streaming/input/sdl-darwin-scancodes.h \
        streaming/macwindow.h \
        streaming/macdisplaygeometry.h \
        streaming/video/ffmpeg-renderers/vt.h \
        streaming/macclipboardsync.h \
        streaming/clipboardpolltimer.h \
        streaming/plankclipboard.h
    OBJECTIVE_SOURCES += streaming/macclipboardsync.mm
}
embedded {
    message(Embedded build)

    DEFINES += EMBEDDED_BUILD
}
glslow {
    message(GL slow build)

    DEFINES += GL_IS_SLOW
}
vkslow {
    message(Vulkan slow build)

    DEFINES += VULKAN_IS_SLOW
}
gpuslow {
    message(GPU slow build)

    DEFINES += GL_IS_SLOW VULKAN_IS_SLOW
}
wayland {
    message(Wayland extensions enabled)

    DEFINES += HAS_WAYLAND
    SOURCES += streaming/video/ffmpeg-renderers/pacer/waylandvsyncsource.cpp
    HEADERS += \
        streaming/plankwaylandcursor.h \
        streaming/plankembeddedcursor.h \
        streaming/plankwaylandtoolbar.h \
        streaming/video/ffmpeg-renderers/pacer/waylandvsyncsource.h
}

RESOURCES += \
    resources.qrc \
    qml.qrc

TRANSLATIONS += \
    languages/qml_zh_CN.ts \
    languages/qml_de.ts \
    languages/qml_fr.ts \
    languages/qml_nb_NO.ts \
    languages/qml_ru.ts \
    languages/qml_es.ts \
    languages/qml_ja.ts \
    languages/qml_vi.ts \
    languages/qml_th.ts \
    languages/qml_ko.ts \
    languages/qml_hu.ts \
    languages/qml_nl.ts \
    languages/qml_sv.ts \
    languages/qml_tr.ts \
    languages/qml_uk.ts \
    languages/qml_zh_TW.ts \
    languages/qml_el.ts \
    languages/qml_hi.ts \
    languages/qml_it.ts \
    languages/qml_pt.ts \
    languages/qml_pt_BR.ts \
    languages/qml_pl.ts \
    languages/qml_cs.ts \
    languages/qml_he.ts \
    languages/qml_ckb.ts \
    languages/qml_lt.ts \
    languages/qml_et.ts \
    languages/qml_bg.ts \
    languages/qml_eo.ts \
    languages/qml_ta.ts

# Additional import path used to resolve QML modules in Qt Creator's code model
QML_IMPORT_PATH =

# Additional import path used to resolve QML modules just for Qt Quick Designer
QML_DESIGNER_IMPORT_PATH =

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../moonlight-common-c/release/ -lmoonlight-common-c
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../moonlight-common-c/debug/ -lmoonlight-common-c
else:unix: LIBS += -L$$OUT_PWD/../moonlight-common-c/ -lmoonlight-common-c

INCLUDEPATH += $$PWD/../moonlight-common-c/moonlight-common-c/src
DEPENDPATH += $$PWD/../moonlight-common-c/moonlight-common-c/src

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../qmdnsengine/release/ -lqmdnsengine
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../qmdnsengine/debug/ -lqmdnsengine
else:unix: LIBS += -L$$OUT_PWD/../qmdnsengine/ -lqmdnsengine

INCLUDEPATH += $$PWD/../qmdnsengine/qmdnsengine/src/include $$PWD/../qmdnsengine
DEPENDPATH += $$PWD/../qmdnsengine/qmdnsengine/src/include $$PWD/../qmdnsengine

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../h264bitstream/release/ -lh264bitstream
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../h264bitstream/debug/ -lh264bitstream
else:unix: LIBS += -L$$OUT_PWD/../h264bitstream/ -lh264bitstream

INCLUDEPATH += $$PWD/../h264bitstream
DEPENDPATH += $$PWD/../h264bitstream

!winrt {
    win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../AntiHooking/release/ -lAntiHooking
    else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../AntiHooking/debug/ -lAntiHooking

    INCLUDEPATH += $$PWD/../AntiHooking
    DEPENDPATH += $$PWD/../AntiHooking
}

unix:!macx: {
    # The packaged Linux executable lives in /usr/bin while its qualified
    # FFmpeg runtime remains private to PLANK under /usr/lib/plank. Resolve
    # those libraries relative to the executable instead of relying on a
    # launcher-provided LD_LIBRARY_PATH.
    QMAKE_LFLAGS += "-Wl,-rpath,'\$$ORIGIN/../lib/plank'"

    isEmpty(PREFIX) {
        PREFIX = /usr/local
    }
    isEmpty(BINDIR) {
        BINDIR = bin
    }
    isEmpty(DATADIR) {
        DATADIR = share
    }

    target.path = $$PREFIX/$$BINDIR/

    desktop.files = deploy/linux/la.instinctual.Plank.Client.desktop
    desktop.path = $$PREFIX/$$DATADIR/applications/

    icons.files = res/plank-logo.png
    icons.path = $$PREFIX/$$DATADIR/icons/hicolor/512x512/apps/

    appstream.files = deploy/linux/la.instinctual.Plank.Client.appdata.xml
    appstream.path = $$PREFIX/$$DATADIR/metainfo/

    INSTALLS += target desktop icons appstream
}
win32 {
    RC_ICONS = moonlight.ico
    QMAKE_TARGET_COMPANY = Instinctual
    QMAKE_TARGET_DESCRIPTION = PLANK Client
    QMAKE_TARGET_PRODUCT = PLANK

    CONFIG -= embed_manifest_exe
    QMAKE_LFLAGS += /MANIFEST:embed /MANIFESTINPUT:$${PWD}/plank-client.exe.manifest
}
macx {
    # One SDK27-built arm64 application for macOS15 and newer, not separate
    # reduced-capability and modern editions. New APIs need availability guards.
    isEmpty(PLANK_MAC_CLIENT_MIN_MACOS): PLANK_MAC_CLIENT_MIN_MACOS = 15.0
    !equals(PLANK_MAC_CLIENT_MIN_MACOS, 15.0) {
        error("PLANK Client deployment target must be 15.0 (build with SDK27 or newer)")
    }
    QMAKE_MACOSX_DEPLOYMENT_TARGET = $$PLANK_MAC_CLIENT_MIN_MACOS
    QMAKE_APPLE_DEVICE_ARCHS = arm64
    QMAKE_CFLAGS += -Werror=unguarded-availability-new
    QMAKE_CXXFLAGS += -Werror=unguarded-availability-new
    QMAKE_INFO_PLIST = $$PWD/Info.plist

    APP_BUNDLE_RESOURCES.files = moonlight.icns
    APP_BUNDLE_RESOURCES.path = Contents/Resources

    QMAKE_BUNDLE_DATA += APP_BUNDLE_RESOURCES

    !disable-prebuilts {
        APP_BUNDLE_FRAMEWORKS.files = $$files(../libs/mac/Frameworks/*.framework, true) $$files(../libs/mac/lib/*.dylib, true)
        APP_BUNDLE_FRAMEWORKS.path = Contents/Frameworks

        QMAKE_BUNDLE_DATA += APP_BUNDLE_FRAMEWORKS

        QMAKE_RPATHDIR += @executable_path/../Frameworks
    }
}

isEmpty(PLANK_VERSION) {
    PLANK_VERSION = development
}
VERSION = "$$section(PLANK_VERSION, -, 0, 0)"
DEFINES += PLANK_VERSION_STR=\\\"$$PLANK_VERSION\\\"
