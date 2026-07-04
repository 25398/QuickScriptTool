#pragma once
// ──────────────────────────────────────────────────────────────────
// app_branding.h — 关于页品牌信息（版本、官网、联系方式等）
// 通过 Set* 接口可在运行时更新，便于后续配置或远程下发。
// ──────────────────────────────────────────────────────────────────

#include <string>

namespace quickscript {

class AppBranding {
public:
    static const std::wstring& AppDisplayName();
    static const std::wstring& Version();
    static const std::wstring& Tagline();
    static const std::wstring& WebsiteUrl();
    static const std::wstring& ContactInfo();
    static const std::wstring& QqGroup();
    static const std::wstring& CopyrightText();

    static void SetVersion(std::wstring value);
    static void SetTagline(std::wstring value);
    static void SetWebsiteUrl(std::wstring value);
    static void SetContactInfo(std::wstring value);
    static void SetQqGroup(std::wstring value);
    static void SetCopyrightText(std::wstring value);

private:
    static std::wstring appDisplayName_;
    static std::wstring version_;
    static std::wstring tagline_;
    static std::wstring websiteUrl_;
    static std::wstring contactInfo_;
    static std::wstring qqGroup_;
    static std::wstring copyrightText_;
};

}  // namespace quickscript
