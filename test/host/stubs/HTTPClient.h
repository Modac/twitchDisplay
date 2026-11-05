#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"
#include <sstream>
#include <deque>
#include <vector>
#define HTTP_CODE_OK 200
#define HTTP_CODE_UNAUTHORIZED 401
struct FakeResponse { int code; std::string body; int size = -2; }; // size -2: body size
struct FakeRequest { std::string method, url, auth, clientId, body, userAgent; bool tls; int follow = 0; };
#define HTTPC_STRICT_FOLLOW_REDIRECTS 1
extern std::deque<FakeResponse> g_responses;   // scripted, consumed in order
extern std::string g_response; extern int g_httpCode; // used when nothing is scripted
extern std::vector<FakeRequest> g_requests;
struct FakeStream {
  std::string data; size_t pos = 0;
  size_t available(){ return std::min<size_t>(data.size() - pos, 700); } // arrives in pieces
  size_t readBytes(uint8_t* b, size_t n){ n = std::min(n, data.size() - pos); memcpy(b, data.data() + pos, n); pos += n; return n; }
};
inline std::string g_responseDate;
struct HTTPClient {
  std::istringstream ss; FakeRequest req; bool ended=false; FakeStream fs; int size = -1;
  void begin(const String& u){ req.url=u; req.tls=false; }
  void begin(WiFiClientSecure&, const String& u){ req.url=u; req.tls=true; }
  void useHTTP10(bool){} void setAuthorizationType(const char*){}
  void setAuthorization(const char* a){ req.auth=a; }
  void addHeader(const char* k,const char* v){ if(std::string(k)=="Client-Id") req.clientId=v; }
  int respond(){ g_requests.push_back(req); FakeResponse r{g_httpCode,g_response};
    if(!g_responses.empty()){ r=g_responses.front(); g_responses.pop_front(); }
    ss.str(r.body); fs.data = r.body; fs.pos = 0; size = r.size == -2 ? (int)r.body.size() : r.size; return r.code; }
  void setUserAgent(const char* u){ req.userAgent = u; } void setUserAgent(const String& u){ req.userAgent = u; }
  void setFollowRedirects(int f){ req.follow = f; } void setTimeout(int){}
  int getSize(){ return size; } FakeStream* getStreamPtr(){ return &fs; } bool connected(){ return fs.pos < fs.data.size(); }   // the server closes after the body
  int GET(){ req.method="GET"; return respond(); }
  int POST(const char* b){ req.method="POST"; req.body=b; return respond(); }
  std::istream& getStream(){ return ss; }
  void end(){ ended=true; }
  void collectHeaders(const char**, size_t){}
  String header(const char* n){ return std::string(n) == "Date" ? String(g_responseDate.c_str()) : String(); }
  String errorToString(int){ return String("err"); }
};
