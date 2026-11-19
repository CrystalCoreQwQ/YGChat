#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <unordered_map>
#include <ctime>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <conio.h>
#include <wchar.h>

using namespace std;

template <size_t N>
static void copyW(wchar_t (&dst)[N], const wchar_t* src) {
	if (!src) {
		dst[0] = 0;
		return;
	}
	wcsncpy(dst, src, N - 1);
	dst[N - 1] = 0;
}

const int PORT = 8888;
SOCKET g_sock = INVALID_SOCKET;

typedef int (WSAAPI *pWSAStartup)(WORD, LPWSADATA);
typedef int (WSAAPI *pWSACleanup)();
typedef SOCKET (WSAAPI *psocket)(int, int, int);
typedef int (WSAAPI *psetsockopt)(SOCKET, int, int, const char*, int);
typedef int (WSAAPI *pbind)(SOCKET, const sockaddr*, int);
typedef int (WSAAPI *psendto)(SOCKET, const char*, int, int, const sockaddr*, int);
typedef int (WSAAPI *precvfrom)(SOCKET, char*, int, int, sockaddr*, int*);
typedef int (WSAAPI *pclosesocket)(SOCKET);
typedef unsigned long (WSAAPI *pinet_addr)(const char*);
typedef char* (WSAAPI *pinet_ntoa)(struct in_addr);
typedef DWORD (WINAPI *pGetAdaptersInfo)(PIP_ADAPTER_INFO, PULONG);
typedef BOOL (WINAPI *pShell_NotifyIconW)(DWORD, PNOTIFYICONDATAW);

pWSAStartup myWSAStartup = nullptr;
pWSACleanup myWSACleanup = nullptr;
psocket mysocket = nullptr;
psetsockopt mysetsockopt = nullptr;
pbind mybind = nullptr;
psendto mysendto = nullptr;
precvfrom myrecvfrom = nullptr;
pclosesocket myclosesocket = nullptr;
pinet_addr myinet_addr = nullptr;
pinet_ntoa myinet_ntoa = nullptr;
pGetAdaptersInfo myGetAdaptersInfo = nullptr;
pShell_NotifyIconW myShell_NotifyIconW = nullptr;

unsigned short custom_htons(unsigned short h) {
	return (h << 8) | (h >> 8);
}

enum class MessageType { Others, Selfs, SelfPrivate, OtherPrivate, System };

struct Message {
	MessageType type;
	string name;
	string content;
	int color;
};

volatile bool g_running = true;
volatile bool g_msgDirty = true;
CRITICAL_SECTION g_cs;

string g_myNick = "匿名";
string g_currentChatTarget = "所有人";
string ReceivePath = ".";
string g_dataDir = ".";
int g_myColor = 7;
int g_scrollOffset = 0;
int g_dbgMouse = 0;
int g_dbgWheel = 0;
int g_dbgSeq  = 0;
int g_dbgKey  = 0;
int g_dbgChar = 0;
bool Whitelist_state = false;

vector<Message> g_chatLog;
vector<string> g_broadcastIPs;
unordered_map<string, pair<DWORD, string>> g_onlineUsers;
unordered_map<string, int> g_userColors;

set<string> Whitelist;
set<string> g_receivedMsgIDs;
deque<string> g_recvOrder;
set<string> g_sentMsgIDs;
deque<string> g_sentOrder;

DWORD g_lastConflictTime = 0;

volatile bool g_notifyPrivate = true;
volatile bool g_notifyFile    = true;
volatile LONG g_notifyShowing = 0;

const int FILE_CHUNK_RAW      = 1024;
const int FILE_SEND_BATCH     = 128;
const int FILE_NAK_BATCH      = 200;
const int FILE_NAK_MAX_BYTES  = 1200;
const int FILE_NAK_MAX_RANGE  = 20000;
const int FILE_NAK_MAX_CHUNKS = 200000;
const DWORD FILE_RX_TIMEOUT   = 120000;
const long long FILE_MAX_SIZE = 5LL * 1024 * 1024 * 1024;
const int FILE_MAX_CHUNKS     = 3145728;

enum class FileTxState { Pending, Sending, Done, Failed };

struct FileTx {
	string target, tid, name, path;
	long long size = 0;
	int totalChunks = 0;
	FileTxState state = FileTxState::Pending;
	vector<int> nextSend;
	vector<int> nakList;
	DWORD nakRound = 0;
	bool nakPending = false;
	string error;
	DWORD finishedAt = 0;
};

struct FileRx {
	string sender, tid, name, path;
	long long size = 0;
	int totalChunks = 0;
	vector<char> bitmap;
	int gotChunks = 0;
	FILE* fp = nullptr;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	HANDLE hMap = NULL;
	void* mapBase = nullptr;
	DWORD lastActivity = 0;
	int lastPct = 0;
};

void releaseRxStorage(FileRx& rx) {
	if (rx.mapBase) {
		UnmapViewOfFile(rx.mapBase);
		rx.mapBase = nullptr;
	}
	if (rx.hMap) {
		CloseHandle(rx.hMap);
		rx.hMap = NULL;
	}
	if (rx.hFile != INVALID_HANDLE_VALUE) {
		CloseHandle(rx.hFile);
		rx.hFile = INVALID_HANDLE_VALUE;
	}
	if (rx.fp) {
		fclose(rx.fp);
		rx.fp = nullptr;
	}
}

unordered_map<string, FileTx> g_fileTx;
unordered_map<string, FileRx> g_fileRx;
struct FileRxDone {
	string sender, senderIP, name, finalPath;
	DWORD nakRound = 0;
	DWORD finishedAt = 0;
};
unordered_map<string, FileRxDone> g_fileRxDone;

HANDLE g_hOut = INVALID_HANDLE_VALUE;
HANDLE g_hIn  = INVALID_HANDLE_VALUE;
DWORD  g_origInMode = 0;
bool   g_hasOrigInMode = false;
int g_bufWidth = 80;
int g_width = 80;
string g_input;
const int MSG_ROWS = 25;
const int WHEEL_STEP = 3;
const int WHEEL_INPUT_DELTA = 120;
const int TITLE_ROW = 0;
const int MSG_TOP = 1;
const int SEP_ROW = MSG_TOP + MSG_ROWS;
const int INPUT_ROW = SEP_ROW + 1;

struct SuggItem {
	string text;
	string desc;
};
vector<SuggItem> g_sugg;
string g_suggBase;
int g_suggSel = -1;
bool g_suggLock = false;

void setColor(int color) {
	SetConsoleTextAttribute(g_hOut, color);
}
void resetColor() {
	setColor(7);
}

bool isAllSpaces(const string& s) {
	for (unsigned char c : s) if (!isspace(c)) return false;
	return true;
}

int displayWidth(const string& s) {
	int w = 0;
	for (size_t i = 0; i < s.size(); ) {
		unsigned char c = s[i];
		if (c >= 0x81 && i + 1 < s.size()) {
			w += 2;
			i += 2;
		} else {
			w += 1;
			i += 1;
		}
	}
	return w;
}

int namePrefixWidth(const string& name) {
	return 1 + displayWidth(name) + 2;
}

int selfPrivatePrefixWidth(const string& name) {
	return displayWidth("[私聊] 我 -> ") + displayWidth(name) + displayWidth(": ");
}

int otherPrivatePrefixWidth(const string& name) {
	return displayWidth("[私聊] ") + displayWidth(name) + displayWidth(" -> 我: ");
}

string escapeField(const string& s) {
	string r;
	r.reserve(s.size() + 8);
	for (unsigned char c : s) {
		if (c == '%')       r += "%25";
		else if (c == 0x01) r += "%01";
		else if (c == 0x00) r += "%00";
		else r += (char)c;
	}
	return r;
}

int hexVal(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

string unescapeField(const string& s) {
	if (s.find('%') == string::npos) return s;
	string r;
	r.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size()) {
			int a = hexVal(s[i + 1]), b = hexVal(s[i + 2]);
			if (a >= 0 && b >= 0) {
				r += (char)(a * 16 + b);
				i += 2;
				continue;
			}
		}
		r += s[i];
	}
	return r;
}

string buildPacket(int type, const string& sender, const string& receiver,
                   const string& msgID, int color, const string& content,
                   bool escapeContent = true) {
	string r;
	r.reserve(64 + content.size());
	r += to_string(type);
	r += '\x01';
	r += escapeField(sender);
	r += '\x01';
	r += escapeField(receiver);
	r += '\x01';
	r += escapeField(msgID);
	r += '\x01';
	r += to_string(color);
	r += '\x01';
	if (escapeContent) r += escapeField(content);
	else r += content;
	return r;
}

bool parsePacket(const string& raw, int& type, string& sender, string& receiver,
                 string& msgID, int& color, string& content) {
	size_t p[5], start = 0;
	for (int i = 0; i < 5; i++) {
		p[i] = raw.find('\x01', start);
		if (p[i] == string::npos) return false;
		start = p[i] + 1;
	}
	type     = atoi(raw.substr(0, p[0]).c_str());
	sender   = unescapeField(raw.substr(p[0] + 1, p[1] - p[0] - 1));
	receiver = unescapeField(raw.substr(p[1] + 1, p[2] - p[1] - 1));
	msgID    = unescapeField(raw.substr(p[2] + 1, p[3] - p[2] - 1));
	color    = atoi(raw.substr(p[3] + 1, p[4] - p[3] - 1).c_str());
	string contentRaw = raw.substr(p[4] + 1);
	if ((type >= 5 && type <= 8) || contentRaw.find('%') == string::npos)
		content.swap(contentRaw);
	else content = unescapeField(contentRaw);
	return true;
}

bool isValidName(const string& name) {
	if (name.empty() || displayWidth(name) > 50) return false;
	for (unsigned char c : name) {
		if (c < 0x20 || c == 0x7f || c == 0x01) return false;
	}
	return true;
}

