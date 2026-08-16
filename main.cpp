#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <commdlg.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <shlobj.h>
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#include <tmmintrin.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

static const UINT WM_APP_STREAM = WM_APP + 1;
static const UINT WM_APP_DONE   = WM_APP + 2;
static const UINT WM_APP_STATUS = WM_APP + 3;

struct Attachment {
    std::wstring path;
    std::wstring name;
    std::string fileId;
    bool image = false;
};
struct Message {
    std::string role;
    std::wstring text;
    std::vector<Attachment> attachments;
};
struct Session {
    std::wstring id;
    std::wstring title;
    std::vector<Message> messages;
};

static HWND gMain=0,gSidebar=0,gTranscript=0,gInput=0,gAttach=0,gSend=0,gNew=0,gModel=0,gStatus=0,gAttachmentBar=0;
static HFONT gFont=0,gFontSmall=0,gFontMono=0;
static HBRUSH gBrushBg=0,gBrushPanel=0,gBrushInput=0;
static COLORREF C_BG=RGB(33,33,33), C_PANEL=RGB(23,23,23), C_INPUT=RGB(47,47,47), C_TEXT=RGB(235,235,235), C_MUTED=RGB(170,170,170);
static Session gSession;
static std::vector<Attachment> gPending;
static volatile LONG gBusy=0,gCancel=0;
static HINTERNET gActiveRequest=0;
static CRITICAL_SECTION gReqCS;
static ULONG_PTR gGdiToken=0;
static std::wstring gDataDir,gChatsDir;
static WNDPROC gOldInputProc=0;

