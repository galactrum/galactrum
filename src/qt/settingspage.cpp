#include "settingspage.h"
#include "util.h" // for GetBoolArg
#if defined(HAVE_CONFIG_H)
#include "config/galactrum-config.h"
#endif

#include "optionsdialog.h"
#include "ui_optionsdialog.h"

#include "bitcoinunits.h"
#include "guiutil.h"
#include "optionsmodel.h"

#include "validation.h" // for DEFAULT_SCRIPTCHECK_THREADS and MAX_SCRIPTCHECK_THREADS
#include "netbase.h"
#include "txdb.h" // for -dbcache defaults

#ifdef ENABLE_WALLET
#include "wallet/wallet.h" // for CWallet::GetRequiredFee()
#endif

#include "privatesend-client.h"

#include <boost/thread.hpp>

#include <QDataWidgetMapper>
#include <QDir>
#include <QIntValidator>
#include <QLocale>
#include <QMessageBox>
#include <QTimer>
#include <QMouseEvent>

#include "ui_settingspage.h"

extern CWallet* pwalletMain;

SettingsPage::SettingsPage(const PlatformStyle *platformStyle, QWidget *parent) :
    ui(new Ui::SettingsPage)
{
    ui->setupUi(this);
    /* Main elements init */
    ui->databaseCache->setMinimum(nMinDbCache);
    ui->databaseCache->setMaximum(nMaxDbCache);
    ui->threadsScriptVerif->setMinimum(-GetNumCores());
    ui->threadsScriptVerif->setMaximum(MAX_SCRIPTCHECK_THREADS);

    /* Network elements init */
#ifndef USE_UPNP
    ui->mapPortUpnp->setEnabled(false);
#endif

    ui->proxyIp->setEnabled(false);
    ui->proxyPort->setEnabled(false);
    ui->proxyPort->setValidator(new QIntValidator(1, 65535, this));

    ui->proxyIpTor->setEnabled(false);
    ui->proxyPortTor->setEnabled(false);
    ui->proxyPortTor->setValidator(new QIntValidator(1, 65535, this));

    connect(ui->connectSocks, SIGNAL(toggled(bool)), ui->proxyIp, SLOT(setEnabled(bool)));
    connect(ui->connectSocks, SIGNAL(toggled(bool)), ui->proxyPort, SLOT(setEnabled(bool)));
    connect(ui->connectSocks, SIGNAL(toggled(bool)), this, SLOT(updateProxyValidationState()));

    connect(ui->connectSocksTor, SIGNAL(toggled(bool)), ui->proxyIpTor, SLOT(setEnabled(bool)));
    connect(ui->connectSocksTor, SIGNAL(toggled(bool)), ui->proxyPortTor, SLOT(setEnabled(bool)));
    connect(ui->connectSocksTor, SIGNAL(toggled(bool)), this, SLOT(updateProxyValidationState()));

    /* Window elements init */
#ifdef Q_OS_MAC
    /* remove Window tab on Mac */
    ui->hideTrayIcon->setVisible(false);
    ui->minimizeToTray->setVisible(false);
    ui->minimizeOnClose->setVisible(false);
    ui->labelSettingsTray->setVisible(false);
#endif
#ifdef ENABLE_WALLET
    /* if compiled with wallet support, -disablewallet can still disable the wallet */
    enableWallet = !GetBoolArg("-disablewallet", false);
#else
    enableWallet = false;
#endif // ENABLE_WALLET
    /* remove Wallet tab in case of -disablewallet */
    if (!enableWallet) {
        // TODO
        //ui->tabWidget->removeTab(ui->tabWidget->indexOf(ui->tabWallet));
    }

    /* Display elements init */

    /* Number of displayed decimal digits selector */
    QString digits;
    for(int index = 2; index <=8; index++){
        digits.setNum(index);
        ui->digits->addItem(digits, digits);
    }


    /* Language selector */
    QDir translations(":translations");
    ui->lang->addItem(QString("(") + tr("default") + QString(")"), QVariant(""));
    Q_FOREACH(const QString &langStr, translations.entryList())
    {
        QLocale locale(langStr);

        /** check if the locale name consists of 2 parts (language_country) */
        if(langStr.contains("_"))
        {
#if QT_VERSION >= 0x040800
            /** display language strings as "native language - native country (locale name)", e.g. "Deutsch - Deutschland (de)" */
            ui->lang->addItem(locale.nativeLanguageName() + QString(" - ") + locale.nativeCountryName() + QString(" (") + langStr + QString(")"), QVariant(langStr));
#else
            /** display language strings as "language - country (locale name)", e.g. "German - Germany (de)" */
            ui->lang->addItem(QLocale::languageToString(locale.language()) + QString(" - ") + QLocale::countryToString(locale.country()) + QString(" (") + langStr + QString(")"), QVariant(langStr));
#endif
        }
        else
        {
#if QT_VERSION >= 0x040800
            /** display language strings as "native language (locale name)", e.g. "Deutsch (de)" */
            ui->lang->addItem(locale.nativeLanguageName() + QString(" (") + langStr + QString(")"), QVariant(langStr));
#else
            /** display language strings as "language (locale name)", e.g. "German (de)" */
            ui->lang->addItem(QLocale::languageToString(locale.language()) + QString(" (") + langStr + QString(")"), QVariant(langStr));
#endif
        }
    }

    ui->unit->setModel(new BitcoinUnits(this));

    /* Widget-to-option mapper */
    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
    mapper->setOrientation(Qt::Vertical);

    /* setup/change UI elements when proxy IPs are invalid/valid */
    ui->proxyIp->setCheckValidator(new ProxyAddressValidator(parent));
    ui->proxyIpTor->setCheckValidator(new ProxyAddressValidator(parent));
    connect(ui->proxyIp, SIGNAL(validationDidChange(QValidatedLineEdit *)), this, SLOT(updateProxyValidationState()));
    connect(ui->proxyIpTor, SIGNAL(validationDidChange(QValidatedLineEdit *)), this, SLOT(updateProxyValidationState()));
    connect(ui->proxyPort, SIGNAL(textChanged(const QString&)), this, SLOT(updateProxyValidationState()));
    connect(ui->proxyPortTor, SIGNAL(textChanged(const QString&)), this, SLOT(updateProxyValidationState()));

    ui->buttonChevron1->setIcon(QIcon(":/icons/light/chevron-right"));
    ui->buttonChevron1->setIconSize(QSize(32, 32));
    ui->buttonChevron2->setIcon(QIcon(":/icons/light/chevron-right"));
    ui->buttonChevron2->setIconSize(QSize(32, 32));
    ui->buttonChevron3->setIcon(QIcon(":/icons/light/chevron-right"));
    ui->buttonChevron3->setIconSize(QSize(32, 32));

    ui->buttonOpen1->setIcon(QIcon(":icons/light/export"));
    ui->buttonOpen1->setIconSize(QSize(32, 32));
    ui->buttonOpen2->setIcon(QIcon(":icons/light/export"));
    ui->buttonOpen2->setIconSize(QSize(32, 32));
    ui->buttonOpen3->setIcon(QIcon(":icons/light/export"));
    ui->buttonOpen3->setIconSize(QSize(32, 32));

    ui->frameMain->setVisible(false);
    ui->frameNetwork->setVisible(false);
    ui->frameExpert->setVisible(false);
    ui->scrollArea->setVisible(false);
    ui->frameControlButtons->setVisible(false);

    connect(ui->frameNavMain, SIGNAL(clicked()), this, SLOT(showMainPage()));
    ui->frameNavMain->installEventFilter(this);
    ui->frameNavNetwork->installEventFilter(this);
    ui->frameNavExpert->installEventFilter(this);
    ui->frameNavOpenConf->installEventFilter(this);
    ui->frameNavOpenMNConf->installEventFilter(this);
    ui->frameNavOpenBackups->installEventFilter(this);

    connect(ui->cancelButton, SIGNAL(clicked()), this, SLOT(handleCancelClicked()));
    connect(ui->saveButton, SIGNAL(clicked()), this, SLOT(handleSaveClicked()));
}

