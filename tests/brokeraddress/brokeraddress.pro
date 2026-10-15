TEMPLATE = app
TARGET = brokeraddress
QT = core
CONFIG += console c++17 warn_on
CONFIG -= app_bundle
QMAKE_CXXFLAGS += -Werror
SOURCES += test_brokeraddress.cpp
HEADERS += ../../app/backend/brokeraddress.h
