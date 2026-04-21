QT += core testlib
CONFIG += testcase c++11 console
CONFIG -= app_bundle
msvc: QMAKE_CXXFLAGS += /utf-8
win32-g++: QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

TARGET = libfinder_tests
TEMPLATE = app
COMMON_BIN_DIR = $$clean_path($$OUT_PWD/../../bin)
DESTDIR = $$COMMON_BIN_DIR

INCLUDEPATH += $$PWD/../../libfinder

SOURCES += \
    tst_ntfsusnutils.cpp \
    ../../libfinder/ntfsusn_utils.cpp

HEADERS += \
    ../../libfinder/ntfsusn_utils.h
