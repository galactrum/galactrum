#/bin/bash

clear
cd ~
echo "**********************************************************************"
echo "* Ubuntu 16.04 is the recommended opearting system for this install. *"
echo "*                                                                    *"
echo "* This script will install and configure your Galactrum masternode.  *"
echo "**********************************************************************"
echo && echo && echo
echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
echo "!                                                 !"
echo "! Make sure you double check before hitting enter !"
echo "!                                                 !"
echo "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
echo && echo && echo
sleep 3

# Check for systemd
systemctl --version >/dev/null 2>&1 || { echo "systemd is required. Are you using Ubuntu 16.04?"  >&2; exit 1; }

# Gather input from user
read -e -p "Masternode Private Key (e.g. 7edfjLCUzGczZi3JQw8GHp434R9kNY33eFyMGeKRymkB56G4324h) : " key
if [[ "$key" == "" ]]; then
    echo "WARNING: No private key entered, exiting!!!"
    echo && exit
fi
read -e -p "Server IP Address : " ip
echo && echo "Pressing ENTER will use the default value for the next prompts."
echo && sleep 3
read -e -p "Add swap space? (Recommended) [Y/n] : " add_swap
if [[ ("$add_swap" == "y" || "$add_swap" == "Y" || "$add_swap" == "") ]]; then
    read -e -p "Swap Size [2G] : " swap_size
    if [[ "$swap_size" == "" ]]; then
        swap_size="2G"
    fi
fi    
read -e -p "Install Fail2ban? (Recommended) [Y/n] : " install_fail2ban
read -e -p "Install UFW and configure ports? (Recommended) [Y/n] : " UFW

# Add swap if needed
if [[ ("$add_swap" == "y" || "$add_swap" == "Y" || "$add_swap" == "") ]]; then
    if [ ! -f /swapfile ]; then
        echo && echo "Adding swap space..."
        sleep 3
        sudo fallocate -l $swap_size /swapfile
        sudo chmod 600 /swapfile
        sudo mkswap /swapfile
        sudo swapon /swapfile
        echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab
        sudo sysctl vm.swappiness=10
        sudo sysctl vm.vfs_cache_pressure=50
        echo 'vm.swappiness=10' | sudo tee -a /etc/sysctl.conf
        echo 'vm.vfs_cache_pressure=50' | sudo tee -a /etc/sysctl.conf
    else
        echo && echo "WARNING: Swap file detected, skipping add swap!"
        sleep 3
    fi
fi


# Add masternode group and user
sudo groupadd masternode
sudo useradd -m -g masternode masternode

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
    libboost-filesystem-dev \
    libboost-program-options-dev \
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

# Install firewall if needed
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
    echo "y" | sudo ufw enable
    echo && echo "Firewall installed and enabled!"
fi

# Download Galactrum
echo && echo "Downloading Galactrum v1.1.6..."
sleep 3
wget https://github.com/galactrum/galactrum/releases/download/v1.1.6/galactrum-1.1.6-linux64.tar.gz
tar -xvf galactrum-1.1.6-linux64.tar.gz
rm galactrum-1.1.6-linux64.tar.gz

# Install Galactrum
echo && echo "Installing Galactrum v1.1.6..."
sleep 3
sudo mv galactrum-1.1.6/bin/galactrum{d,-cli} /usr/local/bin

# Create config for Galactrum
echo && echo "Configuring Galactrum v1.1.6..."
sleep 3
rpcuser=`cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 32 | head -n 1`
rpcpassword=`cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 32 | head -n 1`
sudo mkdir -p /home/masternode/.galactrum
sudo touch /home/masternode/.galactrum/galactrum.conf
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
masternodeprivkey='$key'
masternode=1
' | sudo -E tee /home/masternode/.galactrum/galactrum.conf
sudo chown -R masternode:masternode /home/masternode/.galactrum

# Setup systemd service
echo && echo "Starting Galactrum Daemon..."
sleep 3
sudo touch /etc/systemd/system/galactrumd.service
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
' | sudo -E tee /etc/systemd/system/galactrumd.service
sudo systemctl enable galactrumd
sudo systemctl start galactrumd

# Download and install sentinel
echo && echo "Installing Sentinel..."
sleep 3
sudo apt-get -y install virtualenv python-pip
sudo git clone https://github.com/galactrum/sentinel /home/masternode/sentinel
cd /home/masternode/sentinel
virtualenv venv
. venv/bin/activate
pip install -r requirements.txt
export EDITOR=nano
(crontab -l -u masternode 2>/dev/null; echo '* * * * * cd /home/masternode/sentinel && ./venv/bin/python bin/sentinel.py >/dev/null 2>&1') | sudo crontab -u masternode -
sudo chown -R masternode:masternode /home/masternode/sentinel
echo "galactrum_conf=/home/masternode/.galactrum/galactrum.conf" | tee -a /home/masternode/sentinel/test/test_sentinel.conf
cd ~

# Add alias to run galactrum-cli
echo && echo "Masternode setup complete!"
touch ~/.bash_aliases
echo "alias galactrum-cli='galactrum-cli -conf=/home/masternode/.galactrum/galactrum.conf -datadir=/home/masternode/.galactrum'" | tee -a ~/.bash_aliases

echo && echo "Now run 'source ~/.bash_aliases' (without quotes) to use galactrum-cli"