static std::string WideToUtf8(const std::wstring& s){
    if(s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),(int)s.size(),0,0,0,0);
    std::string r(n,'\0'); WideCharToMultiByte(CP_UTF8,0,s.c_str(),(int)s.size(),&r[0],n,0,0); return r;
}
static std::wstring Utf8ToWide(const std::string& s){
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),0,0);
    std::wstring r(n,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.c_str(),(int)s.size(),&r[0],n); return r;
}
static std::wstring GetText(HWND h){ int n=GetWindowTextLengthW(h); std::wstring s(n,L'\0'); if(n) GetWindowTextW(h,&s[0],n+1); return s; }
static void SetText(HWND h,const std::wstring&s){ SetWindowTextW(h,s.c_str()); }
static std::wstring Trim(std::wstring s){ while(!s.empty() && iswspace(s.front()))s.erase(s.begin()); while(!s.empty()&&iswspace(s.back()))s.pop_back(); return s; }
static std::wstring BaseName(const std::wstring&p){ size_t i=p.find_last_of(L"\\/"); return i==std::wstring::npos?p:p.substr(i+1); }
static std::wstring ExtLower(const std::wstring&p){ size_t i=p.find_last_of(L'.'); std::wstring e=i==std::wstring::npos?L"":p.substr(i); std::transform(e.begin(),e.end(),e.begin(),::towlower); return e; }
static bool IsImage(const std::wstring&p){ auto e=ExtLower(p); return e==L".png"||e==L".jpg"||e==L".jpeg"||e==L".webp"||e==L".gif"; }
static std::string JsonEscape(const std::string&s){ std::string o; o.reserve(s.size()+16); for(unsigned char c:s){ switch(c){case '"':o+="\\\"";break;case '\\':o+="\\\\";break;case '\b':o+="\\b";break;case '\f':o+="\\f";break;case '\n':o+="\\n";break;case '\r':o+="\\r";break;case '\t':o+="\\t";break;default: if(c<0x20){ char b[7]; wsprintfA(b,"\\u%04x",c); o+=b;} else o+=(char)c; }} return o; }
static std::string Hex(const std::string&s){ static const char*h="0123456789ABCDEF"; std::string o; o.resize(s.size()*2); for(size_t i=0;i<s.size();++i){o[2*i]=h[(unsigned char)s[i]>>4];o[2*i+1]=h[(unsigned char)s[i]&15];} return o; }
static std::string Unhex(const std::string&s){ auto v=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return 0;}; std::string o; for(size_t i=0;i+1<s.size();i+=2)o.push_back((char)((v(s[i])<<4)|v(s[i+1]))); return o; }
static std::wstring AppPath(){ wchar_t b[MAX_PATH]; GetModuleFileNameW(0,b,MAX_PATH); std::wstring p=b; size_t i=p.find_last_of(L"\\/"); return i==std::wstring::npos?L".":p.substr(0,i); }
static void EnsureDirs(){ wchar_t app[MAX_PATH]={0}; if(SUCCEEDED(SHGetFolderPathW(0,CSIDL_APPDATA,0,SHGFP_TYPE_CURRENT,app))) gDataDir=std::wstring(app)+L"\\ChatGPT-Win7"; else gDataDir=AppPath()+L"\\data"; gChatsDir=gDataDir+L"\\chats"; CreateDirectoryW(gDataDir.c_str(),0); CreateDirectoryW(gChatsDir.c_str(),0); }
static std::wstring NewId(){ SYSTEMTIME st; GetLocalTime(&st); wchar_t b[64]; wsprintfW(b,L"%04d%02d%02d-%02d%02d%02d-%03d",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,st.wMilliseconds); return b; }
static std::wstring SessionPath(const std::wstring&id){return gChatsDir+L"\\"+id+L".chat";}
static bool WriteUtf8File(const std::wstring& path,const std::string& data){ HANDLE h=CreateFileW(path.c_str(),GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);if(h==INVALID_HANDLE_VALUE)return false;DWORD done=0;BOOL ok=data.empty()||WriteFile(h,data.data(),(DWORD)data.size(),&done,0);CloseHandle(h);return ok&&done==data.size(); }
static bool ReadUtf8File(const std::wstring& path,std::string& data){ HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);if(h==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER n;if(!GetFileSizeEx(h,&n)||n.QuadPart<0||n.QuadPart>16*1024*1024){CloseHandle(h);return false;}data.resize((size_t)n.QuadPart);DWORD got=0;BOOL ok=data.empty()||ReadFile(h,&data[0],(DWORD)data.size(),&got,0);CloseHandle(h);if(!ok)return false;data.resize(got);return true; }
static void SaveSession(){ if(gSession.id.empty()||gSession.messages.empty())return; std::ostringstream f; f<<"CHATGPTWIN7V2\n"; f<<"T|"<<Hex(WideToUtf8(gSession.title))<<"\n"; for(auto&m:gSession.messages){ f<<"M|"<<m.role<<"|"<<Hex(WideToUtf8(m.text))<<"\n"; for(auto&a:m.attachments) f<<"A|"<<(a.image?"I":"F")<<"|"<<a.fileId<<"|"<<Hex(WideToUtf8(a.name))<<"|"<<Hex(WideToUtf8(a.path))<<"\n"; } WriteUtf8File(SessionPath(gSession.id),f.str()); }
static bool LoadSessionFile(const std::wstring&path,Session&out){ std::string raw;if(!ReadUtf8File(path,raw))return false;std::istringstream f(raw); std::string line; if(!std::getline(f,line)||line!="CHATGPTWIN7V2")return false; out=Session(); while(std::getline(f,line)){ if(line.size()>2&&line[1]=='|'){ if(line[0]=='T'){out.title=Utf8ToWide(Unhex(line.substr(2)));} else if(line[0]=='M'){ size_t p=line.find('|',2); if(p!=std::string::npos){ Message m; m.role=line.substr(2,p-2); m.text=Utf8ToWide(Unhex(line.substr(p+1))); out.messages.push_back(m);} } else if(line[0]=='A'&&!out.messages.empty()){ std::vector<std::string> q; size_t s=2; while(true){size_t p=line.find('|',s); if(p==std::string::npos){q.push_back(line.substr(s));break;}q.push_back(line.substr(s,p-s));s=p+1;} if(q.size()>=4){Attachment a;a.image=q[0]=="I";a.fileId=q[1];a.name=Utf8ToWide(Unhex(q[2]));a.path=Utf8ToWide(Unhex(q[3]));out.messages.back().attachments.push_back(a);} } } } size_t slash=path.find_last_of(L"\\/"); size_t dot=path.find_last_of(L'.'); out.id=path.substr(slash+1,dot-slash-1); return true; }
static void RefreshSidebar(){ SendMessageW(gSidebar,LB_RESETCONTENT,0,0); WIN32_FIND_DATAW fd; HANDLE h=FindFirstFileW((gChatsDir+L"\\*.chat").c_str(),&fd); std::vector<Session> tmp; if(h!=INVALID_HANDLE_VALUE){do{Session s;if(LoadSessionFile(gChatsDir+L"\\"+fd.cFileName,s))tmp.push_back(s);}while(FindNextFileW(h,&fd));FindClose(h);} std::sort(tmp.begin(),tmp.end(),[](const Session&a,const Session&b){return a.id>b.id;}); for(auto&s:tmp){int idx=(int)SendMessageW(gSidebar,LB_ADDSTRING,0,(LPARAM)(s.title.empty()?L"Nuova chat":s.title.c_str())); std::wstring* id=new std::wstring(s.id); SendMessageW(gSidebar,LB_SETITEMDATA,idx,(LPARAM)id);} }
static void FreeSidebarData(){ int n=(int)SendMessageW(gSidebar,LB_GETCOUNT,0,0); for(int i=0;i<n;++i){auto p=(std::wstring*)SendMessageW(gSidebar,LB_GETITEMDATA,i,0); if(p && p!=(std::wstring*)LB_ERR)delete p;} }
static std::wstring DisplayTranscript(){ std::wstring o; for(auto&m:gSession.messages){ if(m.role=="user")o+=L"Tu\r\n";else o+=L"ChatGPT\r\n"; if(!m.attachments.empty()){ for(auto&a:m.attachments)o+=L"  ["+(a.image?std::wstring(L"Immagine"):std::wstring(L"File"))+L"] "+a.name+L"\r\n"; } o+=m.text+L"\r\n\r\n"; } return o; }
static void RenderTranscript(){ SetText(gTranscript,DisplayTranscript()); SendMessageW(gTranscript,EM_SETSEL,-1,-1); SendMessageW(gTranscript,EM_SCROLLCARET,0,0); }
static void RenderPending(){ std::wstring s; for(size_t i=0;i<gPending.size();++i){if(i)s+=L"   ";s+=(gPending[i].image?L"[IMG] ":L"[FILE] ")+gPending[i].name+L"  x";} SetText(gAttachmentBar,s); ShowWindow(gAttachmentBar,gPending.empty()?SW_HIDE:SW_SHOW); }
static void PostStatus(const std::wstring&s){ PostMessageW(gMain,WM_APP_STATUS,0,(LPARAM)new std::wstring(s)); }
static void PostStream(const std::wstring&s){ PostMessageW(gMain,WM_APP_STREAM,0,(LPARAM)new std::wstring(s)); }

