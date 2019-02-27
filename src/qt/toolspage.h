#ifndef BITCOIN_QT_TOOLSPAGE_H
#define BITCOIN_QT_TOOLSPAGE_H

#include "guiutil.h"
#include "peertablemodel.h"
#include "trafficgraphdata.h"
#include "interfaces/wallet.h"

#include "net.h"

#include <QWidget>
#include <QCompleter>
#include <QThread>
#include <QPointer>

class ClientModel;
class WalletModel;
class PlatformStyle;
class RPCTimerInterface;
class WalletModel;
class RPCConsole;
class CBitcoinAddress;
class TPoSAddressesTableModel;


namespace Ui {
    class ToolsPage;
}
namespace GUIUtil {
class TableViewLastColumnResizingFixer;
}

QT_BEGIN_NAMESPACE
class QMenu;
class QItemSelection;
QT_END_NAMESPACE

class ToolsPage: public QWidget
{
    Q_OBJECT

public:
    explicit ToolsPage(const PlatformStyle* platformStyle, QWidget *parent = 0);
    ~ToolsPage();

    void setClientModel(ClientModel *model);
    void addWallet(WalletModel * const walletModel);
    void setWalletModel(WalletModel* model);
    void refresh();

    enum MessageClass {
        MC_ERROR,
        MC_DEBUG,
        CMD_REQUEST,
        CMD_REPLY,
        CMD_ERROR
    };

    enum TabTypes {
        TAB_INFO = 0,
        TAB_CONSOLE = 1,
        TAB_GRAPH = 2,
        TAB_PEERS = 3,
        TAB_REPAIR = 4,
        TAB_STAKE_CONTRACT = 5
    };

protected:
    virtual bool eventFilter(QObject* obj, QEvent *event);
    void keyPressEvent(QKeyEvent *);

private Q_SLOTS:
    void on_lineEdit_returnPressed();
    void on_tabWidget_currentChanged(int index);
    /** open the debug.log from the current datadir */
    void on_openDebugLogfileButton_clicked();
    /** change the time range of the network traffic graph */
    void on_sldGraphRange_valueChanged(int value);
    /** update traffic statistics */
    void updateTrafficStats(quint64 totalBytesIn, quint64 totalBytesOut);
    void resizeEvent(QResizeEvent *event);
    void showEvent(QShowEvent *event);
    void hideEvent(QHideEvent *event);
    /** Show custom context menu on Peers tab */
    void showPeersTableContextMenu(const QPoint& point);
    /** Show custom context menu on Bans tab */
    void showBanTableContextMenu(const QPoint& point);
    /** Hides ban table if no bans are present */
    void showOrHideBanTableIfRequired();
    /** clear the selected node */
    void clearSelectedNode();
    /* Stake Contract */
    void onStakeClicked();
    void onClearClicked();
    void onCancelClicked();
    void onShowRequestClicked();

public Q_SLOTS:
    void clear(bool clearHistory = true);
    void fontBigger();
    void fontSmaller();
    void setFontSize(int newSize);

    /** Wallet repair options */
    void walletSalvage();
    void walletRescan();
    void walletZaptxes1();
    void walletZaptxes2();
    void walletUpgrade();
    void walletReindex();

    /** Append the message to the message widget */
    void message(int category, const QString &message, bool html = false);
    /** Set number of connections shown in the UI */
    void setNumConnections(int count);
    /** Set network state shown in the UI */
    void setNetworkActive(bool networkActive);
    /** Set number of masternodes shown in the UI */
    void setMasternodeCount(const QString &strMasternodes);
    /** Set number of blocks and last block date shown in the UI */
    void setNumBlocks(int count, const QDateTime& blockDate, double nVerificationProgress, bool headers);
    /** Set size (number of transactions and memory usage) of the mempool in the UI */
    void setMempoolSize(long numberOfTxs, size_t dynUsage);
    /** Go forward or back in history */
    void browseHistory(int offset);
    /** Scroll console view to end */
    void scrollToEnd();
    /** Handle selection of peer in peers list */
    void peerSelected(const QItemSelection &selected, const QItemSelection &deselected);
    /** Handle selection caching before update */
    void peerLayoutAboutToChange();
    /** Handle updated peer information */
    void peerLayoutChanged();
    /** Disconnect a selected node on the Peers tab */
    void disconnectSelectedNode();
    /** Ban a selected node on the Peers tab */
    void banSelectedNode(int bantime);
    /** Unban a selected node on the Bans tab */
    void unbanSelectedNode();
    /** set which tab has the focus (is visible) */
    void setTabFocus(enum TabTypes tabType);

Q_SIGNALS:
    // For RPC command executor
    void stopExecutor();
    void cmdRequest(const QString &command, const QString &walletID);
    /** Get restart command-line parameters and handle restart */
    void handleRestart(QStringList args);

private:
    static QString FormatBytes(quint64 bytes);
    void startExecutor();
    void setTrafficGraphRange(TrafficGraphData::GraphRange range);
    /** Build parameter list for restart */
    void buildParameterlist(QString arg);
    /** show detailed information on ui about selected node */
    void updateNodeDetail(const CNodeCombinedStats *stats);
    /* Stake Contract */
    void onStakeError();
    void SendToAddress(const CTxDestination &address, CAmount nValue, int splitCount);
    void sendToTPoSAddress(const CBitcoinAddress &tposAddress);
    CBitcoinAddress GetNewAddress();

    std::unique_ptr<interfaces::PendingWalletTx> CreateContractTransaction(QWidget *widget,
                                          const CBitcoinAddress &tposAddress,
                                          const CBitcoinAddress &stakenodeAddress,
                                          int stakenodeCommission);

    std::unique_ptr<interfaces::PendingWalletTx> CreateCancelContractTransaction(QWidget *widget,
                                                const TPoSContract &contract);

    enum ColumnWidths
    {
        ADDRESS_COLUMN_WIDTH = 170,
        SUBVERSION_COLUMN_WIDTH = 150,
        PING_COLUMN_WIDTH = 80,
        BANSUBNET_COLUMN_WIDTH = 200,
        BANTIME_COLUMN_WIDTH = 250

    };
    Ui::ToolsPage *ui;
    ClientModel *clientModel = nullptr;
    WalletModel *walletModel = nullptr;
    QStringList history;
    int historyPtr = 0;
    QString cmdBeforeBrowsing;
    QList<NodeId> cachedNodeids;
    const PlatformStyle* const platformStyle;
    RPCTimerInterface *rpcTimerInterface = nullptr;
    QMenu *peersTableContextMenu = nullptr;
    QMenu *banTableContextMenu = nullptr;
    int consoleFontSize = 0;
    QCompleter *autoCompleter = nullptr;
    QThread thread;
    QString m_last_wallet_id;
    RPCConsole *rpcConsole;
    GUIUtil::TableViewLastColumnResizingFixer* _columnResizingFixer = nullptr;
    QPointer<TPoSAddressesTableModel> _addressesTableModel;


    /** Update UI with latest network info from model. */
    void updateNetworkState();
};
#endif