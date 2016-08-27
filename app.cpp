// +----------------------------------------------------------------------
// | ZYSOFT [ MAKE IT OPEN ]
// +----------------------------------------------------------------------
// | Copyright (c) 2016 ZYSOFT All rights reserved.
// +----------------------------------------------------------------------
// | Licensed ( http://www.apache.org/licenses/LICENSE-2.0 )
// +----------------------------------------------------------------------
// | Author: zy_cwind <391321232@qq.com>
// +----------------------------------------------------------------------

/**
 * g++ -o app app.cpp ./cJSON/cJSON.c ./socks5/server.o -mwindows -DBUILDLIB -I./wxWidgets-3.0.2/win32/include -I./wxWidgets-3.0.2/win32/include/wx-3.0 -I./wxWidgets-3.0.2/win32/lib/wx/include/msw-unicode-static-3.0 -I./libqrencode/win32/include -I./cJSON -I./socks5/libevent-release-2.0.22-stable/win32/include -I./socks5/turnclient ./wxWidgets-3.0.2/win32/lib/libwx_mswu-3.0.a ./wxWidgets-3.0.2/win32/lib/libwxpng-3.0.a ./wxWidgets-3.0.2/win32/lib/libwxzlib-3.0.a ./wxWidgets-3.0.2/win32/lib/libwxexpat-3.0.a ./libqrencode/win32/lib/libqrencode.a ./socks5/turnclient/win32/lib/libturnclient.a ./socks5/libevent-release-2.0.22-stable/win32/lib/libevent.a -lgdi32 -lcomctl32 -lole32 -loleaut32 -lcomdlg32 -lwinspool -luuid -lws2_32 -static-libgcc -static-libstdc++ -static -lpthread
 *
 *
 */

#include "wx/wx.h"
#include "wx/msw/registry.h"
#include "wx/xrc/xmlres.h"
#include "wx/thread.h"
#include "wx/protocol/http.h"
#include "wx/uri.h"
#include "wx/sstream.h"

#include "qrencode.h"
#include "cJSON.h"

#define MAX_BUF_SIZE 1024

/**
 * 出口线程
 *
 *
 */
extern char *id;

extern "C" int work(int argc, char *argv[]);

class Thread : public wxThread {
public:
    
    wxThread::ExitCode Entry() {
        work(0, NULL);
    }
    
};

/**
 * 可以使用 wxWeakRef 智能指针
 *
 *
 *
 */
class LogFrame : public wxFrame, public wxThreadHelper {
public:
    
    LogFrame(wxWindow *parent) : wxFrame(parent, wxID_ANY, wxT("日志")) {
        wxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        wxTextCtrl *logger = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
        sizer->Add(logger, 1, wxGROW | wxALL, 5);
        SetSizer(sizer);
        mRedirector = new wxStreamToTextRedirector(logger);
        if (CreateThread(wxTHREAD_JOINABLE) == wxTHREAD_NO_ERROR)
            GetThread()->Run();
    }
    
    virtual ~LogFrame() {
        delete mRedirector;
    }
    
    wxThread::ExitCode Entry() {
        HANDLE hIn, hOut;
        
        CreatePipe(&hIn, &hOut, NULL, 0);
        FILE *fpOut = _fdopen(_open_osfhandle((intptr_t ) hOut, _O_TEXT) , "wt");   
        *stdout = *fpOut;
        setvbuf(stdout, NULL, _IONBF, 0);
        
        /**
         * 重定向 std::cout 到控件
         *
         *
         */
        while (!GetThread()->TestDestroy()) {
            char buf[MAX_BUF_SIZE];
            unsigned long len;
            
            if (ReadFile(hIn, buf, MAX_BUF_SIZE - 1, &len, NULL)) {
                buf[len] = '\0';
                std::cout << buf;
            }
        }
        
        return (wxThread::ExitCode) 0;
    }
    
private:
    wxStreamToTextRedirector *mRedirector;
    
};

/**
 * 使用一个线程查询状态
 *
 *
 */
class Frame : public wxFrame, public wxThreadHelper {
public:
	
