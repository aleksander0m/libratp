
# libratp

libratp is a small **[C library](https://aleksander0m.github.io/libratp/)**
that allows creating, controlling and using data links using the **Reliable
Asynchronous Transfer Protocol (RATP)**, defined in
[RFC 916](https://tools.ietf.org/pdf/rfc916).

## Building

### options and dependencies

The basic dependencies to build the libratp project are **libevent 2** and
**gtk-doc** (only if building from a git checkout).

On a Debian based system, the dependencies may be installed as follows:
```
$ sudo apt-get install libevent-dev gtk-doc-tools
```

### configure, compile and install

```
$ NOCONFIGURE=1 ./autogen.sh     # only needed if building from git
$ ./configure --prefix=/usr
$ make
$ sudo make install
```

## License

This project is licensed under the LGPLv2.1+ license.

* Copyright © 2017 Zodiac Inflight Innovations
* Copyright © 2017 Aleksander Morgado <aleksander@aleksander.es>
