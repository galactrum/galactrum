// Copyright (c) 2011-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The Dash Core developers
// Copyright (c) 2017-2018 The Galactrum developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "toolspage.h"
#include "rpcconsole.h"
#include "ui_toolspage.h"

#include "bantablemodel.h"
#include "clientmodel.h"
#include "guiutil.h"
#include "platformstyle.h"

#include <interfaces/node.h>
#include "chainparams.h"
#include "netbase.h"
#include "rpc/server.h"
#include "rpc/client.h"
#include "util.h"
#include "utilmoneystr.h"
#include "script/sign.h"
#include "stakenode/tposutils.h"
#include "tposaddressestablemodel.h"
#include <interfaces/wallet.h>
#include <wallet/coincontrol.h>
#include <qt/walletmodel.h>

#include <openssl/crypto.h>

#include <univalue.h>

#ifdef ENABLE_WALLET
#include <db_cxx.h>
#endif

#include <QDir>
#include <QKeyEvent>
#include <QMenu>
#include <QScrollBar>
#include <QSettings>
#include <QSignalMapper>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QStringList>

#if QT_VERSION < 0x050000
#include <QUrl>
#endif

// TODO: add a scrollback limit, as there is currently none
// TODO: make it possible to filter out categories (esp debug messages when implemented)
// TODO: receive errors and debug messages through ClientModel

const int CONSOLE_HISTORY = 50;
const QSize FONT_RANGE(4, 40);
const char fontSizeSettingsKey[] = "consoleFontSize";

const TrafficGraphData::GraphRange INITIAL_TRAFFIC_GRAPH_SETTING = TrafficGraphData::Range_30m;

// Repair parameters
const QString SALVAGEWALLET("-salvagewallet");
const QString RESCAN("-rescan");
const QString ZAPTXES1("-zapwallettxes=1");
const QString ZAPTXES2("-zapwallettxes=2");
const QString UPGRADEWALLET("-upgradewallet");
const QString REINDEX("-reindex");

const struct {
    const char *url;
    const char *source;
} ICON_MAPPING[] = {
    {"cmd-request", "tx_input"},
    {"cmd-reply", "tx_output"},
    {"cmd-error", "tx_output"},
    {"misc", "tx_inout"},
    {NULL, NULL}
};


/* Object for executing console RPC commands in a separate thread. */

class RPCExecutor : public QObject
{
    Q_OBJECT
public:
    RPCExecutor(interfaces::Node& node) : m_node(node) {}

public Q_SLOTS:
    void request(const QString &command, const QString &walletID);

Q_SIGNALS:
    void reply(int category, const QString &command);

private:
    interfaces::Node& m_node;
};


/** Class for handling RPC timers
 * (used for e.g. re-locking the wallet after a timeout)*/

class QtRPCTimerBase: public QObject, public RPCTimerBase
{
    Q_OBJECT
public:
    QtRPCTimerBase(boost::function<void(void)>& func, int64_t millis):
        func(func)
    {
        timer.setSingleShot(true);
        connect(&timer, SIGNAL(timeout()), this, SLOT(timeout()));
        timer.start(millis);
    }
    ~QtRPCTimerBase() {}
private Q_SLOTS:
    void timeout() { func(); }
private:
    QTimer timer;
    boost::function<void(void)> func;
};

class QtRPCTimerInterface: public RPCTimerInterface
{
public:
    ~QtRPCTimerInterface() {}
    const char *Name() { return "Qt"; }
    RPCTimerBase* NewTimer(boost::function<void(void)>& func, int64_t millis)
    {
        return new QtRPCTimerBase(func, millis);
    }
};


ToolsPage::ToolsPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ToolsPage),
    clientModel(0),
    historyPtr(0),
    platformStyle(platformStyle),
    peersTableContextMenu(0),
    banTableContextMenu(0),
    consoleFontSize(0)
{
    ui->setupUi(this);
    GUIUtil::restoreWindowGeometry("nToolsPageWindow", this->size(), this);
    QString theme = GUIUtil::getThemeName();
    if (platformStyle->getImagesOnButtons()) {
        ui->openDebugLogfileButton->setIcon(QIcon(":/icons/" + theme + "/export"));
        ui->openDebugLogfileButton->setIconSize(QSize(32, 32));
    }
    // Needed on Mac also
    ui->clearButton->setIcon(QIcon(":/icons/" + theme + "/remove"));
    ui->fontBiggerButton->setIcon(QIcon(":/icons/" + theme + "/fontbigger"));
    ui->fontSmallerButton->setIcon(QIcon(":/icons/" + theme + "/fontsmaller"));

    // Install event filter for up and down arrow
    ui->lineEdit->installEventFilter(this);
    ui->messagesWidget->installEventFilter(this);

    connect(ui->clearButton, SIGNAL(clicked()), this, SLOT(clear()));
    connect(ui->fontBiggerButton, SIGNAL(clicked()), this, SLOT(fontBigger()));
    connect(ui->fontSmallerButton, SIGNAL(clicked()), this, SLOT(fontSmaller()));
    connect(ui->btnClearTrafficGraph, SIGNAL(clicked()), ui->trafficGraph, SLOT(clear()));

    // Wallet Repair Buttons
    // connect(ui->btn_salvagewallet, SIGNAL(clicked()), this, SLOT(walletSalvage()));
    // Disable salvage option in GUI, it's way too powerful and can lead to funds loss
    ui->btn_salvagewallet->setEnabled(false);
    connect(ui->btn_rescan, SIGNAL(clicked()), this, SLOT(walletRescan()));
    connect(ui->btn_zapwallettxes1, SIGNAL(clicked()), this, SLOT(walletZaptxes1()));
    connect(ui->btn_zapwallettxes2, SIGNAL(clicked()), this, SLOT(walletZaptxes2()));
    connect(ui->btn_upgradewallet, SIGNAL(clicked()), this, SLOT(walletUpgrade()));
    connect(ui->btn_reindex, SIGNAL(clicked()), this, SLOT(walletReindex()));

    // set library version labels
#ifdef ENABLE_WALLET
    ui->berkeleyDBVersion->setText(DbEnv::version(0, 0, 0));
    std::string walletPath = GetDataDir().string();
    walletPath += QDir::separator().toLatin1() + gArgs.GetArg("-wallet", "wallet.dat");
    ui->wallet_path->setText(QString::fromStdString(walletPath));
#else
    ui->label_berkeleyDBVersion->hide();
    ui->berkeleyDBVersion->hide();
#endif

    setTrafficGraphRange(INITIAL_TRAFFIC_GRAPH_SETTING);

    ui->peerHeading->setText(tr("Select a peer to view detailed information."));

    QSettings settings;
    consoleFontSize = settings.value(fontSizeSettingsKey, QFontInfo(QFont()).pointSize()).toInt();
    clear();

    ui->promptIcon->setIcon(QIcon(":/icons/light/chevron-right"));
    ui->promptIcon->setIconSize(QSize(32, 32));
}