static bool HasSSSE3(){
#ifdef _MSC_VER
    int info[4]={}; __cpuid(info,1); return (info[2]&(1<<9))!=0;
#else
    unsigned a=0,b=0,c=0,d=0; if(!__get_cpuid(1,&a,&b,&c,&d))return false; return (c&(1u<<9))!=0;
#endif
}
#if defined(__GNUC__)
__attribute__((target("ssse3"))) static uint32_t SSSE3Checksum(const unsigned char*p,size_t n){
    __m128i acc=_mm_setzero_si128(); size_t i=0; for(;i+16<=n;i+=16){__m128i x=_mm_loadu_si128((const __m128i*)(p+i));acc=_mm_add_epi8(acc,_mm_abs_epi8(x));} alignas(16) unsigned char b[16];_mm_storeu_si128((__m128i*)b,acc);uint32_t r=0;for(int k=0;k<16;k++)r+=b[k];for(;i<n;i++)r+=p[i];return r;
}
#else
static uint32_t SSSE3Checksum(const unsigned char*p,size_t n){uint32_t r=0;for(size_t i=0;i<n;i++)r+=p[i];return r;}
#endif
static void TouchSSSE3(const std::vector<unsigned char>&v){ if(HasSSSE3()&&!v.empty()) volatile uint32_t x=SSSE3Checksum(v.data(),v.size()); }

