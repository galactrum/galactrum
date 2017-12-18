#/bin/bash

clear
cd ~

echo "Make sure you double check before hitting enter!"
echo "Ubuntu 16.04 is the recommended opearting system or this install."
sleep 3
echo "This script will install a firewall and configure your masternode."
sleep 3
echo ''

# Check that we have sudo powers
if "$(whereis sudo)" == *'/'* && "$(sudo -nv 2>&1)"; then 
    echo "Sudo powers are required for running this script!"
    exit;
fi

# Check for systemd
systemctl --version >/dev/null 2>&1 || { echo "systemd is required. Are you using Ubuntu 16.04?"  >&2; exit 1; }

# Gather input from user
read -e -p "Server IP Address : " ip
read -e -p "Masternode Private Key (e.g. 7edfjLCUzGczZi3JQw8GHp434R9kNY33eFyMGeKRymkB56G4324h # THE KEY YOU GENERATED EARLIER) : " key
read -e -p "Add swap space? (Recommended) [Y/n]"
read -e -p "Install Fail2ban? (Recommended) [Y/n] : " install_fail2ban
read -e -p "Install UFW and configure ports? (Recommended) [Y/n] : " UFW

# Add masternode group and user
groupadd masternode
useradd -m -d -g masternode masternode

# Update system 
echo && echo "Upgrading system..."
sleep 3
sudo apt-get -y update
sudo apt-get -y upgrade

# Add Berkely PPA
echo && echo "Installing bitcoin PPA..."
sleep 3
sudo apt-get -y install software-properties-common
sudo apt-add-repository -y ppa:bitcoin/bitcoin
sudo apt-get -y update

# Install required packages
echo && echo "Installing base packages..."
sleep 3
sudo apt-get -y install \
    wget \
    git \
    libevent-dev \
    libboost-dev \
    libboost-chrono-dev \
    libboost-program-options-dev
    libboost-system-dev \
    libboost-test-dev \
    libboost-thread-dev \
    libdb4.8-dev \
    libdb4.8++-dev \
    libminiupnpc-dev 

# Install fail2ban if needed
if [[ ("$install_fail2ban" == "y" || "$install_fail2ban" == "Y" || "$install_fail2ban" == "") ]]; then
    echo && echo "Installing fail2ban..."
    sleep 3
    sudo apt-get -y install fail2ban
    sudo service fail2ban restart 
fi

if [[ ("$UFW" == "y" || "$UFW" == "Y" || "$UFW" == "") ]]; then
    echo && echo "Installing UFW..."
    sleep 3
    sudo apt-get -y install ufw
    echo && echo "Configuring UFW..."
    sleep 3
    sudo ufw default deny incoming
    sudo ufw default allow outgoing
    sudo ufw allow ssh
    sudo ufw allow 6270/tcp
    sudo ufw enable -y
    echo && echo "Firewall installed and enabled!"
fi

# Download Galactrum
echo && echo "Downloading Galactrum v1.1.3..."
sleep 3
wget https://github.com/galactrum/galactrum/releases/download/v1.1.3/galactrum-1.1.3-ubuntu16.04-server.tar.gz
tar -xcf galactrum-1.1.3-ubuntu16.04-server.tar.gz
rm galactrum-1.1.3-ubuntu16.04-server.tar.gz

# Install Galactrum
echo && echo "Installing Galactrum v1.1.3..."
sleep 3
sudo mv galactrum{d,-cli} /usr/local/bin

# Create config for Galactrum
echo && echo "Configuring Galactrum v1.1.3..."
sleep 3
rpcuser=`cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 32 | head -n 1`
rpcpassword=`cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 32 | head -n 1`
sudo mkdir -p /home/masternode/.galactrum
echo '
rpcuser='$rpcuser'
rpcpassword='$rpcpassword'
rpcallowip=127.0.0.1
listen=1
server=1
daemon=0 # required for systemd
logtimestamps=1
maxconnections=256
externalip='$ip'
bind='$ip':6270
masternodeaddr='$ip'
masternodeprivkey='$key'
masternode=1
' | sudo -E tee /home/masterode/.galactrum/galactrum.conf
sudo chown -R masternode:masternode /home/masternode/.galactrum
sudo chmod 600 /home/masternode/.galactrum/galactrum.conf

# Setup systemd service
echo && echo "Starting Galactrum Daemon..."
echo '[Unit]
Description=galactrumd
After=network.target

[Service]
Type=simple
User=masternode
WorkingDirectory=/home/masternode
ExecStart=/usr/local/bin/galactrumd -conf=/home/masternode/.galactrum/galactrum.conf -datadir=/home/masternode/.galactrum
ExecStop=/usr/local/bin/galactrum-cli -conf=/home/masternode/.galactrum/galactrum.conf -datadir=/home/masternode/.galactrum stop
Restart=on-abort

[Install]
WantedBy=multi-user.target
' | sudo -E tee /etc/system/systemd/galactrumd.service
sudo systemctl enable galactrumd
sudo systemctl start galactrumd

# Download and install sentinel
echo && echo "Installing Sentinel..."
sleep 3
sudo apt-get -y install python-virtualenv python-pip
sudo git clone https://github.com/galactrum/sentinel /home/masternode/sentinel
cd /home/masternode/sentinel
virtualenv venv
source venv/bin/activate
pip install -r requirements.txt
sudo crontab -l -e -u masternode 2>/dev/null; echo '* * * * * cd /home/masternode/sentinel && ./venv/bin/python bin/sentinel.py >/dev/null 2>&1') | sudo crontab -e -u masternode

echo && echo "Masternode setup complete!"