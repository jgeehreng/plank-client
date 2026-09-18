QT += core testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = macrawwacom
INCLUDEPATH += ../../app/streaming/input ../../moonlight-common-c/moonlight-common-c/src
SOURCES += test_macrawwacom.cpp
HEADERS += ../../app/streaming/input/macrawwacomlogic.h ../../app/streaming/input/macrawwacomasync.h ../../app/streaming/input/macwacomvendordriver.h
