// version 1.5.5
#define _DISABLE_RECV_LIMIT

#ifndef _WIN32
#include "multiprocessing.hpp"
#else
#define ENABLE_U8STRING
#define popen(a, b) _popen(a, b)
#define pclose(a) _pclose(a)
#endif

#include <thread>
#include <vector>
#include <map>
#include <fstream>
#include <filesystem>
#include <set>
#include <csignal>
#include <cstdio>
#include "ssocket.hpp"
#include "strlib.hpp"
#include "argparse.hpp"
#include "libjson.hpp"

using namespace std;
namespace fs = std::filesystem;
string lws_version = "1.5.5";

map<string, string> contenttype = { {"html", "text/html"}, {"htm", "text/html"}, {"txt", "text/plain"}, {"py", "text/x-python"}, {"ico", "image/x-icon"}, {"css", "text/css"}, {"js", "application/javascript"}, {"jpg", "image/jpeg"},{"png", "image/png"}, {"gif", "image/gif"}, {"mp3", "audio/mp3"}, {"ogg", "audio/ogg"}, {"wav", "audio/wav"}, {"opus", "audio/opus"}, {"m4a", "audio/mp4"}, {"mp4", "video/mp4"}, {"webm", "video/webm"}, {"pdf", "application/pdf"}, {"json", "text/json"}, {"xml", "text/xml"}, {"image", "svg+xml"}, {"other", "application/octet-stream"} };
map<int, string> strcode = { {200, "OK"}, {206, "Partial Content"}, {400, "Bad Request"}, {403, "Forbidden"}, {404, "Not Found"}, {500, "Internal Server Error"}, {501, "Not Implemented"} };
vector<string> methods = { "GET", "HEAD", "POST" };
vector<string> cgidirs = { "cgi-bin", "htbin" };
int recvtimeout;

struct srvresp {
	int code;
	string textdata;
	string ext = "html";
	string method = "GET";
	string connection = "close";
	bool filestream = false;
	fs::path filepath;
	size_t filelength;
	bool AcceptRanges = false;
	bool ContentRange = false;
	size_t ContentRangeData;
};

bool check_cgi(fs::path path) {
	string str_path = u8tostr(path.u8string());
	vector<string> path_split = split(str_path, '/');

	for (string i : cgidirs) if (find(path_split.begin(), path_split.end(), i) != path_split.end()) return true;
	return false;
}

void socksend(SSocket sock, srvresp data) {
	string headers = strformat("HTTP/1.1 %d %s\r\nContent-Type: %s; charset=UTF-8\r\n", data.code, strcode[data.code], contenttype[data.ext].c_str());

	if (data.AcceptRanges) {
		headers += "Accept-Ranges: bytes\r\n";
	}

	headers += strformat("Connection: %s\r\n", data.connection.c_str());
	if (toLower(data.connection) != "close") headers += strformat("Keep-Alive: timeout=%d, max=100\r\n", recvtimeout);

	headers += strformat("Server: LWS/%s\r\n", lws_version.c_str());

	if (data.filestream) {
		ifstream file(data.filepath, ios::binary);

		if (data.ContentRange) {
			headers += strformat("Content-Range: bytes %zu-%zu/%zu\r\n", data.ContentRangeData, data.filelength - 1, data.filelength);
			headers += strformat("Content-Length: %zu\r\n\r\n", data.filelength - data.ContentRangeData);
			file.seekg(data.ContentRangeData);
		}
		else headers += strformat("Content-Length: %zu\r\n\r\n", data.filelength);

		sock.ssendall(headers);

		if (data.method == "GET" || data.method == "POST") sock.ssend_file(file);

		file.close();

	}
	else {
		if (data.ContentRange) {
			headers += strformat("Content-Range: bytes %zu-%zu/%zu\r\n", data.ContentRangeData, data.textdata.size() - 1, data.textdata.size());
			headers += strformat("Content-Length: %zu\r\n\r\n", data.textdata.size() - data.ContentRangeData);
			data.textdata = data.textdata.substr(data.ContentRangeData);
		}
		else headers += strformat("Content-Length: %zu\r\n\r\n", data.textdata.size());

		sock.ssendall(headers);

		if (data.method == "GET" || data.method == "POST") sock.ssendall(data.textdata);
	}

}

