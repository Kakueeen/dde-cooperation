#include "../mainwindow.h"
#include "../mainwindow_p.h"
#include "../connect/readywidget.h"
#include "../connect/choosewidget.h"
#include "../connect/connectwidget.h"
#include "../connect/promptwidget.h"
#include "../connect/startwidget.h"
#include "../connect/networkdisconnectionwidget.h"
#include "../connect/licensewidget.h"

#include "../select/appselectwidget.h"
#include "../select/selectmainwidget.h"
#include "../select/configselectwidget.h"
#include "../select/fileselectwidget.h"

#include "../transfer/successwidget.h"
#include "../transfer/transferringwidget.h"
#include "../transfer/errorwidget.h"

#include "../getbackup/createbackupfilewidget.h"
#include "../getbackup/zipfileprocesswidget.h"
#include "../getbackup/zipfileprocessresultwidget.h"
#include "utils/transferhepler.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolBar>
#include <QDesktopWidget>
#include <QScreen>
#include <QToolButton>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QCoreApplication>
#include <QApplication>
#include <QDebug>
#include <QThreadPool>
#include <QTimer>
using namespace data_transfer_core;

void MainWindowPrivate::initWindow()
{
    q->setWindowFlags(Qt::FramelessWindowHint);
    q->setAttribute(Qt::WA_TranslucentBackground);
    q->setFixedSize(740, 552);
    QWidget *centerWidget = new QWidget(q);
    QVBoxLayout *layout = new QVBoxLayout(centerWidget);
    centerWidget->setLayout(layout);
    layout->setContentsMargins(0, 0, 0, 0);

    q->setCentralWidget(centerWidget);
    windowsCentralWidgetSidebar = new QHBoxLayout(centerWidget);
    windowsCentralWidgetContent = new QHBoxLayout(centerWidget);
    windowsCentralWidget = new QHBoxLayout(centerWidget);

    windowsCentralWidget->addLayout(windowsCentralWidgetSidebar);
    windowsCentralWidget->addLayout(windowsCentralWidgetContent);

    q->setWindowIcon(QIcon(":/icon/icon.svg"));

    initSideBar();
    initTitleBar();
    layout->setSpacing(0);
    layout->addLayout(windowsCentralWidget);
}

void MainWindowPrivate::initWidgets()
{
    StartWidget *startwidget = new StartWidget(q);
    LicenseWidget *licensewidget = new LicenseWidget(q);
    ChooseWidget *choosewidget = new ChooseWidget(q);
    TransferringWidget *transferringwidget = new TransferringWidget(q);
    ReadyWidget *readywidget = new ReadyWidget(q);
    PromptWidget *promptwidget = new PromptWidget(q);
    SuccessWidget *successwidget = new SuccessWidget(q);
    FileSelectWidget *filewselectidget =
            new FileSelectWidget(qobject_cast<SidebarWidget *>(sidebar->widget()), q);
    ConfigSelectWidget *configselectwidget = new ConfigSelectWidget(q);
    AppSelectWidget *appselectwidget = new AppSelectWidget(q);
    ErrorWidget *errorwidget = new ErrorWidget(q);
    QStackedWidget *stackedWidget = new QStackedWidget(q);
    NetworkDisconnectionWidget *networkdisconnectionwidget = new NetworkDisconnectionWidget(q);
    zipFileProcessWidget *zipfileprocesswidget = new zipFileProcessWidget(q);
    ZipFileProcessResultWidget *zipfileprocessresultwidget = new ZipFileProcessResultWidget(q);
    CreateBackupFileWidget *createbackupfilewidget = new CreateBackupFileWidget(q);

    SelectMainWidget *selectmainwidget = new SelectMainWidget(q);
    stackedWidget->insertWidget(PageName::startwidget, startwidget);
    stackedWidget->insertWidget(PageName::licensewidget, licensewidget);
    stackedWidget->insertWidget(PageName::choosewidget, choosewidget);
    stackedWidget->insertWidget(PageName::promptwidget, promptwidget);
    stackedWidget->insertWidget(PageName::readywidget, readywidget);
    stackedWidget->insertWidget(PageName::selectmainwidget, selectmainwidget);
    stackedWidget->insertWidget(PageName::transferringwidget, transferringwidget);
    stackedWidget->insertWidget(PageName::successtranswidget, successwidget);
    stackedWidget->insertWidget(PageName::filewselectidget, filewselectidget);
    stackedWidget->insertWidget(PageName::configselectwidget, configselectwidget);
    stackedWidget->insertWidget(PageName::appselectwidget, appselectwidget);
    stackedWidget->insertWidget(PageName::errorwidget, errorwidget);
    stackedWidget->insertWidget(PageName::createbackupfilewidget, createbackupfilewidget);
    stackedWidget->insertWidget(PageName::networkdisconnectwidget, networkdisconnectionwidget);
    stackedWidget->insertWidget(PageName::zipfileprocesswidget, zipfileprocesswidget);
    stackedWidget->insertWidget(PageName::zipfileprocessresultwidget, zipfileprocessresultwidget);

    stackedWidget->setCurrentIndex(PageName::startwidget);

    windowsCentralWidgetContent->setContentsMargins(8, 8, 8, 8);
    windowsCentralWidgetContent->addWidget(stackedWidget);

    QObject::connect(stackedWidget, &QStackedWidget::currentChanged, this,
                     &MainWindowPrivate::handleCurrentChanged);

    QObject::connect(
            TransferHelper::instance(), &TransferHelper::transferSucceed, this,
            [stackedWidget] { stackedWidget->setCurrentIndex(PageName::successtranswidget); });

    QObject::connect(appselectwidget, &AppSelectWidget::isOk, selectmainwidget,
                     &SelectMainWidget::changeSelectframeState);
    QObject::connect(filewselectidget, &FileSelectWidget::isOk, selectmainwidget,
                     &SelectMainWidget::changeSelectframeState);
    QObject::connect(configselectwidget, &ConfigSelectWidget::isOk, selectmainwidget,
                     &SelectMainWidget::changeSelectframeState);


    QObject::connect(selectmainwidget, &SelectMainWidget::updateBackupFileSize,
                     createbackupfilewidget, &CreateBackupFileWidget::updaeBackupFileSize);
    // add backup file exit button
    QObject::connect(zipfileprocessresultwidget, &ZipFileProcessResultWidget::exit, q, [this]() {
        QCoreApplication::quit();
        //        qApp->quit();
    });

QObject:
    connect(TransferHelper::instance(), &TransferHelper::onlineStateChanged,

            [stackedWidget, errorwidget](bool online) {
                if (online)
                    return;
                int index = stackedWidget->currentIndex();
                //only these need jump to networkdisconnectwidget
                if (index >= PageName::promptwidget && index <= PageName::appselectwidget )
                    stackedWidget->setCurrentIndex(PageName::networkdisconnectwidget);
                if (index == PageName::transferringwidget) {
                    stackedWidget->setCurrentIndex(PageName::errorwidget);
                    errorwidget->setErrorType(ErrorType::networkError);
                }
            });
}
void MainWindowPrivate::paintEvent(QPaintEvent *event)
{
    QPainter painter(q);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(220, 220, 220));
    painter.setPen(Qt::NoPen);

    QPainterPath path;
    path.addRoundedRect(q->rect(), 20, 20);
    painter.drawPath(path);
}