static const char B64C[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64rev(unsigned char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

string base64Encode(const unsigned char* data, size_t len) {
	size_t olen = ((len + 2) / 3) * 4;
	string out(olen, '\0');
	char* o = olen ? &out[0] : nullptr;
	size_t i = 0;
	while (i + 2 < len) {
		unsigned v = ((unsigned)data[i] << 16) |
		             ((unsigned)data[i + 1] << 8) | data[i + 2];
		*o++ = B64C[(v >> 18) & 63];
		*o++ = B64C[(v >> 12) & 63];
		*o++ = B64C[(v >> 6) & 63];
		*o++ = B64C[v & 63];
		i += 3;
	}
	if (i + 1 == len) {
		unsigned v = (unsigned)data[i] << 16;
		*o++ = B64C[(v >> 18) & 63];
		*o++ = B64C[(v >> 12) & 63];
		*o++ = '=';
		*o++ = '=';
	} else if (i + 2 == len) {
		unsigned v = ((unsigned)data[i] << 16) | ((unsigned)data[i + 1] << 8);
		*o++ = B64C[(v >> 18) & 63];
		*o++ = B64C[(v >> 12) & 63];
		*o++ = B64C[(v >> 6) & 63];
		*o++ = '=';
	}
	return out;
}

string base64Decode(const string& s) {
	size_t n = s.size();
	string out((n / 4) * 3 + 3, '\0');
	char* o = &out[0];
	size_t i = 0;
	while (i + 3 < n) {
		int a = b64rev((unsigned char)s[i]);
		int b = b64rev((unsigned char)s[i + 1]);
		int c = b64rev((unsigned char)s[i + 2]);
		int d = b64rev((unsigned char)s[i + 3]);
		if (a < 0 || b < 0) break;
		unsigned v = ((unsigned)a << 18) | ((unsigned)b << 12) |
		             ((unsigned)(c < 0 ? 0 : c) << 6) |
		             (unsigned)(d < 0 ? 0 : d);
		*o++ = (char)((v >> 16) & 0xFF);
		if (c >= 0) *o++ = (char)((v >> 8) & 0xFF);
		if (d >= 0) *o++ = (char)(v & 0xFF);
		i += 4;
		if (c < 0 || d < 0) {
			out.resize((size_t)(o - &out[0]));
			return out;
		}
	}
	unsigned buf = 0;
	int bits = 0;
	for (; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (ch == '=') break;
		int v = b64rev(ch);
		if (v < 0) continue;
		buf = (buf << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			*o++ = (char)((buf >> bits) & 0xFF);
		}
	}
	out.resize((size_t)(o - &out[0]));
	return out;
}

string sanitizeFilename(const string& name) {
	string r;
	for (char c : name) {
		unsigned char u = (unsigned char)c;
		if (u < 0x20) continue;
		if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
		    c == '"'  || c == '<' || c == '>' || c == '|') continue;
		r += c;
	}
	if (r.empty() || r == "." || r == "..") r = "file";
	if (r.size() > 180) r = r.substr(0, 180);
	return r;
}

struct NotifyParam {
	string title, text;
};

static wstring gbkToWide(const string& s) {
	if (s.empty()) return L"";
	int n = MultiByteToWideChar(936, 0, s.c_str(), (int)s.size(), NULL, 0);
	wstring w(n, L'\0');
	MultiByteToWideChar(936, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

LRESULT CALLBACK notifyWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

DWORD WINAPI notifyThread(LPVOID p) {
	NotifyParam* np = (NotifyParam*)p;
	wstring tw = gbkToWide(np->title);
	wstring cw = gbkToWide(np->text);

	HINSTANCE hInst = GetModuleHandleW(NULL);
	const wchar_t* clsName = L"LanChatNotifyWnd";
	WNDCLASSW wc = {};
	wc.lpfnWndProc = notifyWndProc;
	wc.hInstance = hInst;
	wc.lpszClassName = clsName;
	RegisterClassW(&wc);

	HWND hwnd = CreateWindowExW(0, clsName, L"", 0, 0, 0, 0, 0,
	                            HWND_MESSAGE, NULL, hInst, NULL);
	if (!hwnd) {
		delete np;
		InterlockedExchange(&g_notifyShowing, 0);
		return 0;
	}

	NOTIFYICONDATAW nid = {};
	nid.cbSize = sizeof(nid);
	nid.hWnd = hwnd;
	nid.uID = 1;
	nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	nid.uCallbackMessage = WM_APP + 1;
	nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_INFORMATION);
	copyW(nid.szTip, L"\u5c40\u57df\u7f51\u804a\u5929\u5ba4");

	bool added = false;
	if (myShell_NotifyIconW && myShell_NotifyIconW(NIM_ADD, &nid)) {
		added = true;
		nid.uVersion = NOTIFYICON_VERSION_4;
		myShell_NotifyIconW(NIM_SETVERSION, &nid);

		nid.uFlags = NIF_INFO;
		nid.dwInfoFlags = NIIF_INFO;
		nid.uTimeout = 5000;
		copyW(nid.szInfoTitle, tw.c_str());
		copyW(nid.szInfo, cw.c_str());
		myShell_NotifyIconW(NIM_MODIFY, &nid);
	}

	Sleep(5500);

	if (added) {
		NOTIFYICONDATAW del = {};
		del.cbSize = sizeof(del);
		del.hWnd = hwnd;
		del.uID = 1;
		myShell_NotifyIconW(NIM_DELETE, &del);
	}
	DestroyWindow(hwnd);

	delete np;
	InterlockedExchange(&g_notifyShowing, 0);
	return 0;
}

void popNotify(const string& title, const string& text) {
	if (InterlockedCompareExchange(&g_notifyShowing, 1, 0) != 0) return;
	if (!myShell_NotifyIconW) {
		InterlockedExchange(&g_notifyShowing, 0);
		return;
	}
	NotifyParam* np = new NotifyParam{ title, text };
	HANDLE h = CreateThread(NULL, 0, notifyThread, np, 0, NULL);
	if (h) CloseHandle(h);
	else {
		delete np;
		InterlockedExchange(&g_notifyShowing, 0);
	}
}

void sendRaw(const string& packet, const vector<string>& ips) {
	sockaddr_in dest{};
	dest.sin_family = AF_INET;
	dest.sin_port = custom_htons(PORT);
	for (const auto& ip : ips) {
		if (ip.empty()) continue;
		dest.sin_addr.s_addr = myinet_addr(ip.c_str());
		mysendto(g_sock, packet.c_str(), (int)packet.size(), 0,
		         (sockaddr*)&dest, sizeof(dest));
	}
}

void sendToAddr(const string& packet, const sockaddr_in& dest) {
	mysendto(g_sock, packet.c_str(), (int)packet.size(), 0,
	         (const sockaddr*)&dest, sizeof(dest));
}

void sendBroadcast(const string& packet) {
	vector<string> ips = g_broadcastIPs;
	ips.push_back("224.0.0.100");
	EnterCriticalSection(&g_cs);
	for (auto& kv : g_onlineUsers) ips.push_back(kv.second.second);
	LeaveCriticalSection(&g_cs);
	sendRaw(packet, ips);
}

bool sendPrivate(const string& packet, const string& target) {
	string ip;
	EnterCriticalSection(&g_cs);
	auto it = g_onlineUsers.find(target);
	if (it != g_onlineUsers.end()) ip = it->second.second;
	LeaveCriticalSection(&g_cs);
	if (ip.empty()) return false;
	sendRaw(packet, { ip });
	return true;
}

void markSent(const string& id) {
	EnterCriticalSection(&g_cs);
	if (g_sentMsgIDs.insert(id).second) {
		g_sentOrder.push_back(id);
		while (g_sentOrder.size() > 800) {
			g_sentMsgIDs.erase(g_sentOrder.front());
			g_sentOrder.pop_front();
		}
	}
	LeaveCriticalSection(&g_cs);
}

bool isSentEcho(const string& id) {
	EnterCriticalSection(&g_cs);
	bool r = g_sentMsgIDs.count(id) > 0;
	LeaveCriticalSection(&g_cs);
	return r;
}

void markReceived(const string& id) {
	if (g_receivedMsgIDs.insert(id).second) {
		g_recvOrder.push_back(id);
		while (g_recvOrder.size() > 800) {
			g_receivedMsgIDs.erase(g_recvOrder.front());
			g_recvOrder.pop_front();
		}
	}
}

void clampLogLocked() {
	if (g_chatLog.size() > 1000)
		g_chatLog.erase(g_chatLog.begin(), g_chatLog.begin() + 500);
}

void pushMessage(const Message& m) {
	EnterCriticalSection(&g_cs);
	g_chatLog.push_back(m);
	clampLogLocked();
	g_msgDirty = true;
	LeaveCriticalSection(&g_cs);
}

void sysMsg(const string& s, int color = 8) {
	pushMessage({ MessageType::System, "", s, color });
}

string whitelistFile() {
	return g_dataDir + "YGChat_whitelist.txt";
}

bool wlEnabled() {
	EnterCriticalSection(&g_cs);
	bool on = Whitelist_state;
	LeaveCriticalSection(&g_cs);
	return on;
}

bool isFriend(const string& name) {
	EnterCriticalSection(&g_cs);
	bool f = Whitelist.count(name) > 0;
	LeaveCriticalSection(&g_cs);
	return f;
}

bool wlAllow(const string& name) {
	EnterCriticalSection(&g_cs);
	bool ok = !Whitelist_state || Whitelist.count(name) > 0;
	LeaveCriticalSection(&g_cs);
	return ok;
}

vector<string> friendIPs() {
	vector<string> ips;
	EnterCriticalSection(&g_cs);
	for (auto& kv : g_onlineUsers) {
		if (kv.first == g_myNick) continue;
		if (Whitelist.count(kv.first)) ips.push_back(kv.second.second);
	}
	LeaveCriticalSection(&g_cs);
	return ips;
}

vector<string> friendNames(bool onlineOnly) {
	vector<string> out;
	EnterCriticalSection(&g_cs);
	for (auto& f : Whitelist) {
		if (onlineOnly && !g_onlineUsers.count(f)) continue;
		out.push_back(f);
	}
	LeaveCriticalSection(&g_cs);
	return out;
}

vector<string> onlineOthers() {
	vector<string> out;
	EnterCriticalSection(&g_cs);
	for (auto& kv : g_onlineUsers) if (kv.first != g_myNick) out.push_back(kv.first);
	LeaveCriticalSection(&g_cs);
	return out;
}

void saveWhitelist() {
	vector<string> names;
	bool on;
	EnterCriticalSection(&g_cs);
	on = Whitelist_state;
	for (auto& n : Whitelist) names.push_back(n);
	LeaveCriticalSection(&g_cs);
	FILE* fp = fopen(whitelistFile().c_str(), "wb");
	if (!fp) return;
	fprintf(fp, "YGChat-Whitelist-v1\n");
	fprintf(fp, "state=%d\n", on ? 1 : 0);
	for (auto& n : names) fprintf(fp, "name=%s\n", n.c_str());
	fclose(fp);
}

void loadWhitelist() {
	FILE* fp = fopen(whitelistFile().c_str(), "rb");
	if (!fp) return;
	char line[512];
	bool on = false;
	EnterCriticalSection(&g_cs);
	Whitelist.clear();
	while (fgets(line, sizeof(line), fp)) {
		string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
		if (s.empty() || s == "YGChat-Whitelist-v1") continue;
		if (s.rfind("state=", 0) == 0) {
			on = atoi(s.c_str() + 6) != 0;
			continue;
		}
		if (s.rfind("name=", 0) == 0) s = s.substr(5);
		if (s.empty() || !isValidName(s)) continue;
		Whitelist.insert(s);
	}
	Whitelist_state = on;
	LeaveCriticalSection(&g_cs);
	fclose(fp);
}

unordered_map<string, DWORD> g_unfriendNotice;
unordered_map<string, DWORD> g_rejectShown;

void notifyRejectToSender(const string& name, const string& ip, const string& kind) {
	if (name.empty() || ip.empty()) return;
	DWORD now = GetTickCount();
	EnterCriticalSection(&g_cs);
	DWORD& last = g_unfriendNotice[name];
	bool send = (last == 0 || now - last > 100);
	if (send) last = now;
	LeaveCriticalSection(&g_cs);
	if (!send) return;
	string id = "uf_" + to_string(now) + "_" + to_string(rand() % 100000);
	markSent(id);
	string packet = buildPacket(9, g_myNick, name, id, g_myColor, kind);
	sendRaw(packet, { ip });
}

void onRejectedNotice(const string& name, const string& kind) {
	string what = (kind == "file") ? "文件未发送" : "消息未送达";
	sysMsg("[系统] 该用户未与你添加好友：" + name + "（" + what + "）", 12);
}


bool suggPrefixMatch(const string& text, const string& in) {
	if (text.size() < in.size()) return false;
	for (size_t i = 0; i < in.size(); ++i) {
		char a = text[i], b = in[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b) return false;
	}
	return true;
}

void clearSuggest() {
	if (!g_sugg.empty()) g_msgDirty = true;
	g_sugg.clear();
	g_suggSel = -1;
	g_suggBase.clear();
}

void applySuggSel() {
	if (g_suggSel >= 0 && g_suggSel < (int)g_sugg.size())
		g_input = g_sugg[g_suggSel].text;
	else
		g_input = g_suggBase;
	g_suggLock = true;
	g_msgDirty = true;
}

void buildSuggest() {
	size_t oldCount = g_sugg.size();
	g_sugg.clear();
	g_suggSel = -1;
	g_suggBase = g_input;
	if (g_input.empty() || g_input[0] != '/') {
		if (oldCount) g_msgDirty = true;
		return;
	}

	string in = g_input;
	vector<SuggItem> all;

	size_t sp = in.find(' ');
	if (sp == string::npos) {
		all.push_back({ "/all", "切换到群聊" });
		all.push_back({ "/chat", "与好友私聊（/chat -help 查看用法）" });
		all.push_back({ "/clear", "清空聊天记录" });
		all.push_back({ "/color", "改名字颜色（/color -help 查看用法）" });
		all.push_back({ "/exit", "退出程序" });
		all.push_back({ "/help", "查看全部指令" });
		all.push_back({ "/notify", "Windows 通知开关" });
		all.push_back({ "/quit", "退出程序" });
		all.push_back({ "/send", "发送文件（/send -help 查看用法）" });
		all.push_back({ "/whitelist", "好友白名单（/whitelist -help 查看用法）" });
	} else {
		string cmd = in.substr(0, sp);
		string arg = in.substr(sp + 1);
		size_t argSp = arg.find(' ');
		string sub = (argSp == string::npos) ? arg : arg.substr(0, argSp);
		if (cmd == "/whitelist") {
			if (argSp == string::npos) {
				all.push_back({ "/whitelist on", "开启白名单：仅好友可聊天与传文件" });
				all.push_back({ "/whitelist off", "关闭白名单：所有人可聊天" });
				all.push_back({ "/whitelist add", "添加好友" });
				all.push_back({ "/whitelist remove", "删除好友" });
				all.push_back({ "/whitelist list", "查看好友列表" });
				all.push_back({ "/whitelist clear", "清空全部好友" });
				all.push_back({ "/whitelist -help", "查看白名单用法" });
			} else if (sub == "add") {
				for (const auto& n : onlineOthers())
					if (!isFriend(n))
						all.push_back({ "/whitelist add " + n, "在线，尚未添加" });
			} else if (sub == "remove") {
				for (const auto& n : friendNames(false))
					all.push_back({ "/whitelist remove " + n, "已在好友名单" });
			}
		} else if (cmd == "/chat") {
			if (argSp == string::npos) {
				for (const auto& n : onlineOthers())
					if (isFriend(n))
						all.push_back({ "/chat " + n, "在线好友" });
				all.push_back({ "/chat -help", "查看 chat 用法" });
			}
		} else if (cmd == "/color") {
			if (argSp == string::npos) {
				static const char* cn[7] = { "白色", "绿色", "黄色", "深蓝色", "紫色", "红色", "浅蓝色" };
				for (int i = 0; i < 7; ++i)
					all.push_back({ string("/color ") + cn[i], "名字颜色" });
			}
		} else if (cmd == "/send") {
			all.push_back({ "/send -help", "查看 send 用法" });
			all.push_back({ "/send -all", "参数：发给所有在线好友（写在路径前）" });
			all.push_back({ "/send -opts", "参数：手动选择发送对象（写在路径前）" });
		}
	}

	for (const auto& it : all) {
		if (g_sugg.size() >= 9) break;
		if (it.text == in) continue;
		if (!suggPrefixMatch(it.text, in)) continue;
		g_sugg.push_back(it);
	}
	if (oldCount || !g_sugg.empty()) g_msgDirty = true;
}


static HHOOK g_mouseHook = NULL;
static volatile LONG g_wheelAccum = 0;
static int g_dbgHook = 0;

static bool foregroundIsOurConsole() {
	HWND fg = GetForegroundWindow();
	if (!fg) return false;
	HWND con = GetConsoleWindow();
	if (con && fg == con) return true;
	DWORD pid = 0;
	GetWindowThreadProcessId(fg, &pid);
	if (!pid) return false;
	if (pid == GetCurrentProcessId()) return true;
	static DWORD lastPid = 0;
	static bool lastOk = false;
	static DWORD lastTick = 0;
	DWORD now = GetTickCount();
	if (pid == lastPid && now - lastTick < 500) return lastOk;
	lastPid = pid;
	lastTick = now;
	lastOk = false;
	HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (hp) {
		WCHAR wpath[MAX_PATH] = {0};
		DWORD sz = MAX_PATH;
		if (QueryFullProcessImageNameW(hp, 0, wpath, &sz)) {
			const wchar_t* base = wcsrchr(wpath, L'\\');
			base = base ? base + 1 : wpath;
			static const wchar_t* okNames[] = {
				L"conhost.exe", L"OpenConsole.exe", L"WindowsTerminal.exe",
				L"WindowsTerminalPreview.exe", L"wt.exe", L"mintty.exe",
				L"wezterm-gui.exe", L"alacritty.exe", L"Code.exe", L"cmd.exe",
				L"powershell.exe", L"pwsh.exe"
			};
			for (size_t i = 0; i < sizeof(okNames) / sizeof(okNames[0]); i++) {
				if (_wcsicmp(base, okNames[i]) == 0) {
					lastOk = true;
					break;
				}
			}
		} else {
			lastOk = true;
		}
		CloseHandle(hp);
	} else {
		lastOk = true;
	}
	return lastOk;
}

static LRESULT CALLBACK lowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION && wParam == WM_MOUSEWHEEL) {
		const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lParam;
		SHORT delta = (SHORT)HIWORD(ms->mouseData);
		if (delta != 0) InterlockedExchangeAdd(&g_wheelAccum, (LONG)delta);
	}
	return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static HANDLE g_hookThread = NULL;

static DWORD WINAPI mouseHookThread(LPVOID) {
	g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, lowLevelMouseProc, GetModuleHandleW(NULL), 0);
	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	if (g_mouseHook) {
		UnhookWindowsHookEx(g_mouseHook);
		g_mouseHook = NULL;
	}
	return 0;
}

