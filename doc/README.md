Galactrum
=============

Setup
---------------------
Galactrum is the original Galactrum client and it builds the backbone of the network. It downloads and, by default, stores the entire history of Galactrum transactions (which is currently more than 100 GBs); depending on the speed of your computer and network connection, the synchronization process can take anywhere from a few hours to a day or more.

To download Galactrum, visit [galactrum.org](https://galactrum.org/#download).

Running
---------------------
The following are some helpful notes on how to run Galactrum on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/galactrum-qt` (GUI) or
- `bin/galactrumd` (headless)

### Windows

Unpack the files into a directory, and then run galactrum-qt.exe.

### OS X

Drag Galactrum to your applications folder, and then run Galactrum.

### Need Help?

* See the documentation at the [Galactrum Wiki](http://wiki.galactrum.org)
for help and more information.
* Ask for help on [#galactrum](http://webchat.freenode.net?channels=galactrum) on Freenode. If you don't have an IRC client use [webchat here](http://webchat.freenode.net?channels=galactrum).
* Ask for help on the [GalactrumTalk](https://galactrumtalk.org/) forums, in the [Technical Support board](https://galactrumtalk.org/index.php?board=4.0).

Building
---------------------
The following are developer notes on how to build Galactrum on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [OS X Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [Windows Build Notes](build-windows.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)
- [Gitian Building Guide](gitian-building.md)

Development
---------------------
The Galactrum repo's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Release Notes](release-notes.md)
- [Release Process](release-process.md)
- [Source Code Documentation (External Link)](https://dev.visucore.com/galactrum/doxygen/)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [Travis CI](travis-ci.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Shared Libraries](shared-libraries.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)

### Resources
* Discuss on the [GalactrumTalk](https://galactrumtalk.org/) forums, in the [Development & Technical Discussion board](https://galactrumtalk.org/index.php?board=6.0).
* Discuss project-specific development on #galactrum-dev on Freenode. If you don't have an IRC client use [webchat here](http://webchat.freenode.net/?channels=galactrum-dev).
* Discuss general Galactrum development on #galactrum-dev on Freenode. If you don't have an IRC client use [webchat here](http://webchat.freenode.net/?channels=galactrum-dev).

### Miscellaneous
- [Assets Attribution](assets-attribution.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [ZMQ](zmq.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
This product includes software developed by the OpenSSL Project for use in the [OpenSSL Toolkit](https://www.openssl.org/). This product includes
cryptographic software written by Eric Young ([eay@cryptsoft.com](mailto:eay@cryptsoft.com)), and UPnP software written by Thomas Bernard.
