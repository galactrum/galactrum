#!/bin/bash

sudo systemctl stop galactrumd
sudo sh -c "echo 'reindex=1' >> /home/masternode/.galactrum/galactrum.conf"
sudo systemctl start galactrumd
sleep 15
sudo sed -i '/reindex/d' /home/masternode/.galactrum/galactrum.conf