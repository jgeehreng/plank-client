TEMPLATE = app
TARGET = test_linuxrawwacom
CONFIG += console c++17
CONFIG -= app_bundle
QT -= core gui
SOURCES += test_reportworker.cpp
unix: LIBS += -pthread
