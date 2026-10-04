#include "Server.hpp"
#include "RequestParser.hpp"
#include "log.hpp"
#include "resp/ResponseBuilder.hpp"
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/wait.h>
#include <utility>
#include <netdb.h>
#include <cstring>
#include <vector>
#include "CgiProcess.hpp"
#include "UniqueName.hpp"
#include "ServerManager.hpp"

// #define TEST_MOCK_WRITE  // 通常ビルドではコメントアウト

// #ifdef TEST_MOCK_WRITE
// static ssize_t writeForhandleCgiInput(int fd, const void* buf, size_t len) {
//     (void)fd; (void)buf; (void)len;
//     return -1; // モック動作: write が常に0を返す
// }
// #else
// static ssize_t writeForhandleCgiInput(int fd, const void* buf, size_t len) {
//     return write(fd, buf, len); // 本来の write
// }
// #endif

// #define TEST_MOCK_WRITE2  // 通常ビルドではコメントアウト

// #ifdef TEST_MOCK_WRITE2
// static ssize_t writeForHandleClientSend2(int fd, const void* buf, size_t len) {
//     (void)fd; (void)buf; (void)len;
//     return -1; // モック動作: write が常に0を返す
// }
// #else
// static ssize_t writeForHandleClientSend2(int fd, const void* buf, size_t len) {
//     return write(fd, buf, len); // 本来の write
// }

// #endif

// ----------------------------
// コンストラクタ・デストラクタ
// ----------------------------

// サーバー初期化（ポート指定）
Server::Server(const ServerConfig &c)
	: cfg(c),
	  serverFd(-1),
	  port(c.port),
	  host(c.host),
	  root(c.root),
	  errorPages(c.errorPages)
{
	// fds[], nfds は完全廃止なので何も必要ない
}

Server::~Server()
{
	// 接続中クライアントをすべて close
	for (std::map<int, ClientInfo>::iterator it = clients.begin();
		 it != clients.end(); ++it)
	{
		if (it->first >= 0)
			close(it->first);
	}
	clients.clear();

	if (serverFd >= 0)
		close(serverFd);
}

// ----------------------------
// 初期化系関数
// ----------------------------

// サーバー全体の初期化（ソケット作成＋バインド＋リッスン）
bool Server::init()
{
	if (!createSocket())
		return false;

	if (!bindAndListen())
	{
		// bind に失敗してもサーバをスキップする
		std::ostringstream oss;
		oss << port;
		logMessage(ERROR, "Server on " + host + ":" + oss.str() + " skipped");
		// serverFd は close しておく
		close(serverFd);
		serverFd = -1;
		return true; // プロセス自体は継続
	}

	fds[0].fd = serverFd;
	fds[0].events = POLLIN;

	std::cout << "Server listening on port " << port << std::endl;
	return true;
}

// ソケット作成とオプション設定
bool Server::createSocket()
{
	serverFd = socket(AF_INET, SOCK_STREAM, 0);
	if (serverFd < 0)
	{
		logMessage(ERROR, "socket() failed");
		return false;
	}

	int opt = 1;
	if (setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
	{
		logMessage(ERROR, "setsockopt() failed");
		return false;
	}
	if (!setNonBlocking(serverFd))
		return false;

	return true;
}

bool Server::setNonBlocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1)
	{
		logMessage(ERROR, "fcntl(F_GETFL) failed");
		return false;
	}
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		logMessage(ERROR, "fcntl(O_NONBLOCK) failed");
		return false;
	}
	return true;
}

// bind & listen 設定
bool Server::bindAndListen()
{
	// addrinfo のヒントを設定
	struct addrinfo hints;
	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;		 // IPv4
	hints.ai_socktype = SOCK_STREAM; // TCP
	hints.ai_flags = AI_PASSIVE;	 // hostが空なら INADDR_ANY 的に

	// ポート番号を文字列に変換
	std::ostringstream oss;
	oss << port;
	std::string portStr = oss.str();

	// host が空文字なら NULL を渡して 0.0.0.0 (ANY) 扱い
	const char *node = host.empty() ? NULL : host.c_str();

	struct addrinfo *res = NULL;
	int rc = getaddrinfo(node, portStr.c_str(), &hints, &res);
	if (rc != 0)
	{
		logMessage(ERROR,
				   std::string("getaddrinfo() failed: ") + gai_strerror(rc));
		return false;
	}

	bool bound = false;

	// 返ってきた候補を順に試す（通常1件だが念のため）
	for (struct addrinfo *ai = res; ai != NULL && !bound; ai = ai->ai_next)
	{
		if (bind(serverFd, ai->ai_addr, ai->ai_addrlen) == 0)
		{
			bound = true;
		}
		else
		{
			const int e = errno; // 直後に退避
			logMessage(ERROR,
					   "bind() failed on candidate: " + std::string(strerror(e)));
		}
	}

	freeaddrinfo(res);

	if (!bound)
	{
		logMessage(ERROR,
				   "bind() failed for " + host + ":" + portStr);
		return false;
	}

	if (listen(serverFd, SOMAXCONN) < 0)
	{
		const int e = errno;
		logMessage(ERROR,
				   "listen() failed: " + std::string(strerror(e)));
		return false;
	}

	return true;
}

// ----------------------------
// クライアント接続処理
// ----------------------------

// 新規接続ハンドラ
void Server::handleNewConnection()
{
	int clientFd = acceptClient();
	if (clientFd < 0)
		return;

	// MAX_CLIENTS チェックは clients.size() のみ
	if (clients.size() >= MAX_CLIENTS)
	{
		std::ostringstream oss;
		oss << "Max clients reached, rejecting fd=" << clientFd;
		logMessage(WARNING, oss.str());
		close(clientFd);
		return;
	}

	clients[clientFd] = ClientInfo();

	printf("New client connected: fd=%d\n", clientFd);
}

