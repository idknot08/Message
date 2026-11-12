#include "pch.h"
#include "ChatDlg.h"
#include <stdexcept>
#include <afxdlgs.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <atlimage.h>
#include <filesystem>

namespace {
// Chuyển chuỗi Unicode của MFC sang UTF-8 để gửi trong request JSON.
std::string Utf8(const CString& text) {
    if (text.IsEmpty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, text.GetLength(), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode");
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, text.GetLength(), result.data(), size, nullptr, nullptr);
    return result;
}
// Chuyển chuỗi UTF-8 từ server sang Unicode để hiển thị trong MFC.
CString Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) return L"[UTF-8 không hợp lệ]";
    CString result;
    auto* buffer = result.GetBuffer(size);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), buffer, size);
    result.ReleaseBuffer(size);
    return result;
}
// Hộp thoại dùng cho tìm tài khoản, đặt tên nhóm và chọn thành viên; không làm mạng trên UI.
class PickDialog : public CDialog {
public:
    using Entry=std::pair<int64_t,CString>;
    PickDialog(CString title,std::vector<Entry> entries,bool nameInput=false,bool multiple=false,bool requireChoice=true,CString initial={})
        : CDialog(IDD_PICK), title_(title), entries_(std::move(entries)), nameInput_(nameInput), multiple_(multiple), requireChoice_(requireChoice), value(initial) {}
    CString value;
    std::vector<int64_t> chosen;
protected:
    BOOL OnInitDialog() override {
        CDialog::OnInitDialog(); SetWindowTextW(title_);
        SetDlgItemTextW(IDC_PICK_HINT,nameInput_ ? L"Nhập tên. Chọn người được mời (Ctrl để chọn nhiều)." : L"Tìm theo tên và chọn người trong danh sách.");
        SetDlgItemTextW(IDC_PICK_TEXT,value); Fill();
        if(entries_.empty() && !requireChoice_) GetDlgItem(IDC_PICK_LIST)->ShowWindow(SW_HIDE);
        return TRUE;
    }
    BOOL OnCommand(WPARAM w,LPARAM l) override {
        if(LOWORD(w)==IDC_PICK_TEXT && HIWORD(w)==EN_CHANGE && !nameInput_) {Fill();return TRUE;}
        return CDialog::OnCommand(w,l);
    }
    void OnOK() override {
        GetDlgItemTextW(IDC_PICK_TEXT,value); value.Trim();
        if(nameInput_ && value.IsEmpty()) {MessageBoxW(L"Hãy nhập tên.");return;}
        auto* list=static_cast<CListBox*>(GetDlgItem(IDC_PICK_LIST));
        const int n=list->GetSelCount();
        if((requireChoice_ && n<1) || (!multiple_ && n>1)) {MessageBoxW(L"Hãy chọn một người.");return;}
        std::vector<int> rows(n); if(n) list->GetSelItems(n,rows.data());
        for(int row:rows) chosen.push_back(static_cast<int64_t>(list->GetItemData(row)));
        CDialog::OnOK();
    }
private:
    void Fill() {
        auto* list=static_cast<CListBox*>(GetDlgItem(IDC_PICK_LIST)); if(!list) return;
        CString filter;GetDlgItemTextW(IDC_PICK_TEXT,filter);filter.MakeLower();
        list->ResetContent();
        for(const auto& entry:entries_) {
            CString name=entry.second;name.MakeLower();
            if(!nameInput_ && name.Find(filter)<0) continue;
            int index=list->AddString(entry.second);list->SetItemData(index,static_cast<DWORD_PTR>(entry.first));
        }
    }
    CString title_;
    std::vector<Entry> entries_;
    bool nameInput_,multiple_,requireChoice_;
};

// Tạo ảnh vuông vừa ô hiển thị; không có ảnh thì dùng chữ đầu tên tài khoản.
HBITMAP AvatarBitmap(const std::string& bytes, const CString& name, int size) {
    CImage source, target;
    if (!bytes.empty()) {
        CComPtr<IStream> stream;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
            ULONG written = 0;
            stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written);
            LARGE_INTEGER zero{}; stream->Seek(zero, STREAM_SEEK_SET, nullptr);
            source.Load(stream);
        }
    }
    target.Create(size, size, 24);
    CDC* dc = CDC::FromHandle(target.GetDC());
    dc->FillSolidRect(0, 0, size, size, RGB(224, 233, 246));
    if (!source.IsNull()) {
        const int side = (std::min)(source.GetWidth(), source.GetHeight());
        source.Draw(dc->GetSafeHdc(), 0, 0, size, size,
                    (source.GetWidth()-side)/2, (source.GetHeight()-side)/2, side, side);
    } else {
        CFont font; font.CreatePointFont(140, L"Segoe UI");
        auto* old = dc->SelectObject(&font);
        dc->SetBkMode(TRANSPARENT); dc->SetTextColor(RGB(43, 82, 135));
        CRect rect(0, 0, size, size); CString letter = name.Left(1); letter.MakeUpper();
        dc->DrawText(letter, rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        dc->SelectObject(old);
    }
    target.ReleaseDC();
    return target.Detach();
}

}

