# FreeBSD Build Guide

Bitcoin Core is supported on all [supported FreeBSD releases](https://www.freebsd.org/security/#sup).

This guide describes how to build bitcoind and command-line utilities on the most recent production release.

## Preparation

### 1. Install Required Dependencies
Run the following as root to install the base dependencies for building.

```bash
pkg install boost-libs cmake git
```

SQLite is required for the wallet:

```bash
pkg install sqlite3
```

To build Bitcoin Core without the wallet, use `-DENABLE_WALLET=OFF`.

Cap'n Proto is needed for IPC functionality (see [multiprocess.md](multiprocess.md)):

```bash
pkg install capnproto
```

Compile with `-DENABLE_IPC=OFF` if you do not need IPC functionality.

See [dependencies.md](dependencies.md) for a complete overview.

### 2. Clone Bitcoin Repo
Now that `git` and all the required dependencies are installed, let's clone the Bitcoin Core repository to a directory. All build scripts and commands will run from this directory.
```bash
git clone https://github.com/bitcoin/bitcoin.git
```

### 3. Install Optional Dependencies

#### GUI Dependencies

The GUI of Chains, `chains-app` (an HTML interface in a native web view), is made for Linux and
Windows; this system is not supported yet. Configure with `-DBUILD_WEBUI=OFF`.

---

#### Notifications
###### ZeroMQ

Bitcoin Core can provide notifications via ZeroMQ. To compile ZMQ support, install the following dependency and pass `-DWITH_ZMQ=ON` when configuring.
```bash
pkg install libzmq4 pkgconf
```

#### Test Suite Dependencies
There is an included test suite that is useful for testing code changes when developing.
To run the test suite (recommended), you will need to have Python 3 installed:

```bash
pkg install python3 databases/py-sqlite3 net/py-pyzmq lsof
```
---

## Building Bitcoin Core

### 1. Configuration

There are many ways to configure Bitcoin Core, here are a few common examples:

##### Wallet:

```bash
cmake -B build -DBUILD_WEBUI=OFF
```

Run `cmake -B build -LH` to see the full list of available options.

##### No Wallet
```bash
cmake -B build -DENABLE_WALLET=OFF -DBUILD_WEBUI=OFF
```

### 2. Compile

```bash
cmake --build build     # Append "-j N" for N parallel jobs.
ctest --test-dir build  # Append "-j N" for N parallel tests.
```
