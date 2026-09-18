#pragma once
// 压缩网页可访问性快照（对齐 Playwright MCP：交互节点 + ref=eN）。
// 扩展抓树，宿主解析/截断后喂规划模型。canvas 页禁止走 HTML。

#include <string>
#include <vector>

enum class PageKind {
    Unknown = 0,
    Dom,
    Mixed,
    Canvas,
};

struct PageSnapshotNode {
    std::wstring ref;
    std::wstring role;
    std::wstring name;
    std::wstring path;
    std::wstring value;
    std::wstring href;
    bool checked = false;
    bool inView = true;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int screenX = 0;
    int screenY = 0;
};

struct PageSnapshot {
    PageKind kind = PageKind::Unknown;
    double canvasRatio = 0.0;
    int interactive = 0;
    int vw = 0;
    int vh = 0;
    int scrollY = 0;
    int pageHeight = 0;
    /// 扩展统计的主区可视内容数；-1 表示旧快照未提供，由节点推断。
    int contentInView = -1;
    /// observePage(query) 控件名命中数；-1 表示未带 query。
    int queryHits = -1;
    std::wstring query;
    bool skippedHtml = false;
    bool attachedNow = false;
    std::wstring url;
    std::wstring title;
    std::wstring error;
    std::vector<PageSnapshotNode> nodes;
};

/// 与扩展 JS 同一套阈值：大画布且可交互很少 → canvas；否则有明显画布 → mixed。
PageKind ClassifyPageKind(double canvasRatio, int interactiveCount);

const wchar_t* PageKindName(PageKind kind);
PageKind ParsePageKindName(const std::wstring& name);

/// Playwright 式 ref：e1 … e9999
bool LooksLikePageSnapshotRef(const std::wstring& ref);

PageSnapshot ParsePageSnapshotJson(const std::string& jsonUtf8);

/// 节点是否与视口相交（缺 inView 时用 y/vh 推断）。
bool PageSnapshotNodeInView(const PageSnapshotNode& n, const PageSnapshot& snap);
/// 视口主区里像列表卡片的项（排除顶栏）。≥3 则不应先滚轮。
int CountPageSnapshotInViewMainContent(const PageSnapshot& snap);

/// 规划模型可读文本。默认上限 14KB（目标 8–16KB）。
constexpr size_t kMaxPageSnapshotChars = 14000;
std::wstring FormatPageSnapshotForAgent(const PageSnapshot& snap,
    size_t maxChars = kMaxPageSnapshotChars);

/// 站点 JSON/搜索 API（api.*、/x/web-interface 等）。打开或抓取会触发风控。
bool LooksLikeNonUserFacingWebUrl(const std::wstring& url);
/// space.bilibili.com 根路径（空主页）。
bool LooksLikeEmptyUserSpaceUrl(const std::wstring& url);
/// space.bilibili.com 根路径或纯数字 UID（猜主页，不是搜索结果）。
bool LooksLikeGuessedUserSpaceUrl(const std::wstring& url);
/// space.bilibili.com 任意用户空间（含有稿件列表的站内搜）。
bool LooksLikeUserSpaceSiteUrl(const std::wstring& url);
/// 播放页 / 列表内容卡（/video/、/bangumi/、/watch）。
bool LooksLikeWatchVideoSiteUrl(const std::wstring& url);
/// 用户主页等「身份卡」才 tabs.update；列表内容卡应点元素（与看见的第 1 张对齐）。
bool ShouldDirectNavigateSnapshotHref(const std::wstring& url);
/// 视口里重复内容卡网格的第 1 项 ref（同类卡片最上最左）。不足 2 张则空。
std::wstring SnapshotListFirstRef(const PageSnapshot& snap);
/// 树上名字能对上 locate 描述的 ref（优先 button；内容卡 href 降权）。
std::wstring SnapshotRefMatchingLocateTarget(const PageSnapshot& snap, const std::wstring& target);

