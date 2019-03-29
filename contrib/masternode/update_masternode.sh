#!/bin/bash

VER=1.3.0

wget https://github.com/galactrum/galactrum/releases/download/v$VER/galactrum-$VER-linux64.tar.gz
tar -xvf galactrum-$VER-linux64.tar.gz
rm galactrum-$VER-linux64.tar.gz
sudo systemctl stop galactrumd
echo '[Unit]
Description=galactrumd
After=network.target

[Service]
Type=simple
User=masternode
WorkingDirectory=/home/masternode
ExecStart=/usr/local/bin/galactrumd -datadir=/home/masternode/.galactrum
ExecStop=/usr/local/bin/galactrum-cli -datadir=/home/masternode/.galactrum stop
Restart=on-abort

[Install]
WantedBy=multi-user.target
' | sudo -E tee /etc/systemd/system/galactrumd.service
sudo systemctl daemon-reload
sudo mv galactrum-$VER/bin/galactrum{d,-cli} /usr/local/bin
sudo sh -c "echo 'reindex=1' >> /home/masternode/.galactrum/galactrum.conf"
sudo systemctl start galactrumd
sudo rm -rf /home/masternode/sentinel
sleep 3
sudo sed -i '/reindex/d' /home/masternode/.galactrum/galactrum.conf