    Frame() {
        wxXmlResource::Get()->LoadFrame(this, NULL, wxT("ID_WXFRAME"));
        
        /**
         * 初始化控件
         *
         *
         */
        mId = static_cast<wxTextCtrl *>(FindWindow(wxT("ID_TEXTCTRL")));
        mIp = static_cast<wxTextCtrl *>(FindWindow(wxT("ID_TEXTCTRL1")));
        mLocation = static_cast<wxTextCtrl *>(FindWindow(wxT("ID_TEXTCTRL2")));
        mBindingState = static_cast<wxTextCtrl *>(FindWindow(wxT("ID_TEXTCTRL3")));
        mAutostart = GetMenuBar()->FindItem(XRCID("ID_MENUITEM"));
        mInfoPanel = static_cast<wxPanel *>(FindWindow(wxT("ID_PANEL1")));
        mAccount = static_cast<wxTextCtrl *>(FindWindow(wxT("ID_TEXTCTRL4")));
        mTxd = static_cast<wxTextCtrl *>(FindWindow(wxT("ID_TEXTCTRL5")));
        mQRCodePanel = static_cast<wxPanel *>(FindWindow(wxT("ID_PANEL")));
        mQRCode = static_cast<wxStaticBitmap *>(FindWindow(wxT("ID_QRCODE")));
        
        mKey = new wxRegKey(wxRegKey::HKCU, "Software\\Microsoft\\Windows\\CurrentVersion\\Run");
        
        /**
         * 初始化状态
         *
         *
         */
        SetSize(400, 550);
        mInfoPanel->Show(false);
        mQRCodePanel->Show(false);
        GetSizer()->Layout();
        mAutostart->Check(mKey->HasValue(wxT("server-windows")));
        Bind(wxEVT_THREAD, &Frame::OnUpdate, this);
        if (CreateThread(wxTHREAD_JOINABLE) == wxTHREAD_NO_ERROR)
            GetThread()->Run();
    }
	
    wxThread::ExitCode Entry() {
        while (!GetThread()->TestDestroy()) {
            {
                wxCriticalSectionLocker locker(mDataCS);
                if (id) {
                    mData.id = id;
                    wxHTTP http;
                    unsigned long port;
                    
                    http.SetHeader(wxT("Content-type"), wxT("text/html; charset=utf-8"));
                    
                    /**
                     * 基本信息
                     *
                     *
                     */
                    Frame::GET_INFO.GetPort().ToULong(&port);
                    if (http.Connect(Frame::GET_INFO.GetServer(), port)) {
                        wxInputStream *httpStream = http.GetInputStream(Frame::GET_INFO.GetPath() + wxT("?uid=") + id);
                        
                        if (http.GetError() == wxPROTO_NOERR) {
                            wxString res;
                            cJSON *resJSON;
                            
                            wxStringOutputStream outputStream(&res);
                            httpStream->Read(outputStream);
                            if ((resJSON = cJSON_Parse(res))) {
                                cJSON *p;
                                if (resJSON->child && \
                                    resJSON->child->next && \
                                    (p = resJSON->child->next->child)) {
                                    do {
                                        wxString key(p->string);
                                        /**
                                         * 解析数据
                                         *
                                         *
                                         */
                                        if (key == wxT("remote_ip"))
                                            mData.ip = p->valuestring;
                                        else if (key == wxT("area"))
                                            mData.location = p->valuestring;
                                        p = p->next;
                                    } while (p);
                                }
                            }
                        }
                    }
                    http.Close();    
                    Frame::GET_BINDINGSTATE.GetPort().ToULong(&port);
                    if (http.Connect(Frame::GET_BINDINGSTATE.GetServer(), port)) {
                        wxInputStream *httpStream = http.GetInputStream(Frame::GET_BINDINGSTATE.GetPath() + wxT("?device.uid=") + id + wxT("&type=0"));
                        
                        if (http.GetError() == wxPROTO_NOERR) {
                            wxString res;
                            cJSON *resJSON;
                            
                            wxStringOutputStream outputStream(&res);
                            httpStream->Read(outputStream);
                            if ((resJSON = cJSON_Parse(res))) {
                                cJSON *p;
                                
                            }
                        }
                    }
		    http.Close();    
                }
            }
            wxQueueEvent(GetEventHandler(), new wxThreadEvent());
            wxSleep(10);
        }
        return 0;
    }
    
    /**
     * 解除绑定
     *
     *
     */
    void OnUnbind(wxCommandEvent& event) {
    }
    
    /**
     * 打开日志
     *
     *
     */
    void OnEnableLog(wxCommandEvent& event) {
        if (!mLog)
	    mLog = new LogFrame(this);
        mLog->Show(true);
    }
	
