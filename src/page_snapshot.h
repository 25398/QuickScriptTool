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
    /// ★★ **容器标识**（2026-10-03 方向 1）：扩展按"最近的、含 ≥2 个同构孩子的祖先"
    /// 算出的稳定 id（`TAG.class@idx`）⇒ 宿主按它分组，**不依赖坐标** ⇒ 对滚动稳定 ✓
    /// ⚠ 通用性：判据是**结构**（同构兄弟）而不是 `radio/checkbox` 语义 ⇒ 列表型页面通吃 ✓
    std::wstring group;
    std::wstring value;
    std::wstring href;
    bool checked = false;
    bool inView = true;
    /// ★内容与视口**完全不相交**（需要滚动才看得见）。当前扩展里它与 inView 同源
    ///   （同一判据）⇒ 恒有 `absent == !inView`；两个都留着是因为**语义不同**：
    ///   inView 算「可视 N」，absent 算「屏外 M」。将来若要区分「横跨边界」的
    ///   部分可见节点（算可视但不算在屏内），只需改扩展那一行。
    ///   -1 = 旧扩展未提供，此时由 y/vh 回落到旧推断（见 PageSnapshotNodeAbsent）。
    int absent = -1;
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
    /// ★本页**可枚举的控件总数**（扩展在分页前的完整候选数）；-1 = 旧扩展未提供。
    ///   有了它才能判断「树是不是被砍过」—— 只看 nodes.size() 永远看不出来。
    int enumerateTotal = -1;
    /// 本次切片在 enumerateTotal 里的起点（offset 分页用）。
    int offset = 0;
    /// 下一段的 offset；0 = 已经取完（配合 enumerateTotal/offset 判「有没有漏」）。
    int nextOffset = 0;
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
/// 节点内容是否**完全**在视口之外（需要滚动才看得见）。
/// 与 PageSnapshotNodeInView 不同：横跨可视区边界的节点「不在 inView 但也不 absent」。
/// 优先用扩展上报的 absent 字段；旧扩展缺失时回落到 y/vh 推断。
bool PageSnapshotNodeAbsent(const PageSnapshotNode& n, const PageSnapshot& snap);
/// 视口主区里像列表卡片的项（排除顶栏）。≥3 则不应先滚轮。
int CountPageSnapshotInViewMainContent(const PageSnapshot& snap);

/// 规划模型可读文本。默认上限 14KB（目标 8–16KB）。
constexpr size_t kMaxPageSnapshotChars = 14000;
std::wstring FormatPageSnapshotForAgent(const PageSnapshot& snap,
    size_t maxChars = kMaxPageSnapshotChars);

/// 站点 JSON/搜索 API（api.*、/x/web-interface 等）。打开或抓取会触发风控。
bool LooksLikeNonUserFacingWebUrl(const std::wstring& url);
// ★批 D（docs §47 / D1）删掉的**站点专属启发式**（判据里写死具体域名/URL 形状 ⇒
//   换个网站就坏，D-c）：某站空间 URL / 空根路径 / 纯数字 UID 三条、播放页 URL 形状、
//   「用户主页才直接导航」、快照里的「列表第1项」、搜索结果页 URL 形状。
//   它们的消费点（放行/拒绝 openWebpage、挡滚动、挡点别的卡、引擎代导航…）一并删除；
//   删除后「没它就不知道该拦还是放行」的地方按 D1 口径**放行**（引擎不决策）。
//   ⚠ 具体符号名与完整叙述见 docs §47 —— 源码注释里不留已删符号名（审计会算命中）。
/// 树上名字能对上 locate 描述的 ref（优先 button）。
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

// ── 批量选择题流水线（新路由，2026-10-02 用户批准）────────────────────────
// 树里出现「组标记 + 每组成员选项」的批量选择页（题库/表单）时，
// 不再让模型自由发挥，改走固定流水线：逐题短提示 → 只回答一个字母 → clickRef。

struct QuizChoiceOption {
    std::wstring letter;   // 选项字母（名字开头的 A/B/C/D…；取不到则整组弃用）
    std::wstring text;     // 选项文字（树里 radio 的名字）
    std::wstring ref;      // 扩展树的 ref（直接给 clickRef）
    int y = 0;
};

struct QuizChoiceGroup {
    std::wstring title;    // 组标记名（如「题目 3.」）
    std::vector<QuizChoiceOption> options;
};

/// 把树里的 radio 按**视觉 y 坐标**归入其上方的组标记（ref 编号顺序不可靠，坐标可靠）。
/// 判据：≥2 个组、每组 ≥minOptionsPerGroup 个选项（默认 2）；字母取名字首个 A-Z。
/// 返回 false ⇒ 不满足批量选择页形状，调用方照旧走 agent 循环。
bool BuildQuizChoiceGroups(const PageSnapshot& snap, std::vector<QuizChoiceGroup>& out,
                           int minOptionsPerGroup = 2);

/// 第 N 题的**极短**决策提示（只做判断题，不给工具，豆包想得快）。
std::wstring FormatQuizSingleQuestionPrompt(const QuizChoiceGroup& g,
                                            int questionNo, int totalQuestions);

/// 从模型回复里取答案字母：优先"整条回复只有一个 A-D 字母"，否则取第一个大写字母；
/// 找不到返回 0。纯函数（自检可逐格断言）。
wchar_t ParseQuizAnswerLetter(const std::wstring& reply);

/// ★★★ **一次性**拼「本页全部题目」的提示词（2026-10-03 用户要求"一次传全"）。
///
/// 目的：**1 轮 API 换 N 轮** —— 逐题问要 N 轮（真机每题 10~15 秒），
/// 一次问只要 1 轮 ⇒ 20 题从 3~5 分钟降到 ~15 秒 ✓
/// ⚠ 提示词里**只说一次输出格式**，每题只列题干 + 选项（不重复啰嗦）✓
std::wstring FormatQuizAllQuestionsPrompt(const std::vector<QuizChoiceGroup>& groups);

/// ★★★ 解析「一次返回的答案数组」（如 `["A","C","?","D"]`）。
/// 容错：允许裸字母串（`AC?D`）、带题号（`1.A 2.C`）、代码块围栏 ⇒ 一律抽出字母。
/// @param count 期望个数（= 题目数）；不足的位置填 0（表示"没答出来"）
std::vector<wchar_t> ParseQuizAnswerArray(const std::wstring& reply, size_t count);

/// 在树里找「下一页」类翻页控件（role=button/link，名字含 下一页/下一頁/下页/next/后一页/›/»/→，
/// 且短文本）。通用判据（任何分页站点），返回空串表示没有。收割循环靠它翻页。
std::wstring FindNextPageControlRef(const PageSnapshot& snap);

/// 把自然语言短标签压成扩展 observePage(query=…) 的关键字。
/// 扩展侧过滤是「控件名包含关键字」，整句「点击登录按钮」必然 0 命中；
/// 压成「登录」才有召回，多出来的噪声交给 PickPageSnapshotRefForText 打分过滤。
std::wstring PageSnapshotTargetKeyword(const std::wstring& target);
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