static std::string GetApiKey(){ wchar_t b[4096]; DWORD n=GetEnvironmentVariableW(L"OPENAI_API_KEY",b,4096); if(!n||n>=4096)return {}; return WideToUtf8(b); }
static bool ReadAll(const std::wstring&path,std::vector<unsigned char>&v){ HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);if(h==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER n;if(!GetFileSizeEx(h,&n)||n.QuadPart<0||n.QuadPart>50LL*1024*1024){CloseHandle(h);return false;}v.resize((size_t)n.QuadPart);DWORD got=0;BOOL ok=v.empty()||ReadFile(h,v.data(),(DWORD)v.size(),&got,0);CloseHandle(h);if(!ok||got!=v.size())return false;TouchSSSE3(v);return true; }
static std::string JsonStringValue(const std::string&s,const std::string&key){ size_t p=s.find("\""+key+"\""); if(p==std::string::npos)return{}; p=s.find(':',p); if(p==std::string::npos)return{}; p=s.find('"',p); if(p==std::string::npos)return{}; ++p; std::string o; for(;p<s.size();++p){char c=s[p];if(c=='"')break;if(c=='\\'&&p+1<s.size()){char e=s[++p];switch(e){case 'n':o+='\n';break;case 'r':o+='\r';break;case 't':o+='\t';break;case 'b':o+='\b';break;case 'f':o+='\f';break;case '"':o+='"';break;case '\\':o+='\\';break;case '/':o+='/';break;case 'u':{if(p+4<s.size()){unsigned x=0;for(int i=0;i<4;i++){char h=s[++p];x=x*16+(h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h-'A'+10);} wchar_t w=(wchar_t)x; o+=WideToUtf8(std::wstring(1,w));}break;}default:o+=e;}}else o+=c;}return o; }

static void SetActive(HINTERNET h){EnterCriticalSection(&gReqCS);gActiveRequest=h;LeaveCriticalSection(&gReqCS);} 
static bool TakeActive(HINTERNET h){bool own=false;EnterCriticalSection(&gReqCS);if(gActiveRequest==h){gActiveRequest=0;own=true;}LeaveCriticalSection(&gReqCS);return own;} 
static void AbortActive(){InterlockedExchange(&gCancel,1);EnterCriticalSection(&gReqCS);if(gActiveRequest){WinHttpCloseHandle(gActiveRequest);gActiveRequest=0;}LeaveCriticalSection(&gReqCS);} 