void handler(SSocket sock) {
	sock.setrecvtimeout(recvtimeout);

	bool connection_keep_alive = false;
	string client_connection = "close";

	do {
		try {
			auto clrecv_char = sock.srecv(32768);
			if (clrecv_char.length == 0) break;

			string clrecv((char*)clrecv_char.buffer, clrecv_char.length);

			auto clrtmp = split(clrecv, "\r\n\r\n", 1);

			if (clrtmp.size() < 2) throw 400;

			string httphead = clrtmp[0];
			string httpdata = clrtmp[1];

			vector<string> headtmp = split(httphead, "\r\n");
			vector<string> httpstate = split(headtmp[0], " ");

			if (httpstate.size() < 3) throw 400;

			string method = httpstate[0];
			string default_path = urlDecode(httpstate[1]);
			string custom_path = httpstate[1].substr(1);
			string version = httpstate[2];
			vector<string> uploaded_files;
			map<string, map<string, string>> user_agent;
			fs::path path;

			if (find(methods.begin(), methods.end(), method) == methods.end()) throw 501;

			try { path = fs::current_path() / strtou8(urlDecode(custom_path)); }
			catch (...) { throw 400; }
			//---------------------parsing user agent---------------------
			for (int i = 1; i < headtmp.size(); i++) {
				auto tmp = split(headtmp[i], ": ");

				if (tmp.size() > 1) {
					for (auto& j : split(tmp[1], "; ")) {
						auto _tmp = split(j, "=");

						if (_tmp.size() > 1) user_agent[tmp[0]][_tmp[0]] = _tmp[1];
						else user_agent[tmp[0]][""] = _tmp[0];
					}
				}
			}
			//------------------------------------------------------------
			//-----------------------get connection-----------------------
			if (user_agent.find("Connection") != user_agent.end())
				if ((connection_keep_alive = toLower(user_agent["Connection"][""]) == "keep-alive"))
					client_connection = toLower(user_agent["Connection"][""]);
			//------------------------------------------------------------
			//----------------------recv client data----------------------
			if (user_agent.find("Content-Type") != user_agent.end()) {
				string boundary = user_agent["Content-Type"]["boundary"];
				size_t length = stoull(user_agent["Content-Length"][""]);

				length -= httpdata.length();

				while (length > 0) {
					auto datarecv = sock.srecv(32768);

					if (datarecv.length == 0) throw 400;
					httpdata.append((char*)datarecv.buffer, datarecv.length);

					length -= datarecv.length;
				}

				uploaded_files = split(httpdata, "--" + boundary);
				uploaded_files.erase(uploaded_files.begin());
				uploaded_files.pop_back();
			}
			//------------------------------------------------------------
			if (fs::exists(path)) {
				if (fs::is_directory(path)) {
					if (check_cgi(path)) throw 403;
					//-------------------------index.html-------------------------
					if (fs::exists(path / "index.html") && fs::is_regular_file(path / "index.html")) {

						socksend(sock, { .code = 200, .method = method, .connection = client_connection, .filestream = true, .filepath = path / "index.html", .filelength = fs::file_size(path / "index.html"), .AcceptRanges = true });

					}
					else if (fs::exists(path / "index.htm") && fs::is_regular_file(path / "index.htm")) {

						socksend(sock, { .code = 200, .method = method, .connection = client_connection, .filestream = true, .filepath = path / "index.htm", .filelength = fs::file_size(path / "index.htm"), .AcceptRanges = true });

					}
					//------------------------------------------------------------
					//----------------------directory listing---------------------
					else {
						string textdata = strformat("<!DOCTYPE html>\r\n<html>\r\n<head>\r\n<title>Directory listing for %s</title>\r\n</head>\r\n<body>\r\n<h1>Directory listing for %s</h1>\r\n<hr>\r\n<ul>\r\n", default_path.c_str(), default_path.c_str());

						set<fs::path> sorted;
						for (auto& i : fs::directory_iterator(path)) sorted.insert(i.path());

						for (auto& i : sorted) {
							auto fname = i.filename().u8string();

							if (fs::is_directory(i)) { textdata += strformat("<li><a href = \"%s/\">%s</a></li>\r\n", urlEncode(u8tostr(fname)).c_str(), fname.c_str()); }
							else { textdata += strformat("<li><a href = \"%s\">%s</a></li>\r\n", urlEncode(u8tostr(fname)).c_str(), fname.c_str()); }

						}
						textdata += "</ul>\r\n<hr>\r\n";

						socksend(sock, { .code = 200, .textdata = textdata, .method = method, .connection = client_connection });
					}
				}
				//------------------------------------------------------------
				//----------------process binary content & CGI----------------
				else {
					string ext = path.extension().string().substr(1);

					if (check_cgi(path)) {
						stringstream cmd_command;

						Json json;
						JsonNode node;
						node.addPair("method", method);
						node.addPair("version", version);
						node.addPair("path", custom_path);

						for (auto i : user_agent) {
							JsonNode node2;
							for (auto j : i.second) node2.addPair(j.first, j.second);

							node.addPair(i.first, node2);
						}

						if (ext == "py") {
#ifdef _WIN32
							cmd_command << "python " << path.string() << " " << quoted(json.dump(node));
#else
							cmd_command << "python3 " << path.string() << " " << quoted(json.dump(node));
#endif
						}

						else if (ext == "bin") {
							cmd_command << "./ " << path.string() << " " << quoted(json.dump(node));
						}

						FILE* fp = popen(cmd_command.str().c_str(), "r");
						char buffer[4096];

						string strbuff;
						while (!feof(fp)) strbuff.append(buffer, fread(buffer, 1, 4096, fp));

						srvresp resp;
						resp.code = 200;
						resp.method = method;
						resp.connection = client_connection;
						resp.textdata = strbuff;

						socksend(sock, resp);

						pclose(fp);
					}

					else {
						if (user_agent.find("Range") != user_agent.end()) {
							size_t range = stoull(split(user_agent["Range"]["bytes"], "-")[0]);

							srvresp resp;
							resp.code = 206;
							resp.ext = (contenttype.find(ext) != contenttype.end()) ? ext : "other";
							resp.method = method;
							resp.connection = client_connection;
							resp.AcceptRanges = true;
							resp.filestream = true;
							resp.filepath = path;
							resp.filelength = fs::file_size(path);
							resp.ContentRange = true;
							resp.ContentRangeData = range;

							socksend(sock, resp);
						}

						else {
							srvresp resp;
							resp.code = 200;
							resp.ext = (contenttype.find(ext) != contenttype.end()) ? ext : "other";
							resp.method = method;
							resp.connection = client_connection;
							resp.AcceptRanges = true;
							resp.filestream = true;
							resp.filepath = path;
							resp.filelength = fs::file_size(path);

							socksend(sock, resp);
						}
					}
				}
				//------------------------------------------------------------
			}
			else { throw 404; }


		}
		catch (int code) {
			switch (code) {
			case 400: socksend(sock, { .code = code, .textdata = "<h1>400 Bad request</h1><br>", .connection = client_connection }); break;
			case 403: socksend(sock, { .code = code, .textdata = "<h1>403 Forbidden</h1>", .connection = client_connection }); break;
			case 404: socksend(sock, { .code = code, .textdata = "<h1>404 Not Found</h1>", .connection = client_connection }); break;
			case 501: socksend(sock, { .code = code, .textdata = "<h1>501 Not Implemented</h1><br>", .connection = client_connection }); break;
			default: socksend(sock, { .code = 500, .textdata = "<h1>500 Internal server error</h1><br>", .connection = client_connection }); break;
			}
		}
	} while (connection_keep_alive);

	sock.sclose();
}