void SettingsPage::setMapper()
{
    /* Main */
    mapper->addMapping(ui->bitcoinAtStartup, OptionsModel::StartAtStartup);
    mapper->addMapping(ui->threadsScriptVerif, OptionsModel::ThreadsScriptVerif);
    mapper->addMapping(ui->databaseCache, OptionsModel::DatabaseCache);

    /* Wallet */
    mapper->addMapping(ui->coinControlFeatures, OptionsModel::CoinControlFeatures);
    mapper->addMapping(ui->showMasternodesTab, OptionsModel::ShowMasternodesTab);
    mapper->addMapping(ui->showAdvancedPSUI, OptionsModel::ShowAdvancedPSUI);
    mapper->addMapping(ui->lowKeysWarning, OptionsModel::LowKeysWarning);
    mapper->addMapping(ui->privateSendMultiSession, OptionsModel::PrivateSendMultiSession);
    mapper->addMapping(ui->spendZeroConfChange, OptionsModel::SpendZeroConfChange);
    mapper->addMapping(ui->privateSendRounds, OptionsModel::PrivateSendRounds);
    mapper->addMapping(ui->privateSendAmount, OptionsModel::PrivateSendAmount);

    /* Network */
    mapper->addMapping(ui->mapPortUpnp, OptionsModel::MapPortUPnP);
    mapper->addMapping(ui->allowIncoming, OptionsModel::Listen);

    mapper->addMapping(ui->connectSocks, OptionsModel::ProxyUse);
    mapper->addMapping(ui->proxyIp, OptionsModel::ProxyIP);
    mapper->addMapping(ui->proxyPort, OptionsModel::ProxyPort);

    mapper->addMapping(ui->connectSocksTor, OptionsModel::ProxyUseTor);
    mapper->addMapping(ui->proxyIpTor, OptionsModel::ProxyIPTor);
    mapper->addMapping(ui->proxyPortTor, OptionsModel::ProxyPortTor);

    /* Window */
#ifndef Q_OS_MAC
    mapper->addMapping(ui->hideTrayIcon, OptionsModel::HideTrayIcon);
    mapper->addMapping(ui->minimizeToTray, OptionsModel::MinimizeToTray);
    mapper->addMapping(ui->minimizeOnClose, OptionsModel::MinimizeOnClose);
#endif

    /* Display */
    mapper->addMapping(ui->digits, OptionsModel::Digits);
    mapper->addMapping(ui->lang, OptionsModel::Language);
    mapper->addMapping(ui->unit, OptionsModel::DisplayUnit);


}