// Đo và vẽ dùng cùng font/độ rộng để phần mô tả không bị cắt.
void InvitationList::PrepareFont() {
    if(!bold_.GetSafeHandle()) { LOGFONT font{};GetFont()->GetLogFont(&font);font.lfWeight=FW_BOLD;bold_.CreateFontIndirect(&font); }
}
int InvitationList::RowHeight(CDC& dc,const Row& row,int width) {
    PrepareFont();const int textWidth=(std::max)(40,width-66);
    CRect title(0,0,textWidth,0),detail(0,0,textWidth,0);
    auto* old=dc.SelectObject(&bold_);
    dc.DrawText(row.name,title,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    dc.SelectObject(GetFont());dc.DrawText(row.detail,detail,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);dc.SelectObject(old);
    return (std::max)(60,title.Height()+detail.Height()+26);
}
void InvitationList::MeasureItem(LPMEASUREITEMSTRUCT item) {
    if(item->itemID<rows.size()) {CClientDC dc(this);CRect rect;GetClientRect(&rect);item->itemHeight=RowHeight(dc,rows[item->itemID],rect.Width());}
    else item->itemHeight=76;
}
void InvitationList::DrawItem(LPDRAWITEMSTRUCT item) {
    if(item->itemID>=rows.size()) return;
    PrepareFont();auto& row=rows[item->itemID];CDC dc;dc.Attach(item->hDC);
    const bool selected=(item->itemState&ODS_SELECTED)!=0;
    const COLORREF bg=selected?GetSysColor(COLOR_HIGHLIGHT):RGB(255,255,255);
    dc.FillSolidRect(&item->rcItem,bg);dc.SetBkMode(TRANSPARENT);
    dc.SetTextColor(selected?GetSysColor(COLOR_HIGHLIGHTTEXT):RGB(35,45,60));
    CImage image;image.Attach(AvatarBitmap(row.avatar,row.name,40));
    image.Draw(dc.GetSafeHdc(),item->rcItem.left+8,item->rcItem.top+10);
    CRect title(item->rcItem.left+58,item->rcItem.top+10,item->rcItem.right-8,item->rcItem.bottom-8);
    auto* old=dc.SelectObject(&bold_);CRect measured=title;
    dc.DrawText(row.name,measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    title.bottom=title.top+measured.Height();dc.DrawText(row.name,title,DT_WORDBREAK|DT_NOPREFIX);
    dc.SelectObject(GetFont());CRect detail(title.left,title.bottom+4,title.right,item->rcItem.bottom-8);
    dc.DrawText(row.detail,detail,DT_WORDBREAK|DT_NOPREFIX);dc.SelectObject(old);
    if(item->itemState&ODS_FOCUS) dc.DrawFocusRect(&item->rcItem);
    dc.Detach();
}
void InvitationList::Populate(std::vector<Row> items,int64_t selected) {
    SetRedraw(FALSE);ResetContent();rows=std::move(items);
    for(size_t i=0;i<rows.size();++i) {AddString(rows[i].name);if(rows[i].id==selected)SetCurSel(static_cast<int>(i));}
    Reflow();SetRedraw(TRUE);Invalidate();
}
void InvitationList::Reflow() {
    if(!GetSafeHwnd()) return;
    CClientDC dc(this);
    // Lần thứ hai đo lại sau khi thanh cuộn xuất hiện làm hẹp vùng chữ.
    for(int pass=0;pass<2;++pass) {CRect rect;GetClientRect(&rect);for(size_t i=0;i<rows.size();++i)SetItemHeight(static_cast<int>(i),RowHeight(dc,rows[i],rect.Width()));}
    Invalidate();
}

BEGIN_MESSAGE_MAP(ChatDlg, CDialog)
    ON_BN_CLICKED(IDC_CHECK, &ChatDlg::OnCheck)
    ON_BN_CLICKED(IDC_MODE, &ChatDlg::OnMode)
    ON_BN_CLICKED(IDC_CONNECT, &ChatDlg::OnLogin)
    ON_BN_CLICKED(IDC_DISCONNECT, &ChatDlg::OnLogout)
    ON_BN_CLICKED(IDC_SEND, &ChatDlg::OnSend)
    ON_CBN_SELCHANGE(IDC_VIEW, &ChatDlg::OnViewChanged)
    ON_LBN_SELCHANGE(IDC_INVITE_LIST, &ChatDlg::OnInviteSelected)
    ON_BN_CLICKED(IDC_ADD_FRIEND, &ChatDlg::OnAddFriend)
    ON_BN_CLICKED(IDC_NEW_GROUP, &ChatDlg::OnCreateGroup)
    ON_BN_CLICKED(IDC_ACCEPT, &ChatDlg::OnAcceptInvite)
    ON_BN_CLICKED(IDC_DECLINE, &ChatDlg::OnDeclineInvite)
    ON_BN_CLICKED(IDC_CANCEL_INVITE, &ChatDlg::OnCancelInvite)
    ON_BN_CLICKED(IDC_MEMBERS, &ChatDlg::OnMembers)
    ON_BN_CLICKED(IDC_GROUP_MENU, &ChatDlg::OnGroupMenu)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_USERS, &ChatDlg::OnUserChanged)
    ON_NOTIFY(EN_LINK, IDC_CHAT, &ChatDlg::OnFileLink)
    ON_BN_CLICKED(IDC_FILE, &ChatDlg::OnSendFile)
    ON_BN_CLICKED(IDC_AVATAR, &ChatDlg::OnAvatar)
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_CTLCOLOR()
    ON_MESSAGE(WM_CHAT_EVENT, &ChatDlg::OnChatEvent)
END_MESSAGE_MAP()

// Khởi tạo các ô tài khoản hoặc bảng hội thoại theo loại dialog đang mở.
BOOL ChatDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    session_.Attach(m_hWnd);
    if (auth_) {
        SetDlgItemTextW(IDC_NAME, session_.username);
        SetDlgItemTextW(IDC_STATUS, session_.status);
        SendDlgItemMessage(IDC_NAME, EM_SETLIMITTEXT, 32);
        SendDlgItemMessage(IDC_PASSWORD, EM_SETLIMITTEXT, 128);
        SendDlgItemMessage(IDC_CONFIRM, EM_SETLIMITTEXT, 128);
        UpdateControls();
        GetDlgItem(IDC_NAME)->SetFocus();
        return FALSE;
    }
    SetWindowTextW(L"Chat — " + session_.username);
    auto* views=static_cast<CComboBox*>(GetDlgItem(IDC_VIEW));
    for(auto text:{L"Mọi người",L"Bạn bè",L"Nhóm",L"Lời mời"}) views->AddString(text);
    views->SetCurSel(view_);
    SetDlgItemTextW(IDC_ACCOUNT, session_.username);
    LOGFONT accountStyle{}; GetFont()->GetLogFont(&accountStyle);
    accountStyle.lfHeight = MulDiv(accountStyle.lfHeight, 14, 10);
    accountStyle.lfWeight = FW_BOLD;
    accountFont_.CreateFontIndirect(&accountStyle);
    GetDlgItem(IDC_ACCOUNT)->SetFont(&accountFont_);
    SendDlgItemMessage(IDC_INPUT, EM_EXLIMITTEXT, 0, 4096);
    userList_.SubclassDlgItem(IDC_USERS, this);
    invitationList_.SubclassDlgItem(IDC_INVITE_LIST,this);
    invitationList_.ShowWindow(SW_HIDE);
    chatList_.SubclassDlgItem(IDC_CHAT, this);
    chatList_.LimitText(0x7ffffffe);
    chatList_.SetBackgroundColor(FALSE, RGB(255, 255, 255));
    chatList_.SetEventMask(chatList_.GetEventMask() | ENM_LINK);
    CHARFORMAT2 format{}; format.cbSize = sizeof(format);
    format.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    wcscpy_s(format.szFaceName, L"Segoe UI"); format.yHeight = 210; format.crTextColor = RGB(35, 45, 60);
    chatList_.SetDefaultCharFormat(format);
    SendDlgItemMessage(IDC_INPUT, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    userList_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    userList_.InsertColumn(0, L"Người dùng", LVCFMT_LEFT, 150);
    userList_.InsertColumn(1, L"Trạng thái", LVCFMT_LEFT);
    userList_.SetColumnWidth(1, 86);
    ownAvatar_.Attach(AvatarBitmap({}, session_.username, 40));
    static_cast<CStatic*>(GetDlgItem(IDC_MY_AVATAR))->SetBitmap(ownAvatar_);
    CRect area; GetClientRect(&area); Layout(area.Width(), area.Height());
    UpdateControls();
    return TRUE;
}

// Tạo kết nối mạng với callback xếp ChatEvent và báo cho cửa sổ đang gắn.
ClientSession::ClientSession() : connection([this](ChatEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
    if (window_) ::PostMessage(window_, WM_CHAT_EVENT, 0, 0); // winAPI
}) {}

// Dừng kết nối và các luồng mạng trước khi hủy phiên.
ClientSession::~ClientSession() { connection.Stop(); }

// Gắn cửa sổ nhận sự kiện mạng, hoặc bỏ gắn khi chuyển/đóng dialog.
void ClientSession::Attach(HWND window) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = window;
    if (window_ && !events_.empty()) ::PostMessage(window_, WM_CHAT_EVENT, 0, 0);
}