int main(int argc, char** argv) {
	setlocale(LC_ALL, "");

	ArgumentParser parser(argc, argv);
	parser.add_argument({ .flag1 = "-p", .flag2 = "--port", .type = ANYINTEGER });
	parser.add_argument({ .flag1 = "-rd", .flag2 = "--root-directory" });
	parser.add_argument({ .flag1 = "-pp", .flag2 = "--parallel-processes", .type = ANYINTEGER });
	parser.add_argument({ .flag2 = "--CGI", .without_value = true });
	parser.add_argument({ .flag1 = "-t", .flag2 = "--timeout", .type = ANYINTEGER });
	auto i = parser.parse();

	int port = (i["--port"].type != ANYNONE) ? i["--port"].integer : 80;
	if (i["--root-directory"].type != ANYNONE) fs::current_path(strtou8(i["--root-directory"].str));
	int pp = (i["--parallel-processes"].type != ANYNONE) ? i["--parallel-processes"].integer : 1;
	recvtimeout = (i["--timeout"].type != ANYNONE) ? i["--timeout"].integer : 5;

	signal(SIGINT, [](int e) {exit(0); });
	SSocket sock(AF_INET, SOCK_STREAM);

	try {
		sock.ssetsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
		sock.sbind("", port);
		sock.slisten(0);
	}
	catch (int e) { cout << "Error: " << sstrerror(e) << endl; exit(e); }

#ifndef _WIN32
	for (int i = 1; i < pp; i++) process("HTTP Worker").start([&](process) { while (true) try { thread(handler, sock.saccept().first).detach(); } catch (...) {} })->detach();
#endif

	while (true) try { thread(handler, sock.saccept().first).detach(); }
	catch (...) {}
}