static void installMouseHook() {
	if (g_mouseHook || g_hookThread) return;
	g_hookThread = CreateThread(NULL, 0, mouseHookThread, NULL, 0, NULL);
}

static void uninstallMouseHook() {
	if (g_hookThread) {
		PostThreadMessageW(GetThreadId(g_hookThread), WM_QUIT, 0, 0);
		WaitForSingleObject(g_hookThread, 500);
		CloseHandle(g_hookThread);
		g_hookThread = NULL;
	}
}

static void applyPendingWheel() {
	LONG wa = InterlockedExchange(&g_wheelAccum, 0);
	if (wa == 0) return;
	if (g_dbgSeq > 0) return;
	if (g_dbgWheel > 0) return;
	if (!foregroundIsOurConsole()) return;
	++g_dbgHook;
	int steps = (int)(wa / WHEEL_INPUT_DELTA);
	if (steps == 0) steps = (wa > 0 ? 1 : -1);
	if (steps > 0) {
		g_scrollOffset += steps * WHEEL_STEP;
	} else {
		g_scrollOffset -= (-steps) * WHEEL_STEP;
		if (g_scrollOffset < 0) g_scrollOffset = 0;
	}
	g_msgDirty = true;
}

static bool processIsElevated() {
	HANDLE hTok = NULL;
	bool elev = false;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok)) {
		TOKEN_ELEVATION te;
		DWORD cb = 0;
		if (GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &cb))
			elev = (te.TokenIsElevated != 0);
		CloseHandle(hTok);
	}
	return elev;
}

typedef BOOL (WINAPI *PFN_GetOpenFileNameW)(LPOPENFILENAMEW);
static bool pickFileToSend() {
	HMODULE hCd = LoadLibraryA("comdlg32.dll");
	if (!hCd) return false;
	PFN_GetOpenFileNameW pFn = (PFN_GetOpenFileNameW)GetProcAddress(hCd, "GetOpenFileNameW");
	if (!pFn) {
		FreeLibrary(hCd);
		return false;
	}
	static wchar_t wzFile[1024];
	wzFile[0] = 0;
	OPENFILENAMEW ofn;
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner   = GetConsoleWindow();
	ofn.lpstrFile   = wzFile;
	ofn.nMaxFile    = 1024;
	static wchar_t wzTitle[128];
	ofn.lpstrFilter = L"All Files\0*.*\0\0";
	if (MultiByteToWideChar(936, 0, "选择要发送的文件", -1, wzTitle, 128) > 0)
		ofn.lpstrTitle = wzTitle;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	BOOL ok = pFn(&ofn);
	FreeLibrary(hCd);
	if (!ok || !wzFile[0]) return false;
	char gbk[2048];
	int n = WideCharToMultiByte(936, 0, wzFile, -1, gbk, (int)sizeof(gbk) - 1, NULL, NULL);
	if (n <= 0) return false;
	bool reqAll = false, reqOpts = false;
	{
		string rest = g_input;
		if (rest.rfind("/send", 0) == 0) rest = rest.substr(5);
		size_t p = 0;
		while (p < rest.size()) {
			while (p < rest.size() && (rest[p] == ' ' || rest[p] == '\t')) ++p;
			if (p >= rest.size()) break;
			size_t q = rest.find_first_of(" \t", p);
			string tok = (q == string::npos) ? rest.substr(p) : rest.substr(p, q - p);
			string low = tok;
			for (size_t k = 0; k < low.size(); ++k) low[k] = (char)tolower((unsigned char)low[k]);
			if (low == "-all") reqAll = true;
			else if (low == "-opts") reqOpts = true;
			if (q == string::npos) break;
			p = q + 1;
		}
	}
	g_input = "/send";
	if (reqAll) g_input += " -all";
	if (reqOpts) g_input += " -opts";
	g_input += " \"";
	g_input += gbk;
	g_input += "\"";
	return true;
}

void showMouseDiag() {
	DWORD inMode = 0, outMode = 0;
	GetConsoleMode(g_hIn, &inMode);
	GetConsoleMode(g_hOut, &outMode);
	CONSOLE_SCREEN_BUFFER_INFO ci;
	char buf[1024];
	int winH = -1, bufH = -1;
	if (GetConsoleScreenBufferInfo(g_hOut, &ci)) {
		winH = ci.srWindow.Bottom - ci.srWindow.Top + 1;
		bufH = ci.dwSize.Y;
	}
	char fgCls[128] = {0};
	HWND fg = GetForegroundWindow();
	if (fg) GetClassNameA(fg, fgCls, sizeof(fgCls) - 1);
	char conCls[128] = {0};
	HWND cw = GetConsoleWindow();
	if (cw) GetClassNameA(cw, conCls, sizeof(conCls) - 1);
	sprintf(buf,
	        "[诊断] 滚轮=%d 钩子=%d 按键=%d 入框字符=%d | MOUSE_INPUT=%d QUICK_EDIT=%d | 缓冲高=%d 窗口高=%d | 前台=%s | 宿主=%s 提权=%d",
	        g_dbgWheel, g_dbgHook, g_dbgKey, g_dbgChar,
	        (inMode & ENABLE_MOUSE_INPUT) ? 1 : 0,
	        (inMode & ENABLE_QUICK_EDIT_MODE) ? 1 : 0,
	        bufH, winH, fg ? fgCls : "(无)",
	        conCls[0] ? conCls : "(无)", processIsElevated() ? 1 : 0);
	sysMsg(string(buf), 11);
	g_msgDirty = true;
}

void gotoxy(int x, int y) {
	cout.flush();
	COORD c = { (SHORT)x, (SHORT)y };
	SetConsoleCursorPosition(g_hOut, c);
}

void clearLine(int y) {
	cout.flush();
	COORD c = { 0, (SHORT)y };
	DWORD written;
	FillConsoleOutputCharacterA(g_hOut, ' ', g_bufWidth, c, &written);
	FillConsoleOutputAttribute(g_hOut, 7, g_bufWidth, c, &written);
}

void appendSeg(vector<pair<string, int>>& segs, const string& s, int color,
               int& w, int maxW) {
	string part;
	for (size_t i = 0; i < s.size(); ) {
		unsigned char c = s[i];
		int bytes = 1, cw = 1;
		if (c >= 0x81 && i + 1 < s.size()) {
			bytes = 2;
			cw = 2;
		}
		if (c < 0x20) {
			i += bytes;
			continue;
		}
		if (w + cw > maxW) break;
		part.append(s, i, bytes);
		w += cw;
		i += bytes;
	}
	if (!part.empty()) segs.push_back({ part, color });
}

void drawMessageAt(int row, const Message& msg, int maxW, bool continuation) {
	clearLine(row);
	gotoxy(0, row);
	vector<pair<string, int>> segs;
	int w = 0;
	switch (msg.type) {
		case MessageType::Others:
		case MessageType::Selfs:
			if (continuation) {
				int pw = namePrefixWidth(msg.name);
				appendSeg(segs, string((size_t)pw, ' '), 7, w, maxW);
			} else {
				appendSeg(segs, "[", 7, w, maxW);
				appendSeg(segs, msg.name, msg.color, w, maxW);
				appendSeg(segs, "] ", 7, w, maxW);
			}
			appendSeg(segs, msg.content, 7, w, maxW);
			break;
		case MessageType::SelfPrivate:
			if (continuation) {
				int pw = selfPrivatePrefixWidth(msg.name);
				appendSeg(segs, string((size_t)pw, ' '), 7, w, maxW);
			} else {
				appendSeg(segs, "[私聊] 我 -> ", 13, w, maxW);
				appendSeg(segs, msg.name, msg.color, w, maxW);
				appendSeg(segs, ": ", 7, w, maxW);
			}
			appendSeg(segs, msg.content, 7, w, maxW);
			break;
		case MessageType::OtherPrivate:
			if (continuation) {
				int pw = otherPrivatePrefixWidth(msg.name);
				appendSeg(segs, string((size_t)pw, ' '), 7, w, maxW);
			} else {
				appendSeg(segs, "[私聊] ", 13, w, maxW);
				appendSeg(segs, msg.name, msg.color, w, maxW);
				appendSeg(segs, " -> 我: ", 7, w, maxW);
			}
			appendSeg(segs, msg.content, 7, w, maxW);
			break;
		case MessageType::System:
			appendSeg(segs, msg.content, msg.color, w, maxW);
			break;
	}
	for (auto& p : segs) {
		setColor(p.second);
		cout << p.first;
	}
	resetColor();
}

string tailToWidth(const string& s, int avail) {
	if (avail <= 0) return "";
	vector<pair<size_t, int>> units;
	for (size_t i = 0; i < s.size(); ) {
		unsigned char c = s[i];
		if (c >= 0x81 && i + 1 < s.size()) {
			units.push_back({ i, 2 });
			i += 2;
		} else {
			units.push_back({ i, 1 });
			i += 1;
		}
	}
	int total = 0;
	int startUnit = (int)units.size();
	for (int k = (int)units.size() - 1; k >= 0; k--) {
		if (total + units[k].second > avail) break;
		total += units[k].second;
		startUnit = k;
	}
	if (startUnit >= (int)units.size()) return "";
	return s.substr(units[startUnit].first);
}

void drawSuggestions() {
	int n = (int)g_sugg.size();
	if (n <= 0) return;
	if (n > MSG_ROWS - 2) n = MSG_ROWS - 2;
	auto clip = [&](const string & s, int maxw) {
		int acc = 0;
		string out;
		for (size_t i = 0; i < s.size(); ) {
			unsigned char c = (unsigned char)s[i];
			int wch = (c >= 0x81 && i + 1 < s.size()) ? 2 : 1;
			if (acc + wch > maxw) break;
			out.append(s, i, (size_t)wch);
			acc += wch;
			i += (size_t)wch;
		}
		return out;
	};
	for (int i = 0; i < n; ++i) {
		int row = SEP_ROW - n + i;
		if (row < MSG_TOP) break;
		bool sel = (i == g_suggSel);
		string line = "[" + to_string(i + 1) + "] " + g_sugg[i].text;
		int pad = 34 - displayWidth(line);
		if (pad > 0) line += string((size_t)pad, ' ');
		line += g_sugg[i].desc;
		line = clip(line, g_width - 2);
		clearLine(row);
		gotoxy(0, row);
		setColor(sel ? 14 : 7);
		cout << line;
	}
	resetColor();
}

void drawInputLine() {
	int online;
	int friends = 0;
	bool wlOn = false;
	string target;
	EnterCriticalSection(&g_cs);
	online = (int)g_onlineUsers.size() + 1;
	target = g_currentChatTarget;
	friends = (int)Whitelist.size();
	wlOn = Whitelist_state;
	LeaveCriticalSection(&g_cs);

	clearLine(INPUT_ROW);
	gotoxy(0, INPUT_ROW);

	int w = 0;
	auto put = [&](const string & s, int col) {
		setColor(col);
		cout << s;
		w += displayWidth(s);
	};
	put(">> [", 7);
	if (target == "所有人") put("群聊", 10);
	else put("私聊:" + target, 13);
	put(" | 在线:" + to_string(online) + " | 好友:" + to_string(friends) + "] ", 7);
	put(wlOn ? "[白名单:开] " : "[白名单:关] ", wlOn ? 14 : 8);
	put("( 键入 /help 获取指令集 ) ", 10);

	int avail = g_width - 1 - w;
	if (avail > 0) {
		setColor(15);
		cout << tailToWidth(g_input, avail);
	}
	resetColor();
	drawSuggestions();
}

void makeBufferWindowSized() {
	CONSOLE_SCREEN_BUFFER_INFO ci;
	if (!GetConsoleScreenBufferInfo(g_hOut, &ci)) return;
	SHORT winW = (SHORT)(ci.srWindow.Right - ci.srWindow.Left + 1);
	SHORT winH = (SHORT)(ci.srWindow.Bottom - ci.srWindow.Top + 1);
	if (winW < 20 || winH < 5) return;
	if (winH < MSG_ROWS + 3) return;
	if (ci.dwSize.Y == winH) return;

	SMALL_RECT tiny = { 0, 0, 0, 0 };
	if (!SetConsoleWindowInfo(g_hOut, TRUE, &tiny)) return;
	COORD size = ci.dwSize;
	size.Y = winH;
	SetConsoleScreenBufferSize(g_hOut, size);
	SMALL_RECT want = { 0, 0, (SHORT)(winW - 1), (SHORT)(winH - 1) };
	SetConsoleWindowInfo(g_hOut, TRUE, &want);
}

bool g_vtMouseOn = false;

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
	#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