// Lấy một ChatEvent đầu hàng đợi để luồng giao diện xử lý.
bool ClientSession::Next(ChatEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (events_.empty()) return false;
    event = std::move(events_.front());
    events_.pop_front();
    return true;
}

// Đọc IP/cổng từ client.ini cạnh chương trình, kiểm tra rồi lưu vào phiên.
bool ChatDlg::ReadEndpoint(std::string& host, int& port) {
    wchar_t module[32768]{};
    GetModuleFileNameW(nullptr, module, _countof(module));
    const auto config = std::filesystem::path(module).parent_path() / L"client.ini";
    if (!std::filesystem::exists(config)) {
        WritePrivateProfileStringW(L"Server", L"IP", L"127.0.0.1", config.c_str());
        WritePrivateProfileStringW(L"Server", L"Port", L"8080", config.c_str());
    }
    wchar_t addressBuffer[256]{}, portBuffer[32]{};
    GetPrivateProfileStringW(L"Server", L"IP", L"127.0.0.1", addressBuffer, _countof(addressBuffer), config.c_str());
    GetPrivateProfileStringW(L"Server", L"Port", L"8080", portBuffer, _countof(portBuffer), config.c_str());
    CString address(addressBuffer), number(portBuffer);
    address.Trim(); number.Trim();
    wchar_t* end = nullptr;
    const auto value = wcstol(number, &end, 10);
    if (address.IsEmpty() || end == number.GetString() || *end || value < 1 || value > 65535 ||
        address.Find(L'/') >= 0 || address.Find(L' ') >= 0) {
        MessageBoxW(L"Kiểm tra mục [Server] trong client.ini: IP/tên máy không kèm http://, Port từ 1 đến 65535.");
        return false;
    }
    session_.host = address;
    session_.port = number;
    host = Utf8(address);
    port = static_cast<int>(value);
    return true;
}