static bool HttpRequest(const wchar_t*method,const wchar_t*path,const std::wstring&headers,const unsigned char*body,DWORD bodyLen,std::string&resp,DWORD*statusOut=nullptr,bool stream=false){
    HINTERNET ses=WinHttpOpen(L"ChatGPT-Win7/2.0",WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0); if(!ses)return false;
    DWORD tls=WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2; WinHttpSetOption(ses,WINHTTP_OPTION_SECURE_PROTOCOLS,&tls,sizeof(tls));
    HINTERNET con=WinHttpConnect(ses,L"api.openai.com",INTERNET_DEFAULT_HTTPS_PORT,0); if(!con){WinHttpCloseHandle(ses);return false;}
    HINTERNET req=WinHttpOpenRequest(con,method,path,0,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE); if(!req){WinHttpCloseHandle(con);WinHttpCloseHandle(ses);return false;}
    SetActive(req);
    BOOL ok=WinHttpSendRequest(req,headers.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:headers.c_str(),headers.empty()?0:(DWORD)-1L,(LPVOID)body,bodyLen,bodyLen,0);
    if(ok)ok=WinHttpReceiveResponse(req,0);
    DWORD st=0,sz=sizeof(st); if(ok)WinHttpQueryHeaders(req,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&st,&sz,WINHTTP_NO_HEADER_INDEX); if(statusOut)*statusOut=st;
    if(ok){ char buf[8192]; DWORD got=0; while(InterlockedCompareExchange(&gCancel,0,0)==0 && WinHttpReadData(req,buf,sizeof(buf),&got)&&got){resp.append(buf,buf+got); if(stream){ size_t pos=0,sep=2; while(true){pos=resp.find("\n\n");sep=2;size_t cr=resp.find("\r\n\r\n");if(cr!=std::string::npos&&(pos==std::string::npos||cr<pos)){pos=cr;sep=4;}if(pos==std::string::npos)break; std::string frame=resp.substr(0,pos);resp.erase(0,pos+sep); size_t d=frame.find("data:"); if(d!=std::string::npos){std::string json=frame.substr(d+5);while(!json.empty()&&(json[0]==' '||json[0]=='\r'))json.erase(json.begin()); if(json.find("response.output_text.delta")!=std::string::npos){std::string delta=JsonStringValue(json,"delta");if(!delta.empty())PostStream(Utf8ToWide(delta));} if(json.find("response.failed")!=std::string::npos){std::string msg=JsonStringValue(json,"message");if(!msg.empty())PostStatus(L"Errore API: "+Utf8ToWide(msg));} } } } } }
    bool own=TakeActive(req); if(own)WinHttpCloseHandle(req); WinHttpCloseHandle(con);WinHttpCloseHandle(ses); return ok&&st>=200&&st<300;
}
static bool UploadFile(Attachment&a,const std::string&key){
    std::vector<unsigned char> data; if(!ReadAll(a.path,data)){PostStatus(L"Impossibile leggere "+a.name+L" (max 50 MB per file).");return false;}
    std::string boundary="----ChatGPTWin7Boundary7MA4YWxk"; std::string purpose=a.image?"vision":"user_data";
    std::string head="--"+boundary+"\r\nContent-Disposition: form-data; name=\"purpose\"\r\n\r\n"+purpose+"\r\n--"+boundary+"\r\nContent-Disposition: form-data; name=\"file\"; filename=\""+JsonEscape(WideToUtf8(a.name))+"\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    std::string tail="\r\n--"+boundary+"--\r\n";
    std::vector<unsigned char> body; body.reserve(head.size()+data.size()+tail.size());body.insert(body.end(),head.begin(),head.end());body.insert(body.end(),data.begin(),data.end());body.insert(body.end(),tail.begin(),tail.end());
    std::wstring headers=L"Authorization: Bearer "+Utf8ToWide(key)+L"\r\nContent-Type: multipart/form-data; boundary="+Utf8ToWide(boundary)+L"\r\n";
    std::string resp;DWORD st=0; if(!HttpRequest(L"POST",L"/v1/files",headers,body.data(),(DWORD)body.size(),resp,&st,false)){PostStatus(L"Upload fallito per "+a.name+L" (HTTP "+std::to_wstring(st)+L").");return false;}
    a.fileId=JsonStringValue(resp,"id"); if(a.fileId.empty()){PostStatus(L"Upload completato ma ID file non trovato.");return false;} return true;
}
static std::string BuildResponseJson(const std::wstring&model){
    std::string j="{\"model\":\""+JsonEscape(WideToUtf8(model))+"\",\"stream\":true,\"input\":[";
    for(size_t mi=0;mi<gSession.messages.size();++mi){ if(mi)j+=","; auto&m=gSession.messages[mi]; j+="{\"role\":\""+m.role+"\",\"content\":["; bool first=true; for(auto&a:m.attachments){ if(a.fileId.empty())continue; if(!first)j+=",";first=false; if(a.image)j+="{\"type\":\"input_image\",\"file_id\":\""+JsonEscape(a.fileId)+"\",\"detail\":\"auto\"}"; else j+="{\"type\":\"input_file\",\"file_id\":\""+JsonEscape(a.fileId)+"\"}"; } if(!m.text.empty()){if(!first)j+=","; j+="{\"type\":\"input_text\",\"text\":\""+JsonEscape(WideToUtf8(m.text))+"\"}";} j+="]}"; }
    j+="]}"; return j;
}
struct WorkerArgs{ std::wstring model; };
static DWORD WINAPI WorkerProc(LPVOID p){ WorkerArgs*wa=(WorkerArgs*)p; std::wstring model=wa->model;delete wa; std::string key=GetApiKey(); if(key.empty()){PostStatus(L"Manca OPENAI_API_KEY. Usa set_api_key.bat e riapri il programma.");PostMessageW(gMain,WM_APP_DONE,0,0);return 0;}
    Message&cur=gSession.messages.back(); for(size_t i=0;i<cur.attachments.size();++i){ if(InterlockedCompareExchange(&gCancel,0,0))break; if(cur.attachments[i].fileId.empty()){PostStatus(L"Caricamento "+cur.attachments[i].name+L"..."); if(!UploadFile(cur.attachments[i],key)){PostMessageW(gMain,WM_APP_DONE,0,0);return 0;}} }
    if(InterlockedCompareExchange(&gCancel,0,0)){PostMessageW(gMain,WM_APP_DONE,0,0);return 0;}
    PostStatus(L"ChatGPT sta rispondendo..."); std::string body=BuildResponseJson(model); Message assistant;assistant.role="assistant";gSession.messages.push_back(assistant); std::wstring headers=L"Authorization: Bearer "+Utf8ToWide(key)+L"\r\nContent-Type: application/json\r\nAccept: text/event-stream\r\n"; std::string scratch;DWORD st=0; bool ok=HttpRequest(L"POST",L"/v1/responses",headers,(const unsigned char*)body.data(),(DWORD)body.size(),scratch,&st,true); if(!ok && !InterlockedCompareExchange(&gCancel,0,0))PostStatus(L"Richiesta fallita (HTTP "+std::to_wstring(st)+L"). Controlla connessione, API key e TLS 1.2."); PostMessageW(gMain,WM_APP_DONE,ok?1:0,0); return 0; }

