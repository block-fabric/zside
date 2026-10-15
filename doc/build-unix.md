# UNIX BUILD NOTES

Some notes on how to build Bitcoin Core in Unix.

(For BSD specific instructions, see `build-*bsd.md` in this directory.)

## To Build

```bash
cmake -B build
```
Run `cmake -B build -LH` to see the full list of available options.

```bash
cmake --build build    # Append "-j N" for N parallel jobs
cmake --install build  # Optional
```

See below for instructions on how to [install the dependencies on popular Linux
distributions](#dependencies).

## Memory Requirements

C++ compilers are memory-hungry. It is recommended to have at least 1.5 GB of
memory available when compiling Bitcoin Core. On systems with less, gcc can be
tuned to conserve memory with additional `CMAKE_CXX_FLAGS`:


    cmake -B build -DCMAKE_CXX_FLAGS="--param ggc-min-expand=1 --param ggc-min-heapsize=32768"

Alternatively, or in addition, debugging information can be skipped for compilation.
For the default build type `RelWithDebInfo`, the default compile flags are
`-O2 -g`, and can be changed with:

    cmake -B build -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g0"

Finally, clang (often less resource hungry) can be used instead of gcc, which is used by default:

    cmake -B build -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang

## Dependencies

You can either build from self-compiled [depends](/depends/README.md) or
install the dependencies from your distribution package manager. Dependencies
for additional features in later columns are optional.

| Package manager         | Required build dependencies | SQLite (wallet) | Cap'n Proto (IPC) | ZMQ <sup><a href="#note1">[1]</a></sup> | USDT | GTK, WebKitGTK and npm (GUI, chains-app) |
| ----------------------- | --------------------------- | --------------- | ----------------- | --- | ---- | ------------------------ |
| Debian / Ubuntu (`apt`) | `build-essential cmake python3 libboost-dev` | `libsqlite3-dev` | `libcapnp-dev capnproto` | `libzmq3-dev pkgconf` | `systemtap-sdt-dev` | `libgtk-3-dev libwebkit2gtk-4.1-dev pkgconf nodejs npm` |
| Fedora (`dnf`)          | `gcc-c++ cmake make python3 boost-devel` | `sqlite-devel` | `capnproto capnproto-devel` | `zeromq-devel pkgconf` | `systemtap-sdt-devel` | `gtk3-devel webkit2gtk4.1-devel pkgconf nodejs npm` |
| Alpine (`apk`)          | `build-base cmake linux-headers python3 boost-dev` | `sqlite-dev` | `capnproto capnproto-dev` | `zeromq-dev` | Not supported | `gtk+3.0-dev webkit2gtk-4.1-dev pkgconf nodejs npm` |
| Arch (`pacman`)         | `gcc make cmake python boost` | `sqlite` | `capnproto` | `zeromq` | `systemtap` | `gtk3 webkit2gtk-4.1 pkgconf nodejs npm` |

<a id="note1"></a>1. Some ZMQ packages do not vendor the CMake config files, which requires `pkgconf` or `pkg-config` to be installed.

For Debian "oldstable", or earlier Ubuntu LTS releases, you may need to pick a
later compiler version, according to the [dependencies](/doc/dependencies.md)
documentation.

To build Bitcoin Core without the wallet, see [*Disable-wallet mode*](#disable-wallet-mode)
and use `-DENABLE_WALLET=OFF` to build without the wallet and skip the SQLite dependency.

Cap'n Proto is needed for IPC functionality (see [multiprocess.md](multiprocess.md)).
Compile with `-DENABLE_IPC=OFF` if you do not need IPC functionality.

ZMQ-enabled binaries are compiled with `-DWITH_ZMQ=ON` and require libzmq.

User-Space, Statically Defined Tracing (USDT) requires the systemtap-sdt
development package and must be enabled via `-DWITH_USDT=ON`.

GUI dependencies:

The GUI of Chains is `chains-app`: the node with its wallet and an HTML interface in a native web
view (WebKitGTK on Linux), built by default (`-DBUILD_WEBUI=ON`). It needs GTK 3, WebKitGTK 4.1
and, to build its interface, Node.js with npm: the first build runs `npm ci` in `src/webui/app`
(the exact versions of its `package-lock.json`), later builds need no network. With
`-DWEBUI_PREBUILT_DIST=<dir>` the interface built elsewhere is used, and no npm is needed. Pass
`-DBUILD_WEBUI=OFF` if you don't intend to use the GUI. See [webui.md](webui.md).

### Disable-wallet mode

When the intention is to only run a P2P node, without a wallet, Bitcoin Core can
be compiled in disable-wallet mode with:

    cmake -B build -DENABLE_WALLET=OFF

In this case there is no dependency on SQLite.

Mining is also possible in disable-wallet mode using the `getblocktemplate` RPC call.
