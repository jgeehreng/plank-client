TEMPLATE = app
TARGET = brokerheartbeat
CONFIG += console c++17 warn_on
CONFIG -= qt app_bundle
QMAKE_CXXFLAGS += -Werror
SOURCES += test_brokerheartbeat.cpp
HEADERS += ../../app/backend/brokerheartbeatpolicy.h