static int GetEncoderClsid(const WCHAR*format,CLSID*p){ UINT n=0,size=0;GetImageEncodersSize(&n,&size);if(!size)return -1; std::vector<BYTE>b(size);ImageCodecInfo*i=(ImageCodecInfo*)b.data();GetImageEncoders(n,size,i);for(UINT x=0;x<n;x++)if(wcscmp(i[x].MimeType,format)==0){*p=i[x].Clsid;return x;}return -1; }
static bool ClipboardImageToPng(std::wstring&out){ if(!OpenClipboard(gMain))return false; HBITMAP hb=(HBITMAP)GetClipboardData(CF_BITMAP); if(!hb){CloseClipboard();return false;} HBITMAP copy=(HBITMAP)CopyImage(hb,IMAGE_BITMAP,0,0,LR_CREATEDIBSECTION); CloseClipboard(); if(!copy)return false; CLSID png;if(GetEncoderClsid(L"image/png",&png)<0){DeleteObject(copy);return false;} std::wstring dir=gDataDir+L"\\clipboard";CreateDirectoryW(dir.c_str(),0);out=dir+L"\\clipboard-"+NewId()+L".png"; Bitmap bmp(copy,0); Status st=bmp.Save(out.c_str(),&png,0);DeleteObject(copy);return st==Ok; }
static bool AddAttachmentPath(const std::wstring&p){ WIN32_FILE_ATTRIBUTE_DATA d;if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&d)||d.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)return false; ULONGLONG n=((ULONGLONG)d.nFileSizeHigh<<32)|d.nFileSizeLow;if(n>50ULL*1024*1024){MessageBoxW(gMain,L"Il file supera 50 MB.",L"Allegato",MB_ICONWARNING);return false;} Attachment a;a.path=p;a.name=BaseName(p);a.image=IsImage(p);gPending.push_back(a);RenderPending();return true; }
static void PickFiles(){ wchar_t buf[32768]={0}; OPENFILENAMEW of={sizeof(of)};of.hwndOwner=gMain;of.lpstrFile=buf;of.nMaxFile=32768;of.lpstrFilter=L"File supportati\0*.png;*.jpg;*.jpeg;*.webp;*.gif;*.pdf;*.txt;*.md;*.json;*.html;*.xml;*.doc;*.docx;*.rtf;*.odt;*.ppt;*.pptx;*.csv;*.xls;*.xlsx\0Tutti i file\0*.*\0";of.Flags=OFN_EXPLORER|OFN_ALLOWMULTISELECT|OFN_FILEMUSTEXIST; if(!GetOpenFileNameW(&of))return; std::wstring first=buf; wchar_t* p=buf+first.size()+1;if(!*p)AddAttachmentPath(first);else{std::wstring dir=first;while(*p){std::wstring name=p;AddAttachmentPath(dir+L"\\"+name);p+=name.size()+1;}} }
static void StartSend(){ if(InterlockedCompareExchange(&gBusy,0,0)){AbortActive();SetText(gStatus,L"Interruzione...");return;} std::wstring text=Trim(GetText(gInput));if(text.empty()&&gPending.empty())return; if(gSession.id.empty())gSession.id=NewId(); if(gSession.title.empty()){gSession.title=text.empty()?L"Chat con allegato":text.substr(0,std::min<size_t>(44,text.size()));}
    Message m;m.role="user";m.text=text;m.attachments=gPending;gPending.clear();gSession.messages.push_back(m);SetText(gInput,L"");RenderPending();RenderTranscript();SaveSession(); InterlockedExchange(&gCancel,0);InterlockedExchange(&gBusy,1);SetText(gSend,L"Stop");EnableWindow(gAttach,FALSE); WorkerArgs*wa=new WorkerArgs;wa->model=GetText(gModel);CreateThread(0,0,WorkerProc,wa,0,0); }
