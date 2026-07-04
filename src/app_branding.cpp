#include "app_branding.h"

namespace quickscript {

std::wstring AppBranding::appDisplayName_ = L"鼠大侠";
std::wstring AppBranding::version_ = L"v1.0.0";
std::wstring AppBranding::tagline_ = L"最多人用的鼠标连点器";
std::wstring AppBranding::websiteUrl_;
std::wstring AppBranding::contactInfo_;
std::wstring AppBranding::qqGroup_;
std::wstring AppBranding::copyrightText_ = L"Copyright (C) shudaxia.com Inc. All Right Reserved";

const std::wstring& AppBranding::AppDisplayName() { return appDisplayName_; }
const std::wstring& AppBranding::Version() { return version_; }
const std::wstring& AppBranding::Tagline() { return tagline_; }
const std::wstring& AppBranding::WebsiteUrl() { return websiteUrl_; }
const std::wstring& AppBranding::ContactInfo() { return contactInfo_; }
const std::wstring& AppBranding::QqGroup() { return qqGroup_; }
const std::wstring& AppBranding::CopyrightText() { return copyrightText_; }

void AppBranding::SetVersion(std::wstring value) { version_ = std::move(value); }
void AppBranding::SetTagline(std::wstring value) { tagline_ = std::move(value); }
void AppBranding::SetWebsiteUrl(std::wstring value) { websiteUrl_ = std::move(value); }
void AppBranding::SetContactInfo(std::wstring value) { contactInfo_ = std::move(value); }
void AppBranding::SetQqGroup(std::wstring value) { qqGroup_ = std::move(value); }
void AppBranding::SetCopyrightText(std::wstring value) { copyrightText_ = std::move(value); }

}  // namespace quickscript
