# msquic-asio
![ci](https://github.com/youyuanwu/msquic-asio/actions/workflows/build.yaml/badge.svg)
[![codecov](https://codecov.io/gh/youyuanwu/msquic-asio/branch/main/graph/badge.svg?token=2LTDXV5K35)](https://codecov.io/gh/youyuanwu/msquic-asio)

An experiment to intergrate [MsQuic](https://github.com/microsoft/msquic) with boost asio.

MsQuic event model is not driven by the user app but by its internal threads/worker pools. Asio event action like connection acception or stream read write are initiated by user app. In this lib such gap is filled by using windows event and c++20 semaphore, so how well this overhead plays out remains to be seen.

## Build
Currently only Windows is supported. Linux support can be added in future.
```ps1
cmake . -B build
cmake --build build
```

## MISC
For more mature networking streaming libs on windows, please use winhttp and http.sys.
Checkout [winasio](https://github.com/youyuanwu/winasio) for using winhttp and http.sys with boost asio.

## License
msquic-asio has MIT license.