// accept + ノンブロッキング設定をまとめた関数
int Server::acceptClient()
{
	int clientFd = accept(serverFd, NULL, NULL);
	if (clientFd < 0)
	{
		logMessage(ERROR, "accept() failed");
		return -1;
	}

	if (!setNonBlocking(clientFd))
	{
		close(clientFd);
		return -1;
	}

	return clientFd;
}

// ----------------------------
// クライアント受信処理
// ----------------------------

bool Server::handleRedirect(int fd, const ServerConfig::Location *loc)
{
	if (!loc || loc->ret.empty())
		return false; // 続行してOK

	std::map<int, std::string>::const_iterator it = loc->ret.begin();
	int code = it->first;
	const std::string &target = it->second;

	std::ostringstream res;
	res << "HTTP/1.1 " << code << " Moved Permanently\r\n"
		<< "Location: " << target << "\r\n"
		<< "Content-Length: 0\r\n"
		<< "Connection: close\r\n\r\n";

	queueSend(fd, res.str()); // Server 内の関数を呼ぶ

	return true;
}

void Server::handleClient(int fd)
{
	char buffer[1024];
	int bytes = recv(fd, buffer, sizeof(buffer) - 1, 0);

	if (bytes <= 0)
	{
		handleDisconnect(fd, bytes);
		return;
	}
	else if (bytes > 0)
	{
		clients[fd].timeoutCounter = 0;
		buffer[bytes] = '\0';
		clients[fd].recvBuffer.append(buffer);

		// もしヘッダ解析済みなら max_body_size チェック
		Request &req = clients[fd].currentRequest;
		LocationMatch m = getLocationForUri(req.uri);
		const ServerConfig::Location *loc = m.loc;

		if (loc && clients[fd].receivedBodySize + bytes >
					   static_cast<size_t>(loc->max_body_size))
		{
			ResponseBuilder res_build;
			std::string res = res_build.buildErrorResponse(cfg, loc, 413, true);
			queueSend(fd, res);
			return;
		}

		// 累積ボディサイズを更新
		clients[fd].receivedBodySize += req.body.size();

		// 1リクエストずつ処理
		while (true)
		{
			std::string requestStr =
				extractNextRequest(fd, clients[fd].recvBuffer, clients[fd].currentRequest);
			if (requestStr.empty())
				break;

			Request &req = clients[fd].currentRequest;
			LocationMatch m = getLocationForUri(req.uri);
			const ServerConfig::Location *loc = m.loc;
			const std::string &locPath = m.path;

			// 1リクエスト分の body が max_body_size を超えていないかチェック
			if (!checkMaxBodySize(fd, req.body.size(), cfg, loc))
			{
				// handleDisconnect(fd, index, 0);
				break;
			}

			printf("Request complete from fd=%d\n", fd);

			// メソッド許可チェック
			if (!handleMethodCheck(fd, req, loc, requestStr.size()))
				continue;

			// CGI / POST / GET 処理
			// --- リダイレクト処理 ---
			if (handleRedirect(fd, loc))
			{
				// redirect を queueSend したらこのリクエスト処理は完了
				// ループを抜けて次の recv まで待つ
				break;
			}

			// CGI / POST / GET 処理
			processRequest(fd, req, loc, locPath, requestStr.size());
		}
	}
}

// Server.cpp に実装
bool Server::checkMaxBodySize(int fd, int bytes, const ServerConfig &cfg, const ServerConfig::Location *loc)
{
	if (!loc)
		return true;

	clients[fd].receivedBodySize += bytes;
	if ((static_cast<size_t>(loc->max_body_size) != 0) &&
		(clients[fd].receivedBodySize >
		 static_cast<size_t>(loc->max_body_size)))
	{
		ResponseBuilder res_build;
		std::string res = res_build.buildErrorResponse(cfg, loc, 413, true);
		queueSend(fd, res);
		clients[fd].recvBuffer.clear();
		return false; // 超過
	}
	return true;
}

bool Server::handleMethodCheck(int fd, Request &req,
							   const ServerConfig::Location *loc,
							   size_t reqSize)
{
	// 実装済みのMethodかチェック。PUTは未実装なので501で返す。
	if (req.method != "GET" && req.method != "POST" && req.method != "DELETE" && req.method != "HEAD")
	{
		ResponseBuilder res_build;
		std::string res = res_build.buildErrorResponse(cfg, loc, 501, true);
		queueSend(fd, res);
		clients[fd].recvBuffer.erase(0, reqSize);
		return false;
	}
	if (!isMethodAllowed(req.method, loc))
	{
		ResponseBuilder res_build;
		std::string res = res_build.buildErrorResponse(cfg, loc, 405, true);
		queueSend(fd, res);
		clients[fd].recvBuffer.erase(0, reqSize);
		return false;
	}
	return true;
}

void Server::processRequest(int fd, Request &req,
							const ServerConfig::Location *loc,
							const std::string &locPath, size_t reqSize)
{
	if (isCgiRequest(req))
	{
		startCgiProcess(fd, req, *loc);
	}
	else if (req.method == "POST")
	{
		handlePost(fd, req, loc);
	}
	else
	{
		ResponseBuilder rb;
		queueSend(fd, rb.generateResponse(req, cfg, loc, locPath));
	}
	clients[fd].recvBuffer.erase(0, reqSize);
}

std::string generateUniqueFilename()
{
	return makeUniqueName("file", "txt");
}