// Bật/tắt và ẩn/hiện các nút, ô nhập theo màn hình, đăng nhập và trạng thái bận.
void ChatDlg::UpdateControls() {
    const bool loggedIn = userId_ != 0;
    if (auth_) {
        for (int id : {IDC_NAME, IDC_PASSWORD, IDC_CONFIRM, IDC_CONNECT, IDC_MODE})
            GetDlgItem(id)->EnableWindow(!busy_);
        GetDlgItem(IDC_CONFIRM)->ShowWindow(registration_ ? SW_SHOW : SW_HIDE);
        GetDlgItem(IDC_CONFIRM_LABEL)->ShowWindow(registration_ ? SW_SHOW : SW_HIDE);
        // Thu chiều cao theo chế độ, không để khoảng trống của ô nhập lại khi đăng nhập.
        const int buttonY = registration_ ? 100 : 72;
        auto moveAuth = [&](int id, CRect rect) { MapDialogRect(&rect); GetDlgItem(id)->MoveWindow(rect); };
        moveAuth(IDC_CONNECT, CRect(100, buttonY, 186, buttonY + 23));
        moveAuth(IDC_MODE, CRect(192, buttonY, 304, buttonY + 23));
        moveAuth(IDC_STATUS, CRect(16, buttonY + 28, 304, buttonY + 50));
        CRect frame(0, 0, 320, registration_ ? 160 : 132); MapDialogRect(&frame);
        AdjustWindowRectEx(&frame, GetStyle(), FALSE, GetExStyle());
        SetWindowPos(nullptr, 0, 0, frame.Width(), frame.Height(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowTextW(registration_ ? L"Đăng ký tài khoản" : L"Đăng nhập");
        SetDlgItemTextW(IDC_CONNECT, registration_ ? L"Đăng ký" : L"Đăng nhập");
        SetDlgItemTextW(IDC_MODE, registration_ ? L"Về đăng nhập" : L"Tạo tài khoản");
        return;
    }
    GetDlgItem(IDC_DISCONNECT)->EnableWindow(loggedIn && !busy_);
    GetDlgItem(IDC_SEND)->EnableWindow(loggedIn && selected_ != 0 && !busy_);
    for(int id:{IDC_ADD_FRIEND,IDC_NEW_GROUP,IDC_VIEW}) GetDlgItem(id)->EnableWindow(loggedIn && !busy_);
    for(int id:{IDC_MEMBERS,IDC_GROUP_MENU}) {
        GetDlgItem(id)->ShowWindow(selected_<0?SW_SHOW:SW_HIDE);
        GetDlgItem(id)->EnableWindow(loggedIn && selected_<0 && !busy_);
    }
    const ChatInvite* invite=nullptr;
    for(const auto& row:invitations_) if(row.id==selectedInvite_) invite=&row;
    for(int id:{IDC_ACCEPT,IDC_DECLINE,IDC_CANCEL_INVITE}) {
        GetDlgItem(id)->ShowWindow(view_==3?SW_SHOW:SW_HIDE);
        bool allowed=invite && (id==IDC_CANCEL_INVITE ? invite->sender==userId_ : invite->receiver==userId_);
        GetDlgItem(id)->EnableWindow(loggedIn && !busy_ && allowed);
    }
    GetDlgItem(IDC_FILE)->EnableWindow(loggedIn && selected_ != 0 && !busy_);
    GetDlgItem(IDC_AVATAR)->EnableWindow(loggedIn && !busy_);
    userList_.EnableWindow(loggedIn && !busy_);
    invitationList_.EnableWindow(loggedIn && !busy_);
    GetDlgItem(IDC_INPUT)->EnableWindow(loggedIn);
}

// Đọc IP/cổng và xếp công việc kiểm tra kết nối lên worker.
void ChatDlg::OnCheck() {
    std::string host; int port;
    if (!ReadEndpoint(host, port)) return;
    busy_ = true; UpdateControls();
    SetDlgItemTextW(IDC_STATUS, L"Đang kiểm tra kết nối...");
    session_.connection.Check(host, port);
}
// Đọc tài khoản/mật khẩu, kiểm tra nhập lại rồi xếp Job đăng ký hoặc đăng nhập.
void ChatDlg::Authenticate(bool registration) {
    std::string host; int port;
    if (!ReadEndpoint(host, port)) return;
    CString name, password;
    GetDlgItemTextW(IDC_NAME, name);
    GetDlgItemTextW(IDC_PASSWORD, password);
    name.Trim();
    if (name.IsEmpty() || password.IsEmpty()) {
        MessageBoxW(L"Nhập tên tài khoản và mật khẩu.");
        return;
    }
    if (registration) {
        CString confirm;
        GetDlgItemTextW(IDC_CONFIRM, confirm);
        if (password != confirm) {
            SetDlgItemTextW(IDC_STATUS, L"Mật khẩu nhập lại chưa khớp.");
            GetDlgItem(IDC_CONFIRM)->SetFocus();
            return;
        }
    }
    busy_ = true; UpdateControls();
    SetDlgItemTextW(IDC_STATUS, registration ? L"Đang tạo tài khoản..." : L"Đang đăng nhập...");
    if (registration) session_.connection.Register(host, port, Utf8(name), Utf8(password));
    else session_.connection.Login(host, port, Utf8(name), Utf8(password));
}
// Chuyển màn hình tài khoản giữa chế độ đăng nhập và đăng ký.
void ChatDlg::OnMode() {
    registration_ = !registration_;
    SetDlgItemTextW(IDC_CONFIRM, L"");
    SetDlgItemTextW(IDC_STATUS, L"");
    UpdateControls();
}
// Xử lý nút xác nhận bằng Authenticate() theo chế độ tài khoản đang chọn.
void ChatDlg::OnLogin() { if (!busy_) Authenticate(registration_); }
// Xử lý phím Enter để xác nhận khi đang ở màn hình tài khoản và chưa bận.
void ChatDlg::OnOK() { if (auth_ && !busy_) OnLogin(); }
// Khóa thao tác đang xử lý và xếp Job đăng xuất lên worker.
void ChatDlg::OnLogout() {
    busy_ = true; UpdateControls();
    SetDlgItemTextW(IDC_STATUS, L"Đang đăng xuất...");
    session_.connection.Logout();
}
// Đọc nội dung và người nhận đang chọn rồi gọi connection.Send() để xếp Job gửi tin.
void ChatDlg::OnSend() {
    CString text;
    GetDlgItemTextW(IDC_INPUT, text);
    if (text.IsEmpty()) return;
    const auto content = Utf8(text);
    if (content.size() > 4096) {
        MessageBoxW(L"Tin nhắn vượt giới hạn 4096 byte UTF-8.");
        return;
    }
    busy_ = true; UpdateControls();
    SetDlgItemTextW(IDC_STATUS, L"Đang gửi tin...");
    session_.connection.Send(selected_, content);
}

// Dùng chung nguồn avatar cho danh bạ, lời mời và đầu hội thoại.
const std::string& ChatDlg::UserAvatar(int64_t userId) const {
    for (const auto& user : users_) if (user.id == userId) return user.avatar;
    static const std::string empty;
    return empty;
}

// Vẽ lại bảng người dùng cùng trạng thái online và giữ người đang được chọn.
void ChatDlg::ShowUsers() {
    renderingUsers_=true; userList_.SetRedraw(FALSE); userList_.DeleteAllItems();rowKeys_.clear();
    userList_.SetImageList(nullptr,LVSIL_SMALL);avatars_.DeleteImageList();avatars_.Create(32,32,ILC_COLOR24,0,8);
    userList_.SetImageList(&avatars_,LVSIL_SMALL);
    auto add=[&](int64_t key,CString name,CString status,const std::string& avatar) {
        CBitmap picture;picture.Attach(AvatarBitmap(avatar,name,32));
        int icon=avatars_.Add(&picture,static_cast<CBitmap*>(nullptr));
        int row=userList_.InsertItem(static_cast<int>(rowKeys_.size()),name,icon);rowKeys_.push_back(key);
        userList_.SetItemText(row,1,status);
        if(key==(view_==3?selectedInvite_:selected_)) userList_.SetItemState(row,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
    };
    userList_.ShowWindow(view_==3?SW_HIDE:SW_SHOW);
    invitationList_.ShowWindow(view_==3?SW_SHOW:SW_HIDE);
    if(view_==2) {
        for(const auto& room:rooms_) add(-room.id,Wide(room.name),room.owner==userId_?L"Trưởng nhóm":L"Thành viên",room.avatar);
    } else if(view_==3) {
        std::vector<InvitationList::Row> rows;
        for(const auto& invite:invitations_) {
            const bool incoming=invite.receiver==userId_;
            CString name=Wide(incoming?invite.senderName:invite.receiverName);
            CString detail;
            if(invite.kind=="group") detail=(incoming?L"Mời bạn vào nhóm “":L"Bạn đã mời vào nhóm “")+Wide(invite.roomName)+L"”";
            else detail=incoming?L"Gửi lời mời kết bạn":L"Bạn đã gửi lời mời kết bạn";
            rows.push_back({invite.id,name,detail,UserAvatar(incoming?invite.sender:invite.receiver)});
        }
        invitationList_.Populate(std::move(rows),selectedInvite_);
    } else {
        for(const auto& user:users_) {
            if(view_==1 && std::find(friends_.begin(),friends_.end(),user.id)==friends_.end()) continue;
            add(user.id,Wide(user.name),user.online?L"Online":L"Offline",UserAvatar(user.id));
        }
    }
    userList_.SetRedraw(TRUE);userList_.Invalidate();renderingUsers_=false;ShowPeer();
}
// Định dạng hội thoại trong Rich Edit và gắn liên kết tải file.
void ChatDlg::ShowMessages() {
    chatList_.SetRedraw(FALSE);
    chatList_.SetWindowTextW(L"");
    fileLinks_.clear();
    CString peerName;
    for (const auto& user : users_) if (user.id == selected_) peerName = Wide(user.name);
    auto append = [&](const CString& text, COLORREF color, bool bold, bool link = false) {
        chatList_.SetSel(-1, -1);
        CHARFORMAT2 fmt{}; fmt.cbSize = sizeof(fmt);
        fmt.dwMask = CFM_COLOR | CFM_BOLD | CFM_LINK | CFM_UNDERLINE;
        fmt.dwEffects = (bold ? CFE_BOLD : 0) | (link ? CFE_LINK | CFE_UNDERLINE : 0);
        fmt.crTextColor = color;
        long start, end;
        chatList_.GetSel(start, end);
        chatList_.ReplaceSel(text);
        long caret; chatList_.GetSel(caret, end);
        chatList_.SetSel(start, end);
        chatList_.SetSelectionCharFormat(fmt);
        chatList_.SetSel(end, end);
    };
    for (const auto& message : messages_) {
        chatList_.SetSel(-1, -1);
        PARAFORMAT2 para{}; para.cbSize = sizeof(para);
        para.dwMask = PFM_ALIGNMENT | PFM_STARTINDENT | PFM_RIGHTINDENT | PFM_SPACEAFTER;
        para.wAlignment = message.sender == userId_ ? PFA_RIGHT : PFA_LEFT;
        para.dxStartIndent = 180; para.dxRightIndent = 180; para.dySpaceAfter = 100;
        chatList_.SetParaFormat(para);
        const auto color = message.sender == userId_ ? RGB(32, 92, 170) : RGB(40, 50, 65);
        append((message.sender == userId_ ? session_.username : (message.senderName.empty()?peerName:Wide(message.senderName))) + L"  ·  " + Wide(message.time) + L" UTC\r\n", color, true);
        if (!message.filename.empty()) {
            long start, end; chatList_.GetSel(start, end);
            CString label; label.Format(L"Tải file: %s (%lld byte)", Wide(message.filename).GetString(), message.fileSize);
            append(label, RGB(24, 103, 185), false, true);
            long last; chatList_.GetSel(last, end);
            fileLinks_.push_back({start, end, message.id, Wide(message.filename)});
            append(L"\r\n\r\n", color, false);
        } else append(Wide(message.content) + L"\r\n\r\n", color, false);
    }
    chatList_.SetSel(-1, -1);
    chatList_.SendMessage(EM_SCROLLCARET);
    chatList_.SetRedraw(TRUE); chatList_.Invalidate();
}

// Hiện người đang trò chuyện và ảnh đại diện ngay trên hội thoại.
void ChatDlg::ShowPeer() {
    CString name=L"Chọn người hoặc nhóm để trò chuyện",status;
    std::string avatar;
    if(selected_<0) {
        for(const auto& room:rooms_) if(room.id==-selected_) {name=Wide(room.name);status=room.owner==userId_?L" · Trưởng nhóm":L" · Nhóm";avatar=room.avatar;}
    } else for(const auto& user:users_) if(user.id==selected_) {name=Wide(user.name);status=user.online?L" · Online":L" · Offline";
        if(std::find(friends_.begin(),friends_.end(),user.id)==friends_.end()) status=L" · Người lạ"+status;
        avatar=UserAvatar(user.id);}
    SetDlgItemTextW(IDC_PEER,name+status);
    auto* picture=static_cast<CStatic*>(GetDlgItem(IDC_PEER_AVATAR));picture->SetBitmap(nullptr);peerAvatar_.DeleteObject();
    peerAvatar_.Attach(AvatarBitmap(avatar,name,40));picture->SetBitmap(peerAvatar_);
}

// Ghi nhận người vừa chọn, xóa bảng tin cũ và yêu cầu worker tải hội thoại đó.
void ChatDlg::OnUserChanged(NMHDR* header, LRESULT* result) {
    *result = 0;
    const auto* item = reinterpret_cast<NMLISTVIEW*>(header);
    if (renderingUsers_ || !(item->uChanged & LVIF_STATE) || !(item->uNewState & LVIS_SELECTED) ||
        item->iItem < 0 || static_cast<size_t>(item->iItem) >= rowKeys_.size()) return;
    const auto peer = rowKeys_[item->iItem];
    if(view_==3) {selectedInvite_=peer;UpdateControls();return;}
    if (peer == selected_) return;
    selected_ = peer;
    members_.clear(); messages_.clear(); ShowMessages();
    ShowPeer();
    busy_ = true; UpdateControls();
    session_.connection.Select(selected_);
}
// Gửi tác vụ quản lý; giao diện chỉ mở lại thao tác khi worker báo hoàn tất.
void ChatDlg::SendSocial(std::string op,int64_t id,int64_t target,std::string text,std::vector<int64_t> members) {
    busy_=true;UpdateControls();SetDlgItemTextW(IDC_STATUS,L"Đang cập nhật...");
    session_.connection.Social(std::move(op),id,target,std::move(text),std::move(members));
}
void ChatDlg::OnInviteSelected() {
    const auto index=invitationList_.GetCurSel();
    selectedInvite_=index>=0 && static_cast<size_t>(index)<invitationList_.rows.size()?invitationList_.rows[index].id:0;
    UpdateControls();
}
void ChatDlg::OnViewChanged() {
    view_=static_cast<CComboBox*>(GetDlgItem(IDC_VIEW))->GetCurSel();selectedInvite_=0;
    ShowUsers();UpdateControls();CRect rect;GetClientRect(&rect);Layout(rect.Width(),rect.Height());
}
// Danh sách tìm kiếm gồm tài khoản hiện có; gửi lời mời không tự tạo quan hệ bạn bè.
void ChatDlg::OnAddFriend() {
    std::vector<PickDialog::Entry> choices;
    for(const auto& user:users_) if(std::find(friends_.begin(),friends_.end(),user.id)==friends_.end()) choices.push_back({user.id,Wide(user.name)});
    PickDialog dialog(L"Tìm tài khoản để kết bạn",choices);
    if(dialog.DoModal()==IDOK) SendSocial("friend_invite",0,dialog.chosen.front());
}
void ChatDlg::OnCreateGroup() {
    std::vector<PickDialog::Entry> choices;
    for(const auto& user:users_) if(std::find(friends_.begin(),friends_.end(),user.id)!=friends_.end()) choices.push_back({user.id,Wide(user.name)});
    PickDialog dialog(L"Tạo nhóm và mời bạn bè",choices,true,true,false);
    if(dialog.DoModal()==IDOK) {
        view_=2;static_cast<CComboBox*>(GetDlgItem(IDC_VIEW))->SetCurSel(2);
        SendSocial("group_create",0,0,Utf8(dialog.value),dialog.chosen);
        CRect rect;GetClientRect(&rect);Layout(rect.Width(),rect.Height());
    }
}
void ChatDlg::OnAcceptInvite() { if(selectedInvite_) SendSocial("invite_accept",selectedInvite_); }
void ChatDlg::OnDeclineInvite() { if(selectedInvite_) SendSocial("invite_decline",selectedInvite_); }
void ChatDlg::OnCancelInvite() { if(selectedInvite_) SendSocial("invite_cancel",selectedInvite_); }
void ChatDlg::OnMembers() {
    if(selected_>=0 || busy_) return;
    memberAction_="view";busy_=true;UpdateControls();session_.connection.Members(-selected_);
}
// Menu phụ thuộc vai trò; server vẫn kiểm tra quyền ở mọi thao tác.
void ChatDlg::OnGroupMenu() {
    if(selected_>=0 || busy_) return;
    auto it=std::find_if(rooms_.begin(),rooms_.end(),[&](const ChatRoom& r){return r.id==-selected_;});
    if(it==rooms_.end()) return;
    const auto room=*it;const bool owner=room.owner==userId_;
    CMenu menu;menu.CreatePopupMenu();
    if(owner) {
        menu.AppendMenuW(MF_STRING,1,L"Mời bạn vào nhóm");
        menu.AppendMenuW(MF_STRING,2,L"Đổi tên nhóm");
        menu.AppendMenuW(MF_STRING,7,L"Đổi ảnh nhóm");
        menu.AppendMenuW(MF_STRING,3,L"Chuyển trưởng nhóm");
        menu.AppendMenuW(MF_STRING,4,L"Xóa thành viên");
        menu.AppendMenuW(MF_STRING,6,L"Giải tán nhóm");
    }
    menu.AppendMenuW(MF_STRING,5,L"Rời nhóm");
    CRect button;GetDlgItem(IDC_GROUP_MENU)->GetWindowRect(&button);
    auto action=menu.TrackPopupMenu(TPM_RETURNCMD|TPM_LEFTALIGN,button.left,button.bottom,this);
    if(action==7) {
        ChangeAvatar(room.id);
    } else if(action==1) {
        std::vector<PickDialog::Entry> choices;
        for(const auto& user:users_) if(std::find(friends_.begin(),friends_.end(),user.id)!=friends_.end()) choices.push_back({user.id,Wide(user.name)});
        PickDialog dialog(L"Mời bạn vào nhóm",choices);
        if(dialog.DoModal()==IDOK) SendSocial("group_invite",room.id,dialog.chosen.front());
    } else if(action==2) {
        PickDialog dialog(L"Đổi tên nhóm",{},true,false,false,Wide(room.name));
        if(dialog.DoModal()==IDOK) SendSocial("group_rename",room.id,0,Utf8(dialog.value));
    } else if(action==3 || action==4) {
        memberAction_=action==3?"group_transfer":"group_kick";
        busy_=true;UpdateControls();session_.connection.Members(room.id);
    } else if(action==5) {
        if(owner) {MessageBoxW(L"Hãy chuyển trưởng nhóm cho thành viên khác, hoặc chọn Giải tán nhóm.");return;}
        if(MessageBoxW(L"Bạn muốn rời nhóm này?",L"Rời nhóm",MB_YESNO|MB_ICONQUESTION)==IDYES) SendSocial("group_leave",room.id);
    } else if(action==6 && MessageBoxW(L"Giải tán sẽ thu hồi quyền truy cập nhóm của tất cả thành viên. Tiếp tục?",L"Giải tán nhóm",MB_YESNO|MB_ICONWARNING)==IDYES) SendSocial("group_delete",room.id);
}
// Hiện danh sách mới nhất nhận từ server trước khi chọn người để kick/chuyển quyền.
void ChatDlg::ProcessMembers(const ChatEvent& event) {
    members_=event.members;
    const auto action=std::move(memberAction_);memberAction_.clear();
    if(action.empty()) return;
    std::vector<PickDialog::Entry> choices;
    for(const auto& member:members_) {
        if(action!="view" && member.id==userId_) continue;
        choices.push_back({member.id,Wide(member.name)+(member.owner?L" — Trưởng nhóm":L"")});
    }
    PickDialog dialog(action=="view"?L"Thành viên nhóm":action=="group_kick"?L"Chọn thành viên cần xóa":L"Chọn trưởng nhóm mới",choices,false,false,action!="view");
    if(dialog.DoModal()==IDOK && action!="view") SendSocial(action,-event.peer,dialog.chosen.front());
}

// Chỉ liên kết file do chương trình tạo mới được tải; không tự mở file vừa nhận.
void ChatDlg::OnFileLink(NMHDR* header, LRESULT* result) {
    *result = 0;
    const auto* link = reinterpret_cast<ENLINK*>(header);
    if (busy_ || link->msg != WM_LBUTTONUP) return;
    for (const auto& file : fileLinks_) if (link->chrg.cpMin < file.end && link->chrg.cpMax > file.start) {
        CFileDialog dialog(FALSE, nullptr, file.name, OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
                           L"Tất cả file (*.*)|*.*||", this);
        if (dialog.DoModal() != IDOK) return;
        busy_ = true; UpdateControls();
        SetDlgItemTextW(IDC_STATUS, L"Đang tải file...");
        session_.connection.Download(file.id, dialog.GetPathName().GetString());
        *result = 1; return;
    }
}

// Chọn file và chuyển đường dẫn cho luồng mạng đọc/gửi, tối đa 20 MB.
void ChatDlg::OnSendFile() {
    if (busy_ || !selected_) return;
    CFileDialog dialog(TRUE, nullptr, nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                       L"Tất cả file (*.*)|*.*||", this);
    if (dialog.DoModal() != IDOK) return;
    busy_ = true; UpdateControls(); SetDlgItemTextW(IDC_STATUS, L"Đang gửi file...");
    session_.connection.SendFile(selected_, dialog.GetPathName().GetString());
}

// Chuẩn hóa ảnh người dùng chọn thành PNG 96x96 trước khi gửi lên server.
void ChatDlg::OnAvatar() { ChangeAvatar(0); }
void ChatDlg::ChangeAvatar(int64_t room) {
    CFileDialog dialog(TRUE, nullptr, nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                       L"Ảnh (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp||", this);
    if (dialog.DoModal() != IDOK) return;
    std::error_code error;
    const auto size = std::filesystem::file_size(dialog.GetPathName().GetString(), error);
    if (error || size > 20 * 1024 * 1024) { MessageBoxW(L"Chọn ảnh dưới 20 MB."); return; }
    CImage source;
    if (FAILED(source.Load(dialog.GetPathName())) || source.IsNull() || source.GetWidth() <= 0 || source.GetHeight() <= 0) {
        MessageBoxW(L"Không đọc được ảnh này."); return;
    }
    CImage image; image.Create(96, 96, 24);
    HDC dc = image.GetDC();
    RECT rect{0, 0, 96, 96}; FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SetStretchBltMode(dc, HALFTONE);
    const int side = (std::min)(source.GetWidth(), source.GetHeight());
    source.Draw(dc, 0, 0, 96, 96, (source.GetWidth()-side)/2, (source.GetHeight()-side)/2, side, side);
    image.ReleaseDC();
    CComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || FAILED(image.Save(stream, Gdiplus::ImageFormatPNG))) {
        MessageBoxW(L"Không chuyển được ảnh sang PNG."); return;
    }
    STATSTG stat{}; stream->Stat(&stat, STATFLAG_NONAME);
    std::string png(static_cast<size_t>(stat.cbSize.QuadPart), '\0');
    LARGE_INTEGER zero{}; stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    if (FAILED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &read)) || read != png.size()) return;
    busy_ = true; UpdateControls(); SetDlgItemTextW(IDC_STATUS, L"Đang đổi ảnh...");
    session_.connection.SetAvatar(std::move(png),room);
}