void writeVTSeq(const char* s) {
	DWORD oldMode = 0;
	if (!GetConsoleMode(g_hOut, &oldMode)) return;
	if (!SetConsoleMode(g_hOut, oldMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) return;
	DWORD wrote = 0;
	WriteConsoleA(g_hOut, s, (DWORD)strlen(s), &wrote, NULL);
	SetConsoleMode(g_hOut, oldMode);
}

void setTerminalMouse(bool on) {
	writeVTSeq(on ? "\x1b[?1000h\x1b[?1006h" : "\x1b[?1000l\x1b[?1006l");
	g_vtMouseOn = on;
}

bool consumeMouseSeqChar(WCHAR wc, bool& dirty) {
	static string buf;
	if (buf.empty()) {
		if (wc != 27) return false;
		buf = "\x1b";
		return true;
	}
	if (buf.size() == 1) {
		if (wc == L'[') {
			buf += '[';
			return true;
		}
		buf.clear();
		return false;
	}
	if (buf.size() == 2) {
		if (wc == L'<' || wc == L'M') {
			buf += (char)wc;
			return true;
		}
		buf.clear();
		return false;
	}
	if ((wc >= L'0' && wc <= L'9') || wc == L';') {
		if (buf.size() < 24) buf += (char)wc;
		else buf.clear();
		return true;
	}
	if (wc == L'M' || wc == L'm') {
		buf += (char)wc;
		unsigned int btn = 0;
		if (sscanf(buf.c_str() + 3, "%u", &btn) == 1) {
			int code = (int)(btn & 0x7f);
			if (code == 64) {
				g_scrollOffset += WHEEL_STEP;
				++g_dbgSeq;
				dirty = true;
			} else if (code == 65) {
				g_scrollOffset -= WHEEL_STEP;
				if (g_scrollOffset < 0) g_scrollOffset = 0;
				++g_dbgSeq;
				dirty = true;
			}
		}
		buf.clear();
		return true;
	}
	buf.clear();
	return false;
}

void renderScreen() {
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	if (GetConsoleScreenBufferInfo(g_hOut, &csbi)) {
		g_bufWidth = csbi.dwSize.X;
		g_width = csbi.dwSize.X;
	}
	if (g_width < 20) g_width = 20;

	vector<Message> msgs;
	EnterCriticalSection(&g_cs);
	int total = (int)g_chatLog.size();
	int maxOff = total > MSG_ROWS ? total - MSG_ROWS : 0;
	if (g_scrollOffset > maxOff) g_scrollOffset = maxOff;
	if (g_scrollOffset < 0) g_scrollOffset = 0;
	int end = total - g_scrollOffset;
	if (end < 0) end = 0;
	if (end > total) end = total;
	int start = end > MSG_ROWS ? end - MSG_ROWS : 0;
	for (int i = start; i < end; i++) msgs.push_back(g_chatLog[i]);
	LeaveCriticalSection(&g_cs);

	clearLine(TITLE_ROW);
	gotoxy(0, TITLE_ROW);
	setColor(14);
	string title = "=============== YGChat 1.1.0 ===============";
	cout << title;
	resetColor();

	for (int i = 0; i < MSG_ROWS; i++) {
		int row = MSG_TOP + i;
		if (i < (int)msgs.size()) {
			bool cont = false;
			if (i > 0) {
				const Message& p = msgs[i - 1];
				const Message& c = msgs[i];
				int pw = 0;
				bool ok = false;

				if ((p.type == MessageType::Others || p.type == MessageType::Selfs) &&
				    (c.type == MessageType::Others || c.type == MessageType::Selfs) &&
				    p.name == c.name) {
					pw = namePrefixWidth(c.name);
					ok = true;
				} else if (p.type == MessageType::SelfPrivate && c.type == MessageType::SelfPrivate &&
				           p.name == c.name) {
					pw = selfPrivatePrefixWidth(c.name);
					ok = true;
				} else if (p.type == MessageType::OtherPrivate && c.type == MessageType::OtherPrivate &&
				           p.name == c.name) {
					pw = otherPrivatePrefixWidth(c.name);
					ok = true;
				}

				if (ok && pw <= (g_width - 1) - 10) cont = true;
			}
			drawMessageAt(row, msgs[i], g_width - 1, cont);
		} else {
			clearLine(row);
		}
	}

	clearLine(SEP_ROW);
	gotoxy(0, SEP_ROW);
	setColor(8);
	cout << string((size_t)max(0, min(g_width - 1, 44)), '-');
	resetColor();

	drawInputLine();
}

int readMenuKey() {
	while (true) {
		INPUT_RECORD recs[16];
		DWORD got = 0;
		if (!ReadConsoleInputW(g_hIn, recs, 16, &got)) {
			Sleep(10);
			continue;
		}
		for (DWORD i = 0; i < got; i++) {
			if (recs[i].EventType != KEY_EVENT) continue;
			KEY_EVENT_RECORD& k = recs[i].Event.KeyEvent;
			if (!k.bKeyDown) continue;
			WORD vk = k.wVirtualKeyCode;
			WCHAR wc = k.uChar.UnicodeChar;
			if (vk == VK_PROCESSKEY) continue;
			if (vk == VK_UP)   return 72;
			if (vk == VK_DOWN) return 80;
			if (vk == VK_ESCAPE) return 27;
			if (wc == L'w' || wc == L'W') return 'w';
			if (wc == L's' || wc == L'S') return 's';
			if (wc == L' ' || wc == L'\r') return 13;
		}
	}
}

int showInteractiveMenu(const vector<string>& options) {
	int sel = 0, count = (int)options.size();
	while (true) {
		system("cls");
		setColor(11);
		cout << "================ 【 选 择 】 ================\n\n";
		resetColor();
		for (int i = 0; i < count; i++) {
			if (i == sel) {
				setColor(14);
				cout << "  > " << options[i] << " <\n";
				resetColor();
			} else cout << "    " << options[i] << "\n";
		}
		setColor(8);
		cout << "\n=============================================\n";
		cout << " (W/S 或 方向键 移动, 空格/回车 确认, ESC 取消)\n";
		resetColor();

		int key = readMenuKey();
		if (key == 27)      return -1;
		if (key == 72)      sel = (sel - 1 + count) % count;
		else if (key == 80) sel = (sel + 1) % count;
		else if (key == 'w' || key == 'W') sel = (sel - 1 + count) % count;
		else if (key == 's' || key == 'S') sel = (sel + 1) % count;
		else if (key == 13) return sel;
	}
}

void showListScreen(const string& title, const string& footer,
                    const vector<string>& items, const vector<int>& colors) {
	int count = (int)items.size();
	int top = 0;
	const int pageMax = 18;
	int page = (count < pageMax) ? count : pageMax;
	if (page < 1) page = 1;
	while (true) {
		system("cls");
		setColor(11);
		cout << "=============== 【 " << title << " 】 ===============\n\n";
		resetColor();
		if (count == 0) {
			setColor(8);
			cout << "    （暂无内容）\n";
			resetColor();
		}
		for (int i = top; i < count && i < top + page; ++i) {
			setColor(i < (int)colors.size() ? colors[i] : 7);
			cout << "    " << items[i] << "\n";
		}
		setColor(8);
		cout << "\n=============================================\n";
		cout << " " << footer << "\n";
		if (count > page)
			cout << " W/S 或 方向键 滚动, 当前 " << (top + 1) << "-"
			     << ((top + page < count) ? (top + page) : count) << "/" << count << "\n";
		resetColor();

		int key = readMenuKey();
		if (key == 13 || key == 27) return;
		if (key == 72 || key == 'w' || key == 'W') {
			if (top > 0) --top;
		} else if (key == 80 || key == 's' || key == 'S') {
			if (top + page < count) ++top;
		}
	}
}

void sendMessage(int type, const string& target, const string& content) {
	if (type != 0 && !wlAllow(target)) {
		sysMsg("[系统] 该用户未与你添加好友：" + target, 12);
		return;
	}
	string msgID = to_string(GetTickCount()) + "_" + to_string(rand() % 100000);
	string packet = buildPacket(type, g_myNick, target, msgID, g_myColor, content);
	markSent(msgID);

	if (type == 0) {
		if (wlEnabled()) {
			vector<string> ips = friendIPs();
			if (ips.empty()) {
				sysMsg("[系统] 白名单已开启：当前没有在线好友，消息未发送", 12);
				return;
			}
			sendRaw(packet, ips);
			pushMessage({ MessageType::Selfs, g_myNick, content, g_myColor });
			sysMsg("[白名单] 群聊消息仅发送给 " + to_string(ips.size()) + " 位在线好友", 8);
			return;
		}
		sendBroadcast(packet);
		pushMessage({ MessageType::Selfs, g_myNick, content, g_myColor });
	} else {
		if (!sendPrivate(packet, target)) {
			pushMessage({ MessageType::System, "", "[系统] " + target + " 不在线，消息未发送", 12 });
			return;
		}
		pushMessage({ MessageType::SelfPrivate, target, content, g_myColor });
	}
}

bool checkNameConflict(const string& name) {
	string probeID = "probe_" + to_string(GetTickCount()) + "_" + to_string(rand() % 10000);
	string packet = buildPacket(2, name, "所有人", probeID, g_myColor, name);
	sendBroadcast(packet);

	DWORD start = GetTickCount();
	while (GetTickCount() - start < 800) {
		char buf[2048];
		sockaddr_in from{};
		int fromLen = sizeof(from);
		int n = myrecvfrom(g_sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
		if (n <= 0) continue;
		string raw(buf, n);
		int type, color;
		string sender, receiver, msgID, content;
		if (!parsePacket(raw, type, sender, receiver, msgID, color, content)) continue;
		if (type == 3 && receiver == name && msgID == probeID) return true;
	}
	return false;
}

void initBroadcastIPs() {
	g_broadcastIPs.push_back("255.255.255.255");
	ULONG size = 0;
	if (myGetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW) return;
	vector<BYTE> buf(size);
	PIP_ADAPTER_INFO info = (PIP_ADAPTER_INFO)buf.data();
	if (myGetAdaptersInfo(info, &size) != ERROR_SUCCESS) return;
	for (PIP_ADAPTER_INFO p = info; p; p = p->Next) {
		if (p->Type == MIB_IF_TYPE_LOOPBACK) continue;
		for (IP_ADDR_STRING * ipAddr = &p->IpAddressList; ipAddr; ipAddr = ipAddr->Next) {
			string ipStr = ipAddr->IpAddress.String;
			if (ipStr.empty() || ipStr == "0.0.0.0") continue;
			unsigned long ip = myinet_addr(ipStr.c_str());
			unsigned long mask = myinet_addr(ipAddr->IpMask.String);
			struct in_addr bAddr;
			bAddr.S_un.S_addr = ip | ~mask;
			string bStr = myinet_ntoa(bAddr);
			bool exists = false;
			for (auto& s : g_broadcastIPs) if (s == bStr) exists = true;
			if (!exists) g_broadcastIPs.push_back(bStr);
		}
	}
}

void failTx(const string& tid, const string& err) {
	EnterCriticalSection(&g_cs);
	auto it = g_fileTx.find(tid);
	string name, target;
	if (it != g_fileTx.end()) {
		it->second.state = FileTxState::Failed;
		it->second.error = err;
		it->second.finishedAt = GetTickCount();
		name = it->second.name;
		target = it->second.target;
	}
	LeaveCriticalSection(&g_cs);
	if (!name.empty())
		sysMsg("[文件] 发送失败 " + name + " -> " + target + "：" + err, 12);
}

void runFileTransfer(const string& tid) {
	FileTx info;
	{
		EnterCriticalSection(&g_cs);
		auto it = g_fileTx.find(tid);
		if (it == g_fileTx.end()) {
			LeaveCriticalSection(&g_cs);
			return;
		}
		it->second.state = FileTxState::Sending;
		info = it->second;
		LeaveCriticalSection(&g_cs);
	}

	string targetIP;
	EnterCriticalSection(&g_cs);
	auto uit = g_onlineUsers.find(info.target);
	if (uit != g_onlineUsers.end()) targetIP = uit->second.second;
	LeaveCriticalSection(&g_cs);
	if (targetIP.empty()) {
		failTx(tid, "目标离线");
		return;
	}

	struct SrcFile {
		HANDLE hFile = INVALID_HANDLE_VALUE;
		HANDLE hMap = NULL;
		const char* base = nullptr;
		FILE* fp = nullptr;
		~SrcFile() {
			if (base) UnmapViewOfFile(base);
			if (hMap) CloseHandle(hMap);
			if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
			if (fp) fclose(fp);
		}
	} src;

	src.hFile = CreateFileA(info.path.c_str(), GENERIC_READ,
	                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (src.hFile != INVALID_HANDLE_VALUE) {
		src.hMap = CreateFileMappingA(src.hFile, NULL, PAGE_READONLY, 0, 0, NULL);
		if (src.hMap) {
			src.base = (const char*)MapViewOfFile(src.hMap, FILE_MAP_READ, 0, 0, 0);
			if (!src.base) {
				CloseHandle(src.hMap);
				src.hMap = NULL;
			}
		}
		if (!src.base) {
			CloseHandle(src.hFile);
			src.hFile = INVALID_HANDLE_VALUE;
		}
	}
	if (!src.base) {
		src.fp = fopen(info.path.c_str(), "rb");
		if (!src.fp) {
			failTx(tid, "无法打开文件");
			return;
		}
		setvbuf(src.fp, NULL, _IOFBF, 4 * 1024 * 1024);
	}

	sockaddr_in txDest{};
	txDest.sin_family = AF_INET;
	txDest.sin_port = custom_htons(PORT);
	txDest.sin_addr.s_addr = myinet_addr(targetIP.c_str());

	string meta = base64Encode((const unsigned char*)info.name.c_str(), info.name.size())
	              + "\x02" + to_string(info.size) + "\x02" + to_string(info.totalChunks);
	string metaPkt = buildPacket(5, g_myNick, info.target, tid, 0, meta);

	string chunkHead = "6\x01" + escapeField(g_myNick) + "\x01" +
	                   escapeField(info.target) + "\x01";

	vector<int> nextSend;
	vector<char> buf(FILE_CHUNK_RAW);

	int maxRounds = max(64, (info.totalChunks + 99) / 100 * 2 + 16);
	for (int round = 0; round < maxRounds && g_running; round++) {
		EnterCriticalSection(&g_cs);
		auto resetIt = g_fileTx.find(tid);
		if (resetIt != g_fileTx.end()) {
			resetIt->second.nakPending = false;
			resetIt->second.nakRound++;
			resetIt->second.nakList.clear();
		}
		LeaveCriticalSection(&g_cs);

		sendToAddr(metaPkt, txDest);
		sendToAddr(metaPkt, txDest);

		vector<int> toSend;
		if (nextSend.empty() && round == 0) {
			for (int i = 0; i < info.totalChunks; i++) toSend.push_back(i);
		} else if (!nextSend.empty()) {
			toSend = nextSend;
		} else {
			break;
		}

		long long lastReadEnd = -1;
		int reportPct = 25;
		for (size_t k = 0; k < toSend.size() && g_running; k++) {
			int idx = toSend[k];
			long long pos = (long long)idx * FILE_CHUNK_RAW;

			const char* srcPtr = nullptr;
			int n = 0;
			if (src.base) {
				long long remain = info.size - pos;
				if (remain <= 0) break;
				n = (int)min((long long)FILE_CHUNK_RAW, remain);
				srcPtr = src.base + pos;
			} else {
				if (pos != lastReadEnd) {
					if (_fseeki64(src.fp, pos, SEEK_SET) != 0) break;
				}
				n = (int)fread(buf.data(), 1, FILE_CHUNK_RAW, src.fp);
				if (n <= 0) break;
				lastReadEnd = pos + n;
				srcPtr = buf.data();
			}

			string chunkB64 = base64Encode((const unsigned char*)srcPtr, (size_t)n);
			string cid = tid + "_" + to_string(idx);
			string pkt;
			pkt.reserve(chunkHead.size() + cid.size() + chunkB64.size() + 4);
			pkt += chunkHead;
			pkt += cid;
			pkt += '\x01';
			pkt += '0';
			pkt += '\x01';
			pkt += chunkB64;
			sendToAddr(pkt, txDest);

			if ((k + 1) % FILE_SEND_BATCH == 0 && g_running) {
				SwitchToThread();
			}

			if (round == 0 && toSend.size() > 4) {
				int pct = (int)((long long)k * 100 / toSend.size());
				if (pct >= reportPct) {
					sysMsg("[文件] " + info.name + " 发送 " + to_string(pct) + "%", 8);
					reportPct += 25;
				}
			}
		}

		DWORD nakRound = 0;
		EnterCriticalSection(&g_cs);
		auto roundIt = g_fileTx.find(tid);
		if (roundIt != g_fileTx.end()) nakRound = roundIt->second.nakRound;
		LeaveCriticalSection(&g_cs);
		string endPkt = buildPacket(7, g_myNick, info.target, tid, 0, to_string(nakRound));
		sendToAddr(endPkt, txDest);

		DWORD waitStart = GetTickCount();
		DWORD lastEndSend = waitStart;
		DWORD lastMetaSend = waitStart;
		bool gotNak = false;
		vector<int> nakList;
		while (GetTickCount() - waitStart < FILE_RX_TIMEOUT && g_running) {
			DWORD now = GetTickCount();
			if (now - lastMetaSend >= 2000) {
				sendToAddr(metaPkt, txDest);
				lastMetaSend = now;
			}
			if (now - lastEndSend >= 250) {
				sendToAddr(endPkt, txDest);
				lastEndSend = now;
			}
			EnterCriticalSection(&g_cs);
			auto it = g_fileTx.find(tid);
			if (it != g_fileTx.end() && it->second.nakPending) {
				gotNak = true;
				nakList = it->second.nakList;
				it->second.nakPending = false;
				it->second.nakList.clear();
			}
			LeaveCriticalSection(&g_cs);
			if (gotNak) break;
			Sleep(5);
		}

		if (!gotNak) {
			nextSend.clear();
			continue;
		}
		if (nakList.empty()) {
			EnterCriticalSection(&g_cs);
			auto it = g_fileTx.find(tid);
			if (it != g_fileTx.end()) {
				it->second.state = FileTxState::Done;
				it->second.finishedAt = GetTickCount();
			}
			LeaveCriticalSection(&g_cs);
			sysMsg("[文件] 已发送给 " + info.target + "：" + info.name, 10);
			if (g_notifyFile)
				popNotify("文件发送完成", info.name + "\n已发送给: " + info.target);
			return;
		}
		nextSend = nakList;
	}

	failTx(tid, "重传次数过多 / 对方可能防火墙拦截或 IP 不可达");
}

static bool parseNakBody(const string& body, int totalChunks, vector<int>& out) {
	size_t pos = 0;
	while (pos < body.size()) {
		size_t comma = body.find(',', pos);
		if (comma == string::npos) comma = body.size();
		string tok = body.substr(pos, comma - pos);
		if (!tok.empty()) {
			size_t dash = tok.find('-');
			string a = tok.substr(0, dash);
			string b = (dash == string::npos) ? a : tok.substr(dash + 1);
			char* e1 = nullptr;
			char* e2 = nullptr;
			long long start = strtoll(a.c_str(), &e1, 10);
			long long end = strtoll(b.c_str(), &e2, 10);
			if (e1 == a.c_str() || *e1 != '\0' || e2 == b.c_str() || *e2 != '\0')
				return false;
			if (start < 0 || end < start || start >= totalChunks) return false;
			if (end >= totalChunks) end = totalChunks - 1;
			if (end - start + 1 > FILE_NAK_MAX_RANGE)
				end = start + FILE_NAK_MAX_RANGE - 1;
			for (long long i = start; i <= end; i++) {
				out.push_back((int)i);
				if ((int)out.size() >= FILE_NAK_MAX_CHUNKS) return true;
			}
		}
		pos = comma + 1;
	}
	return true;
}

void handleFilePacket(int type, const string& sender, const string& receiver,
                      const string& msgID, const string& content,
                      const string& senderIP) {
	if (receiver != g_myNick) return;
	if (!wlAllow(sender)) {
		if (type == 5) notifyRejectToSender(sender, senderIP, "file");
		return;
	}

	if (type == 5) {
		size_t p1 = content.find('\x02');
		if (p1 == string::npos) return;
		size_t p2 = content.find('\x02', p1 + 1);
		if (p2 == string::npos) return;
		string name = base64Decode(content.substr(0, p1));
		long long fsize = atoll(content.substr(p1 + 1, p2 - p1 - 1).c_str());
		int totalChunks = atoi(content.substr(p2 + 1).c_str());

		if (fsize <= 0 || fsize > FILE_MAX_SIZE) return;
		if (totalChunks <= 0 || totalChunks > FILE_MAX_CHUNKS) return;
		long long maxBytes = (long long)totalChunks * FILE_CHUNK_RAW;
		if (fsize > maxBytes || fsize <= maxBytes - FILE_CHUNK_RAW) return;

		EnterCriticalSection(&g_cs);
		if (g_fileRx.count(msgID) || g_fileRxDone.count(msgID)) {
			LeaveCriticalSection(&g_cs);
			return;
		}
		FileRx rx;
		rx.sender = sender;
		rx.tid = msgID;
		rx.name = sanitizeFilename(name);
		rx.size = fsize;
		rx.totalChunks = totalChunks;
		rx.bitmap.assign(totalChunks, 0);
		rx.lastActivity = GetTickCount();
		CreateDirectoryA(ReceivePath.data(), NULL);
		rx.path = ReceivePath + rx.name + "." + msgID + ".part";

		HANDLE hf = CreateFileA(rx.path.c_str(), GENERIC_READ | GENERIC_WRITE,
		                        FILE_SHARE_READ, NULL, CREATE_ALWAYS,
		                        FILE_ATTRIBUTE_NORMAL, NULL);
		if (hf != INVALID_HANDLE_VALUE) {
			LARGE_INTEGER li;
			li.QuadPart = fsize;
			if (SetFilePointerEx(hf, li, NULL, FILE_BEGIN) && SetEndOfFile(hf)) {
				HANDLE hm = CreateFileMappingA(hf, NULL, PAGE_READWRITE, 0, 0, NULL);
				if (hm) {
					void* base = MapViewOfFile(hm, FILE_MAP_WRITE, 0, 0, 0);
					if (base) {
						rx.hFile = hf;
						rx.hMap = hm;
						rx.mapBase = base;
					} else {
						CloseHandle(hm);
					}
				}
			}
			if (!rx.mapBase) CloseHandle(hf);
		}

		if (!rx.mapBase) {
			rx.fp = fopen(rx.path.c_str(), "wb");
			if (!rx.fp) {
				LeaveCriticalSection(&g_cs);
				return;
			}
			setvbuf(rx.fp, NULL, _IONBF, 0);
			if (_fseeki64(rx.fp, fsize - 1, SEEK_SET) == 0) {
				fputc(0, rx.fp);
			}
		}
		g_fileRx[msgID] = rx;
		LeaveCriticalSection(&g_cs);

		sysMsg("[文件] 开始接收: " + rx.name + " (" + to_string(fsize) + " B)，发送者：" + [ = ]() -> string {
			for (const auto& v : g_onlineUsers)
				if (v.second.second == senderIP) {
					return v.first;
				}
			return "未找到对应用户名";
		}(), 11);
		return;
	}

	if (type == 6) {
		size_t us = msgID.rfind('_');
		if (us == string::npos) return;
		string tid = msgID.substr(0, us);
		int idx = atoi(msgID.c_str() + us + 1);

		FILE* fp = nullptr;
		void* mapBase = nullptr;
		long long offset = 0, fileSize = 0;
		string name;
		EnterCriticalSection(&g_cs);
		auto it = g_fileRx.find(tid);
		if (it == g_fileRx.end() || (!it->second.fp && !it->second.mapBase)) {
			LeaveCriticalSection(&g_cs);
			return;
		}
		FileRx& rx = it->second;
		if (idx < 0 || idx >= rx.totalChunks) {
			LeaveCriticalSection(&g_cs);
			return;
		}
		fp = rx.fp;
		mapBase = rx.mapBase;
		offset = (long long)idx * FILE_CHUNK_RAW;
		fileSize = rx.size;
		rx.lastActivity = GetTickCount();
		name = rx.name;
		LeaveCriticalSection(&g_cs);

		string data = base64Decode(content);
		long long expected = min((long long)FILE_CHUNK_RAW, fileSize - offset);
		if (data.empty() || (long long)data.size() != expected) return;

		bool reportPct = false;
		int pct = 0;
		EnterCriticalSection(&g_cs);
		auto writeIt = g_fileRx.find(tid);
		if (writeIt == g_fileRx.end() || writeIt->second.mapBase != mapBase ||
		    writeIt->second.fp != fp || writeIt->second.bitmap[idx]) {
			LeaveCriticalSection(&g_cs);
			return;
		}
		if (mapBase) {
			memcpy((char*)mapBase + offset, data.data(), data.size());
		} else if (_fseeki64(fp, offset, SEEK_SET) != 0 ||
		           fwrite(data.data(), 1, data.size(), fp) != data.size()) {
			LeaveCriticalSection(&g_cs);
			return;
		}
		writeIt->second.lastActivity = GetTickCount();
		writeIt->second.bitmap[idx] = 1;
		writeIt->second.gotChunks++;
		int got = writeIt->second.gotChunks, total = writeIt->second.totalChunks;
		pct = total > 0 ? (int)((long long)got * 100 / total) : 100;
		int last = writeIt->second.lastPct;
		if (pct / 25 != last / 25) {
			writeIt->second.lastPct = pct;
			reportPct = true;
		}
		LeaveCriticalSection(&g_cs);
		if (reportPct) sysMsg("[文件] " + name + " 接收 " + to_string(pct) + "%", 8);
		return;
	}

	if (type == 7) {
		string tid = msgID;
		DWORD nakRound = (DWORD)strtoul(content.c_str(), nullptr, 10);
		string completedIP;
		bool alreadyDone = false;
		EnterCriticalSection(&g_cs);
		auto doneIt = g_fileRxDone.find(tid);
		if (doneIt != g_fileRxDone.end() && doneIt->second.sender == sender) {
			doneIt->second.finishedAt = GetTickCount();
			completedIP = doneIt->second.senderIP;
			alreadyDone = true;
		}
		LeaveCriticalSection(&g_cs);
		if (alreadyDone) {
			string ack = buildPacket(8, g_myNick, sender, tid, 0,
			                         "0/1/" + to_string(nakRound) + ";");
			sendRaw(ack, { completedIP });
			return;
		}
		vector<int> missing;
		string name, finalPath;
		bool allDone = false;
		EnterCriticalSection(&g_cs);
		auto it = g_fileRx.find(tid);
		if (it == g_fileRx.end()) {
			LeaveCriticalSection(&g_cs);
			return;
		}
		FileRx& rx = it->second;
		rx.lastActivity = GetTickCount();
		for (int i = 0; i < rx.totalChunks; i++) if (!rx.bitmap[i]) missing.push_back(i);
		allDone = missing.empty();
		name = rx.name;
		if (allDone) {
			releaseRxStorage(rx);
			string partPath = rx.path;
			finalPath = ReceivePath + rx.name;
			if (GetFileAttributesA(finalPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
				string stem = rx.name, ext;
				size_t d = stem.rfind('.');
				if (d != string::npos && d > 0) {
					ext = stem.substr(d);
					stem = stem.substr(0, d);
				}
				for (int k = 1; k < 1000; k++) {
					string cand = ReceivePath + stem + "(" + to_string(k) + ")" + ext;
					if (GetFileAttributesA(cand.c_str()) == INVALID_FILE_ATTRIBUTES) {
						finalPath = cand;
						break;
					}
				}
			}
			if (rename(partPath.c_str(), finalPath.c_str()) != 0) {
				remove(finalPath.c_str());
				if (rename(partPath.c_str(), finalPath.c_str()) != 0) finalPath = partPath;
			}
			FileRxDone done;
			done.sender = sender;
			done.senderIP = senderIP;
			done.name = name;
			done.finalPath = finalPath;
			done.nakRound = nakRound;
			done.finishedAt = GetTickCount();
			g_fileRxDone[tid] = done;
			g_fileRx.erase(it);
		}
		LeaveCriticalSection(&g_cs);

		string nakContent = "0/1/" + to_string(nakRound) + ";";
		if (!allDone) {
			size_t i = 0;
			int items = 0;
			while (i < missing.size() && items < FILE_NAK_BATCH &&
			       nakContent.size() < FILE_NAK_MAX_BYTES) {
				int start = missing[i];
				int end = start;
				size_t j = i;
				while (j + 1 < missing.size() && missing[j + 1] == end + 1 &&
				       (end - start + 1) < FILE_NAK_MAX_RANGE) {
					++j;
					++end;
				}
				string token = (end > start)
				               ? to_string(start) + "-" + to_string(end)
				               : to_string(start);
				if (nakContent.size() + token.size() + 2 > FILE_NAK_MAX_BYTES) break;
				if (items) nakContent += ",";
				nakContent += token;
				++items;
				i = j + 1;
			}
		}
		string nakPkt = buildPacket(8, g_myNick, sender, tid, 0, nakContent);
		sendRaw(nakPkt, { senderIP });

		if (allDone) {
			sysMsg("[文件] 接收完成: " + name + " -> " + finalPath, 10);
			if (g_notifyFile)
				popNotify("文件接收完成", name + "\n保存至: " + finalPath);
		}
		return;
	}

	if (type == 8) {
		string tid = msgID;
		int totalChunks = 0;
		EnterCriticalSection(&g_cs);
		auto tit = g_fileTx.find(tid);
		if (tit != g_fileTx.end()) totalChunks = tit->second.totalChunks;
		LeaveCriticalSection(&g_cs);
		if (totalChunks <= 0) return;

		vector<int> missing;
		size_t sep = content.find(';');
		if (sep == string::npos) {
			if (!parseNakBody(content, totalChunks, missing)) return;
			EnterCriticalSection(&g_cs);
			auto legacyIt = g_fileTx.find(tid);
			if (legacyIt != g_fileTx.end()) {
				legacyIt->second.nakList = missing;
				legacyIt->second.nakPending = true;
			}
			LeaveCriticalSection(&g_cs);
			return;
		}
		size_t slash = content.find('/');
		size_t slash2 = content.find('/', slash == string::npos ? 0 : slash + 1);
		if (slash == string::npos || slash2 == string::npos || slash2 > sep) return;
		int batchIndex = atoi(content.substr(0, slash).c_str());
		int batchCount = atoi(content.substr(slash + 1, slash2 - slash - 1).c_str());
		DWORD nakRound = (DWORD)strtoul(content.substr(slash2 + 1, sep - slash2 - 1).c_str(), nullptr, 10);
		if (batchCount != 1 || batchIndex != 0) return;
		if (sep + 1 < content.size()) {
			if (!parseNakBody(content.substr(sep + 1), totalChunks, missing)) return;
		}
		EnterCriticalSection(&g_cs);
		auto it = g_fileTx.find(tid);
		if (it != g_fileTx.end() && nakRound == it->second.nakRound) {
			it->second.nakList = missing;
			it->second.nakPending = true;
		}
		LeaveCriticalSection(&g_cs);
		return;
	}
}

DWORD WINAPI fileSendThread(LPVOID) {
	while (g_running) {
		string tid;
		EnterCriticalSection(&g_cs);
		for (auto& kv : g_fileTx) {
			if (kv.second.state == FileTxState::Pending) {
				tid = kv.first;
				break;
			}
		}
		LeaveCriticalSection(&g_cs);
		if (tid.empty()) {
			Sleep(50);
			continue;
		}
		runFileTransfer(tid);
	}
	return 0;
}

DWORD WINAPI networkThread(LPVOID) {
	char buf[8192];
	DWORD lastHeartbeat = 0;

	while (g_running) {
		DWORD now = GetTickCount();

		if (now - lastHeartbeat > 100) {
			string hbID = "hb_" + to_string(now) + "_" + to_string(rand() % 100000);
			string packet = buildPacket(4, g_myNick, "所有人", hbID, g_myColor, "hb");
			markSent(hbID);
			sendBroadcast(packet);
			lastHeartbeat = now;
		}

		sockaddr_in from{};
		int fromLen = sizeof(from);
		int n = myrecvfrom(g_sock, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
		if (n <= 0) continue;

		string raw(buf, n);
		int type, color;
		string sender, receiver, msgID, content;
		if (!parsePacket(raw, type, sender, receiver, msgID, color, content)) continue;

		if (isSentEcho(msgID)) continue;

		string senderIP = myinet_ntoa(from.sin_addr);

		if (type == 2) {
			if (content == g_myNick) {
				string reply = buildPacket(3, g_myNick, sender, msgID, g_myColor, "");
				sendRaw(reply, { senderIP });
			}
			continue;
		}

		if (sender == g_myNick) {
			EnterCriticalSection(&g_cs);
			if (now - g_lastConflictTime > 5000) {
				g_lastConflictTime = now;
				g_chatLog.push_back({ MessageType::System, "",
				                      "[系统] 昵称冲突：" + senderIP + " 也使用了 \"" + g_myNick + "\"，请重启更换昵称", 12 });
				clampLogLocked();
				g_msgDirty = true;
			}
			LeaveCriticalSection(&g_cs);
			continue;
		}

		if (type == 9) {
			if (receiver == g_myNick) {
				if (!g_receivedMsgIDs.count(msgID)) {
					markReceived(msgID);
					onRejectedNotice(sender, content);
				}
			}
			continue;
		}

		bool track = (type == 0 || type == 1 || type == 4 || (type >= 5 && type <= 8));
		EnterCriticalSection(&g_cs);
		bool isNew = track && g_onlineUsers.find(sender) == g_onlineUsers.end();
		if (track) {
			g_onlineUsers[sender] = { GetTickCount(), senderIP };
			g_userColors[sender] = color;
		}
		if (isNew && wlAllow(sender)) {
			g_chatLog.push_back({ MessageType::System, "", "[系统] " + sender + " 上线了", 8 });
			clampLogLocked();
			g_msgDirty = true;
		}
		LeaveCriticalSection(&g_cs);

		if (type >= 5 && type <= 8) {
			handleFilePacket(type, sender, receiver, msgID, content, senderIP);
			continue;
		}

		if (type == 0 || type == 1) {
			if (g_receivedMsgIDs.count(msgID)) continue;
			markReceived(msgID);

			if (type == 0) {
				if (!wlAllow(sender)) {
					notifyRejectToSender(sender, senderIP, "msg");
					continue;
				}
				pushMessage({ MessageType::Others, sender, content, color });
			} else if (receiver == g_myNick) {
				if (!wlAllow(sender)) {
					notifyRejectToSender(sender, senderIP, "msg");
					continue;
				}
				pushMessage({ MessageType::OtherPrivate, sender, content, color });
				if (g_notifyPrivate)
					popNotify("新私聊消息", "来自: " + sender + "\n内容: " + content);
			}
		}
	}
	return 0;
}

vector<string> multiSelect(const vector<string>& items) {
	vector<string> result;
	if (items.empty()) return result;
	vector<bool> picked(items.size(), false);
	while (true) {
		vector<string> opts;
		for (size_t i = 0; i < items.size(); ++i)
			opts.push_back(string(picked[i] ? "[*] " : "[ ] ") + items[i]);
		opts.push_back("全选 / 全不选");
		opts.push_back("确定");
		int choice = showInteractiveMenu(opts);
		if (choice >= 0 && choice < (int)items.size()) {
			picked[choice] = !picked[choice];
		} else if (choice == (int)items.size()) {
			bool all = true;
			for (size_t i = 0; i < items.size(); ++i) if (!picked[i]) all = false;
			for (size_t i = 0; i < items.size(); ++i) picked[i] = !all;
		} else if (choice < 0) {
			return vector<string>();
		} else {
			break;
		}
	}
	for (size_t i = 0; i < items.size(); ++i) if (picked[i]) result.push_back(items[i]);
	return result;
}

vector<string> GetManyUser() {
	vector<string> names = onlineOthers();
	if (names.empty()) {
		sysMsg("[系统] 当前没有其他人在线！", 12);
		return names;
	}
	return multiSelect(names);
}

void showWhitelist() {
	vector<string> names = friendNames(false);
	vector<string> items;
	vector<int> colors;
	EnterCriticalSection(&g_cs);
	for (const auto& n : names) {
		bool online = g_onlineUsers.count(n) > 0;
		items.push_back((online ? "● " : "○ ") + n + (online ? "    [在线]" : "    [离线]"));
		colors.push_back(online ? 10 : 8);
	}
	LeaveCriticalSection(&g_cs);

	string footer;
	if (items.empty())
		footer = "好友列表为空，按 回车/ESC 返回后用 '/whitelist add' 添加好友";
	else
		footer = "共 " + to_string(items.size()) + " 位好友（● 在线　○ 离线），按 回车/ESC 返回";
	showListScreen("好 友 列 表", footer, items, colors);
	g_msgDirty = true;
}

void showWlResult(const string& title, const string& footer,
                  const vector<string>& items, const vector<int>& colors) {
	showListScreen(title, footer, items, colors);
	g_msgDirty = true;
}

void addFriendsByMenu() {
	vector<string> online = onlineOthers();
	vector<string> names;
	for (const auto& n : online)
		if (!isFriend(n)) names.push_back(n);

	if (names.empty()) {
		string tip = online.empty()
		             ? "[!] 当前没有其他用户在线"
		             : "[=] 在线的 " + to_string(online.size()) + " 位用户都已在好友名单中";
		showWlResult("添 加 好 友", "按 回车/ESC 返回", vector<string> { tip }, vector<int> { 8 });
		return;
	}

	vector<string> users = multiSelect(names);
	if (users.empty()) return;

	vector<string> added, existed;
	for (const auto& v : users) {
		bool ok;
		EnterCriticalSection(&g_cs);
		ok = Whitelist.insert(v).second;
		LeaveCriticalSection(&g_cs);
		if (ok) added.push_back(v);
		else existed.push_back(v);
	}
	if (!added.empty()) saveWhitelist();
	g_msgDirty = true;

	vector<string> items;
	vector<int> colors;
	for (const auto& n : added) {
		items.push_back("[+] 已添加：" + n);
		colors.push_back(10);
	}
	for (const auto& n : existed) {
		items.push_back("[=] 已在名单：" + n);
		colors.push_back(8);
	}
	showWlResult("添 加 好 友 结 果",
	             "新添加 " + to_string(added.size()) + " 位，原本已在名单 " +
	             to_string(existed.size()) + " 位，按 回车/ESC 返回",
	             items, colors);
}

void removeFriendsByMenu() {
	vector<string> names = friendNames(false);
	if (names.empty()) {
		showWlResult("删 除 好 友 结 果", "好友列表为空，按 回车/ESC 返回",
		             vector<string>(), vector<int>());
		return;
	}
	vector<string> users = multiSelect(names);
	if (users.empty()) return;

	vector<string> removed;
	for (const auto& v : users) {
		bool ok;
		EnterCriticalSection(&g_cs);
		ok = Whitelist.erase(v) > 0;
		LeaveCriticalSection(&g_cs);
		if (ok) removed.push_back(v);
	}
	if (!removed.empty()) saveWhitelist();
	g_msgDirty = true;

	vector<string> items;
	vector<int> colors;
	for (const auto& n : removed) {
		items.push_back("[-] 已删除：" + n);
		colors.push_back(12);
	}
	showWlResult("删 除 好 友 结 果",
	             "已删除 " + to_string(removed.size()) + " 位好友，按 回车/ESC 返回",
	             items, colors);
}

void whitelistMenu() {
	while (true) {
		bool on = wlEnabled();
		size_t cnt;
		EnterCriticalSection(&g_cs);
		cnt = Whitelist.size();
		LeaveCriticalSection(&g_cs);
		vector<string> opts;
		opts.push_back(string("白名单：") + (on ? "已开启（仅好友可聊天与传文件）" : "已关闭（所有人可聊天）") + "　（选择切换）");
		{
			size_t avail = 0;
			vector<string> online = onlineOthers();
			for (const auto& n : online)
				if (!isFriend(n)) ++avail;
			opts.push_back("添加好友（在线可添加 " + to_string(avail) + " 位，已在名单的不再显示）");
		}
		opts.push_back("删除好友（从好友名单中选择）");
		opts.push_back("查看好友列表（共 " + to_string(cnt) + " 位）");
		opts.push_back("返回聊天");
		int choice = showInteractiveMenu(opts);
		if (choice == 0) {
			EnterCriticalSection(&g_cs);
			Whitelist_state = !Whitelist_state;
			LeaveCriticalSection(&g_cs);
			saveWhitelist();
		} else if (choice == 1) {
			addFriendsByMenu();
		} else if (choice == 2) {
			removeFriendsByMenu();
		} else if (choice == 3) {
			showWhitelist();
		} else {
			break;
		}
	}
	g_msgDirty = true;
}

void handleEnter() {
	if (g_scrollOffset != 0) {
		g_scrollOffset = 0;
		g_msgDirty = true;
	}
	string input = g_input;
	g_input.clear();
	clearSuggest();
	drawInputLine();

	if (input.empty() || isAllSpaces(input)) return;

	if (input == "/quit" || input == "/exit") {
		g_running = false;
		return;
	}

	if (input == "/all") {
		EnterCriticalSection(&g_cs);
		g_currentChatTarget = "所有人";
		g_chatLog.push_back({ MessageType::System, "", "[系统] 已回到群聊大厅", 8 });
		clampLogLocked();
		g_msgDirty = true;
		LeaveCriticalSection(&g_cs);
		return;
	}

	if (input == "/help") {
		vector<string> help = {
			"",
			"[系统] /all：切换回群聊",
			"[系统] /quit(或 /exit)：退出程序",
			"[系统] /notify：切换 Windows 通知状态（开启/关闭）",
			"[系统] /send：文件传输，可键入 '-help' 参数获得详细用法",
			"[系统] /chat：切换到与目标用户的私聊，可键入 '-help' 参数获得详细用法",
			"[系统] /whitelist：好友白名单（仅好友可私聊与传文件），可键入 '-help' 参数获得详细用法",
			"[系统] /color：切换名字颜色，可键入 '-help' 参数获得详细用法",
			"[系统] /clear：清屏",
			""
		};

		for (const auto& v : help) sysMsg(v, 8);
		return;
	}

	if (input == "/notify") {
		while (true) {
			vector<string> opts;
			opts.push_back(string("私聊通知：") + (g_notifyPrivate ? "开" : "关") + "  （切换）");
			opts.push_back(string("文件通知：") + (g_notifyFile ? "开" : "关") + "  （切换）");
			opts.push_back("返回聊天");
			int choice = showInteractiveMenu(opts);
			if (choice == 0) g_notifyPrivate = !g_notifyPrivate;
			else if (choice == 1) g_notifyFile = !g_notifyFile;
			else break;
		}
		g_msgDirty = true;
		return;
	}

	if (input.rfind("/chat", 0) == 0) {
		if (input == "/chat") {
			bool wlOn = wlEnabled();
			vector<string> users;
			EnterCriticalSection(&g_cs);
			for (auto& kv : g_onlineUsers) {
				if (kv.first == g_myNick) continue;
				if (wlOn && !Whitelist.count(kv.first)) continue;
				users.push_back(kv.first);
			}
			LeaveCriticalSection(&g_cs);

			if (users.empty()) {
				sysMsg(wlOn ? "[系统] 白名单已开启：当前没有在线好友，请先用 /whitelist 添加好友"
				       : "[系统] 当前没有其他人在线！", 12);
			} else {
				users.push_back("取消");
				int uChoice = showInteractiveMenu(users);
				if (uChoice >= 0 && uChoice != (int)users.size() - 1) {
					EnterCriticalSection(&g_cs);
					g_currentChatTarget = users[uChoice];
					g_chatLog.push_back({ MessageType::System, "",
					                      "[系统] 已切换至与 " + g_currentChatTarget + " 的私聊", 8 });
					clampLogLocked();
					LeaveCriticalSection(&g_cs);
				}
				g_msgDirty = true;
			}
			return;
		}

		string name = input.substr(6);
		while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
		while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();

		if (name == "-help") {
			sysMsg("[系统] 用法：\'/chat <用户名>\'，若只键入 \'/chat\'，则进入用户选择界面", 12);
			sysMsg("[白名单] 白名单开启时只能与好友私聊，可用 /whitelist 管理好友", 12);
			return;
		}

		if (!name.empty() && wlEnabled() && !isFriend(name)) {
			sysMsg("[系统] 该用户未与你添加好友：" + name, 12);
			return;
		}

		vector<string> users;
		EnterCriticalSection(&g_cs);
		for (auto& kv : g_onlineUsers) if (kv.first != g_myNick) users.push_back(kv.first);
		LeaveCriticalSection(&g_cs);

		int find = -1;
		for (size_t i = 0; i < users.size(); ++i) {
			if (name == users[i]) {
				find = i;
				break;
			}
		}

		if (find == -1) {
			sysMsg("[系统] 目标用户当前不在线！", 12);
		} else {
			EnterCriticalSection(&g_cs);
			g_currentChatTarget = users[find];
			g_chatLog.push_back({ MessageType::System, "",
			                      "[系统] 已切换至与 " + g_currentChatTarget + " 的私聊", 8 });
			clampLogLocked();
			g_msgDirty = true;
			LeaveCriticalSection(&g_cs);
		}

		return;
	}

	if (input == "/whitelist" || input.rfind("/whitelist ", 0) == 0) {
		string Order = (input.size() > 11) ? input.substr(11) : "";
		while (!Order.empty() && (Order.front() == ' ' || Order.front() == '\t')) Order.erase(0, 1);
		while (!Order.empty() && (Order.back() == ' ' || Order.back() == '\t')) Order.pop_back();

		if (Order.empty()) {
			whitelistMenu();
			return;
		}

		if (Order == "-help") {
			sysMsg("[白名单] 开关：'on' 开启 / 'off' 关闭");
			sysMsg("[白名单] 好友：'add <用户名>' 添加 / 'remove <用户名>' 删除");
			sysMsg("[白名单] 列表：'list' 查看好友 / 'clear' 清空好友");
			sysMsg("[白名单] 直接键入 '/whitelist' 可进入图形化管理界面");
			sysMsg("[白名单] 开启后只能与好友私聊、传输文件，群聊消息也只发给好友；");
			sysMsg("[白名单] 与非好友互动时会提示“该用户未与你添加好友”并忽略其消息");
			sysMsg("[白名单] 好友名单保存在程序目录 YGChat_whitelist.txt");
			return;
		}

		if (Order == "on" || Order == "off") {
			bool want = (Order == "on");
			size_t cnt;
			EnterCriticalSection(&g_cs);
			Whitelist_state = want;
			cnt = Whitelist.size();
			LeaveCriticalSection(&g_cs);
			saveWhitelist();
			if (want)
				sysMsg("[白名单]：已开启白名单（好友 " + to_string(cnt) + " 位，仅好友可聊天与传文件）", 14);
			else
				sysMsg("[白名单]：已关闭白名单（所有人可聊天与传文件）", 10);
			g_msgDirty = true;
			return;
		}

		if (Order == "list") {
			showWhitelist();
			return;
		}

		if (Order == "clear") {
			size_t cnt;
			EnterCriticalSection(&g_cs);
			cnt = Whitelist.size();
			Whitelist.clear();
			LeaveCriticalSection(&g_cs);
			saveWhitelist();
			g_msgDirty = true;
			showWlResult("清 空 好 友 结 果",
			             "按 回车/ESC 返回",
			             vector<string> { "[!] 已清空全部好友（共 " + to_string(cnt) + " 位）" },
			             vector<int> { 12 });
			return;
		}

		if (Order == "add" || Order.rfind("add ", 0) == 0) {
			if (Order == "add") {
				addFriendsByMenu();
				return;
			}
			string name = Order.substr(4);
			while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
			if (name.empty()) {
				sysMsg("[白名单]：请输入要添加的用户名，或直接键入 '/whitelist add' 从在线用户中选择", 12);
				return;
			}
			if (!isValidName(name)) {
				sysMsg("[白名单]：用户名不合法", 12);
				return;
			}
			bool already = false, online = false;
			EnterCriticalSection(&g_cs);
			already = Whitelist.count(name) > 0;
			if (!already) Whitelist.insert(name);
			online = g_onlineUsers.count(name) > 0;
			LeaveCriticalSection(&g_cs);
			if (already) {
				showWlResult("添 加 好 友 结 果", "按 回车/ESC 返回",
				             vector<string> { "[=] 已在名单：" + name }, vector<int> { 8 });
			} else {
				saveWhitelist();
				g_msgDirty = true;
				showWlResult("添 加 好 友 结 果", "按 回车/ESC 返回",
				             vector<string> { "[+] 已添加：" + name +
				                              (online ? "    [在线]" : "    [离线]")
				                            },
				             vector<int> { 10 });
			}
			return;
		}

		if (Order == "remove" || Order.rfind("remove ", 0) == 0) {
			if (Order == "remove") {
				removeFriendsByMenu();
				return;
			}
			string name = Order.substr(7);
			while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
			bool had = false;
			EnterCriticalSection(&g_cs);
			had = Whitelist.erase(name) > 0;
			LeaveCriticalSection(&g_cs);
			if (had) {
				saveWhitelist();
				g_msgDirty = true;
				showWlResult("删 除 好 友 结 果", "按 回车/ESC 返回",
				             vector<string> { "[-] 已删除：" + name }, vector<int> { 12 });
			} else {
				showWlResult("删 除 好 友 结 果", "按 回车/ESC 返回",
				             vector<string> { "[!] 未找到该好友：" + name }, vector<int> { 12 });
			}
			return;
		}

		sysMsg("[白名单]：不存在的指令！输入 /whitelist -help 查看用法", 12);
		return;
	}

	if (input.rfind("/send", 0) == 0) {
		if (input == "/send") {
			if (pickFileToSend()) {
				drawInputLine();
				g_msgDirty = true;
				sysMsg("[文件] 已填入 /send 命令，按回车即可发送", 10);
			} else {
				g_msgDirty = true;
			}
			return;
		}

		string path = input.substr(6);
		while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.erase(0, 1);
		while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) path.pop_back();

		if (path == "-help") {
			sysMsg("[文件] 用法: /send [-all|-opts] <文件路径>（参数写在文件路径之前）", 12);
			sysMsg("[文件] 用法：若只键入 '/send'，进入文件选择界面", 12);
			sysMsg("[文件] 参数：-all 写在文件路径之前，发送给所有在线好友", 12);
			sysMsg("[文件] 参数：-opts 写在文件路径之前，手动选择发送对象", 12);
			sysMsg("[文件] 示例：/send -all \"D:\\我的文件\\报告.pdf\"", 12);
			sysMsg("[文件] 提示：白名单开启时，文件只会发送给好友", 12);
			return;
		}

		bool All = false, Choose = false;
		auto takeFlag = [](const string & tok, bool & a, bool & o) {
			string l = tok;
			for (size_t k = 0; k < l.size(); ++k)
				l[k] = (char)tolower((unsigned char)l[k]);
			if (l == "-all") {
				a = true;
				return true;
			}
			if (l == "-opts") {
				o = true;
				return true;
			}
			return false;
		};
		for (;;) {
			size_t p1 = path.find_first_of(" \t");
			string tok = (p1 == string::npos) ? path : path.substr(0, p1);
			if (!takeFlag(tok, All, Choose)) break;
			path = (p1 == string::npos) ? string() : path.substr(p1 + 1);
			while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.erase(0, 1);
		}
		{
			size_t p3 = path.find_last_of(" \t");
			string tok = (p3 == string::npos) ? path : path.substr(p3 + 1);
			bool a2 = false, o2 = false;
			if (takeFlag(tok, a2, o2)) {
				All = false;
				Choose = false;
				sysMsg("[文件] 请把参数写在文件路径之前，例如：/send -all \"D:\\a.txt\"", 12);
				return;
			}
		}
		if (path.empty()) {
			if (pickFileToSend()) {
				drawInputLine();
				g_msgDirty = true;
				sysMsg("[文件] 已选择文件，按回车即可发送", 10);
			} else {
				sysMsg("[文件] 请补上文件路径，例如：/send -all \"D:\\a.txt\"", 12);
			}
			return;
		}

		if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
			path = path.substr(1, path.size() - 2);

		if (g_currentChatTarget == "所有人" && !Choose) All = true;

		DWORD attrs = GetFileAttributesA(path.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			sysMsg("[文件] 文件不存在: " + path, 12);
			return;
		}
		if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
			sysMsg("[文件] 不能发送目录: " + path, 12);
			return;
		}

		HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ,
		                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		                           FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile == INVALID_HANDLE_VALUE) {
			sysMsg("[文件] 无法打开: " + path, 12);
			return;
		}
		LARGE_INTEGER li;
		bool ok = GetFileSizeEx(hFile, &li) != 0;
		CloseHandle(hFile);
		if (!ok || li.QuadPart <= 0) {
			sysMsg("[文件] 无法获取文件大小", 12);
			return;
		}
		long long size = li.QuadPart;
		if (size > FILE_MAX_SIZE) {
			sysMsg("[文件] 文件过大（上限 5GB）", 12);
			return;
		}

		int totalChunks = (int)((size + FILE_CHUNK_RAW - 1) / FILE_CHUNK_RAW);
		size_t slash = path.find_last_of("\\/");
		string name = (slash == string::npos) ? path : path.substr(slash + 1);
		if (name.empty()) {
			sysMsg("[文件] 无法解析文件名", 12);
			return;
		}

		vector<string> users;
		if (All) {
			EnterCriticalSection(&g_cs);
			for (auto& kv : g_onlineUsers) if (kv.first != g_myNick) users.push_back(kv.first);
			LeaveCriticalSection(&g_cs);
		} else if (Choose) {
			users = GetManyUser();
		} else {
			users.emplace_back(g_currentChatTarget);
		}

		{
			bool wlOn = wlEnabled();
			vector<string> allowed;
			for (const auto& v : users) {
				if (wlOn && !isFriend(v)) sysMsg("[系统] 该用户未与你添加好友：" + v + "，文件未发送", 12);
				else allowed.push_back(v);
			}
			if (allowed.empty()) {
				sysMsg("[文件] 没有可发送的目标，已取消发送", 12);
				return;
			}
			users.swap(allowed);
		}

		for (const auto& v : users) {
			string tid = "f" + to_string(GetTickCount()) + "_" + to_string(rand() % 100000);
			FileTx tx;
			tx.target = v;
			tx.tid = tid;
			tx.name = name;
			tx.path = path;
			tx.size = size;
			tx.totalChunks = totalChunks;
			tx.state = FileTxState::Pending;
			EnterCriticalSection(&g_cs);
			g_fileTx[tid] = tx;
			LeaveCriticalSection(&g_cs);
			sysMsg("[文件] 已加入发送队列: " + name + " (" + to_string(size) + " B) -> " + tx.target, 11);
		}

		return;
	}

	if (input.rfind("/color", 0) == 0) {
		vector<string> colorNames = { "白色", "绿色", "黄色", "深蓝色", "紫色", "红色", "浅蓝色" };
		vector<int> colorCodes = { 7, 10, 14, 9, 13, 12, 11 };

		if (input == "/color") {
			int cChoice = showInteractiveMenu(colorNames);
			if (cChoice < 0) return;
			EnterCriticalSection(&g_cs);
			g_myColor = colorCodes[cChoice];
			g_chatLog.push_back({ MessageType::System, "", "[系统] 名字颜色已更换", 8 });
			clampLogLocked();
			g_msgDirty = true;
			LeaveCriticalSection(&g_cs);
			return;
		}

		string cColor = input.substr(7);

		if (cColor == "-help") {
			sysMsg("[系统] 用法：\'/color <颜色名>\'，若只键入 \'/color\'，则进入颜色选择界面", 12);
			return;
		}

		int find = -1;
		for (size_t i = 0; i < colorNames.size(); ++i)
			if (colorNames[i] == cColor) {
				find = i;
				break;
			}

		if (find == -1) {
			sysMsg("[系统] 暂不支持该颜色！", 8);
			return;
		} else {
			EnterCriticalSection(&g_cs);
			g_myColor = colorCodes[find];
			g_chatLog.push_back({ MessageType::System, "", "[系统] 名字颜色已更换", 8 });
			clampLogLocked();
			g_msgDirty = true;
			LeaveCriticalSection(&g_cs);
			return;
		}
	}

	if (input == "/clear") {
		EnterCriticalSection(&g_cs);
		g_chatLog.clear();
		g_scrollOffset = 0;
		g_msgDirty = true;
		LeaveCriticalSection(&g_cs);
		return;
	}

	if (g_currentChatTarget == "所有人") sendMessage(0, "所有人", input);
	else sendMessage(1, g_currentChatTarget, input);
}

int main(int arvg, char* arvc[]) {
	ReceivePath = arvc[0];
	arvg = arvg + 1 - 1;
	for (size_t i = 0; i < ReceivePath.size(); ++i) ReceivePath[i] = (ReceivePath[i] == '\\' ? '/' : ReceivePath[i]);
	size_t find = ReceivePath.find_last_of("/");
	if (find == string::npos) ReceivePath.clear();
	else ReceivePath.erase(find);
	g_dataDir = ReceivePath + "/";
	ReceivePath += "/received/";

	system("chcp 936 > nul");
	srand((unsigned)time(NULL));

	g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	CONSOLE_SCREEN_BUFFER_INFO csbi;
	if (GetConsoleScreenBufferInfo(g_hOut, &csbi)) {
		g_bufWidth = csbi.dwSize.X;
		g_width = csbi.dwSize.X;
	}
	makeBufferWindowSized();
	installMouseHook();

	g_hIn = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
	                    FILE_SHARE_READ | FILE_SHARE_WRITE,
	                    NULL, OPEN_EXISTING, 0, NULL);
	if (g_hIn == INVALID_HANDLE_VALUE) {
		cout << "无法打开控制台输入句柄！\n";
		return 1;
	}

	if (GetConsoleMode(g_hIn, &g_origInMode))
		g_hasOrigInMode = true;

	int pool[] = { 10, 13, 14, 11, 12, 9 };
	g_myColor = pool[rand() % 6];

	InitializeCriticalSection(&g_cs);
	loadWhitelist();

	HMODULE hWs2 = LoadLibraryA("ws2_32.dll");
	HMODULE hIphlp = LoadLibraryA("iphlpapi.dll");
	HMODULE hShell = LoadLibraryA("shell32.dll");
	if (!hWs2 || !hIphlp) {
		cout << "无法加载系统网络库！\n";
		return 1;
	}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"

	myWSAStartup      = (pWSAStartup)GetProcAddress(hWs2, "WSAStartup");
	myWSACleanup      = (pWSACleanup)GetProcAddress(hWs2, "WSACleanup");
	mysocket          = (psocket)GetProcAddress(hWs2, "socket");
	mysetsockopt      = (psetsockopt)GetProcAddress(hWs2, "setsockopt");
	mybind            = (pbind)GetProcAddress(hWs2, "bind");
	mysendto          = (psendto)GetProcAddress(hWs2, "sendto");
	myrecvfrom        = (precvfrom)GetProcAddress(hWs2, "recvfrom");
	myclosesocket     = (pclosesocket)GetProcAddress(hWs2, "closesocket");
	myinet_addr       = (pinet_addr)GetProcAddress(hWs2, "inet_addr");
	myinet_ntoa       = (pinet_ntoa)GetProcAddress(hWs2, "inet_ntoa");
	myGetAdaptersInfo = (pGetAdaptersInfo)GetProcAddress(hIphlp, "GetAdaptersInfo");
	if (hShell)
		myShell_NotifyIconW = (pShell_NotifyIconW)GetProcAddress(hShell, "Shell_NotifyIconW");

#pragma GCC diagnostic pop

	WSADATA wsa;
	myWSAStartup(MAKEWORD(2, 2), &wsa);

	g_sock = mysocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	int on = 1;
	mysetsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (char*)&on, sizeof(on));
	mysetsockopt(g_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));
	int rcvbuf = 16 * 1024 * 1024;
	mysetsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
	int sndbuf = 16 * 1024 * 1024;
	mysetsockopt(g_sock, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuf, sizeof(sndbuf));
	DWORD timeout = 200;
	mysetsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

	sockaddr_in local{};
	local.sin_family = AF_INET;
	local.sin_port = custom_htons(PORT);
	local.sin_addr.s_addr = INADDR_ANY;

	if (mybind(g_sock, (sockaddr * )&local, sizeof(local)) == SOCKET_ERROR) {
		cout << "绑定端口失败！\n";
		system("pause");
		return 1;
	}

	struct ip_mreq mreq;
	mreq.imr_multiaddr.s_addr = myinet_addr("224.0.0.100");
	mreq.imr_interface.s_addr = INADDR_ANY;
	mysetsockopt(g_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq));

	initBroadcastIPs();

	while (true) {
		cout << "请输入你的昵称: ";
		getline(cin, g_myNick);
		if (g_myNick.size() && g_myNick.back() == '\r') g_myNick.pop_back();
		if (!isValidName(g_myNick)) {
			setColor(12);
			cout << "[错误] 昵称不合法（非空、不超过 50 显示宽度、不含控制字符）！\n\n";
			resetColor();
			continue;
		}
		cout << "正在检查是否重名...\n";
		if (checkNameConflict(g_myNick)) {
			setColor(12);
			cout << "[错误] 昵称已被占用！\n\n";
			resetColor();
			continue;
		}
		break;
	}

	CreateThread(NULL, 0, networkThread, NULL, 0, NULL);
	CreateThread(NULL, 0, fileSendThread, NULL, 0, NULL);

	pushMessage({ MessageType::System, "", "[系统] 欢迎进入聊天室！", 8 });
	sysMsg("[提示] 鼠标滚轮或 PgUp/PgDn/↑/↓ 翻阅历史消息，End 回最新", 10);
	sysMsg("[提示] 选文件按 F9 或者复制文件地址", 10);
	{
		size_t cnt;
		bool on;
		EnterCriticalSection(&g_cs);
		cnt = Whitelist.size();
		on = Whitelist_state;
		LeaveCriticalSection(&g_cs);
		sysMsg("[白名单] 好友 " + to_string(cnt) + " 位，白名单状态：" +
		       (on ? "开（仅好友可聊天与传文件）" : "关（所有人可聊天）"), on ? 14 : 10);
		sysMsg("[提示] 输入 /whitelist 管理好友白名单（添加/删除好友、开关白名单）", 10);
	}

	while (g_running) {
		bool changed = false;
		EnterCriticalSection(&g_cs);
		DWORD now = GetTickCount();
		for (auto it = g_onlineUsers.begin(); it != g_onlineUsers.end();) {
			if (now - it->second.first > 5000) {
				it = g_onlineUsers.erase(it);
				changed = true;
			} else ++it;
		}
		for (auto it = g_fileRx.begin(); it != g_fileRx.end();) {
			if (now - it->second.lastActivity > FILE_RX_TIMEOUT) {
				string path = it->second.path;
				releaseRxStorage(it->second);
				remove(path.c_str());
				string nm = it->second.name;
				it = g_fileRx.erase(it);
				g_chatLog.push_back({ MessageType::System, "", "[文件] 接收超时: " + nm, 12 });
				clampLogLocked();
				g_msgDirty = true;
			} else ++it;
		}
		for (auto it = g_fileTx.begin(); it != g_fileTx.end();) {
			if ((it->second.state == FileTxState::Done || it->second.state == FileTxState::Failed)
			    && it->second.finishedAt && now - it->second.finishedAt > 60000) {
				it = g_fileTx.erase(it);
			} else ++it;
		}
		for (auto it = g_fileRxDone.begin(); it != g_fileRxDone.end();) {
			if (now - it->second.finishedAt > FILE_RX_TIMEOUT)
				it = g_fileRxDone.erase(it);
			else ++it;
		}
		LeaveCriticalSection(&g_cs);
		if (changed) g_msgDirty = true;

		if (g_msgDirty) {
			renderScreen();
			g_msgDirty = false;
		}

		applyPendingWheel();

		DWORD evCount = 0;
		while (GetNumberOfConsoleInputEvents(g_hIn, &evCount) && evCount > 0) {
			DWORD take = (evCount < 128) ? evCount : 128;
			INPUT_RECORD recs[128];
			DWORD got = 0;
			if (!ReadConsoleInputW(g_hIn, recs, take, &got)) break;

			bool inputChanged = false;

			for (DWORD i = 0; i < got; i++) {
				if (recs[i].EventType == MOUSE_EVENT) {
					const MOUSE_EVENT_RECORD& m = recs[i].Event.MouseEvent;
					++g_dbgMouse;
					if (m.dwEventFlags & MOUSE_WHEELED) {
						++g_dbgWheel;
						SHORT delta = (SHORT)HIWORD(m.dwButtonState);
						if (delta > 0) {
							g_scrollOffset += WHEEL_STEP;
							g_msgDirty = true;
						} else if (delta < 0) {
							g_scrollOffset -= WHEEL_STEP;
							if (g_scrollOffset < 0) g_scrollOffset = 0;
							g_msgDirty = true;
						}
					}
					continue;
				}
				if (recs[i].EventType != KEY_EVENT) continue;
				KEY_EVENT_RECORD& k = recs[i].Event.KeyEvent;
				if (!k.bKeyDown) continue;
				g_dbgKey++;

				WORD vk = k.wVirtualKeyCode;
				WCHAR wc = k.uChar.UnicodeChar;

				if (vk == VK_PROCESSKEY) continue;

				if (g_vtMouseOn && wc != 0) {
					bool seqDirty = false;
					if (consumeMouseSeqChar(wc, seqDirty)) {
						if (seqDirty) g_msgDirty = true;
						continue;
					}
				}

				if (wc == 0) {
					if (!g_sugg.empty() && (vk == VK_UP || vk == VK_DOWN)) {
						int n = (int)g_sugg.size();
						if (vk == VK_UP) {
							g_suggSel = (g_suggSel < 0) ? n - 1 : g_suggSel - 1;
						} else {
							g_suggSel = g_suggSel + 1;
							if (g_suggSel > n - 1) g_suggSel = -1;
						}
						applySuggSel();
						inputChanged = true;
						continue;
					}
					if (!g_sugg.empty() && vk == VK_ESCAPE) {
						g_input = g_suggBase;
						clearSuggest();
						g_suggLock = true;
						inputChanged = true;
						continue;
					}
					if (vk == VK_PRIOR) {
						g_scrollOffset += MSG_ROWS;
						g_msgDirty = true;
					} else if (vk == VK_NEXT) {
						g_scrollOffset -= MSG_ROWS;
						if (g_scrollOffset < 0) g_scrollOffset = 0;
						g_msgDirty = true;
					} else if (vk == VK_UP) {
						g_scrollOffset += 1;
						g_msgDirty = true;
					} else if (vk == VK_DOWN) {
						if (g_scrollOffset > 0) g_scrollOffset -= 1;
						g_msgDirty = true;
					} else if (vk == VK_END) {
						g_scrollOffset = 0;
						g_msgDirty = true;
					} else if (vk == VK_HOME) {
						g_scrollOffset = 0x3fffffff;
						g_msgDirty = true;
					} else if (vk == VK_F11) {
						DWORD m = 0;
						if (GetConsoleMode(g_hIn, &m)) {
							if (m & ENABLE_MOUSE_INPUT) {
								SetConsoleMode(g_hIn, m & ~ENABLE_MOUSE_INPUT);
								sysMsg("[提示] 已关闭控制台鼠标事件：拖放文件/选中复制优先", 10);
							} else {
								SetConsoleMode(g_hIn, m | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT);
								sysMsg("[提示] 已开启控制台鼠标事件：滚轮改由控制台派发", 10);
							}
							g_msgDirty = true;
						}
					} else if (vk == VK_F12) {
						showMouseDiag();
					} else if (vk == VK_F9) {
						if (pickFileToSend()) {
							buildSuggest();
							drawInputLine();
							inputChanged = false;
							g_msgDirty = true;
							sysMsg("[文件] 已填入 /send 命令，按回车即可发送", 10);
						} else {
							g_msgDirty = true;
						}
					}
					continue;
				}

				if (vk == VK_TAB && !g_sugg.empty()) {
					g_suggSel = (g_suggSel >= 0 && g_suggSel < (int)g_sugg.size())
					            ? g_suggSel : 0;
					applySuggSel();
					inputChanged = true;
					continue;
				}

				if (!g_sugg.empty() && wc >= L'1' && wc <= L'9') {
					int idx = (int)(wc - L'1');
					if (idx < (int)g_sugg.size()) {
						g_suggSel = idx;
						applySuggSel();
						inputChanged = true;
						continue;
					}
				}

				if (wc == L'\r' && vk == VK_RETURN) {
					handleEnter();
					inputChanged = false;
					continue;
				}

				if (wc == L'\r') {
					g_input += ' ';
					inputChanged = true;
					continue;
				}

				if (wc == L'\n') continue;

				if (wc == L'\b') {
					if (g_input.size() >= 2 &&
					    (unsigned char)g_input[g_input.size() - 2] >= 0x81)
						g_input.erase(g_input.size() - 2);
					else if (!g_input.empty())
						g_input.pop_back();
					inputChanged = true;
					continue;
				}

				if (wc >= 32 && wc != 127) {
					char gbk[8];
					int n = WideCharToMultiByte(936, 0, &wc, 1,
					                            gbk, sizeof(gbk), NULL, NULL);
					for (int j = 0; j < n; j++) {
						unsigned char c = (unsigned char)gbk[j];
						if (c >= 32 && c != 127) {
							g_input += gbk[j];
							g_dbgChar++;
							inputChanged = true;
						}
					}
				}
			}

			if (inputChanged) {
				if (g_suggLock) g_suggLock = false;
				else buildSuggest();
				drawInputLine();
			}
		}

		Sleep(1);
	}

	g_running = false;
	Sleep(100);
	setTerminalMouse(false);
	uninstallMouseHook();
	myclosesocket(g_sock);
	myWSACleanup();
	if (hShell) FreeLibrary(hShell);
	FreeLibrary(hIphlp);
	FreeLibrary(hWs2);
	if (g_hIn != INVALID_HANDLE_VALUE) {
		if (g_hasOrigInMode) SetConsoleMode(g_hIn, g_origInMode);
		CloseHandle(g_hIn);
	}
	DeleteCriticalSection(&g_cs);
	setColor(7);
	return 0;
}

