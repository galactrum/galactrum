#!/bin/bash

wget https://github.com/galactrum/galactrum/releases/download/v1.1.5/galactrum-1.1.5-linux64.tar.gz
sudo systemctl stop galactrumd
tar -xvf galactrum-1.1.5-linux64.tar.gz
sudo mv galactrum-1.1.5/bin/galactrum{d,-cli} /usr/local/bin
sudo mv /home/masternode/.galactrum /home/masternode/.galactrum.bak
mkdir /home/masternode/.galactrum
sudo cp /home/masternode/.galactrum.bak/wallet.dat /home/masternode/.galactrum
sudo cp /home/masternode/.galactrum.bak/galactrum.conf /home/masternode/.galactrum
sudo chown -R masternode:masternode /home/masternode/.galactrum
sudo systemctl start galactrumd