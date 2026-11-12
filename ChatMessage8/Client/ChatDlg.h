#pragma once
#include "framework.h"
#include <deque>
#include <mutex>
#include "ChatConnection.h"
#include "resource.h"
#include <afxrich.h>

constexpr UINT WM_CHAT_EVENT = WM_APP + 1;
struct ClientSession {
    // Tạo kết nối mạng với callback xếp ChatEvent và báo cho cửa sổ đang gắn.
    ClientSession();
    // Dừng kết nối và các luồng mạng trước khi hủy phiên.
    ~ClientSession();
    // Gắn cửa sổ nhận sự kiện mạng, hoặc bỏ gắn khi chuyển/đóng dialog.
    void Attach(HWND window);
    // Lấy một ChatEvent đầu hàng đợi để luồng giao diện xử lý.
    bool Next(ChatEvent& event);
    ChatConnection connection;
    int64_t userId = 0;
    CString username, host = L"127.0.0.1", port = L"8080", status;
private:
    std::mutex mutex_;
    std::deque<ChatEvent> events_;
    HWND window_ = nullptr;
};

// Danh sách lời mời tự đo chiều cao từng mục để tên nhóm dài được xuống dòng.
class InvitationList : public CListBox {
public:
    struct Row { int64_t id; CString name, detail; std::string avatar; };
    std::vector<Row> rows;
    void Populate(std::vector<Row> items, int64_t selected);
    void Reflow();
    int RowHeight(CDC& dc, const Row& row, int width);
    void DrawItem(LPDRAWITEMSTRUCT item) override;
    void MeasureItem(LPMEASUREITEMSTRUCT item) override;
private:
    CFont bold_;
    void PrepareFont();
};

class ChatDlg : public CDialog {
public:
    // Chọn mẫu đăng nhập/chat và gắn dialog với ClientSession được truyền vào.
    explicit ChatDlg(ClientSession& session) : CDialog(session.userId ? IDD_CHAT : IDD_AUTH), // Nếu user khác 0 dùng mẫu cửa sổ chat, = 0 thì là đăng nhập(khác 0 = true)
        session_(session), auth_(!session.userId), userId_(session.userId) {}
protected:
    // Khởi tạo các ô tài khoản hoặc bảng hội thoại theo loại dialog đang mở.
    BOOL OnInitDialog() override;
    // Bỏ gắn cửa sổ nhận sự kiện rồi đóng dialog.
    void OnCancel() override;
    // Xử lý phím Enter để xác nhận khi đang ở màn hình tài khoản và chưa bận.
    void OnOK() override;
    // Đọc IP/cổng và xếp công việc kiểm tra kết nối lên worker.
    afx_msg void OnCheck();
    // Chuyển màn hình tài khoản giữa chế độ đăng nhập và đăng ký.
    afx_msg void OnMode();
    // Xử lý nút xác nhận bằng Authenticate() theo chế độ tài khoản đang chọn.
    afx_msg void OnLogin();
    // Khóa thao tác đang xử lý và xếp Job đăng xuất lên worker.
    afx_msg void OnLogout();
    // Đọc nội dung và người nhận đang chọn rồi gọi connection.Send() để xếp Job gửi tin.
    afx_msg void OnSend();
    // Xếp Job cập nhật danh sách người dùng và tin của hội thoại đang mở.
    afx_msg void OnViewChanged();
    afx_msg void OnInviteSelected();
    afx_msg void OnAddFriend();
    afx_msg void OnCreateGroup();
    afx_msg void OnAcceptInvite();
    afx_msg void OnDeclineInvite();
    afx_msg void OnCancelInvite();
    afx_msg void OnMembers();
    afx_msg void OnGroupMenu();
    // Ghi nhận người vừa chọn, xóa bảng tin cũ và yêu cầu worker tải hội thoại đó.
    afx_msg void OnUserChanged(NMHDR*, LRESULT*);
    // Xử lý liên kết tải file trong vùng hội thoại Rich Edit.
    afx_msg void OnFileLink(NMHDR*, LRESULT*);
    afx_msg void OnSendFile();
    afx_msg void OnAvatar();
    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);
    afx_msg HBRUSH OnCtlColor(CDC* dc, CWnd* window, UINT type);
    // Lấy ChatEvent từ hàng đợi rồi cập nhật UI hoặc chuyển màn hình theo loại sự kiện.
    afx_msg LRESULT OnChatEvent(WPARAM, LPARAM);
    DECLARE_MESSAGE_MAP()
private:
    // Đọc IP/cổng từ client.ini cạnh chương trình, kiểm tra rồi lưu vào phiên.
    bool ReadEndpoint(std::string& host, int& port);
    // Đóng dialog hiện tại với IDRETRY để App mở màn hình kế tiếp bằng cùng phiên.
    void SwitchScreen();
    // Đọc tài khoản/mật khẩu, kiểm tra nhập lại rồi xếp Job đăng ký hoặc đăng nhập.
    void Authenticate(bool registration);
    // Bật/tắt và ẩn/hiện các nút, ô nhập theo màn hình, đăng nhập và trạng thái bận.
    void UpdateControls();
    // Vẽ lại bảng người dùng cùng trạng thái online và giữ người đang được chọn.
    void ShowUsers();
    // Định dạng hội thoại trong Rich Edit và gắn liên kết tải file.
    void ShowMessages();
    ClientSession& session_;
    bool auth_;
    bool registration_ = false;
    std::vector<ChatUser> users_;
    std::vector<ChatMessage> messages_;
    std::vector<int64_t> friends_, rowKeys_;
    std::vector<ChatRoom> rooms_;
    std::vector<ChatInvite> invitations_;
    std::vector<ChatMember> members_;
    int view_ = 0;
    int64_t selectedInvite_ = 0;
    std::string memberAction_;
    void SendSocial(std::string op, int64_t id=0, int64_t target=0, std::string text={}, std::vector<int64_t> members={});
    void ProcessMembers(const ChatEvent& event);
    CListCtrl userList_;
    InvitationList invitationList_;
    CRichEditCtrl chatList_;
    CImageList avatars_;
    CBitmap ownAvatar_, peerAvatar_;
    CFont accountFont_;
    CBrush background_{RGB(245, 247, 251)};
    struct FileLink { long start, end; int64_t id; CString name; };
    std::vector<FileLink> fileLinks_;
    void ShowPeer();
    const std::string& UserAvatar(int64_t userId) const;
    void Layout(int cx, int cy);
    void ChangeAvatar(int64_t room);
    int64_t userId_ = 0;
    int64_t selected_ = 0;
    bool busy_ = false;
    bool renderingUsers_ = false;
};