std::string buildHttpResponse(int statusCode, const std::string &body,
							  const std::string &contentType = "text/plain")
{
	std::stringstream ss;
	ss << "HTTP/1.1 " << statusCode << " "
	   << (statusCode == 201   ? "Created"
		   : statusCode == 403 ? "Forbidden"
		   : statusCode == 500 ? "Internal Server Error"
		   : statusCode == 415 ? "Unsupported Media Type"
							   : "")
	   << "\r\n";

	ss << "Content-Length: " << body.size() << "\r\n";
	ss << "Content-Type: " << contentType << "\r\n";
	ss << "Connection: close\r\n"; // ← 追加
	ss << "\r\n";				   // ヘッダーと本文の区切り
	ss << body;
	return ss.str();
}

void Server::handlePost(int fd, Request &req, const ServerConfig::Location *loc)
{
	std::string contentType;
	if (req.headers.find("content-type") != req.headers.end())
	{
		contentType = req.headers.at("content-type");
	}
	else
	{
		contentType = "";
	}

	bool isChunked = false;
	std::map<std::string, std::string>::iterator it = req.headers.find("transfer-encoding");
	if (it != req.headers.end() && it->second.find("chunked") != std::string::npos)
		isChunked = true;
	if (isChunked)
	{
		handleChunkedBody(fd, req, loc);
		return;
	}

	if (contentType.find("application/x-www-form-urlencoded") != std::string::npos)
	{
		handleUrlEncodedForm(fd, req, loc);
		return;
	}
	else if (contentType.find("multipart/form-data") != std::string::npos)
	{
		handleMultipartForm(fd, req, loc);
	}
	else
	{
		std::string body = "Unsupported Content-Type: " + contentType + "\n";
		queueSend(fd, buildHttpResponse(415, body));
	}
}

void saveBodyToFile(const std::string &body, const std::string &uploadDir)
{
	// 1) ユニークなファイル名を作成（PID・rand・counter 不使用）
	const std::string base = makeUniqueName("POST", "txt");

	// 2) ディレクトリと結合（末尾スラッシュ補正）
	std::ostringstream oss;
	oss << uploadDir;
	if (!uploadDir.empty() && uploadDir[uploadDir.size() - 1] != '/')
		oss << '/';
	oss << base;

	const std::string filename = oss.str();

	// 3) 書き込み
	std::ofstream ofs(filename.c_str(), std::ios::binary);
	if (!ofs.is_open())
	{
		std::cerr << "Failed to open file for writing: " << filename << std::endl;
		return;
	}

	ofs.write(body.c_str(), body.size());
	ofs.close();

	std::cout << "[INFO] Saved POST body to: " << filename << std::endl;
}

void Server::handleChunkedBody(int fd, Request &req, const ServerConfig::Location *loc)
{
	// すでに unchunk された req.body を使って処理
	// 例: ファイル保存や CGI に渡すなど
	if (loc->upload_path.empty())
	{
		queueSend(fd, buildHttpResponse(200, "Chunked data received\n"));
	}
	else
	{
		saveBodyToFile(req.body, loc->upload_path);
		queueSend(fd, buildHttpResponse(201, "File saved\n"));
	}
}

// URLデコード用
std::string urlDecode(const std::string &str)
{
	std::string ret;
	char hex[3] = {0};
	for (size_t i = 0; i < str.size(); ++i)
	{
		if (str[i] == '+')
		{
			ret += ' ';
		}
		else if (str[i] == '%' && i + 2 < str.size())
		{
			hex[0] = str[i + 1];
			hex[1] = str[i + 2];
			ret += static_cast<char>(strtol(hex, NULL, 16));
			i += 2;
		}
		else
		{
			ret += str[i];
		}
	}
	return ret;
}

// x-www-form-urlencoded を処理する関数
bool urlDecode(const std::string &src, std::string &out)
{
	out.clear();
	char hex[3] = {0};

	for (size_t i = 0; i < src.size(); ++i)
	{
		if (src[i] == '+')
		{
			out += ' ';
		}
		else if (src[i] == '%' && i + 2 < src.size())
		{
			hex[0] = src[i + 1];
			hex[1] = src[i + 2];

			// 16進数チェック
			if (!isxdigit(hex[0]) || !isxdigit(hex[1]))
				return false;

			out += static_cast<char>(strtol(hex, NULL, 16));
			i += 2;
		}
		else
		{
			out += src[i];
		}
	}
	return true;
}

void Server::handleUrlEncodedForm(int fd, Request &req,
								  const ServerConfig::Location *loc)
{
	if (!loc || loc->upload_path.empty())
	{
		queueSend(fd, buildHttpResponse(400, "No upload path configured\n"));
		return;
	}

	// ファイル名生成（getpid不要）
	std::string filename = loc->upload_path;
	if (filename[filename.size() - 1] != '/')
		filename += '/';
	filename += makeUniqueName("form", "txt");

	std::ofstream ofs(filename.c_str(), std::ios::out | std::ios::trunc);
	if (!ofs.is_open())
	{
		queueSend(fd, buildHttpResponse(500, "Internal Server Error\n"));
		return;
	}

	std::string &body = req.body;
	size_t pos = 0;
	while (pos < body.size())
	{
		size_t amp = body.find('&', pos);
		if (amp == std::string::npos)
			amp = body.size();

		size_t eq = body.find('=', pos);
		if (eq != std::string::npos && eq < amp)
		{
			std::string key, value;

			// URL デコード（失敗なら400）
			if (!urlDecode(body.substr(pos, eq - pos), key) ||
				!urlDecode(body.substr(eq + 1, amp - eq - 1), value))
			{
				ofs.close();
				std::remove(filename.c_str()); // 部分書き込みファイル削除
				queueSend(fd, buildHttpResponse(400, "Bad Request\n"));
				return;
			}

			ofs << key << "=" << value << "\n";
		}
		else
		{
			ofs << body.substr(pos, amp - pos) << "\n";
		}

		pos = amp + 1;
	}

	ofs.close();
	queueSend(fd, buildHttpResponse(201, "Form received successfully\n"));
}

