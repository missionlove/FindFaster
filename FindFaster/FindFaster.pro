QT       += core gui concurrent

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++11
msvc: QMAKE_CXXFLAGS += /utf-8
win32-g++: QMAKE_CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    findfasterwidget.cpp \
    finderresultitemdelegate.cpp \
    findresultsmodel.cpp \
    win_shell_context_menu.cpp

HEADERS += \
    findfasterwidget.h \
    finderresultitemdelegate.h \
    findresultsmodel.h \
    win_shell_context_menu.h

RESOURCES += \
    resources.qrc

COMMON_BIN_DIR = $$clean_path($$OUT_PWD/../bin)
DESTDIR = $$COMMON_BIN_DIR
LIBFINDER_OUT_DIR = $$COMMON_BIN_DIR
INCLUDEPATH += $$PWD/../libfinder
LIBS += -L$$LIBFINDER_OUT_DIR -l$$qtLibraryTarget(libfinder)

win32 {
    PRE_TARGETDEPS += $$LIBFINDER_OUT_DIR/$$qtLibraryTarget(libfinder).lib
    LIBS += -lshell32 -lole32 -luser32
    RC_FILE += appicon.rc
}
unix {
    PRE_TARGETDEPS += $$LIBFINDER_OUT_DIR/lib$$qtLibraryTarget(libfinder).so
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