SettingsPage::~SettingsPage()
{
    delete ui;
}


void SettingsPage::showConfEditor()
{
    GUIUtil::openConfigfile();
}

void SettingsPage::showMNConfEditor()
{
    GUIUtil::openMNConfigfile();
}

void SettingsPage::showBackups()
{
    GUIUtil::showBackups();
}


bool SettingsPage::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QMouseEvent::MouseButtonPress) {
        if (obj == ui->frameNavMain) {
            ui->frameMain->setVisible(true);
            this->hideNav();
            return true;
        } else if (obj == ui->frameNavNetwork) {
            ui->frameNetwork->setVisible(true);
            this->hideNav();
            return true;
        } else if (obj == ui->frameNavExpert) {
            ui->frameExpert->setVisible(true);
            this->hideNav();
            return true;
        } else if (obj == ui->frameNavOpenConf) {
            this->showConfEditor();
            return true;
        } else if (obj == ui->frameNavOpenMNConf) {
            this->showMNConfEditor();
            return true;
        } else if (obj == ui->frameNavOpenBackups) {
            this->showBackups();
            return true;
        }
    }
    return obj->eventFilter(obj, event);
}

void SettingsPage::hideNav()
{
    //ui->frameNav->setEnabled(false);
    ui->frameNav->setVisible(false);
    ui->scrollArea->setVisible(true);
    ui->frameControlButtons->setVisible(true);
}

void SettingsPage::showNav()
{
    //ui->frameNav->setEnabled(true);
    ui->frameMain->setVisible(false);
    ui->frameNetwork->setVisible(false);
    ui->frameExpert->setVisible(false);
    ui->scrollArea->setVisible(false);
    ui->frameControlButtons->setVisible(false);
    ui->frameNav->setVisible(true);
}

void SettingsPage::handleCancelClicked()
{
    this->showNav();
    this->setOptionsModel(model);
}

void SettingsPage::handleSaveClicked()
{
    this->showNav();
    mapper->submit();
    privateSendClient.nCachedNumBlocks = std::numeric_limits<int>::max();
    pwalletMain->MarkDirty();
    updateDefaultProxyNets();
}

void SettingsPage::setOptionsModel(OptionsModel * model)
{
    this->model = model;

    if(model)
    {
        /* check if client restart is needed and show persistent message */
        if (model->isRestartRequired())
            showRestartWarning(true);

        QString strLabel = model->getOverriddenByCommandLine();
        if (strLabel.isEmpty())
            strLabel = tr("none");
        //ui->overriddenByCommandLineLabel->setText(strLabel);

        mapper->setModel(model);
        setMapper();
        mapper->toFirst();

        updateDefaultProxyNets();
    }

    /* warn when one of the following settings changes by user action (placed here so init via mapper doesn't trigger them) */

    /* Main */
    connect(ui->databaseCache, SIGNAL(valueChanged(int)), this, SLOT(showRestartWarning()));
    connect(ui->threadsScriptVerif, SIGNAL(valueChanged(int)), this, SLOT(showRestartWarning()));
    /* Wallet */
    connect(ui->showMasternodesTab, SIGNAL(clicked(bool)), this, SLOT(showRestartWarning()));
    connect(ui->spendZeroConfChange, SIGNAL(clicked(bool)), this, SLOT(showRestartWarning()));
    /* Network */
    connect(ui->allowIncoming, SIGNAL(clicked(bool)), this, SLOT(showRestartWarning()));
    connect(ui->connectSocks, SIGNAL(clicked(bool)), this, SLOT(showRestartWarning()));
    connect(ui->connectSocksTor, SIGNAL(clicked(bool)), this, SLOT(showRestartWarning()));
    /* Display */
    connect(ui->digits, SIGNAL(valueChanged()), this, SLOT(showRestartWarning()));
    connect(ui->lang, SIGNAL(valueChanged()), this, SLOT(showRestartWarning()));
}

