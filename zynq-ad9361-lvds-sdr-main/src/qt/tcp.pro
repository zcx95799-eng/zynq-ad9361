QT       += core gui network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    monitorprotocol.cpp \
    monitorreceiver.cpp \
    monitorsender.cpp \
    sendpayloadprotocol.cpp \
    demodsymbolmodel.cpp \
    spectrum.cpp \
    plotwidget.cpp

HEADERS += \
    mainwindow.h \
    monitorprotocol.h \
    monitorreceiver.h \
    monitorsender.h \
    sendpayloadprotocol.h \
    demodsymbolmodel.h \
    spectrum.h \
    plotwidget.h

msvc: QMAKE_CXXFLAGS += /utf-8

# The runtime interface is built in MainWindow::buildUi().
# mainwindow.ui is retained as the original Designer file.

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
