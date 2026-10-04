#!/bin/bash
# ============================
# Run CGI Response Test
# ============================

SERVER_BIN=./webserv
CONF=conf/test.conf
PORT=8080

echo "🚀 Starting webserv..."
$SERVER_BIN $CONF >/dev/null 2>&1 &
SERVER_PID=$!
sleep 1

# ---- テスト関数 ----
test_case () {
    local name=$1
    local url=$2
    echo -e "\n===== $name ====="
    curl -s -i "http://localhost:$PORT/cgi-bin/$url"
    echo -e "\n--------------------------"
}

# ---- テスト実行 ----
test_case "✅ CGI with Status header" "ok.py"
test_case "✅ CGI without Status header" "no_status.py"
test_case "✅ CGI error (expect 500)" "error.py"

# ---- 終了処理 ----
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
echo -e "\n🧩 Test finished."