ToolsPage::~ToolsPage()
{
    //GUIUtil::saveWindowGeometry("nToolsPageWindow", this);
    //delete rpcTimerInterface;
    delete ui;
}

bool ToolsPage::eventFilter(QObject* obj, QEvent *event)
{
    if(event->type() == QEvent::KeyPress) // Special key handling
    {
        QKeyEvent *keyevt = static_cast<QKeyEvent*>(event);
        int key = keyevt->key();
        Qt::KeyboardModifiers mod = keyevt->modifiers();
        switch(key)
        {
        case Qt::Key_Up: if(obj == ui->lineEdit) { browseHistory(-1); return true; } break;
        case Qt::Key_Down: if(obj == ui->lineEdit) { browseHistory(1); return true; } break;
        case Qt::Key_PageUp: /* pass paging keys to messages widget */
        case Qt::Key_PageDown:
            if(obj == ui->lineEdit)
            {
                QApplication::postEvent(ui->messagesWidget, new QKeyEvent(*keyevt));
                return true;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // forward these events to lineEdit
            if(obj == autoCompleter->popup()) {
                QApplication::postEvent(ui->lineEdit, new QKeyEvent(*keyevt));
                return true;
            }
            break;
        default:
            // Typing in messages widget brings focus to line edit, and redirects key there
            // Exclude most combinations and keys that emit no text, except paste shortcuts
            if(obj == ui->messagesWidget && (
                  (!mod && !keyevt->text().isEmpty() && key != Qt::Key_Tab) ||
                  ((mod & Qt::ControlModifier) && key == Qt::Key_V) ||
                  ((mod & Qt::ShiftModifier) && key == Qt::Key_Insert)))
            {
                ui->lineEdit->setFocus();
                QApplication::postEvent(ui->lineEdit, new QKeyEvent(*keyevt));
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void ToolsPage::setClientModel(ClientModel *model)
{
    clientModel = model;
    ui->trafficGraph->setClientModel(model);
    if (model && clientModel->getPeerTableModel() && clientModel->getBanTableModel()) {
        // Keep up to date with client
        setNumConnections(model->getNumConnections());
        connect(model, SIGNAL(numConnectionsChanged(int)), this, SLOT(setNumConnections(int)));

        interfaces::Node& node = clientModel->node();
        setNumBlocks(node.getNumBlocks(), QDateTime::fromTime_t(node.getLastBlockTime()), node.getVerificationProgress(), false);;
        connect(model, SIGNAL(numBlocksChanged(int,QDateTime,double,bool)), this, SLOT(setNumBlocks(int,QDateTime,double,bool)));

        updateNetworkState();
        connect(model, SIGNAL(networkActiveChanged(bool)), this, SLOT(setNetworkActive(bool)));

        setMasternodeCount(model->getMasternodeCountString());
        connect(model, SIGNAL(strMasternodesChanged(QString)), this, SLOT(setMasternodeCount(QString)));

        updateTrafficStats(node.getTotalBytesRecv(), node.getTotalBytesSent());;
        connect(model, SIGNAL(bytesChanged(quint64,quint64)), this, SLOT(updateTrafficStats(quint64, quint64)));

        connect(model, SIGNAL(mempoolSizeChanged(long,size_t)), this, SLOT(setMempoolSize(long,size_t)));

        // set up peer table
        ui->peerWidget->setModel(model->getPeerTableModel());
        ui->peerWidget->verticalHeader()->hide();
        ui->peerWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->peerWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->peerWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
        ui->peerWidget->setContextMenuPolicy(Qt::CustomContextMenu);
        ui->peerWidget->setColumnWidth(PeerTableModel::Address, ADDRESS_COLUMN_WIDTH);
        ui->peerWidget->setColumnWidth(PeerTableModel::Subversion, SUBVERSION_COLUMN_WIDTH);
        ui->peerWidget->setColumnWidth(PeerTableModel::Ping, PING_COLUMN_WIDTH);
        ui->peerWidget->horizontalHeader()->setStretchLastSection(true);

        // create peer table context menu actions
        QAction* disconnectAction = new QAction(tr("&Disconnect"), this);
        QAction* banAction1h      = new QAction(tr("Ban for") + " " + tr("1 &hour"), this);
        QAction* banAction24h     = new QAction(tr("Ban for") + " " + tr("1 &day"), this);
        QAction* banAction7d      = new QAction(tr("Ban for") + " " + tr("1 &week"), this);
        QAction* banAction365d    = new QAction(tr("Ban for") + " " + tr("1 &year"), this);

        // create peer table context menu
        peersTableContextMenu = new QMenu(this);
        peersTableContextMenu->addAction(disconnectAction);
        peersTableContextMenu->addAction(banAction1h);
        peersTableContextMenu->addAction(banAction24h);
        peersTableContextMenu->addAction(banAction7d);
        peersTableContextMenu->addAction(banAction365d);

        // Add a signal mapping to allow dynamic context menu arguments.
        // We need to use int (instead of int64_t), because signal mapper only supports
        // int or objects, which is okay because max bantime (1 year) is < int_max.
        QSignalMapper* signalMapper = new QSignalMapper(this);
        signalMapper->setMapping(banAction1h, 60*60);
        signalMapper->setMapping(banAction24h, 60*60*24);
        signalMapper->setMapping(banAction7d, 60*60*24*7);
        signalMapper->setMapping(banAction365d, 60*60*24*365);
        connect(banAction1h, SIGNAL(triggered()), signalMapper, SLOT(map()));
        connect(banAction24h, SIGNAL(triggered()), signalMapper, SLOT(map()));
        connect(banAction7d, SIGNAL(triggered()), signalMapper, SLOT(map()));
        connect(banAction365d, SIGNAL(triggered()), signalMapper, SLOT(map()));
        connect(signalMapper, SIGNAL(mapped(int)), this, SLOT(banSelectedNode(int)));

        // peer table context menu signals
        connect(ui->peerWidget, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(showPeersTableContextMenu(const QPoint&)));
        connect(disconnectAction, SIGNAL(triggered()), this, SLOT(disconnectSelectedNode()));

        // peer table signal handling - update peer details when selecting new node
        connect(ui->peerWidget->selectionModel(), SIGNAL(selectionChanged(const QItemSelection &, const QItemSelection &)),
            this, SLOT(peerSelected(const QItemSelection &, const QItemSelection &)));
        // peer table signal handling - update peer details when new nodes are added to the model
        connect(model->getPeerTableModel(), SIGNAL(layoutChanged()), this, SLOT(peerLayoutChanged()));
        // peer table signal handling - cache selected node ids
        connect(model->getPeerTableModel(), SIGNAL(layoutAboutToBeChanged()), this, SLOT(peerLayoutAboutToChange()));

        // set up ban table
        ui->banlistWidget->setModel(model->getBanTableModel());
        ui->banlistWidget->verticalHeader()->hide();
        ui->banlistWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->banlistWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->banlistWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        ui->banlistWidget->setContextMenuPolicy(Qt::CustomContextMenu);
        ui->banlistWidget->setColumnWidth(BanTableModel::Address, BANSUBNET_COLUMN_WIDTH);
        ui->banlistWidget->setColumnWidth(BanTableModel::Bantime, BANTIME_COLUMN_WIDTH);
        ui->banlistWidget->horizontalHeader()->setStretchLastSection(true);

        // create ban table context menu action
        QAction* unbanAction = new QAction(tr("&Unban"), this);

        // create ban table context menu
        banTableContextMenu = new QMenu(this);
        banTableContextMenu->addAction(unbanAction);

        // ban table context menu signals
        connect(ui->banlistWidget, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(showBanTableContextMenu(const QPoint&)));
        connect(unbanAction, SIGNAL(triggered()), this, SLOT(unbanSelectedNode()));

        // ban table signal handling - clear peer details when clicking a peer in the ban table
        connect(ui->banlistWidget, SIGNAL(clicked(const QModelIndex&)), this, SLOT(clearSelectedNode()));
        // ban table signal handling - ensure ban table is shown or hidden (if empty)
        connect(model->getBanTableModel(), SIGNAL(layoutChanged()), this, SLOT(showOrHideBanTableIfRequired()));
        showOrHideBanTableIfRequired();

        // Provide initial values
        ui->clientVersion->setText(model->formatFullVersion());
        ui->clientUserAgent->setText(model->formatSubVersion());
        ui->dataDir->setText(model->dataDir());
        ui->startupTime->setText(model->formatClientStartupTime());
        ui->networkName->setText(QString::fromStdString(Params().NetworkIDString()));

        //Setup autocomplete and attach it
        QStringList wordList;
        std::vector<std::string> commandList = tableRPC.listCommands();
        for (size_t i = 0; i < commandList.size(); ++i)
        {
            wordList << commandList[i].c_str();
        }

        autoCompleter = new QCompleter(wordList, this);
        ui->lineEdit->setCompleter(autoCompleter);
        autoCompleter->popup()->installEventFilter(this);
        // Start thread to execute RPC commands.
        startExecutor();

        // TPoS
        connect(ui->createStakeContractButton, &QPushButton::clicked, this, &ToolsPage::onStakeClicked);
        connect(ui->clearStakeContractButton, &QPushButton::clicked, this, &ToolsPage::onClearClicked);
        //    connect(ui->showRequestButton, &QPushButton::clicked, this, &ToolsPage::onShowRequestClicked);
        connect(ui->cancelStakeContractButton, &QPushButton::clicked, this, &ToolsPage::onCancelClicked);
    }
    if (!model) {
        // Client model is being set to 0, this means shutdown() is about to be called.
        // Make sure we clean up the executor thread
        Q_EMIT stopExecutor();
        thread.wait();
    }
}

static QString categoryClass(int category)
{
    switch(category)
    {
    case ToolsPage::CMD_REQUEST:  return "cmd-request"; break;
    case ToolsPage::CMD_REPLY:    return "cmd-reply"; break;
    case ToolsPage::CMD_ERROR:    return "cmd-error"; break;
    default:                       return "misc";
    }
}

void ToolsPage::fontBigger()
{
    setFontSize(consoleFontSize+1);
}

void ToolsPage::fontSmaller()
{
    setFontSize(consoleFontSize-1);
}

void ToolsPage::setFontSize(int newSize)
{
    QSettings settings;

    //don't allow a insane font size
    if (newSize < FONT_RANGE.width() || newSize > FONT_RANGE.height())
        return;

    // temp. store the console content
    QString str = ui->messagesWidget->toHtml();

    // replace font tags size in current content
    str.replace(QString("font-size:%1pt").arg(consoleFontSize), QString("font-size:%1pt").arg(newSize));

    // store the new font size
    consoleFontSize = newSize;
    settings.setValue(fontSizeSettingsKey, consoleFontSize);

    // clear console (reset icon sizes, default stylesheet) and re-add the content
    float oldPosFactor = 1.0 / ui->messagesWidget->verticalScrollBar()->maximum() * ui->messagesWidget->verticalScrollBar()->value();
    clear(false);
    ui->messagesWidget->setHtml(str);
    ui->messagesWidget->verticalScrollBar()->setValue(oldPosFactor * ui->messagesWidget->verticalScrollBar()->maximum());
}

/** Restart wallet with "-salvagewallet" */
void ToolsPage::walletSalvage()
{
    buildParameterlist(SALVAGEWALLET);
}

/** Restart wallet with "-rescan" */
void ToolsPage::walletRescan()
{
    buildParameterlist(RESCAN);
}

/** Restart wallet with "-zapwallettxes=1" */
void ToolsPage::walletZaptxes1()
{
    buildParameterlist(ZAPTXES1);
}

/** Restart wallet with "-zapwallettxes=2" */
void ToolsPage::walletZaptxes2()
{
    buildParameterlist(ZAPTXES2);
}

/** Restart wallet with "-upgradewallet" */
void ToolsPage::walletUpgrade()
{
    buildParameterlist(UPGRADEWALLET);
}

/** Restart wallet with "-reindex" */
void ToolsPage::walletReindex()
{
    buildParameterlist(REINDEX);
}

/** Build command-line parameter list for restart */
void ToolsPage::buildParameterlist(QString arg)
{
    // Get command-line arguments and remove the application name
    QStringList args = QApplication::arguments();
    args.removeFirst();

    // Remove existing repair-options
    args.removeAll(SALVAGEWALLET);
    args.removeAll(RESCAN);
    args.removeAll(ZAPTXES1);
    args.removeAll(ZAPTXES2);
    args.removeAll(UPGRADEWALLET);
    args.removeAll(REINDEX);

    // Append repair parameter to command line.
    args.append(arg);

    // Send command-line arguments to BitcoinGUI::handleRestart()
    Q_EMIT handleRestart(args);
}

void ToolsPage::clear(bool clearHistory)
{
    ui->messagesWidget->clear();
    if(clearHistory)
    {
        history.clear();
        historyPtr = 0;
    }
    ui->lineEdit->clear();
    ui->lineEdit->setFocus();

    // Add smoothly scaled icon images.
    // (when using width/height on an img, Qt uses nearest instead of linear interpolation)
    QString iconPath = ":/icons/" + GUIUtil::getThemeName() + "/";
    QString iconName = "";

    for(int i=0; ICON_MAPPING[i].url; ++i)
    {
        iconName = ICON_MAPPING[i].source;
        ui->messagesWidget->document()->addResource(
                    QTextDocument::ImageResource,
                    QUrl(ICON_MAPPING[i].url),
                    QImage(iconPath + iconName).scaled(QSize(consoleFontSize*2, consoleFontSize*2), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }

    // Set default style sheet
    QFontInfo fixedFontInfo(GUIUtil::fixedPitchFont());
    ui->messagesWidget->document()->setDefaultStyleSheet(
        QString(
                "table { }"
                "td.time { color: #ccc; font-size: %2; padding-top: 3px; } "
                "td.message { font-family: %1; font-size: %2; white-space:pre-wrap; } "
                "td.cmd-request { color: #ffd700; } "
                "td.cmd-error { color: red; } "
                "b { color: #ffd700; } "
            ).arg(fixedFontInfo.family(), QString("%1pt").arg(consoleFontSize))
        );

    message(CMD_REPLY, (tr("Welcome to the Galactrum RPC console.") + "<br>" +
                        tr("Use up and down arrows to navigate history, and <b>Ctrl-L</b> to clear screen.") + "<br>" +
                        tr("Type <b>help</b> for an overview of available commands.")), true);
}

void ToolsPage::keyPressEvent(QKeyEvent *event)
{
    if(windowType() != Qt::Widget && event->key() == Qt::Key_Escape)
    {
        close();
    }
}

void ToolsPage::message(int category, const QString &message, bool html)
{
    QTime time = QTime::currentTime();
    QString timeString = time.toString();
    QString out;
    out += "<table><tr><td class=\"time\" width=\"65\">" + timeString + "</td>";
    out += "<td class=\"icon\" width=\"32\"><img src=\"" + categoryClass(category) + "\"></td>";
    out += "<td class=\"message " + categoryClass(category) + "\" valign=\"middle\">";
    if(html)
        out += message;
    else
        out += GUIUtil::HtmlEscape(message, false);
    out += "</td></tr></table>";
    ui->messagesWidget->append(out);
}

void ToolsPage::updateNetworkState()
{
    QString connections = QString::number(clientModel->getNumConnections()) + " (";
    connections += tr("In:") + " " + QString::number(clientModel->getNumConnections(CONNECTIONS_IN)) + " / ";
    connections += tr("Out:") + " " + QString::number(clientModel->getNumConnections(CONNECTIONS_OUT)) + ")";

    if(!clientModel->node().getNetworkActive()) {
        connections += " (" + tr("Network activity disabled") + ")";
    }

    ui->numberOfConnections->setText(connections);
}

void ToolsPage::setNumConnections(int count)
{
    if (!clientModel)
        return;

    updateNetworkState();
}

void ToolsPage::setNetworkActive(bool networkActive)
{
    updateNetworkState();
}

void ToolsPage::setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool headers)
{
    if (!headers) {
        ui->numberOfBlocks->setText(QString::number(count));
        ui->lastBlockTime->setText(blockDate.toString());
    }
}

void ToolsPage::setMasternodeCount(const QString &strMasternodes)
{
    ui->masternodeCount->setText(strMasternodes);
}

void ToolsPage::setMempoolSize(long numberOfTxs, size_t dynUsage)
{
    ui->mempoolNumberTxs->setText(QString::number(numberOfTxs));

    if (dynUsage < 1000000)
        ui->mempoolSize->setText(QString::number(dynUsage/1000.0, 'f', 2) + " KB");
    else
        ui->mempoolSize->setText(QString::number(dynUsage/1000000.0, 'f', 2) + " MB");
}

void ToolsPage::on_lineEdit_returnPressed()
{
    QString cmd = ui->lineEdit->text();
    ui->lineEdit->clear();

    if(!cmd.isEmpty())
    {
        std::string strFilteredCmd;
        try {
            std::string dummy;
            if (!RPCConsole::RPCParseCommandLine(nullptr, dummy, cmd.toStdString(), false, &strFilteredCmd)) {
                // Failed to parse command, so we cannot even filter it for the history
                throw std::runtime_error("Invalid command line");
            }
        } catch (const std::exception& e) {
            QMessageBox::critical(this, "Error", QString("Error: ") + QString::fromStdString(e.what()));
            return;
        }

        ui->lineEdit->clear();

        cmdBeforeBrowsing = QString();

        QString walletID;
#ifdef ENABLE_WALLET
        const int wallet_index = ui->WalletSelector->currentIndex();
        if (wallet_index > 0) {
            walletID = (QString)ui->WalletSelector->itemData(wallet_index).value<QString>();
        }

        if (m_last_wallet_id != walletID) {
            if (walletID.isNull()) {
                message(CMD_REQUEST, tr("Executing command without any wallet"));
            } else {
                message(CMD_REQUEST, tr("Executing command using \"%1\" wallet").arg(walletID));
            }
            m_last_wallet_id = walletID;
        }
#endif

        message(CMD_REQUEST, QString::fromStdString(strFilteredCmd));
        Q_EMIT cmdRequest(cmd, walletID);
        // Remove command, if already in history
        history.removeOne(cmd);
        // Append command to history
        history.append(cmd);
        // Enforce maximum history size
        while(history.size() > CONSOLE_HISTORY)
            history.removeFirst();
        // Set pointer to end of history
        historyPtr = history.size();
        // Scroll console view to end
        scrollToEnd();
    }
}

void ToolsPage::browseHistory(int offset)
{
    historyPtr += offset;
    if(historyPtr < 0)
        historyPtr = 0;
    if(historyPtr > history.size())
        historyPtr = history.size();
    QString cmd;
    if(historyPtr < history.size())
        cmd = history.at(historyPtr);
    ui->lineEdit->setText(cmd);
}

void ToolsPage::startExecutor()
{
    RPCExecutor *executor = new RPCExecutor(clientModel->node());
    executor->moveToThread(&thread);

    // Replies from executor object must go to this object
    connect(executor, SIGNAL(reply(int,QString)), this, SLOT(message(int,QString)));
    // Requests from this object must go to executor
    connect(this, SIGNAL(cmdRequest(QString, QString)), executor, SLOT(request(QString, QString)));

    // On stopExecutor signal
    // - quit the Qt event loop in the execution thread
    connect(this, SIGNAL(stopExecutor()), &thread, SLOT(quit()));
    // - queue executor for deletion (in execution thread)
    connect(&thread, SIGNAL(finished()), executor, SLOT(deleteLater()), Qt::DirectConnection);

    // Default implementation of QThread::run() simply spins up an event loop in the thread,
    // which is what we want.
    thread.start();
}

void ToolsPage::on_tabWidget_currentChanged(int index)
{
    if (ui->tabWidget->widget(index) == ui->tab_console)
        ui->lineEdit->setFocus();
    else if (ui->tabWidget->widget(index) != ui->tab_peers)
        clearSelectedNode();
}

void ToolsPage::on_openDebugLogfileButton_clicked()
{
    GUIUtil::openDebugLogfile();
}

void ToolsPage::scrollToEnd()
{
    QScrollBar *scrollbar = ui->messagesWidget->verticalScrollBar();
    scrollbar->setValue(scrollbar->maximum());
}

void ToolsPage::on_sldGraphRange_valueChanged(int value)
{
    setTrafficGraphRange(static_cast<TrafficGraphData::GraphRange>(value));
}

QString ToolsPage::FormatBytes(quint64 bytes)
{
    if(bytes < 1024)
        return QString(tr("%1 B")).arg(bytes);
    if(bytes < 1024 * 1024)
        return QString(tr("%1 KB")).arg(bytes / 1024);
    if(bytes < 1024 * 1024 * 1024)
        return QString(tr("%1 MB")).arg(bytes / 1024 / 1024);

    return QString(tr("%1 GB")).arg(bytes / 1024 / 1024 / 1024);
}

void ToolsPage::setTrafficGraphRange(TrafficGraphData::GraphRange range)
{
    ui->trafficGraph->setGraphRangeMins(range);
    ui->lblGraphRange->setText(GUIUtil::formatDurationStr(TrafficGraphData::RangeMinutes[range] * 60));
}

void ToolsPage::updateTrafficStats(quint64 totalBytesIn, quint64 totalBytesOut)
{
    ui->lblBytesIn->setText(GUIUtil::formatBytes(totalBytesIn));
    ui->lblBytesOut->setText(GUIUtil::formatBytes(totalBytesOut));
}

void ToolsPage::peerSelected(const QItemSelection &selected, const QItemSelection &deselected)
{
    Q_UNUSED(deselected);

    if (!clientModel || !clientModel->getPeerTableModel() || selected.indexes().isEmpty())
        return;

    const CNodeCombinedStats *stats = clientModel->getPeerTableModel()->getNodeStats(selected.indexes().first().row());
    if (stats)
        updateNodeDetail(stats);
}

void ToolsPage::peerLayoutAboutToChange()
{
    QModelIndexList selected = ui->peerWidget->selectionModel()->selectedIndexes();
    cachedNodeids.clear();
    for(int i = 0; i < selected.size(); i++)
    {
        const CNodeCombinedStats *stats = clientModel->getPeerTableModel()->getNodeStats(selected.at(i).row());
        cachedNodeids.append(stats->nodeStats.nodeid);
    }
}
#ifdef ENABLE_WALLET
void ToolsPage::addWallet(WalletModel * const walletModel)
{
    const QString name = walletModel->getWalletName();
    // use name for text and internal data object (to allow to move to a wallet id later)
    QString display_name = name.isEmpty() ? "["+tr("default wallet")+"]" : name;
    ui->WalletSelector->addItem(display_name, name);
    if (ui->WalletSelector->count() == 2 && !isVisible()) {
        // First wallet added, set to default so long as the window isn't presently visible (and potentially in use)
        ui->WalletSelector->setCurrentIndex(1);
    }
    if (ui->WalletSelector->count() > 2) {
        ui->WalletSelector->setVisible(true);
        ui->WalletSelectorLabel->setVisible(true);
    }
    this->walletModel = walletModel;
}
#endif

void ToolsPage::peerLayoutChanged()
{
    if (!clientModel || !clientModel->getPeerTableModel())
        return;

    const CNodeCombinedStats *stats = NULL;
    bool fUnselect = false;
    bool fReselect = false;

    if (cachedNodeids.empty()) // no node selected yet
        return;

    // find the currently selected row
    int selectedRow = -1;
    QModelIndexList selectedModelIndex = ui->peerWidget->selectionModel()->selectedIndexes();
    if (!selectedModelIndex.isEmpty()) {
        selectedRow = selectedModelIndex.first().row();
    }

    // check if our detail node has a row in the table (it may not necessarily
    // be at selectedRow since its position can change after a layout change)
    int detailNodeRow = clientModel->getPeerTableModel()->getRowByNodeId(cachedNodeids.first());

    if (detailNodeRow < 0)
    {
        // detail node disappeared from table (node disconnected)
        fUnselect = true;
    }
    else
    {
        if (detailNodeRow != selectedRow)
        {
            // detail node moved position
            fUnselect = true;
            fReselect = true;
        }

        // get fresh stats on the detail node.
        stats = clientModel->getPeerTableModel()->getNodeStats(detailNodeRow);
    }

    if (fUnselect && selectedRow >= 0) {
        clearSelectedNode();
    }

    if (fReselect)
    {
        for(int i = 0; i < cachedNodeids.size(); i++)
        {
            ui->peerWidget->selectRow(clientModel->getPeerTableModel()->getRowByNodeId(cachedNodeids.at(i)));
        }
    }

    if (stats)
        updateNodeDetail(stats);
}

void ToolsPage::updateNodeDetail(const CNodeCombinedStats *stats)
{
    // update the detail ui with latest node information
    QString peerAddrDetails(QString::fromStdString(stats->nodeStats.addrName) + " ");
    peerAddrDetails += tr("(node id: %1)").arg(QString::number(stats->nodeStats.nodeid));
    if (!stats->nodeStats.addrLocal.empty())
        peerAddrDetails += "<br />" + tr("via %1").arg(QString::fromStdString(stats->nodeStats.addrLocal));
    ui->peerHeading->setText(peerAddrDetails);
    ui->peerServices->setText(GUIUtil::formatServicesStr(stats->nodeStats.nServices));
    ui->peerLastSend->setText(stats->nodeStats.nLastSend ? GUIUtil::formatDurationStr(GetSystemTimeInSeconds() - stats->nodeStats.nLastSend) : tr("never"));
    ui->peerLastRecv->setText(stats->nodeStats.nLastRecv ? GUIUtil::formatDurationStr(GetSystemTimeInSeconds() - stats->nodeStats.nLastRecv) : tr("never"));
    ui->peerBytesSent->setText(FormatBytes(stats->nodeStats.nSendBytes));
    ui->peerBytesRecv->setText(FormatBytes(stats->nodeStats.nRecvBytes));
    ui->peerConnTime->setText(GUIUtil::formatDurationStr(GetSystemTimeInSeconds() - stats->nodeStats.nTimeConnected));
    ui->peerPingTime->setText(GUIUtil::formatPingTime(stats->nodeStats.dPingTime));
    ui->peerPingWait->setText(GUIUtil::formatPingTime(stats->nodeStats.dPingWait));
    ui->peerMinPing->setText(GUIUtil::formatPingTime(stats->nodeStats.dMinPing));
    ui->timeoffset->setText(GUIUtil::formatTimeOffset(stats->nodeStats.nTimeOffset));
    ui->peerVersion->setText(QString("%1").arg(QString::number(stats->nodeStats.nVersion)));
    ui->peerSubversion->setText(QString::fromStdString(stats->nodeStats.cleanSubVer));
    ui->peerDirection->setText(stats->nodeStats.fInbound ? tr("Inbound") : tr("Outbound"));
    ui->peerHeight->setText(QString("%1").arg(QString::number(stats->nodeStats.nStartingHeight)));
    ui->peerWhitelisted->setText(stats->nodeStats.fWhitelisted ? tr("Yes") : tr("No"));

    // This check fails for example if the lock was busy and
    // nodeStateStats couldn't be fetched.
    if (stats->fNodeStateStatsAvailable) {
        // Ban score is init to 0
        ui->peerBanScore->setText(QString("%1").arg(stats->nodeStateStats.nMisbehavior));

        // Sync height is init to -1
        if (stats->nodeStateStats.nSyncHeight > -1)
            ui->peerSyncHeight->setText(QString("%1").arg(stats->nodeStateStats.nSyncHeight));
        else
            ui->peerSyncHeight->setText(tr("Unknown"));

        // Common height is init to -1
        if (stats->nodeStateStats.nCommonHeight > -1)
            ui->peerCommonHeight->setText(QString("%1").arg(stats->nodeStateStats.nCommonHeight));
        else
            ui->peerCommonHeight->setText(tr("Unknown"));
    }

    ui->detailWidget->show();
}

void ToolsPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if(_columnResizingFixer)
        _columnResizingFixer->stretchColumnWidth(TPoSAddressesTableModel::CommissionPaid);
}

void ToolsPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    if (!clientModel || !clientModel->getPeerTableModel())
        return;

    // start PeerTableModel auto refresh
    clientModel->getPeerTableModel()->startAutoRefresh();
}

void ToolsPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);

    if (!clientModel || !clientModel->getPeerTableModel())
        return;

    // stop PeerTableModel auto refresh
    clientModel->getPeerTableModel()->stopAutoRefresh();
}

void ToolsPage::showPeersTableContextMenu(const QPoint& point)
{
    QModelIndex index = ui->peerWidget->indexAt(point);
    if (index.isValid())
        peersTableContextMenu->exec(QCursor::pos());
}

void ToolsPage::showBanTableContextMenu(const QPoint& point)
{
    QModelIndex index = ui->banlistWidget->indexAt(point);
    if (index.isValid())
        banTableContextMenu->exec(QCursor::pos());
}

void ToolsPage::disconnectSelectedNode()
{
    if(!g_connman)
        return;

    // Get selected peer addresses
    QList<QModelIndex> nodes = GUIUtil::getEntryData(ui->peerWidget, PeerTableModel::NetNodeId);
    for(int i = 0; i < nodes.count(); i++)
    {
        // Get currently selected peer address
        NodeId id = nodes.at(i).data().toInt();
        // Find the node, disconnect it and clear the selected node
        if(g_connman->DisconnectNode(id))
            clearSelectedNode();
    }
}

void ToolsPage::banSelectedNode(int bantime)
{
    if (!clientModel || !g_connman)
        return;

    // Get selected peer addresses
    QList<QModelIndex> nodes = GUIUtil::getEntryData(ui->peerWidget, PeerTableModel::NetNodeId);
    for(int i = 0; i < nodes.count(); i++)
    {
        // Get currently selected peer address
        NodeId id = nodes.at(i).data().toInt();

	// Get currently selected peer address
	int detailNodeRow = clientModel->getPeerTableModel()->getRowByNodeId(id);
	if(detailNodeRow < 0)
	    return;

	// Find possible nodes, ban it and clear the selected node
	const CNodeCombinedStats *stats = clientModel->getPeerTableModel()->getNodeStats(detailNodeRow);
	if(stats) {
	    g_connman->Ban(stats->nodeStats.addr, BanReasonManuallyAdded, bantime);
	}
    }
    clearSelectedNode();
    clientModel->getBanTableModel()->refresh();
}

void ToolsPage::unbanSelectedNode()
{
    if (!clientModel)
        return;

    // Get selected ban addresses
    QList<QModelIndex> nodes = GUIUtil::getEntryData(ui->banlistWidget, BanTableModel::Address);
    for(int i = 0; i < nodes.count(); i++)
    {
        // Get currently selected ban address
        QString strNode = nodes.at(i).data().toString();
        CSubNet possibleSubnet;

        LookupSubNet(strNode.toStdString().c_str(), possibleSubnet);
        if (possibleSubnet.IsValid() && g_connman)
        {
            g_connman->Unban(possibleSubnet);
            clientModel->getBanTableModel()->refresh();
        }
    }
}

void ToolsPage::clearSelectedNode()
{
    ui->peerWidget->selectionModel()->clearSelection();
    cachedNodeids.clear();
    ui->detailWidget->hide();
    ui->peerHeading->setText(tr("Select a peer to view detailed information."));
}

void ToolsPage::showOrHideBanTableIfRequired()
{
    if (!clientModel)
        return;

    bool visible = clientModel->getBanTableModel()->shouldShow();
    ui->banlistWidget->setVisible(visible);
    ui->banHeading->setVisible(visible);
}

void ToolsPage::setTabFocus(enum TabTypes tabType)
{
    ui->tabWidget->setCurrentIndex(tabType);
}



namespace ColumnWidths {
enum Values {
    MINIMUM_COLUMN_WIDTH = 120,
    ADDRESS_COLUMN_WIDTH = 240,
    AMOUNT_MINIMUM_COLUMN_WIDTH = 200,
};
}

static QString PrepareCreateContractQuestionString(const CBitcoinAddress &tposAddress,
                                                   const CBitcoinAddress &stakenodeAddress,
                                                   int commission)
{
    QString questionString = QObject::tr("Are you sure you want to setup stake contract?");
    questionString.append("<br /><br />");

    // Show total amount + all alternative units
    questionString.append(QObject::tr("Owner Address = <b>%1</b><br />Stakenode address = <b>%2</b> <br />Stakenode commission = <b>%3</b>%%")
                          .arg(QString::fromStdString(tposAddress.ToString()))
                          .arg(QString::fromStdString(stakenodeAddress.ToString()))
                          .arg(commission));


    return questionString;
}

std::unique_ptr<interfaces::PendingWalletTx> ToolsPage::CreateContractTransaction(QWidget *widget,
                                                                                 const CBitcoinAddress &tposAddress,
                                                                                 const CBitcoinAddress &stakenodeAddress,
                                                                                 int stakenodeCommission)
{
    std::string strError;
    auto questionString = PrepareCreateContractQuestionString(tposAddress, stakenodeAddress, stakenodeCommission);
    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(widget, QObject::tr("Confirm creating stake contract"),
                                                               questionString,
                                                               QMessageBox::Yes | QMessageBox::Cancel,
                                                               QMessageBox::Cancel);
    if(retval != QMessageBox::Yes)
    {
        return {};
    }
    if(auto walletTx =  walletModel->wallet().createTPoSContractTransaction(tposAddress.Get(), stakenodeAddress.Get(), stakenodeCommission, strError))  {
        return walletTx;
    }

    throw std::runtime_error(QString("Failed to create stake contract transaction: %1").arg(QString::fromStdString(strError)).toStdString());
}

std::unique_ptr<interfaces::PendingWalletTx> ToolsPage::CreateCancelContractTransaction(QWidget *widget,
                                                                                       const TPoSContract &contract)
{
    std::string strError;
    auto questionString = QString("Are you sure you want to cancel contract with address: <b>%1</b>").arg(contract.tposAddress.ToString().c_str());
    // Display message box
    QMessageBox::StandardButton retval = QMessageBox::question(widget, QObject::tr("Confirm canceling stake contract"),
                                                               questionString,
                                                               QMessageBox::Yes | QMessageBox::Cancel,
                                                               QMessageBox::Cancel);
    if(retval != QMessageBox::Yes)
    {
        return {};
    }

    if(auto walletTx = walletModel->wallet().createCancelContractTransaction(contract, strError))
    {
        return walletTx;
    }

    throw std::runtime_error(QString("Failed to create stake contract transaction: %1").arg(QString::fromStdString(strError)).toStdString());
}

static void SendPendingTransaction(interfaces::PendingWalletTx *pendingTx)
{
    std::string rejectReason;
    if (!pendingTx->commit({}, {}, {}, rejectReason))
        throw std::runtime_error(rejectReason);
}

void ToolsPage::SendToAddress(const CTxDestination &address, CAmount nValue, int splitCount)
{
    CAmount curBalance = walletModel->wallet().getBalance();

    // Check amount
    if (nValue <= 0)
        throw std::runtime_error("Invalid amount");

    if (nValue > curBalance)
        throw std::runtime_error("Insufficient funds");

    if (!g_connman)
        std::runtime_error("Error: Peer-to-peer functionality missing or disabled");

    // Parse Galactrum address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    //    CReserveKey reservekey(pwalletMain);
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;

    for (int i = 0; i < splitCount; ++i)
    {
        if (i == splitCount - 1)
        {
            uint64_t nRemainder = nValue % splitCount;
            vecSend.push_back({scriptPubKey, static_cast<CAmount>(nValue / splitCount + nRemainder), false});
        }
        else
        {
            vecSend.push_back({scriptPubKey, static_cast<CAmount>(nValue / splitCount), false});
        }
    }

    auto penWalletTx = walletModel->wallet().createTransaction(vecSend, {}, true, nChangePosRet, nFeeRequired, strError);
    if(!penWalletTx)
    {
        if (nValue + nFeeRequired > walletModel->wallet().getBalance())
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!", FormatMoney(nFeeRequired));
        throw std::runtime_error(strError);
    }
    SendPendingTransaction(penWalletTx.get());
}

CBitcoinAddress ToolsPage::GetNewAddress()
{
    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!walletModel->wallet().getKeyFromPool(false, newKey))
        throw std::runtime_error("Error: Keypool ran out, please call keypoolrefill first");
    CKeyID keyID = newKey.GetID();


    walletModel->wallet().setAddressBook(keyID, std::string(), "stake contract address");

    return CBitcoinAddress(keyID);
}


void ToolsPage::setWalletModel(WalletModel *model)
{
    _addressesTableModel = model->getTPoSAddressModel();
    walletModel = model;
    ui->stakingAddressesView->setModel(_addressesTableModel);

    using namespace ColumnWidths;
    _columnResizingFixer = new GUIUtil::TableViewLastColumnResizingFixer(ui->stakingAddressesView, AMOUNT_MINIMUM_COLUMN_WIDTH, MINIMUM_COLUMN_WIDTH, this);

    ui->stakingAddressesView->setColumnWidth(TPoSAddressesTableModel::Address, ADDRESS_COLUMN_WIDTH);
    ui->stakingAddressesView->setColumnWidth(TPoSAddressesTableModel::AmountStaked, MINIMUM_COLUMN_WIDTH);
    ui->stakingAddressesView->setColumnWidth(TPoSAddressesTableModel::CommissionPaid, MINIMUM_COLUMN_WIDTH);
    ui->stakingAddressesView->setColumnWidth(TPoSAddressesTableModel::Amount, AMOUNT_MINIMUM_COLUMN_WIDTH);
}

void ToolsPage::refresh()
{
    _addressesTableModel->updateModel();
}

void ToolsPage::onStakeClicked()
{
    try
    {
        auto worker = [this]() {
            //            CReserveKey reserveKey(pwalletMain);
            CBitcoinAddress tposAddress = GetNewAddress();
            if(!tposAddress.IsValid())
            {
                throw std::runtime_error("Critical error, stake contract address is empty");
            }
            CBitcoinAddress stakenodeAddress(ui->stakenodeAddress->text().toStdString());
            if(!stakenodeAddress.IsValid())
            {
                throw std::runtime_error("Critical error, Stakenode address is empty");
            }
            auto stakenodeCommission = ui->stakenodeCut->value();
            if(auto penWalletTx = CreateContractTransaction(this, tposAddress, stakenodeAddress, stakenodeCommission))
            {
                SendPendingTransaction(penWalletTx.get());
                sendToTPoSAddress(tposAddress);
            }
        };
        WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();
        if (encStatus == WalletModel::Locked || encStatus == walletModel->UnlockedForStakingOnly)
        {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock());
            if (!ctx.isValid())
            {
                //unlock was cancelled
                throw std::runtime_error("Wallet is locked and user declined to unlock. Can't redeem from stake contract address.");
            }

            worker();
        }
        else
        {
            worker();
        }
    }
    catch(std::exception &ex)
    {
        QMessageBox::warning(this, "Stake Contract", ex.what());
    }
}

