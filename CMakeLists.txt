cmake_minimum_required(VERSION 3.21)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")

project(QScreenRec VERSION 0.3.0 LANGUAGES CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt 6.8 is the minimum: frame/audio-buffer inputs (QVideoFrameInput, QAudioBufferInput) are 6.8,
# window recording (QWindowCapture) is 6.6, screen recording (QScreenCapture) is 6.5.
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Multimedia)
qt_standard_project_setup()

# Qt 6.8 still asks the linker for Apple's AGL framework, which recent Xcode SDKs no longer
# ship ("ld: framework 'AGL' not found"). Nothing uses it, so drop it from Qt's OpenGL target.
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    get_target_property(_wrap_gl_libs WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
    if(_wrap_gl_libs)
        list(FILTER _wrap_gl_libs EXCLUDE REGEX "AGL")
        set_target_properties(WrapOpenGL::WrapOpenGL PROPERTIES INTERFACE_LINK_LIBRARIES "${_wrap_gl_libs}")
    endif()
endif()

set(APP_SOURCES
    src/main.cpp
    src/MainWindow.h        src/MainWindow.cpp
    src/Recorder.h          src/Recorder.cpp
    src/RegionSelector.h    src/RegionSelector.cpp
    src/WindowPicker.h      src/WindowPicker.cpp
    src/audio/SystemAudio.h
)

# --- System-audio ("what you hear") backend ---------------------------------------------------
if(WIN32)
    list(APPEND APP_SOURCES src/audio/SystemAudio_wasapi.cpp packaging/windows/qscreenrec.rc)
elseif(APPLE)
    list(APPEND APP_SOURCES src/audio/SystemAudio_mac.mm packaging/macos/qscreenrec.icns)
    set_source_files_properties(src/audio/SystemAudio_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    set_source_files_properties(packaging/macos/qscreenrec.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PULSE IMPORTED_TARGET libpulse)
    endif()
    # Window capture on Wayland goes through the desktop portal + PipeWire (optional).
    find_package(Qt6 QUIET COMPONENTS DBus)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
    endif()
    if(Qt6DBus_FOUND AND PIPEWIRE_FOUND)
        set(HAVE_PORTAL_CAPTURE ON)
        list(APPEND APP_SOURCES src/PortalCapture.h src/PortalCapture.cpp)
    else()
        message(WARNING "Qt6 DBus and/or libpipewire-0.3 not found (install libpipewire-0.3-dev): "
                        "building without window capture on Wayland.")
    endif()
    if(PULSE_FOUND)
        list(APPEND APP_SOURCES src/audio/SystemAudio_pulse.cpp)
    else()
        message(WARNING "libpulse not found (install libpulse-dev / pulseaudio-libs-devel): "
                        "building without system-audio capture.")
        list(APPEND APP_SOURCES src/audio/SystemAudio_none.cpp)
    endif()
endif()

qt_add_executable(qscreenrec WIN32 MACOSX_BUNDLE ${APP_SOURCES})

target_include_directories(qscreenrec PRIVATE src)
target_compile_definitions(qscreenrec PRIVATE APP_VERSION="${PROJECT_VERSION}")
target_link_libraries(qscreenrec PRIVATE Qt6::Widgets Qt6::Multimedia)

if(WIN32)
    target_link_libraries(qscreenrec PRIVATE ole32)
elseif(APPLE)
    # Weak: the app still launches on macOS 12, where system audio is simply unavailable.
    target_link_libraries(qscreenrec PRIVATE
        "-weak_framework ScreenCaptureKit" "-framework CoreMedia" "-framework Foundation")
elseif(PULSE_FOUND)
    target_link_libraries(qscreenrec PRIVATE PkgConfig::PULSE)
endif()
if(HAVE_PORTAL_CAPTURE)
    target_compile_definitions(qscreenrec PRIVATE HAVE_PORTAL_CAPTURE)
    target_link_libraries(qscreenrec PRIVATE Qt6::DBus PkgConfig::PIPEWIRE)
endif()
if(UNIX AND NOT APPLE)
    # Classic DT_RPATH instead of DT_RUNPATH: unlike RUNPATH it also applies to the libraries
    # that the bundled Qt plugins pull in, so the installed tree finds its own lib/ directory
    # regardless of how the Qt it was built against lays out its plugins.
    target_link_options(qscreenrec PRIVATE "-Wl,--disable-new-dtags")
endif()

if(MSVC)
    target_compile_options(qscreenrec PRIVATE /W4)
else()
    target_compile_options(qscreenrec PRIVATE -Wall -Wextra)
endif()

set_target_properties(qscreenrec PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "QScreenRec"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.archstone.qscreenrec"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "qscreenrec.icns"
)
if(APPLE)
    set_target_properties(qscreenrec PROPERTIES OUTPUT_NAME "QScreenRec")
endif()

# --- Install: a self-contained tree (Qt libraries and plugins included) ------------------------
include(GNUInstallDirs)
if(WIN32)
    # Bundle the MSVC runtime DLLs next to the executable so the installed app does not
    # depend on a Visual C++ redistributable being present on the target machine.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ${CMAKE_INSTALL_BINDIR})
    include(InstallRequiredSystemLibraries)
endif()
install(TARGETS qscreenrec
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
qt_generate_deploy_app_script(
    TARGET qscreenrec
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

if(UNIX AND NOT APPLE)
    install(FILES packaging/linux/qscreenrec.desktop DESTINATION share/applications)
    install(FILES packaging/linux/qscreenrec.png DESTINATION share/icons/hicolor/256x256/apps)
endif()

# --- Installers: run `cpack` in the build directory --------------------------------------------
#   Windows : NSIS setup .exe (+ portable .zip)      needs NSIS on PATH
#   macOS   : .dmg (drag QScreenRec to Applications)
#   Linux   : .deb (+ .rpm when rpmbuild exists, + portable .tar.gz), installs to /opt/qscreenrec
set(CPACK_PACKAGE_NAME "qscreenrec")
set(CPACK_PACKAGE_VENDOR "Archstone")
set(CPACK_PACKAGE_CONTACT "Archstone <support@example.invalid>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Record the screen, an area or a window (with audio) to a video file")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "QScreenRec")
set(CPACK_PACKAGE_EXECUTABLES "qscreenrec;QScreenRec")
set(CPACK_STRIP_FILES OFF)
set(CPACK_VERBATIM_VARIABLES ON)

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")
    set(CPACK_PACKAGE_FILE_NAME "qscreenrec-${PROJECT_VERSION}-windows-x64")
    set(CPACK_NSIS_PACKAGE_NAME "QScreenRec")
    set(CPACK_NSIS_DISPLAY_NAME "QScreenRec ${PROJECT_VERSION}")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/qscreenrec.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/qscreenrec.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\qscreenrec.exe")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "qscreenrec.exe")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_PACKAGE_FILE_NAME "qscreenrec-${PROJECT_VERSION}-macos")
    set(CPACK_DMG_VOLUME_NAME "QScreenRec ${PROJECT_VERSION}")
else()
    set(CPACK_GENERATOR "DEB;TGZ")
    find_program(RPMBUILD_EXECUTABLE rpmbuild)
    if(RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()
    set(CPACK_PACKAGE_FILE_NAME "qscreenrec-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/qscreenrec")
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libc6, libpulse0, libpipewire-0.3-0 | libpipewire-0.3-0t64, libgl1, libegl1, libfontconfig1, libxkbcommon-x11-0, libxcb-cursor0, libxcb-icccm4, libxcb-image0, libxcb-keysyms1, libxcb-render-util0, libxcb-shape0, libxcb-xkb1")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/prerm")
    set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
    set(CPACK_RPM_PACKAGE_AUTOREQPROV OFF)
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/postinst")
    set(CPACK_RPM_PRE_UNINSTALL_SCRIPT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/prerm")
    set(CPACK_RPM_SPEC_MORE_DEFINE "%define __os_install_post %{nil}\n%define _build_id_links none")
endif()
include(CPack)
