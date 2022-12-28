#include "ConfirmDialog.h"

#include <QLabel>
#include <QTimer>

#include <DConfig>

const static QString dConfigAppID = "org.deepin.cooperation";
const static QString dConfigName = "org.deepin.cooperation";

DCORE_USE_NAMESPACE

ConfirmDialog::ConfirmDialog(const QString &ip, const QString &machineName)
    : DDialog()
    , m_titleLabel(new QLabel(this))
    , m_contentLabel(new QLabel(this)) {
    setAttribute(Qt::WA_QuitOnClose);
    setFocus(Qt::MouseFocusReason);

    initTimeout();

    QFont font = m_titleLabel->font();
    font.setBold(true);
    font.setPixelSize(16);
    m_titleLabel->setFont(font);
    m_titleLabel->setText(tr("Cooperation request confirm:"));
    addContent(m_titleLabel, Qt::AlignTop | Qt::AlignHCenter);

    QString content = QString(tr("Machine(%1) %2 request cooperation")).arg(ip).arg(machineName);
    m_contentLabel->setText(content);
    addContent(m_contentLabel, Qt::AlignBottom | Qt::AlignHCenter);

    QStringList btnTexts;
    btnTexts << tr("reject") << tr("accept");
    addButtons(btnTexts);

    connect(this, &ConfirmDialog::buttonClicked, this, [=](int index, const QString &text) {
        Q_UNUSED(text);
        bool isAccepted = index == 1;
        emit onConfirmed(isAccepted);
    });

    connect(this, &ConfirmDialog::closed, this, [=]() { emit onConfirmed(false); });
}

void ConfirmDialog::initTimeout() {
    int interval = 60; // default

    DConfig *dConfigPtr = DConfig::create(dConfigAppID, dConfigName);
    if (dConfigPtr && dConfigPtr->isValid() && dConfigPtr->keyList().contains("timeoutInterval")) {
        interval =  dConfigPtr->value("timeoutInterval").toInt();
    }

    dConfigPtr->deleteLater();

    QTimer::singleShot(interval * 1000, this, [this](){
        blockSignals(true);
        this->close();
        blockSignals(false);
    });
}
