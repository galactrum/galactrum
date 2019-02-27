
// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SRC_STAKENODECONFIG_H_
#define SRC_STAKENODECONFIG_H_

class CStakenodeConfig;
extern CStakenodeConfig stakenodeConfig;

class CStakenodeConfig
{

public:

    class CStakenodeEntry {

    private:
        std::string alias;
        std::string ip;
        std::string stakenodePrivKey;
        std::string hashContractTxId;

    public:

        CStakenodeEntry(std::string alias, std::string ip, std::string stakenodePrivKey, std::string hashContractTxId) {
            this->alias = alias;
            this->ip = ip;
            this->stakenodePrivKey = stakenodePrivKey;
            this->hashContractTxId = hashContractTxId;
        }

        const std::string& getAlias() const {
            return alias;
        }

        void setAlias(const std::string& alias) {
            this->alias = alias;
        }

        const std::string& getStakenodePrivKey() const {
            return stakenodePrivKey;
        }

        const std::string& getContractTxID() const {
            return this->hashContractTxId;
        }

        void setStakenodePrivKey(const std::string& stakenodePrivKey) {
            this->stakenodePrivKey = stakenodePrivKey;
        }

        const std::string& getIp() const {
            return ip;
        }

        void setIp(const std::string& ip) {
            this->ip = ip;
        }
    };

    CStakenodeConfig() {
        entries = std::vector<CStakenodeEntry>();
    }

    void clear();
    bool read(std::string& strErr);
    void add(std::string alias, std::string ip, std::string stakenodePrivKey, std::string hashContractTxId);

    std::vector<CStakenodeEntry>& getEntries() {
        return entries;
    }

    int getCount() {
        return (int)entries.size();
    }

private:
    std::vector<CStakenodeEntry> entries;


};


#endif /* SRC_STAKENODECONFIG_H_ */
