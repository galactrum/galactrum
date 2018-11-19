
Debian
====================
This directory contains files used to package galactrumd/galactrum-qt
for Debian-based Linux systems. If you compile galactrumd/galactrum-qt yourself, there are some useful files here.

## galactrum: URI support ##


galactrum-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install galactrum-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your galactrum-qt binary to `/usr/bin`
and the `../../share/pixmaps/galactrum128.png` to `/usr/share/pixmaps`

galactrum-qt.protocol (KDE)

