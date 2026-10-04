# webserv

C++98 で書いた、ノンブロッキング I/O の HTTP/1.1 サーバーです。
nginx 風の設定ファイルを読み込み、静的ファイル配信、ファイルのアップロードと削除、CGI（Python / PHP）の実行に対応しています。

## 特徴

- **単一の `poll()` ループ**：すべての listen ソケット、クライアントソケット、CGI パイプを 1 つの `poll()` でまとめて扱い、読み書きはすべてノンブロッキングで行います
- **複数サーバー / 複数ポート**：設定ファイルに `server` ブロックを複数書くと、それぞれ別の `host:port` で待ち受けます
- **HTTP メソッド**：`GET` / `HEAD` / `POST` / `DELETE`。これ以外のメソッドには `501` を返します
- **location 単位の設定**：許可するメソッド、ドキュメントルート、index ファイル、autoindex、アップロード先、ボディサイズの上限、リダイレクト、CGI を location ごとに指定できます
- **POST**：`multipart/form-data`、`application/x-www-form-urlencoded`、`Transfer-Encoding: chunked` を受け付けます
- **CGI**：`.py` と `.php` へのリクエストを、`cgi_path` で指定したインタプリタに渡します。`REQUEST_METHOD`、`QUERY_STRING`、`CONTENT_LENGTH`、`CONTENT_TYPE`、`SCRIPT_FILENAME` などの環境変数を設定します
- **カスタムエラーページ**：ステータスコードごとに HTML を差し替えられます（`assets/errors/` にサンプルがあります）
- **タイムアウト**：クライアントからの受信は 5 秒、CGI の実行は 50 秒でタイムアウトします

## 動作環境

- Linux または macOS
- C++98 に対応した C++ コンパイラ（`c++`）
- `make`
- CGI を使う場合は `python3` と `php-cgi`

## ビルド

```sh
make        # ./webserv を生成
make clean  # オブジェクトファイルを削除
make fclean # オブジェクトファイルとバイナリを削除
make re     # fclean してからビルドし直す
```

コンパイルオプションは `-Wall -Wextra -Werror -std=c++98` です。

## 使い方

```sh
./webserv [設定ファイル]
```

設定ファイルを省略すると `./conf/config2.conf` を読み込みます。

```sh
./webserv conf/config2_for_reviews.conf
```

起動したら、別のターミナルから動作を確認できます。

```sh
curl -i http://127.0.0.1:8080/
curl -i -X POST -F "file=@www/test_upload.txt" http://127.0.0.1:8080/
curl -i -X DELETE http://127.0.0.1:8080/delete/target.txt
curl -i http://127.0.0.1:8080/cgi/hello.py
curl -i http://127.0.0.1:8080/redirect_old/
```

## 設定ファイル

書式は nginx に近い形です。各行の末尾に `;` を付けます。`#` で始まる行はコメントです（行の途中に書いた `#` はコメントになりません）。

```nginx
server {
    listen 8080;
    host 127.0.0.1;
    root ./docs/;

    error_page 404 ./assets/errors/404.html;

    location / {
        method GET POST;
        root ./docs/;
        index index.html;
        upload_path ./www/upload/;
        max_body_size 1024;
    }

    location /list/ {
        method GET;
        root ./docs/list/;
        autoindex on;
    }

    location /old/ {
        method GET;
        return 302 /new/;
    }

    location /cgi/ {
        method GET POST;
        root ./www/cgi-bin/;
        cgi_path /usr/bin/python3;
    }
}
```

### server ブロック

| ディレクティブ | 説明 |
| --- | --- |
| `listen <port>` | 待ち受けるポート番号 |
| `host <address>` | 待ち受けるアドレス（例：`127.0.0.1`、`0.0.0.0`） |
| `root <path>` | ドキュメントルート（必須） |
| `error_page <code> <path>` | 指定したステータスコードのときに返す HTML |
| `location <path> { ... }` | パスごとの設定 |