void MainWindowPrivate::initSideBar()
{
    sidebar = new QDockWidget("Sidebar", q);
    q->setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    q->addDockWidget(Qt::LeftDockWidgetArea, sidebar);
    sidebar->setTitleBarWidget(new QWidget());
    sidebar->setFixedWidth(200);

    SidebarWidget *sidebarWidget = new SidebarWidget(q);
    sidebar->setWidget(sidebarWidget);
    sidebar->setVisible(false);
    sidebar->setStyleSheet(".SidebarWidget{"
                           "background-color: white;"
                           "border-bottom-left-radius: 18px;"
                           "}");

    windowsCentralWidgetSidebar->addWidget(sidebar);
}

void MainWindowPrivate::mouseMoveEvent(QMouseEvent *event)
{
    if ((event->buttons() == Qt::LeftButton) && leftButtonPressed) {
        q->move(event->globalPos() - lastPosition);
    }
}

void MainWindowPrivate::mouseReleaseEvent(QMouseEvent *event)
{
    leftButtonPressed = false;
}

void MainWindowPrivate::mousePressEvent(QMouseEvent *event)
{
    if (event->buttons() == Qt::LeftButton) {
        leftButtonPressed = true;
        lastPosition = event->globalPos() - q->pos();
    }
}

void MainWindowPrivate::initTitleBar()
{
    QWidget *titleBar = new QWidget(q->centralWidget());
    titleBar->setFixedHeight(50);
    titleBar->setStyleSheet("QWidget {"
                            "background-color: white;"
                            "border-top-left-radius: 20px;"
                            "border-top-right-radius: 20px;"
                            "}");

    QToolButton *closeButton = new QToolButton(titleBar);
    closeButton->setStyleSheet("QWidget {"
                               "border-top-right-radius: 20px;"
                               "}");
    closeButton->setFixedSize(50, 50);
    closeButton->setIcon(QIcon(":/icon/close_normal.svg"));
    closeButton->setIconSize(QSize(50, 50));

    QToolButton *minButton = new QToolButton(titleBar);
    minButton->setIcon(QIcon(":/icon/minimise_normal.svg"));
    minButton->setFixedSize(50, 50);
    minButton->setIconSize(QSize(50, 50));

    QToolButton *helpButton = new QToolButton(titleBar);
    helpButton->setFixedSize(50, 50);
    helpButton->setIcon(QIcon(":/icon/menu_normal.svg"));
    helpButton->setIconSize(QSize(50, 50));

    QLabel *mainLabel = new QLabel(titleBar);
    mainLabel->setStyleSheet("QWidget {"
                             "border-top-left-radius: 20px;"
                             "}");
    mainLabel->setPixmap(QPixmap(":/icon/icon.svg"));

    QObject::connect(closeButton, &QToolButton::clicked, q, [this]() {
        //qApp->quit();
                QCoreApplication::quit();
    });
    QObject::connect(minButton, &QToolButton::clicked, q, &MainWindow::showMinimized);

    QHBoxLayout *titleLayout = new QHBoxLayout(titleBar);

    titleLayout->addWidget(mainLabel);
    titleLayout->addWidget(minButton, Qt::AlignHCenter);
    titleLayout->addWidget(closeButton, Qt::AlignHCenter);
    titleLayout->setContentsMargins(8, 0, 0, 0);
    titleBar->setLayout(titleLayout);

    q->centralWidget()->layout()->addWidget(titleBar);
}

void MainWindowPrivate::handleCurrentChanged(int index)
{
    if (index == PageName::filewselectidget)
        sidebar->setVisible(true);
    else
        sidebar->setVisible(false);
}