// Giữ các cột và vùng soạn tin ngay hàng khi người dùng thay kích thước cửa sổ.
void ChatDlg::OnSize(UINT type, int cx, int cy) {
    CDialog::OnSize(type, cx, cy);
    if (type != SIZE_MINIMIZED) Layout(cx, cy);
}
// Bố trí control dùng chung khi khởi tạo và khi cửa sổ đổi kích thước.
void ChatDlg::Layout(int cx, int cy) {
    if (auth_ || !chatList_.GetSafeHwnd()) return;
    const int margin = 16, left = 250, gap = 16, x = margin + left + gap;
    const int width = (std::max)(300, cx - x - margin);
    const int bottom = cy - 48, inputTop = bottom - 90;
    auto move = [&](int id, int px, int py, int w, int h) { GetDlgItem(id)->MoveWindow(px, py, w, h); };
    move(IDC_MY_AVATAR, margin, 16, 40, 40); move(IDC_ACCOUNT, 68, 12, cx - 320, 30);
    move(IDC_AVATAR, 68, 44, 100, 28);
    move(IDC_DISCONNECT, cx-120, 20, 104, 32);
    move(IDC_USERS_LABEL, margin, 80, left, 20);
    move(IDC_VIEW,margin,104,left,180);
    move(IDC_ADD_FRIEND,margin,140,118,30);move(IDC_NEW_GROUP,margin+128,140,122,30);
    move(IDC_ACCEPT,margin,bottom-34,76,30);move(IDC_DECLINE,margin+82,bottom-34,76,30);move(IDC_CANCEL_INVITE,margin+164,bottom-34,86,30);
    move(IDC_MEMBERS,cx-228,90,104,30);move(IDC_GROUP_MENU,cx-116,90,100,30);
    move(IDC_PEER_AVATAR, x, 80, 40, 40); move(IDC_PEER, x+52, 92, width-290, 24);
    move(IDC_USERS, margin, 180, left, bottom-180-(view_==3?42:0));
    move(IDC_INVITE_LIST,margin,180,left,bottom-222);invitationList_.Reflow();
    move(IDC_CHAT, x, 132, width, inputTop-170);
    move(IDC_INPUT_LABEL, x, inputTop-28, width, 22);
    move(IDC_INPUT, x, inputTop, width-106, 90);
    move(IDC_SEND, cx-110, inputTop, 94, 38); move(IDC_FILE, cx-110, inputTop+50, 94, 38);
    move(IDC_STATUS, margin, cy-30, cx-2*margin, 22);
    userList_.SetColumnWidth(0, left-90); userList_.SetColumnWidth(1, 86);
}
void ChatDlg::OnGetMinMaxInfo(MINMAXINFO* info) {
    CDialog::OnGetMinMaxInfo(info);
    if (!auth_) { info->ptMinTrackSize.x = 900; info->ptMinTrackSize.y = 620; }
}
HBRUSH ChatDlg::OnCtlColor(CDC* dc, CWnd* window, UINT type) {
    auto brush = CDialog::OnCtlColor(dc, window, type);
    if (type == CTLCOLOR_DLG || type == CTLCOLOR_STATIC) {
        dc->SetBkColor(RGB(245,247,251)); dc->SetTextColor(RGB(35,45,60));
        return background_;
    }
    return brush;
}