std::string extractBoundary(const std::string &contentType)
{
	std::string key = "boundary=";
	size_t pos = contentType.find(key);
	if (pos == std::string::npos)
		return "";
	return "--" + contentType.substr(pos + key.size());
}

std::vector<std::string> splitParts(const std::string &body,
									const std::string &boundary)
{
	std::vector<std::string> parts;
	size_t start = 0, end;

	while ((end = body.find(boundary, start)) != std::string::npos)
	{
		std::string part = body.substr(start, end - start);

		// 末尾の余分な改行を削除
		if (part.size() >= 2 && part.substr(part.size() - 2) == "\r\n")
			part.erase(part.size() - 2);

		// 空文字や Content-Disposition を持たないパートは無視
		if (!part.empty() &&
			part.find("Content-Disposition") != std::string::npos)
		{
			parts.push_back(part);
		}

		start = end + boundary.size();
		if (body.substr(start, 2) == "--")
			break; // 終端ならループ終了
	}

	return parts;
}

void parsePart(const std::string &part, std::string &filename,
			   std::string &content)
{
	size_t headerEnd = part.find("\r\n\r\n");
	if (headerEnd == std::string::npos)
		return;

	std::string header = part.substr(0, headerEnd);
	content = part.substr(headerEnd + 4);

	// filename 抽出
	size_t pos = header.find("filename=\"");
	if (pos != std::string::npos)
	{
		pos += 10;
		size_t end = header.find("\"", pos);
		filename = header.substr(pos, end - pos);
	}
	else
	{
		filename = "upload.bin";
	}
}

void Server::handleMultipartForm(int fd, Request &req,
								 const ServerConfig::Location *loc)
{
	std::cerr << "=== Multipart Raw Body ===\n"
			  << req.body << "\n=========================\n";

	if (loc->upload_path.empty())
	{
		queueSend(fd, buildHttpResponse(403, "Upload path not configured.\n"));
		return;
	}

	std::string boundary = extractBoundary(req.headers["content-type"]);
	if (boundary.empty())
	{
		queueSend(fd,
				  buildHttpResponse(400, "Missing boundary in Content-Type.\n"));
		return;
	}

	std::vector<std::string> parts = splitParts(req.body, boundary);
	if (parts.empty())
	{
		queueSend(fd, buildHttpResponse(400, "No multipart data found.\n"));
		return;
	}

	for (size_t i = 0; i < parts.size(); ++i)
	{
		std::string filename, content;
		parsePart(parts[i], filename, content);
		std::string fullpath = loc->upload_path + "/" + filename;

		std::ofstream ofs(fullpath.c_str(), std::ios::binary);
		if (!ofs)
		{
			queueSend(fd, buildHttpResponse(500, "Failed to open file.\n"));
			return;
		}
		ofs.write(content.data(), content.size());
		ofs.close();
	}

	queueSend(fd, buildHttpResponse(201, "File uploaded successfully.\n"));
}

bool Server::isMethodAllowed(const std::string &method,
							 const ServerConfig::Location *loc)
{
	if (!loc)
		return false;
	for (size_t i = 0; i < loc->method.size(); i++)
	{
		if (loc->method[i] == method)
			return true;
	}
	return false;
}

std::string normalizePath(const std::string &path)
{
	if (path == "/")
		return "/"; // ルートはそのまま
	if (!path.empty() && path[path.size() - 1] == '/')
		return path.substr(0, path.size() - 1);
	return path;
}

Server::LocationMatch Server::getLocationForUri(const std::string &uri) const
{
	LocationMatch bestMatch;
	size_t bestLen = 0;

	std::string normUri = normalizePath(uri);

	for (std::map<std::string, ServerConfig::Location>::const_iterator it =
			 cfg.location.begin();
		 it != cfg.location.end(); ++it)
	{
		std::string normLoc = normalizePath(it->first);
		if (normLoc.empty())
			normLoc = "/";
		if (normUri.compare(0, normLoc.size(), normLoc) == 0)
		{
			if (normLoc.size() > bestLen)
			{
				bestLen = normLoc.size();
				bestMatch.loc = &it->second;
				bestMatch.path = it->first; // 元のパスはそのまま
			}
		}
	}
	return bestMatch;
}

bool Server::isCgiRequest(const Request &req)
{
	// C++98対応版: リスト初期化禁止なので手動で初期化
	static const char *exts[] = {".php", ".py"};
	static const size_t extCount = sizeof(exts) / sizeof(exts[0]);

	// クエリストリングを除去
	std::string uri = req.uri;
	size_t q = uri.find('?');
	if (q != std::string::npos)
		uri = uri.substr(0, q);

	// 拡張子取得
	size_t dot = uri.find_last_of('.');
	if (dot == std::string::npos)
		return false;

	std::string ext = uri.substr(dot);

	// 対応拡張子と比較
	for (size_t i = 0; i < extCount; ++i)
	{
		if (ext == exts[i])
			return true;
	}

	return false;
}

// ----------------------------
// CGI実行用関数
// ----------------------------

std::pair<std::string, std::string> splitUri(const std::string &uri)
{
	size_t pos = uri.find('?');
	if (pos == std::string::npos)
	{
		return std::make_pair(uri, "");
	}
	else
	{
		return std::make_pair(uri.substr(0, pos), uri.substr(pos + 1));
	}
}

