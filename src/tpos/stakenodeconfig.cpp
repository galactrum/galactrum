
#include <netbase.h>
#include <tpos/stakenodeconfig.h>
#include <util.h>
#include <chainparams.h>
#include <utilstrencodings.h>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

CStakenodeConfig stakenodeConfig;

void CStakenodeConfig::add(std::string alias, std::string ip, std::string stakenodePrivKey, std::string hashContractTxId) {
    CStakenodeEntry cme(alias, ip, stakenodePrivKey, hashContractTxId);
    entries.push_back(cme);
}

bool CStakenodeConfig::read(std::string& strErr) {
    int linenumber = 1;
    boost::filesystem::path pathStakenodeConfigFile = GetStakenodeConfigFile();
    boost::filesystem::ifstream streamConfig(pathStakenodeConfigFile);

    if (!streamConfig.good()) {
        FILE* configFile = fopen(pathStakenodeConfigFile.string().c_str(), "a");
        if (configFile != NULL) {
            std::string strHeader = "# Stakenode config file\n"
                          "# Format: alias IP:port stakenodePrivkey contractTxId\n"
                          "# Example: mn1 127.0.0.2:19999 \n";
            fwrite(strHeader.c_str(), std::strlen(strHeader.c_str()), 1, configFile);
            fclose(configFile);
        }
        return true; // Nothing to read, so just return
    }

    for(std::string line; std::getline(streamConfig, line); linenumber++)
    {
        if(line.empty()) continue;

        std::istringstream iss(line);
        std::string comment, alias, ip, stakenodePrivKey, hashContractTxId;

        if (iss >> comment) {
            if(comment.at(0) == '#') continue;
            iss.str(line);
            iss.clear();
        }

        if (!(iss >> alias >> ip >> stakenodePrivKey >> hashContractTxId)) {
            iss.str(line);
            iss.clear();
            if (!(iss >> alias >> ip >> stakenodePrivKey >> hashContractTxId)) {
                strErr = _("Could not parse stakenode.conf") + "\n" +
                        strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"";
                streamConfig.close();
                return false;
            }
        }

        int port = 0;
        std::string hostname = "";
        SplitHostPort(ip, port, hostname);
        if(port == 0 || hostname == "") {
            strErr = _("Failed to parse host:port string") + "\n"+
                    strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"";
            streamConfig.close();
            return false;
        }
        int mainnetDefaultPort = CreateChainParams(CBaseChainParams::MAIN)->GetDefaultPort();
        if(Params().NetworkIDString() == CBaseChainParams::MAIN) {
            if(port != mainnetDefaultPort) {
                strErr = _("Invalid port detected in stakenode.conf") + "\n" +
                        strprintf(_("Port: %d"), port) + "\n" +
                        strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"" + "\n" +
                        strprintf(_("(must be %d for mainnet)"), mainnetDefaultPort);
                streamConfig.close();
                return false;
            }
        } else if(port == mainnetDefaultPort) {
            strErr = _("Invalid port detected in stakenode.conf") + "\n" +
                    strprintf(_("Line: %d"), linenumber) + "\n\"" + line + "\"" + "\n" +
                    strprintf(_("(%d could be used only on mainnet)"), mainnetDefaultPort);
            streamConfig.close();
            return false;
        }


        add(alias, ip, stakenodePrivKey, hashContractTxId);
    }

    streamConfig.close();
    return true;
}