static void NewChat(){ if(InterlockedCompareExchange(&gBusy,0,0))return;SaveSession();gSession=Session();gSession.id=NewId();gPending.clear();RenderPending();RenderTranscript();SetText(gInput,L"");FreeSidebarData();RefreshSidebar();SetText(gStatus,L"Nuova conversazione"); }
static void LoadSelected(){ if(InterlockedCompareExchange(&gBusy,0,0))return;int i=(int)SendMessageW(gSidebar,LB_GETCURSEL,0,0);if(i==LB_ERR)return;auto id=(std::wstring*)SendMessageW(gSidebar,LB_GETITEMDATA,i,0);if(!id||id==(std::wstring*)LB_ERR)return;SaveSession();Session s;if(LoadSessionFile(SessionPath(*id),s)){gSession=s;RenderTranscript();SetText(gStatus,L"Conversazione caricata");} }

static LRESULT CALLBACK InputProc(HWND h,UINT m,WPARAM w,LPARAM l){ if(m==WM_PASTE && IsClipboardFormatAvailable(CF_BITMAP)){std::wstring p;if(ClipboardImageToPng(p)){AddAttachmentPath(p);SetText(gStatus,L"Immagine dagli appunti allegata");return 0;}} if(m==WM_KEYDOWN&&w==VK_RETURN && (GetKeyState(VK_CONTROL)&0x8000)){StartSend();return 0;} return CallWindowProcW(gOldInputProc,h,m,w,l); }
static void Layout(int W,int H){ int side=215, pad=12, top=52,bottom=122; MoveWindow(gNew,10,10,195,32,TRUE);MoveWindow(gSidebar,8,52,199,H-65,TRUE);MoveWindow(gModel,side+pad,10,200,30,TRUE);MoveWindow(gTranscript,side+pad,top,W-side-2*pad,H-top-bottom,TRUE);MoveWindow(gAttachmentBar,side+pad,H-bottom+5,W-side-2*pad,24,TRUE);MoveWindow(gAttach,side+pad,H-78,42,34,TRUE);MoveWindow(gInput,side+pad+50,H-84,W-side-2*pad-145,48,TRUE);MoveWindow(gSend,W-92,H-78,80,34,TRUE);MoveWindow(gStatus,side+pad,H-28,W-side-2*pad,20,TRUE); }
static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){ switch(m){
case WM_CREATE:{
    gNew=CreateWindowW(L"BUTTON",L"+ Nuova chat",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,h,(HMENU)101,0,0);
    gSidebar=CreateWindowExW(0,L"LISTBOX",L"",WS_CHILD|WS_VISIBLE|LBS_NOTIFY|WS_VSCROLL|LBS_NOINTEGRALHEIGHT,0,0,0,0,h,(HMENU)102,0,0);
    gTranscript=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,0,0,0,0,h,(HMENU)103,0,0);
    gModel=CreateWindowW(L"COMBOBOX",L"",WS_CHILD|WS_VISIBLE|CBS_DROPDOWN,0,0,0,0,h,(HMENU)104,0,0);SendMessageW(gModel,CB_ADDSTRING,0,(LPARAM)L"gpt-5.6");SendMessageW(gModel,CB_ADDSTRING,0,(LPARAM)L"gpt-5.5");SendMessageW(gModel,CB_SETCURSEL,0,0);
    gAttachmentBar=CreateWindowW(L"STATIC",L"",WS_CHILD,0,0,0,0,h,(HMENU)105,0,0);
    gAttach=CreateWindowW(L"BUTTON",L"+",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,h,(HMENU)106,0,0);
    gInput=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,0,0,0,0,h,(HMENU)107,0,0);
    gSend=CreateWindowW(L"BUTTON",L"Invia",WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,0,0,0,0,h,(HMENU)108,0,0);
    gStatus=CreateWindowW(L"STATIC",HasSSSE3()?L"Windows 7 x86 • SSSE3 rilevato":L"Windows 7 x86 • fallback CPU",WS_CHILD|WS_VISIBLE,0,0,0,0,h,(HMENU)109,0,0);
    HWND all[]={gNew,gSidebar,gTranscript,gModel,gAttachmentBar,gAttach,gInput,gSend,gStatus};for(HWND x:all)SendMessageW(x,WM_SETFONT,(WPARAM)gFont,TRUE);SendMessageW(gStatus,WM_SETFONT,(WPARAM)gFontSmall,TRUE);gOldInputProc=(WNDPROC)SetWindowLongPtrW(gInput,GWLP_WNDPROC,(LONG_PTR)InputProc);DragAcceptFiles(h,TRUE);RefreshSidebar();gSession.id=NewId();return 0;}