// Đóng dialog hiện tại với IDRETRY để App mở màn hình kế tiếp bằng cùng phiên.
void ChatDlg::SwitchScreen() {
    session_.Attach(nullptr);
    // Để App mở dialog kế tiếp khi dialog hiện tại đóng.
    AfxGetApp()->m_pMainWnd = nullptr;
    EndDialog(IDRETRY);
}

// Lấy ChatEvent từ hàng đợi rồi cập nhật UI hoặc chuyển màn hình theo loại sự kiện.
LRESULT ChatDlg::OnChatEvent(WPARAM, LPARAM) {
    ChatEvent event;
    ChatEvent pendingMembers; bool haveMembers=false;
    while (session_.Next(event)) {
        switch (event.kind) {
        case ChatEvent::Kind::Status:
            memberAction_.clear();
            SetDlgItemTextW(IDC_STATUS, Wide(event.text));
            break;
        case ChatEvent::Kind::Login:
            session_.userId = event.userId;
            session_.username = Wide(event.text);
            session_.status.Empty();
            SwitchScreen();
            return 0;
        case ChatEvent::Kind::Logout:
            session_.userId = 0;
            if (!auth_) {
                SwitchScreen();
                return 0;
            }
            break;
        case ChatEvent::Kind::Users:
            if (!auth_) {
                users_.clear();
                for (auto& user : event.users) {
                    if (user.id == userId_) {
                        auto* picture = static_cast<CStatic*>(GetDlgItem(IDC_MY_AVATAR));
                        picture->SetBitmap(nullptr); ownAvatar_.DeleteObject();
                        ownAvatar_.Attach(AvatarBitmap(user.avatar, session_.username, 40)); picture->SetBitmap(ownAvatar_);
                    } else users_.push_back(std::move(user));
                }
                ShowUsers();
            }
            break;
        case ChatEvent::Kind::Directory:
            if(!auth_) {
                friends_=std::move(event.friends);rooms_=std::move(event.rooms);invitations_=std::move(event.invitations);
                if(selected_<0 && std::none_of(rooms_.begin(),rooms_.end(),[&](const ChatRoom& r){return r.id==-selected_;})) {
                    selected_=0;members_.clear();messages_.clear();ShowMessages();
                    SetDlgItemTextW(IDC_STATUS,L"Bạn không còn quyền truy cập nhóm đã mở.");
                }
                ShowUsers();
            }
            break;
        case ChatEvent::Kind::Members:
            if(!auth_ && event.peer==selected_) {pendingMembers=event;haveMembers=true;}
            break;
        case ChatEvent::Kind::Messages:
            if (!auth_ && event.peer == selected_) {
                if (messages_.size() != event.messages.size() ||
                    (!event.messages.empty() && (messages_.empty() || messages_.back().id != event.messages.back().id))) {
                    messages_ = std::move(event.messages); ShowMessages();
                }
            }
            break;
        case ChatEvent::Kind::Sent: {
            CString current;
            GetDlgItemTextW(IDC_INPUT, current);
            if (event.peer == selected_ && Utf8(current) == event.text)
                SetDlgItemTextW(IDC_INPUT, L"");
            break;
        }
        case ChatEvent::Kind::Busy:
            busy_ = false;
            break;
        }
    }
    UpdateControls();
    if(haveMembers) ProcessMembers(pendingMembers);
    return 0;
}
// Bỏ gắn cửa sổ nhận sự kiện rồi đóng dialog.
void ChatDlg::OnCancel() {
    session_.Attach(nullptr);
    CDialog::OnCancel();
}