// 外部関数（Serverクラス外でも良い）
std::pair<std::string, std::string> buildCgiScriptPath(
	const std::string &uri,
	const ServerConfig::Location &loc,
	const std::map<std::string, ServerConfig::Location> &locations)
{
	std::pair<std::string, std::string> parts = splitUri(uri);
	std::string path_only = parts.first;
	std::string query_str = parts.second;

	std::string scriptPath = loc.root;
	if (!scriptPath.empty() && scriptPath[scriptPath.size() - 1] == '/')
		scriptPath.erase(scriptPath.size() - 1);

	// location キーを探す
	std::string locKey;
	for (std::map<std::string, ServerConfig::Location>::const_iterator it = locations.begin();
		 it != locations.end(); ++it)
	{
		if (&it->second == &loc)
			locKey = it->first;
	}

	if (path_only.find(locKey) == 0)
	{
		std::string rest = path_only.substr(locKey.length());
		if (!rest.empty() && rest[0] != '/')
			scriptPath += '/';
		scriptPath += rest;
	}
	else
	{
		scriptPath += path_only;
	}

	return std::make_pair(scriptPath, query_str);
}

// env 設定を作る関数
std::map<std::string, std::string> buildCgiEnv(
	const Request &req,
	const ServerConfig::Location &loc,
	const std::map<std::string, ServerConfig::Location> &locations)
{
	std::map<std::string, std::string> env;

	env["REQUEST_METHOD"] = req.method;

	std::ostringstream len;
	len << req.body.size();
	env["CONTENT_LENGTH"] = len.str();

	std::map<std::string, std::string>::const_iterator it = req.headers.find("content-type");
	if (it != req.headers.end())
		env["CONTENT_TYPE"] = it->second;
	else
		env["CONTENT_TYPE"] = "";

	std::pair<std::string, std::string> envPaths =
		buildCgiScriptPath(req.uri, loc, locations);

	env["SCRIPT_FILENAME"] = envPaths.first;
	env["QUERY_STRING"] = envPaths.second;
	env["REDIRECT_STATUS"] = "200";

	return env;
}

// 子プロセス側の設定・exec
void executeCgiChild(int inFd, int outFd, const std::string &cgiPath,
					 const std::map<std::string, std::string> &env)
{
	// 標準入出力をパイプに差し替え
	dup2(inFd, STDIN_FILENO);
	dup2(outFd, STDOUT_FILENO);
	close(inFd);
	close(outFd);

	// 1) envマップから "KEY=VALUE" 形式の文字列を作る
	std::vector<std::string> envStorage;
	envStorage.reserve(env.size());

	for (std::map<std::string, std::string>::const_iterator it = env.begin();
		 it != env.end(); ++it)
	{
		envStorage.push_back(it->first + "=" + it->second);
	}

	// 2) execve に渡す char* 配列を構築
	std::vector<char *> envp;
	envp.reserve(envStorage.size() + 1);
	for (size_t i = 0; i < envStorage.size(); ++i)
	{
		envp.push_back(const_cast<char *>(envStorage[i].c_str()));
	}
	envp.push_back(NULL); // envp の終端

	// CGIスクリプトの実際のファイルパスを取得
	std::string scriptPath;
	std::map<std::string, std::string>::const_iterator it = env.find("SCRIPT_FILENAME");
	if (it != env.end())
		scriptPath = it->second;
	else
		scriptPath = "";

	// Pythonや他のインタプリタ系は scriptPath を argv[1] に渡す必要がある
	char *argv[3];
	argv[0] = const_cast<char *>(cgiPath.c_str());
	argv[1] = const_cast<char *>(scriptPath.c_str());
	argv[2] = NULL;

	// 3) envp を自前で渡して execve
	execve(argv[0], argv, &envp[0]);

	// ここに来るのは execve が失敗したときだけ
	// （親プロセスの状態に影響させないため、exit(1)でもOKだが課題的にはここは一応残す）
	exit(1);
}

// 親プロセス側でのパイプ送信
void Server::registerCgiProcess(int clientFd, pid_t pid,
								int inFd, int outFd, const std::string &body,
								std::map<int, CgiProcess> &cgiMap)
{
	// 1. 非ブロッキング設定
	fcntl(outFd, F_SETFL, O_NONBLOCK);
	fcntl(inFd, F_SETFL, O_NONBLOCK);

	// 2. CGIプロセス情報作成
	CgiProcess proc;
	proc.clientFd = clientFd;
	proc.pid = pid;
	proc.inFd = inFd;
	proc.outFd = outFd;
	proc.inputBuffer = body;
	proc.remainingMs = 50000; // タイムアウト

	// 3. 管理マップにはoutFdキーで保存
	cgiMap[outFd] = proc;
}

void Server::startCgiProcess(int clientFd, const Request &req, const ServerConfig::Location &loc)
{
	int inPipe[2], outPipe[2];
	if (pipe(inPipe) < 0 || pipe(outPipe) < 0)
		return;

	pid_t pid = fork();
	if (pid == 0)
	{
		// 子プロセス
		std::map<std::string, std::string> env = buildCgiEnv(req, loc, cfg.location);
		executeCgiChild(inPipe[0], outPipe[1], loc.cgi_path, env);
	}

	// 親プロセス
	close(inPipe[0]);
	close(outPipe[1]);
	std::cout << "[DEBUG] Passing to CGI, body size: " << req.body.size() << std::endl;

	registerCgiProcess(clientFd, pid, inPipe[1], outPipe[0], req.body, cgiMap);
}

