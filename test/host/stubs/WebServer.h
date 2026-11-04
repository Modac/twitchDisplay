#pragma once
#include "Arduino.h"
#define HTTP_UPLOAD_BUFLEN 1436
enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };
enum HTTPAuthMethod { BASIC_AUTH, DIGEST_AUTH };
enum HTTPUploadStatus { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload { HTTPUploadStatus status; String filename; size_t totalSize; size_t currentSize; uint8_t buf[HTTP_UPLOAD_BUFLEN]; };
#include <map>
extern std::map<std::string,std::string> g_args; extern std::map<std::string,std::string> g_respHeaders;
extern bool g_authOk; extern int g_lastCode; extern std::string g_lastBody; extern int g_authChallenges; extern int g_contentLength;
struct Route { std::string uri; HTTPMethod method; std::function<void()> fn, ufn; };
class WebServer {
public:
  HTTPUpload up; std::vector<Route> routes; std::function<void()> notFound;
  WebServer(int){}
  void on(const char* u, HTTPMethod m, std::function<void()> f){ routes.push_back({u,m,f,nullptr}); }
  void on(const char* u, HTTPMethod m, std::function<void()> f, std::function<void()> uf){ routes.push_back({u,m,f,uf}); }
  void onNotFound(std::function<void()> f){ notFound=f; }
  void begin(){} void handleClient(){}
  HTTPUpload& upload(){ return up; }
  bool authenticate(const char*, const char*){ return g_authOk; }
  void requestAuthentication(HTTPAuthMethod, const char*){ g_authChallenges++; g_lastCode=401; g_lastBody=""; }
  void send(int code, const char* =nullptr, const std::string& body=""){ g_lastCode=code; g_lastBody=body; }
  int clientContentLength() const { return g_contentLength; }
  bool hasArg(const char* n){ return g_args.count(n)>0; }
  String arg(const char* n){ auto it=g_args.find(n); return it==g_args.end()?String():String(it->second); }
  void sendHeader(const std::string& k, const std::string& v, bool=false){ g_respHeaders[k]=v; }
  // test helper: call the handler for uri/method
  Route* find(const std::string& u, HTTPMethod m){ for(auto& r: routes) if(r.uri==u && r.method==m) return &r; return nullptr; }
};