    /**
     * 往注测表中写入启动项
     *
     *
     */
    void OnAutostartChecked(wxCommandEvent& event) {
        if (event.IsChecked()) {
            TCHAR fileName[PATH_MAX];
			
            if (GetModuleFileName(NULL, fileName, PATH_MAX))
                if (mKey->SetValue(wxT("server-windows"), fileName))
                    return ;
            mAutostart->Check(false);
        } else
            mKey->DeleteValue(wxT("server-windows"));
    }
    
    void OnUpdate(wxThreadEvent& event) {
        wxCriticalSectionLocker locker(mDataCS);
        mId->SetValue(mData.id);
        mIp->SetValue(mData.ip);
        mLocation->SetValue(mData.location);
        mBindingState->SetValue(mData.bindingState);
        
        if (!mData.id.IsEmpty()) {
            mQRCode->SetBitmap(* GetQRCode(Frame::GET_APP + mData.id));
            mQRCodePanel->Show(true);
            mInfoPanel->Show(false);
            GetSizer()->Layout();
        }
    }
    
private:
    wxWeakRef<LogFrame> mLog;
    
    wxTextCtrl *mId;
    wxTextCtrl *mIp;
    wxTextCtrl *mLocation;
    wxTextCtrl *mBindingState;
    wxMenuItem *mAutostart;
    wxPanel *mInfoPanel;
    wxTextCtrl *mAccount;
    wxTextCtrl *mTxd;
    wxPanel *mQRCodePanel;
    wxStaticBitmap *mQRCode;
    
    wxRegKey *mKey;
    
    /**
     * 数据区
     *
     *
     */
    struct {
        wxString id;
        wxString ip;
        wxString location;
        wxString bindingState;
        wxString name;
        wxString nickname;
    }
    mData;
    wxCriticalSection mDataCS;
    
    /**
     * 获取二维码
     *
     *
     */
    wxBitmap *GetQRCode(const char *string) {
        QRcode *qrCode;
        wxBitmap *bitmap = NULL;
        
        if ((qrCode = QRcode_encodeString(string, 0, QR_ECLEVEL_L, QR_MODE_8, 1))) {
            int size = qrCode->width * 8;
            char *xbmData = (char *) malloc(size * qrCode->width);
            /**
             * XBM 黑白位图
             *
             *
             */
            for (int i = 0; i < size; i++)
                for (int j = 0; j < qrCode->width; j++)
                    xbmData[i * qrCode->width + j] = (qrCode->data[i / 8 * qrCode->width + j] & 0x1) ? 0xFF : 0;
            bitmap = new wxBitmap(xbmData, size, size);
            free(xbmData);
        }
        return bitmap;
    }
	
    DECLARE_EVENT_TABLE()

    /**
     * 常量
     *
     *
     */
    static const wxURI GET_INFO;
    static const wxURI GET_BINDINGSTATE;
    static const wxURI UNBIND;
    
    static const wxString GET_APP;
};

BEGIN_EVENT_TABLE(Frame, wxFrame)
EVT_BUTTON(XRCID("ID_BUTTON"), Frame::OnUnbind)
EVT_MENU(XRCID("ID_MENUITEM"), Frame::OnAutostartChecked)
EVT_MENU(XRCID("ID_MENUITEM1"), Frame::OnEnableLog)
END_EVENT_TABLE()

const wxURI Frame::GET_INFO = wxURI(wxT("http://man1.zed1.cn:9000/manage/cgi/api!getDeviceArea.action"));
const wxURI Frame::GET_BINDINGSTATE = wxURI(wxT("http://zx.zed1.cn:88/ip_share/cgi/device!getBindStatus.action"));
const wxURI Frame::UNBIND = wxURI(wxT("http:// zx.zed1.cn:88/ip_share/cgi/device!unbind.action"));

const wxString Frame::GET_APP = wxT("http://zx.zed1.cn:88/ip_share/download/ipShare.apk?");

/**
 * 应用实例
 *
 *
 */
class App : public wxApp {
public:
	
    virtual bool OnInit() {
        /**
         * 显示窗口
         *
         *
         */
        wxXmlResource::Get()->InitAllHandlers();
        wxXmlResource::Get()->LoadFile(wxFileName(wxT("./window.xrc")));
        Frame *frame = new Frame();
        frame->Show(true);
        
        wxSocketBase::Initialize();
       (new Thread())->Run();
        return true;
    }
    
};

IMPLEMENT_APP(App)
