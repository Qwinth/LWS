//version 1.0
#define _DISABLE_RECV_LIMIT
#include <thread>
#include <vector>
#include <map>
#include <fstream>
#include <filesystem>
#include "../cpplibs/ssocket.hpp"
#include "../cpplibs/strlib.hpp"
#include "../cpplibs/argparse.hpp"
using namespace std;
namespace fs = std::filesystem;

map<string, string> contenttype = { {"html", "text/html"}, {"htm", "text/html"}, {"txt", "text/plain"}, {"ico", "image/x-icon"}, {"css", "text/css"}, {"js", "application/javascript"}, {"jpg", "image/jpeg"},{"png", "image/png"}, {"gif", "image/gif"}, {"mp3", "audio/mp3"}, {"ogg", "audio/ogg"}, {"wav", "audio/wav"}, {"opus", "audio/opus"}, {"m4a", "audio/mp4"}, {"mp4", "video/mp4"}, {"webm", "video/webm"}, {"pdf", "application/pdf"}, {"json", "text/json"}, {"xml", "text/xml"}, {"image", "svg+xml"}, {"other", "application/octet-stream"}};
vector<string> cgidirs = { "cgi-bin", "htbin" };

struct srvresp {
	int code;
	string textdata;
	string ext = "html";
	string method = "GET";
	bool filestream = false;
	fs::path filepath;
	size_t filelength;
	bool AcceptRanges = false;
	bool ContentRange = false;
	size_t ContentRangeData;
};

void socksend(SSocket sock, srvresp data) {
	string headers = strformat("HTTP/1.1 %d\r\nContent-Type: %s; charset=UTF-8\r\n", data.code, contenttype[data.ext].c_str());

	if (data.AcceptRanges) headers += "Accept-Ranges: bytes\r\n";

	headers += "Connection: close\r\n";
	headers += "Server: LWS\r\n";

	if (data.filestream) {
		ifstream file(data.filepath, ios::binary);
		if (data.ContentRange) {
			headers += strformat("Content-Range: bytes %zu-%zu/%zu\r\n", data.ContentRangeData, data.filelength - 1, data.filelength);
			headers += strformat("Content-Length: %zu\r\n\r\n", data.filelength - data.ContentRangeData);
			file.seekg(data.ContentRangeData);
		} else headers += strformat("Content-Length: %zu\r\n\r\n", data.filelength);
		
		
		sock.ssend(headers);

		if (data.method == "GET" || data.method == "POST") sock.ssend_file(file);

		file.close();
	} else {
		if (data.ContentRange) {
			headers += strformat("Content-Range: bytes %zu-%zu/%zu\r\n", data.ContentRangeData, data.textdata.size() - 1, data.textdata.size());
			headers += strformat("Content-Length: %zu\r\n\r\n", data.textdata.size() - data.ContentRangeData);
			data.textdata = data.textdata.substr(data.ContentRangeData);
		} else headers += strformat("Content-Length: %zu\r\n\r\n", data.textdata.size());
		
		sock.ssend(headers);

		if (data.method == "GET" || data.method == "POST") sock.ssend(data.textdata);
	}

}

void handler(SSocket sock) {
	try {
		string clrecv = sock.srecv(65536);

		if (clrecv.length() == 0) {
			sock.sclose();
			return;
		}
		vector<string> cltmp = split(clrecv, "\r\n");
		vector<string> ftmp = split(cltmp[0], " ");

		if (ftmp.size() < 3) throw 400;

		string method = ftmp[0];
		string default_path = urlDecode(ftmp[1]);
		string custom_path = ftmp[1].substr(1);
		fs::path path = fs::current_path() / strtou8(urlDecode(custom_path));
		string version = ftmp[2];
		map<string, string> user_agent;

		for (int i = 1; i < cltmp.size(); i++) {
			auto tmp = split(cltmp[i], ": ");
			if (tmp.size() > 1) user_agent[tmp[0]] = tmp[1];
		}

		if (fs::exists(path)) {
			if (fs::is_directory(path)) {
//-------------------------index.html-------------------------
				if (fs::exists(path / "index.html") && fs::is_regular_file(path / "index.html")) socksend(sock, {.code = 200, .method = method, .filestream = true, .filepath = path / "index.html", .filelength = fs::file_size(path / "index.html"), .AcceptRanges = true });
				else if (fs::exists(path / "index.htm") && fs::is_regular_file(path / "index.htm")) socksend(sock, { .code = 200, .method = method, .filestream = true, .filepath = path / "index.htm", .filelength = fs::file_size(path / "index.htm"), .AcceptRanges = true });
//------------------------------------------------------------
//----------------------directory listing---------------------
				else {
					string textdata = strformat("<!DOCTYPE html>\r\n<html>\r\n<head>\r\n<title>Directory listing for %s</title>\r\n<body>\r\n<h1>Directory listing for %s</h1>\r\n<hr>\r\n<ul>\r\n", default_path.c_str(), default_path.c_str());
					for (auto const &i : fs::directory_iterator(path)) {

						auto fname = i.path().filename().u8string();
						if (i.is_directory()) { textdata += strformat("<li><a href = \"%s/\">%s</a></li>\r\n", urlEncode(u8tostr(fname)).c_str(), fname.c_str()); }
						else { textdata += strformat("<li><a href = \"%s\">%s</a></li>\r\n", urlEncode(u8tostr(fname)).c_str(), fname.c_str()); }

					}
					textdata += "</ul>\r\n<hr>\r\n";

					socksend(sock, { .code = 200, .textdata = textdata, .method = method });
				}
			}
//------------------------------------------------------------
//-------------------process binary content-------------------
			else {
				string ext = path.extension().string().substr(1);

				if (user_agent.find("Range") != user_agent.end()) {
					size_t range = stoull(split(split(user_agent["Range"], "=")[1], "-")[0]);
					
					srvresp resp;
					resp.code = 206;
					resp.ext = (contenttype.find(ext) != contenttype.end()) ? ext : "other";
					resp.method = method;
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
					resp.AcceptRanges = true;
					resp.filestream = true;
					resp.filepath = path;
					resp.filelength = fs::file_size(path);
					
					socksend(sock, resp);
				}
			}
//------------------------------------------------------------
		} else throw 404;


	} catch (int code) {
		switch (code) {
		case 400: socksend(sock, { code, "<h1>400 Bad request</h1><br>" }); break;
		case 403: socksend(sock, { code, "<h1>403 Forbidden</h1>" }); break;
		case 404: socksend(sock, { code, "<h1>404 Not Found</h1>" }); break;
		default: socksend(sock, { 500, "<h1>500 Internal server error</h1><br>" }); break;
		}
	}
	sock.sclose();
}

int main(int argc, char** argv) {
	ArgumentParser parser(argc, argv);
	parser.add_argument({ .flag1 = "-p", .flag2 = "--port", .type = "int" });
	parser.add_argument({ .flag1 = "-rd", .flag2 = "--root-directory" });
	parser.add_argument({ .flag2 = "--CGI", .without_value = true });
	auto i = parser.parse();

	int port = (i["--port"].str != "false") ? i["--port"].integer : 80;
	if (i["--root-directory"].str != "false") fs::current_path(strtou8(i["--root-directory"].str));

	SSocket sock(AF_INET, SOCK_STREAM);

	try {
		sock.ssetsockopt(SOL_SOCKET, SO_REUSEADDR, 1);
		sock.sbind("", port);
		sock.slisten(0);
	}
	catch (int e) { cout << "Error: " << e << endl; }

	while (true) {
		try { thread(handler, sock.saccept()).detach(); } catch (...) {}
	}
}