/// PickPageSnapshotRefForText 结果：DOM 优先点击的候选判定。
struct PageSnapshotPick {
    /// 可信命中的 ref；空 = 树上没有可信命中，调用方应回退识图。
    std::wstring ref;
    std::wstring name;
    std::wstring role;
    int score = 0;
    /// 名字与短标签完全相等（含剥「按钮/图标」等通用后缀后相等）
    bool exact = false;
    /// 仅部分命中且存在近似竞争项 → DOM 点击不可信，应回退识图
    bool ambiguous = false;
    /// 参与打分的候选数（含被否掉的）
    int candidates = 0;
    /// 与最优同名的候选数（>1 表示同名多项，取阅读顺序最前，不算歧义）
    int sameNameCount = 0;
};

/// 目标控件类型：点击类（按钮/链接）还是填写类（输入框）。
/// 两者对 role 的加权不同——「密码」当点击目标会选到「忘记密码」链接，
/// 当填写目标则要选到 textbox，不能用同一套打分。
enum class PageSnapshotPickKind {
    Clickable = 0,
    Input,
};

/// 从控件树里按自然语言短标签挑一个可操作 ref —— 浏览器 DOM 优先路径用。
/// 规则与移动端「一次调用解决」对齐（browser-use/Stagehand 式 indexed click）：
/// 名字完全相等 > 前缀 > 包含 > 反向包含；目标 role 加权重，超大面积降权，
/// 阅读顺序靠前优先。只有部分命中且存在近似竞争项时才判 ambiguous（交回识图）。
PageSnapshotPick PickPageSnapshotRefForText(const PageSnapshot& snap, const std::wstring& text,
    PageSnapshotPickKind kind = PageSnapshotPickKind::Clickable);

/// 把自然语言短标签压成扩展 observePage(query=…) 的关键字。
/// 扩展侧过滤是「控件名包含关键字」，整句「点击登录按钮」必然 0 命中；
/// 压成「登录」才有召回，多出来的噪声交给 PickPageSnapshotRefForText 打分过滤。
std::wstring PageSnapshotTargetKeyword(const std::wstring& target);
/// search.* / keyword= 等全站搜索结果页。
bool LooksLikeSiteSearchResultsUrl(const std::wstring& url);
/// 同文档（主机+路径相同，忽略 ?vt= 等查询串）。
bool PageUrlsSameDocument(const std::wstring& a, const std::wstring& b);
/// 树上 href → 可 tabs.update 的绝对 URL。同路径筛选项/裸 UID 相对路径返回空。
std::wstring ResolvePageNavigationUrl(const std::wstring& href, const std::wstring& pageUrl);
/// 去掉「搜索」等无判别力的词，避免树上只剩搜索框。
std::wstring SanitizeObservePageQuery(const std::wstring& query);
/// Edge/Chrome 窗口标题（勿把游戏/微信绑窗名当网页 hint）。
bool LooksLikeBrowserWindowTitle(const std::wstring& title);

/// 从浏览器**窗口标题**里剥出「当前标签页标题」。
/// 窗口标题形如「设置 和另外 4 个页面 - 个人 - Microsoft Edge」；扩展按标签标题匹配，
/// 带装饰的整串匹配不上，就会一直挂在旧标签上（实测：前台已是「设置」页，
/// observePage 却始终回游戏的 DOM 树，导致连续 10 轮对着错页面操作）。
std::wstring StripBrowserWindowTitleDecorations(const std::wstring& title);
/// 注册域粗提取：search.bilibili.com → bilibili.com（用于同站禁止再编 URL）。
std::wstring ExtractUrlSiteKey(const std::wstring& url);
/// 查询串百分号编码（UTF-8）。
std::wstring UrlEncodeQuery(const std::wstring& text);
/// 按当前页构造全站搜索 URL（Playwright goto 式，不猜 UID）。未知站点返回空。
std::wstring BuildSiteSearchUrl(const std::wstring& query, const std::wstring& currentUrl);
/// 站点首页（路径为 / 或空，可带 query）。用于 typeRef 提交未跳转时改走搜索 URL。
bool LooksLikeSiteHomepageUrl(const std::wstring& url);