void SettingsPage::on_hideTrayIcon_stateChanged(int fState)
{
    if(fState)
    {
        ui->minimizeToTray->setChecked(false);
        ui->minimizeToTray->setEnabled(false);
    }
    else
    {
        ui->minimizeToTray->setEnabled(true);
    }
}

void SettingsPage::showRestartWarning(bool fPersistent)
{
    ui->statusLabel->setStyleSheet("QLabel { color: red; }");

    if(fPersistent)
    {
        ui->statusLabel->setText(tr("Client restart required to activate changes."));
    }
    else
    {
        ui->statusLabel->setText(tr("This change would require a client restart."));
        // clear non-persistent status label after 10 seconds
        // Todo: should perhaps be a class attribute, if we extend the use of statusLabel
        QTimer::singleShot(10000, this, SLOT(clearStatusLabel()));
    }
}

void SettingsPage::clearStatusLabel()
{
    ui->statusLabel->clear();
}

void SettingsPage::updateProxyValidationState()
{
    QValidatedLineEdit *pUiProxyIp = ui->proxyIp;
    QValidatedLineEdit *otherProxyWidget = (pUiProxyIp == ui->proxyIpTor) ? ui->proxyIp : ui->proxyIpTor;
    if (pUiProxyIp->isValid() && (!ui->proxyPort->isEnabled() || ui->proxyPort->text().toInt() > 0) && (!ui->proxyPortTor->isEnabled() || ui->proxyPortTor->text().toInt() > 0))
    {
        //setOkButtonState(otherProxyWidget->isValid()); //only enable ok button if both proxys are valid
        ui->statusLabel->clear();
    }
    else
    {
        //setOkButtonState(false);
        ui->statusLabel->setStyleSheet("QLabel { color: red; }");
        ui->statusLabel->setText(tr("The supplied proxy address is invalid."));
    }
}

void SettingsPage::updateDefaultProxyNets()
{
    proxyType proxy;
    std::string strProxy;
    QString strDefaultProxyGUI;

    GetProxy(NET_IPV4, proxy);
    strProxy = proxy.proxy.ToStringIP() + ":" + proxy.proxy.ToStringPort();
    strDefaultProxyGUI = ui->proxyIp->text() + ":" + ui->proxyPort->text();
    (strProxy == strDefaultProxyGUI.toStdString()) ? ui->proxyReachIPv4->setChecked(true) : ui->proxyReachIPv4->setChecked(false);

    GetProxy(NET_IPV6, proxy);
    strProxy = proxy.proxy.ToStringIP() + ":" + proxy.proxy.ToStringPort();
    strDefaultProxyGUI = ui->proxyIp->text() + ":" + ui->proxyPort->text();
    (strProxy == strDefaultProxyGUI.toStdString()) ? ui->proxyReachIPv6->setChecked(true) : ui->proxyReachIPv6->setChecked(false);

    GetProxy(NET_TOR, proxy);
    strProxy = proxy.proxy.ToStringIP() + ":" + proxy.proxy.ToStringPort();
    strDefaultProxyGUI = ui->proxyIp->text() + ":" + ui->proxyPort->text();
    (strProxy == strDefaultProxyGUI.toStdString()) ? ui->proxyReachTor->setChecked(true) : ui->proxyReachTor->setChecked(false);
}
/*
ProxyAddressValidator::ProxyAddressValidator(QObject *parent) :
QValidator(parent)
{
}

QValidator::State ProxyAddressValidator::validate(QString &input, int &pos) const
{
    Q_UNUSED(pos);
    // Validate the proxy
    CService serv(LookupNumeric(input.toStdString().c_str(), 9050));
    proxyType addrProxy = proxyType(serv, true);
    if (addrProxy.IsValid())
        return QValidator::Acceptable;

    return QValidator::Invalid;
}*/