void Server::handleCgiOutput(int fd)
{
	char buf[4096];
	ssize_t n = read(fd, buf, sizeof(buf));

	if (n > 0)
	{
		// バッファ上限チェック（例: 1MB）
		if (cgiMap[fd].buffer.size() + n > 1024 * 1024)
		{
			std::cerr << "CGI buffer overflow on fd=" << fd << std::endl;
			handleCgiError(fd);
			return;
		}
		cgiMap[fd].buffer.append(buf, n);
	}
	else if (n == 0)
	{
		// EOF → 正常終了
		handleCgiClose(fd);
	}
	else // n < 0
	{
		// 読み取りエラー
		handleCgiError(fd);
	}
}

void Server::handleCgiInput(int fd)
{
	// fd は CGI の inFd
	CgiProcess *proc = getCgiProcessByInFd(fd);
	if (!proc)
		return;

	if (proc->inputBuffer.empty())
	{
		// 書くものがない → POLLOUT解除 + inFdクローズ
		proc->events &= ~POLLOUT;
		if (proc->inFd > 0)
		{
			close(proc->inFd);
			proc->inFd = -1;
		}
		return;
	}

	const char *data = proc->inputBuffer.c_str();
	ssize_t len = proc->inputBuffer.size();
	ssize_t written = write(fd, data, len);

	if (written < 0)
	{
		// 致命的エラーとして終了
		perror("write to CGI stdin failed");
		proc->events &= ~POLLOUT;
		if (proc->inFd > 0)
		{
			close(proc->inFd);
			proc->inFd = -1;
		}
		proc->inputBuffer.clear(); // 念のためバッファクリア
		return;
	}

	if (written == 0)
	{
		// 一時的に書けなかった → 次の poll で再試行
		return;
	}

	// 書けた分を消去
	proc->inputBuffer.erase(0, written);

	if (proc->inputBuffer.empty())
	{
		proc->events &= ~POLLOUT;
		if (proc->inFd > 0)
		{
			close(proc->inFd);
			proc->inFd = -1;
		}
	}
}

CgiProcess *Server::getCgiProcessByInFd(int inFd)
{
	for (std::map<int, CgiProcess>::iterator it = cgiMap.begin();
		 it != cgiMap.end(); ++it)
	{
		if (it->second.inFd == inFd)
			return &it->second;
	}
	return NULL;
}

std::string Server::buildHttpErrorPage(int code, const std::string &message)
{
	std::ostringstream oss;
	oss << "<html><head><title>" << code << " Error</title></head><body>";
	oss << "<h1>" << code << " " << message << "</h1>";
	oss << "<hr><p>Webserv CGI Engine</p></body></html>";
	return oss.str();
}

void Server::handleCgiError(int fd)
{
	if (cgiMap.count(fd) == 0)
		return;

	int clientFd = cgiMap[fd].clientFd;
	std::cerr << "[ERROR] CGI read failed on fd=" << fd << std::endl;

	std::string body = buildHttpErrorPage(500, "Internal Server Error");
	std::ostringstream oss;
	oss << "HTTP/1.1 500 Internal Server Error\r\n";
	oss << "Content-Type: text/html\r\n";
	oss << "Content-Length: " << body.size() << "\r\n";
	oss << "Connection: close\r\n\r\n"; // ← 追加
	oss << body;

	queueSend(clientFd, oss.str());
	close(fd);
	waitpid(cgiMap[fd].pid, NULL, 0);
	cgiMap.erase(fd);
}

void Server::handleCgiClose(int fd)
{
	// --- 1️⃣ 登録確認 ---
	if (cgiMap.count(fd) == 0)
		return;

	CgiProcess &proc = cgiMap[fd];
	int clientFd = proc.clientFd;

	// --- 2️⃣ 子プロセス終了確認 (非ブロッキング) ---
	int status = 0;
	pid_t result = waitpid(proc.pid, &status, WNOHANG);
	if (result == 0)
	{
		// まだ終了していない（再びpollで呼ばれる）
		std::cout << "[DEBUG] CGI still running pid=" << proc.pid << std::endl;
		return;
	}
	else if (result < 0)
	{
		perror("waitpid");
	}

	// --- 子プロセス異常終了チェック ---
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		// 🚨 CGIが異常終了 → HTTP500を返す
		std::string body = buildHttpErrorPage(500, "Internal Server Error");
		std::ostringstream oss;
		oss << "HTTP/1.1 500 Internal Server Error\r\n";
		oss << "Content-Type: text/html\r\n";
		oss << "Content-Length: " << body.size() << "\r\n";
		oss << "Connection: close\r\n\r\n"; // ← 追加
		oss << body;
		queueSend(clientFd, oss.str());
	}
	else
	{
		// ✅ 正常終了 → 通常のレスポンス処理
		std::string response = buildHttpResponseFromCgi(proc.buffer);
		queueSend(clientFd, response);
	}

	int pid = proc.pid; // ← ここでコピーしておく

	// --- 4️⃣ パイプを確実に閉じる ---
	if (proc.inFd > 0)
	{
		close(proc.inFd);
		proc.inFd = -1;
	}
	if (proc.outFd > 0)
	{
		close(proc.outFd);
		proc.outFd = -1;
	}

	// --- 5️⃣ poll監視解除（次ループで再構築される） ---
	proc.events = 0;

	// --- 6️⃣ CGIプロセス削除 ---
	cgiMap.erase(fd);

	std::cout << "[CGI] process pid=" << pid << " cleaned up fd=" << fd << std::endl;
}