case WM_SIZE:Layout(LOWORD(l),HIWORD(l));return 0;
case WM_COMMAND: if(LOWORD(w)==101)NewChat();else if(LOWORD(w)==102&&HIWORD(w)==LBN_DBLCLK)LoadSelected();else if(LOWORD(w)==106)PickFiles();else if(LOWORD(w)==108)StartSend();return 0;
case WM_DROPFILES:{HDROP d=(HDROP)w;UINT n=DragQueryFileW(d,0xFFFFFFFF,0,0);for(UINT i=0;i<n;i++){wchar_t p[MAX_PATH];DragQueryFileW(d,i,p,MAX_PATH);AddAttachmentPath(p);}DragFinish(d);return 0;}
case WM_APP_STREAM:{std::wstring*p=(std::wstring*)l;if(!gSession.messages.empty()&&gSession.messages.back().role=="assistant")gSession.messages.back().text+=*p;delete p;RenderTranscript();return 0;}
case WM_APP_STATUS:{std::wstring*p=(std::wstring*)l;SetText(gStatus,*p);delete p;return 0;}
case WM_APP_DONE:{InterlockedExchange(&gBusy,0);InterlockedExchange(&gCancel,0);SetText(gSend,L"Invia");EnableWindow(gAttach,TRUE);if(w)SetText(gStatus,L"Pronto");SaveSession();FreeSidebarData();RefreshSidebar();return 0;}
case WM_CTLCOLORSTATIC:{HDC dc=(HDC)w;SetTextColor(dc,C_MUTED);SetBkColor(dc,C_BG);return (LRESULT)gBrushBg;}
case WM_CTLCOLOREDIT:{HDC dc=(HDC)w;SetTextColor(dc,C_TEXT);SetBkColor(dc,C_INPUT);return (LRESULT)gBrushInput;}
case WM_CTLCOLORLISTBOX:{HDC dc=(HDC)w;SetTextColor(dc,C_TEXT);SetBkColor(dc,C_PANEL);return (LRESULT)gBrushPanel;}
case WM_ERASEBKGND:{RECT r;GetClientRect(h,&r);FillRect((HDC)w,&r,gBrushBg);RECT s={0,0,215,r.bottom};FillRect((HDC)w,&s,gBrushPanel);return 1;}
case WM_CLOSE:if(InterlockedCompareExchange(&gBusy,0,0))AbortActive();SaveSession();DestroyWindow(h);return 0;
case WM_DESTROY:FreeSidebarData();PostQuitMessage(0);return 0;}
return DefWindowProcW(h,m,w,l);}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int){
    InitializeCriticalSection(&gReqCS); GdiplusStartupInput gi;GdiplusStartup(&gGdiToken,&gi,0);EnsureDirs();
    gFont=CreateFontW(-17,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    gFontSmall=CreateFontW(-14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    gFontMono=CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
    gBrushBg=CreateSolidBrush(C_BG);gBrushPanel=CreateSolidBrush(C_PANEL);gBrushInput=CreateSolidBrush(C_INPUT);
    WNDCLASSW wc={};wc.hInstance=hi;wc.lpfnWndProc=WndProc;wc.lpszClassName=L"ChatGPTWin7V2";wc.hCursor=LoadCursor(0,IDC_ARROW);wc.hIcon=LoadIcon(0,IDI_APPLICATION);wc.hbrBackground=gBrushBg;RegisterClassW(&wc);
    gMain=CreateWindowExW(0,wc.lpszClassName,L"ChatGPT Win7 — x86",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1040,720,0,0,hi,0);ShowWindow(gMain,SW_SHOW);UpdateWindow(gMain);
    MSG msg;while(GetMessageW(&msg,0,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);} 
    if(gFont)DeleteObject(gFont);if(gFontSmall)DeleteObject(gFontSmall);if(gFontMono)DeleteObject(gFontMono);if(gBrushBg)DeleteObject(gBrushBg);if(gBrushPanel)DeleteObject(gBrushPanel);if(gBrushInput)DeleteObject(gBrushInput);GdiplusShutdown(gGdiToken);DeleteCriticalSection(&gReqCS);return 0;
}
