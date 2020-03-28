# ファイル同期サーバ・クライアント
サーバ側の指定したディレクトリ内のファイルをクライアント側で同期させるためのシンプルなシステム。

## ビルド

```shell
> mkdir build
> cd build
> cmake -G Ninja ..
> ninja
```

## サーバ
一度に1つのクライアントとしか接続できない。
ディレクトリも1つのみ。
ファイル更新検出時にコマンドを実行する機能はある。(setting.tomlに記述)

```shell
> ./build/syncserver -p /mnt/data
```

### settings.toml
ファイルが更新された場合に、指定パターンにマッチするファイルに対してコマンドを実行できる。
settings.tomlに正規表現パターンとコマンドを記述する。
```toml
[[update]]
pattern = ".elf$"
command = "strip -x $in"
```
commandで"$in"を書かれた部分は入力ファイルに置き換えられる。
ファイル名の変更には対応していない。(.gzなど付くと別ファイル扱い)

## クライアント

```shell
> ./build/syncclient servername -o data
```