std::string Server::buildHttpResponseFromCgi(const std::string &cgiOutput)
{
	std::string headers;
	std::string content;
	std::string statusLine = "HTTP/1.1 200 OK"; // デフォルト

	// --- 1️⃣ ヘッダと本文を分離 ---
	size_t headerEnd = cgiOutput.find("\r\n\r\n");
	if (headerEnd == std::string::npos)
		headerEnd = cgiOutput.find("\n\n");
	if (headerEnd != std::string::npos)
	{
		headers = cgiOutput.substr(0, headerEnd);
		content = cgiOutput.substr(headerEnd + (cgiOutput[headerEnd] == '\r' ? 4 : 2));
	}
	else
	{
		// ヘッダがない → 全部本文として扱う
		content = cgiOutput;
	}

	// --- 2️⃣ ヘッダ行を個別に処理 ---
	std::istringstream headerStream(headers);
	std::string line;
	std::ostringstream filteredHeaders;

	bool hasContentType = false;

	while (std::getline(headerStream, line))
	{
		// 行末の \r を削除
		if (!line.empty() && line[line.size() - 1] == '\r')
			line.erase(line.size() - 1);

		// 空行スキップ
		if (line.empty())
			continue;

		// case-insensitive 検索のためにコピー
		std::string lower = line;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

		// --- Status ヘッダ ---
		if (lower.find("status:") == 0)
		{
			std::string statusValue = line.substr(7);
			size_t start = statusValue.find_first_not_of(" \t");
			size_t end = statusValue.find_last_not_of(" \t");
			if (start != std::string::npos && end != std::string::npos)
				statusValue = statusValue.substr(start, end - start + 1);
			statusLine = "HTTP/1.1 " + statusValue;
			continue; // StatusヘッダはHTTPヘッダには入れない
		}

		// --- Content-Type ヘッダ確認 ---
		if (lower.find("content-type:") == 0)
			hasContentType = true;

		// その他ヘッダはそのままコピー
		filteredHeaders << line << "\r\n";
	}

	// --- 3️⃣ Content-Type補完 ---
	if (!hasContentType)
		filteredHeaders << "Content-Type: text/html\r\n";

	// --- 4️⃣ HTTPレスポンス組み立て ---
	std::ostringstream oss;
	oss << statusLine << "\r\n";
	oss << "Content-Length: " << content.size() << "\r\n";
	oss << "Connection: close\r\n"; // ← ここで明示的に追加
	oss << filteredHeaders.str();
	oss << "\r\n"
		<< content;

	return oss.str();
}

// ----------------------------
// クライアント送信処理
// ----------------------------

// クライアント送信バッファのデータ送信
void Server::handleClientSend(int fd)
{
	if (!clients.count(fd))
		return;

	ClientInfo &client = clients[fd];

	if (client.sendBuffer.empty())
		return; // 送るデータがないなら何もしない

	size_t sendSize = std::min(client.sendBuffer.size(), static_cast<size_t>(4096));
	ssize_t n = write(fd, client.sendBuffer.data(), sendSize);

	if (n > 0)
	{
		client.sendBuffer.erase(0, n);
	}
	else if (n == 0)
	{
		// ソケットが閉じられた
		std::cerr << "[INFO] write() returned 0, closing fd=" << fd << std::endl;
		handleConnectionClose(fd);
		return;
	}
	else
	{
		// n < 0: エラー発生
		std::cerr << "[ERROR] write() failed, closing fd=" << fd << std::endl;
		handleConnectionClose(fd);
		return;
	}

	// 🔹バッファが空になったら、この時点で送信完了
	if (client.sendBuffer.empty())
		handleConnectionClose(fd);
}

// 送信キューにデータを追加する関数
void Server::queueSend(int fd, const std::string &data)
{
	std::map<int, ClientInfo>::iterator it = clients.find(fd);
	if (it != clients.end())
	{
		// 送信バッファにデータを追加
		it->second.sendBuffer += data;
	}
}

// ----------------------------
// クライアント接続終了処理
// ----------------------------

// クライアント接続クローズ処理
void Server::handleConnectionClose(int fd)
{
	std::cout << "[INFO] Closing connection fd=" << fd << std::endl;

	// 共通処理に任せる
	removeClient(fd);
	// pollfd 配列の更新は ServerManager が担当
}

// 接続切断処理（recv エラーや切断時の処理）
void Server::handleDisconnect(int fd, int bytes)
{
	if (bytes <= 0)
	{
		std::ostringstream oss;
		if (bytes == 0)
			oss << "Client disconnected: fd=" << fd;
		else
			oss << "Client read error or disconnected: fd=" << fd;

		logMessage(INFO, oss.str());

		// 共通処理に任せる
		removeClient(fd);
	}
}

// fd を閉じて clients から削除する共通処理
void Server::removeClient(int fd)
{
	if (fd >= 0)
		close(fd);

	std::map<int, ClientInfo>::iterator it = clients.find(fd);
	if (it != clients.end())
	{
		it->second.sendBuffer.clear();
		it->second.recvBuffer.clear();
		clients.erase(it);
	}
}

// ----------------------------
// ヘッダ解析・リクエスト処理
// ----------------------------

std::string Server::extractNextRequest(int clientFd, std::string &recvBuffer,
									   Request &currentRequest)
{
	RequestParser parser;
	if (!parser.isRequestComplete(recvBuffer))
		return "";

	currentRequest = parser.parse(recvBuffer);

	// --- Content-Length 超過チェック ---
	if (isContentLengthExceeded(currentRequest, recvBuffer))
	{
		sendHttpError(clientFd, 400, "Bad Request", parser.getParsedLength(), recvBuffer);
		return "";
	}

	// --- 不正リクエストかどうかをチェック ---
	if (currentRequest.method.empty())
	{
		sendHttpError(clientFd, 400, "Bad Request", parser.getParsedLength(), recvBuffer);
		return "";
	}

	// --- POST の長さチェック ---
	if (currentRequest.method == "POST" &&
		currentRequest.headers.find("content-length") == currentRequest.headers.end() &&
		currentRequest.headers.find("transfer-encoding") == currentRequest.headers.end())
	{
		sendHttpError(clientFd, 411, "Length Required", parser.getParsedLength(), recvBuffer);
		return "";
	}

	// --- 正常リクエスト ---
	std::string completeRequest = recvBuffer.substr(0, parser.getParsedLength());
	recvBuffer.erase(0, parser.getParsedLength());
	return completeRequest;
}