void ToolsPage::onClearClicked()
{

}

void ToolsPage::onCancelClicked()
{
    auto selectedIndexes = ui->stakingAddressesView->selectionModel()->selectedRows();

    if(selectedIndexes.empty())
        return;

    auto worker = [this, &selectedIndexes] {
        //CReserveKey reserveKey(pwalletMain);
        auto contract = _addressesTableModel->contractByIndex(selectedIndexes.first().row());
        //CWalletTx wtxNew;
        if(auto penWalletTx = CreateCancelContractTransaction(this, contract))
        {
            SendPendingTransaction(penWalletTx.get());
        }
    };

    try
    {
        WalletModel::EncryptionStatus encStatus = walletModel->getEncryptionStatus();
        if (encStatus == WalletModel::Locked || encStatus == walletModel->UnlockedForStakingOnly)
        {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock());
            if (!ctx.isValid())
            {
                //unlock was cancelled
                QMessageBox::warning(this, tr("Stake Contract"),
                                     tr("Wallet is locked and user declined to unlock. Can't redeem from stake contract address."),
                                     QMessageBox::Ok, QMessageBox::Ok);

                return;
            }
            worker();
        }
        else
        {
            worker();
        }
    }
    catch(std::exception &ex)
    {
        QMessageBox::warning(this, "Stake Contract", ex.what());
    }
}

void ToolsPage::onShowRequestClicked()
{
    return;
    QItemSelectionModel *selectionModel = ui->stakingAddressesView->selectionModel();
    auto rows = selectionModel->selectedRows();
    if(!rows.empty())
    {
        QModelIndex index = rows.first();
        QString address = index.data(TPoSAddressesTableModel::Address).toString();
    }

}


void ToolsPage::onStakeError()
{
    //    ui->stakeButton->setEnabled(false);
}


void ToolsPage::sendToTPoSAddress(const CBitcoinAddress &tposAddress)
{
    CAmount amount = ui->stakingAmount->value();
    int numberOfSplits = 1;
    if(amount > walletModel->wallet().getStakeSplitThreshold() * COIN)
        numberOfSplits = std::min<unsigned int>(500, amount / (walletModel->wallet().getStakeSplitThreshold() * COIN));
    SendToAddress(tposAddress.Get(), amount, numberOfSplits);
}