### location ブロック

| ディレクティブ | 説明 |
| --- | --- |
| `method <METHOD> ...` | 許可するメソッド。許可されていないメソッドには `405` を返します |
| `root <path>` | この location のドキュメントルート |
| `index <file>` | ディレクトリにアクセスされたときに返すファイル |
| `autoindex on\|off` | index ファイルがないときにディレクトリの一覧を表示するかどうか |
| `upload_path <path>` | POST でアップロードされたファイルの保存先 |
| `max_body_size <bytes>` | リクエストボディの上限。超えたら `413` を返します |
| `return <code> <url>` | 指定した URL へリダイレクトします（例：`return 301 /new/;`） |
| `cgi_path <path>` | CGI を実行するインタプリタ（例：`/usr/bin/python3`、`/usr/bin/php-cgi`） |

同じ `host:port` を指定した `server` ブロックが複数ある場合は、最初に bind できたものだけが有効になり、それ以降はスキップされます。

### 同梱の設定ファイル

| ファイル | 内容 |
| --- | --- |
| `conf/config2.conf` | デフォルトの設定 |
| `conf/config2_for_reviews.conf` | 評価（レビュー）用。GET / POST / DELETE / autoindex / リダイレクト / CGI / エラーページをひととおり確認できます |
| `conf/01_ports.conf` 〜 `conf/10_multiple_servers.conf` | 機能ごとのテスト用（ポート、ホスト名、エラーページ、ボディサイズ上限、ルーティング、index、メソッド制限、複数ポート、同一ポート、複数サーバー） |

## ディレクトリ構成

```
.
├── Makefile
├── include/            ヘッダファイル
│   └── resp/           レスポンス生成と MIME タイプ
├── src/
│   ├── main.cpp            エントリポイント
│   ├── ConfigParser.cpp    設定ファイルの解析
│   ├── ServerManager.cpp   全サーバーをまとめた poll ループ
│   ├── Server.cpp          接続、送受信、POST、CGI、タイムアウトの処理
│   ├── RequestParser.cpp   HTTP リクエストの解析
│   ├── log.cpp             ログ出力
│   └── resp/               静的ファイル配信、autoindex、DELETE、エラーレスポンス
├── conf/               設定ファイルのサンプル
├── assets/errors/      エラーページのサンプル
├── docs/               評価用の静的コンテンツ（index、list、delete、redirect）
├── www/                静的コンテンツと CGI スクリプト（www/cgi-bin/）
├── test_scripts(ishi)/     curl を使った E2E テスト
└── testenv_scripts(ishi)/  テスト環境とテスト用設定ファイルを生成するスクリプト
```

## 処理の流れ

```
main
 └─ ServerManager::loadConfig      設定ファイルを読み、server ブロックごとに ServerConfig を作る
 └─ ServerManager::initAllServers  Server ごとに socket / bind / listen
 └─ ServerManager::runAllServers   poll() ループ（100ms 刻み）
      ├─ 新しい接続     → accept してノンブロッキングに設定
      ├─ 受信           → RequestParser で解析し、location を決めてメソッドとボディサイズを確認
      │                    ├─ return が設定されている → リダイレクト
      │                    ├─ .py / .php              → CGI を fork / execve し、パイプを poll に登録
      │                    ├─ POST                    → upload_path に保存
      │                    └─ GET / HEAD / DELETE     → ResponseBuilder
      ├─ 送信           → 送信バッファから少しずつ書き出す
      └─ タイムアウト確認 → クライアントと CGI プロセス
```

## テスト

サーバーを起動したうえで、`test_scripts(ishi)/` のスクリプトを実行すると curl でリクエストを送って結果を確認できます。

```sh
bash "test_scripts(ishi)/1.test_all.sh"
```

`testenv_scripts(ishi)/` には、テスト用のディレクトリ、ファイル、設定ファイルを生成するスクリプトが入っています。