bool Server::isContentLengthExceeded(const Request &req,
									 const std::string &recvBuffer)
{
	std::map<std::string, std::string>::const_iterator it =
		req.headers.find("content-length");
	if (it == req.headers.end())
		return false; // Content-Lengthがない

	size_t declaredLength = std::strtoul(it->second.c_str(), NULL, 10);

	size_t headerEnd = recvBuffer.find("\r\n\r\n");
	if (headerEnd == std::string::npos)
		return false;

	size_t bodySize = recvBuffer.size() - (headerEnd + 4);
	return bodySize > declaredLength;
}

// ヘルパー関数: HTTPエラー送信 + バッファ調整
void Server::sendHttpError(int clientFd, int status, const std::string &msg,
						   size_t parsedLength, std::string &recvBuffer)
{
	std::ostringstream res;
	res << "HTTP/1.1 " << status << " " << msg << "\r\n"
		<< "Content-Length: " << msg.size() << "\r\n"
		<< "Content-Type: text/plain\r\n"
		<< "Connection: close\r\n\r\n"
		<< msg;
	queueSend(clientFd, res.str());
	recvBuffer.erase(0, parsedLength);
}

int Server::findFdByRecvBuffer(const std::string &buffer) const
{
	for (std::map<int, ClientInfo>::const_iterator it = clients.begin();
		 it != clients.end(); ++it)
	{
		if (&(it->second.recvBuffer) == &buffer)
		{
			return it->first; // fd を返す
		}
	}
	return -1; // 見つからなければ -1
}

int Server::getServerFd() const { return serverFd; }

std::vector<int> Server::getClientFds() const
{
	std::vector<int> fds;
	for (std::map<int, ClientInfo>::const_iterator it = clients.begin();
		 it != clients.end(); ++it)
	{
		fds.push_back(it->first);
	}
	return fds;
}

void Server::onPollEvent(int fd, short revents)
{
	// --------------------------
	// 1. サーバーFD（新しい接続受付）
	// --------------------------
	if (fd == serverFd)
	{
		if (revents & POLLIN)
			handleNewConnection(); // 新しい接続受け入れ
		if (revents & (POLLERR | POLLHUP))
			handleServerError(fd); // listen socketにエラー
		return;
	}

	// --------------------------
	// 2. CGI FD（出力 or 入力 監視）
	// --------------------------
	CgiProcess *proc = getCgiProcessByFd(fd); // inFd / outFd 両方に対応
	if (proc)
	{
		if (revents & POLLIN)
			handleCgiOutput(proc->outFd);
		if (revents & POLLOUT)
			handleCgiInput(proc->inFd);
		if (revents & (POLLHUP | POLLERR))
			handleCgiClose(proc->outFd);
		return;
	}

	// --------------------------
	// 3. 通常クライアントFD
	// --------------------------
	if (clients.count(fd))
	{
		// 🔹 通常クライアント
		if (revents & POLLIN)
			handleClient(fd); // クライアントからのリクエスト受信
		if (revents & POLLOUT)
			handleClientSend(fd); // クライアントへのレスポンス送信
		if (revents & (POLLERR | POLLHUP))
			handleConnectionClose(fd); // エラーや切断時の後処理
	}
}

CgiProcess *Server::getCgiProcessByFd(int fd)
{
	std::map<int, CgiProcess>::iterator it;

	// cgiMap は outFd をキーにしている前提
	for (it = cgiMap.begin(); it != cgiMap.end(); ++it)
	{
		if (it->second.outFd == fd || it->second.inFd == fd)
		{
			return &(it->second);
		}
	}

	return NULL; // 見つからなかった
}

// listenソケット（サーバーFD）でエラーが発生したときの処理
void Server::handleServerError(int fd)
{
	std::cerr << "[ERROR] Server socket error on fd " << fd << std::endl;

	// listenソケットは通常閉さない
	// 必要に応じてログ出力や管理者通知などをここで行う
	// 例: std::cerr << "Check network/bind settings\n";

	// サーバーを停止する場合はここでclose(fd)するが、
	// Webservでは通常そのまま運用
}

CgiProcess *Server::getCgiProcess(int fd)
{
	std::map<int, CgiProcess>::iterator it = cgiMap.find(fd);
	if (it == cgiMap.end())
	{
		throw std::runtime_error("getCgiProcess: fd not found in cgiMap");
	}
	return &(it->second); // ✅ オブジェクトのアドレスを返す
}

// Server.cpp
void Server::checkClientTimeouts(const int POLL_SLICE_MS, const int READ_TIMEOUT_MS)
{
	std::map<int, ClientInfo>::iterator it = clients.begin();
	while (it != clients.end())
	{
		ClientInfo &client = it->second;
		client.timeoutCounter += POLL_SLICE_MS; // pollスライス単位で加算

		if (client.timeoutCounter >= READ_TIMEOUT_MS)
		{
			int fd = it->first;
			std::cerr << "[TIMEOUT] Closing client fd=" << fd << std::endl;
			handleConnectionClose(fd); // private メソッドはクラス内ならOK
			it = clients.begin();	   // erase後はイテレータ再取得
		}
		else
		{
			++it;
		}
	}
}
