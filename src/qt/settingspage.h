// Copyright (c) 2017-2018 The Galactrum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SETTINGSPAGE_H
#define BITCOIN_QT_SETTINGSPAGE_H

#include "amount.h"

#include <QWidget>
#include <QValidator>
#include <memory>

class ClientModel;
class PlatformStyle;
class WalletModel;
class OptionsModel;
class QValidatedLineEdit;

QT_BEGIN_NAMESPACE
class QDataWidgetMapper;
QT_END_NAMESPACE

namespace Ui {
    class SettingsPage;
}

/** Proxy address widget validator, checks for a valid proxy address.
 
class ProxyAddressValidator : public QValidator
{
    Q_OBJECT

public:
    explicit ProxyAddressValidator(QObject *parent);

    State validate(QString &input, int &pos) const;
};
*/


class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~SettingsPage();

    void setModel(WalletModel *model);
    void setOptionsModel(OptionsModel *model);
    void setMapper();
    //void eventFilter(QObject *obj, QEvent *event);

private Q_SLOTS:
    /* set OK button state (enabled / disabled) */
    //void setOkButtonState(bool fState);
    //void on_resetButton_clicked();
    //void on_okButton_clicked();
    //void on_cancelButton_clicked();
    
    void on_hideTrayIcon_stateChanged(int fState);

    void showRestartWarning(bool fPersistent = false);
    void clearStatusLabel();
    void updateProxyValidationState();
    /* query the networks, for which the default proxy is used */
    void updateDefaultProxyNets();

    void hideNav();
    void showNav();

    void handleCancelClicked();
    void handleSaveClicked();
    void showConfEditor();
    void showMNConfEditor();
    void showBackups();

Q_SIGNALS:
    void proxyIpChecks(QValidatedLineEdit *pUiProxyIp, int nProxyPort);

protected:
    bool eventFilter(QObject *obj, QEvent *ev);

private:
    Ui::SettingsPage *ui;
    OptionsModel *model;
    QDataWidgetMapper *mapper;
    bool enableWallet;
};

#endif