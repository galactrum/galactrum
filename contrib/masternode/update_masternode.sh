#!/bin/bash

wget https://github.com/galactrum/galactrum/releases/download/v1.1.6/galactrum-1.1.6-linux64.tar.gz
sudo systemctl stop galactrumd
tar -xvf galactrum-1.1.6-linux64.tar.gz
sudo mv galactrum-1.1.6/bin/galactrum{d,-cli} /usr/local/bin
sudo systemctl start galactrumd