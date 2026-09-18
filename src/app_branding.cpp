#include "app_branding.h"

namespace quickscript {

std::wstring AppBranding::appDisplayName_ = L"键鼠工坊";
std::wstring AppBranding::version_ = L"v1.3.3";
std::wstring AppBranding::tagline_;
std::wstring AppBranding::websiteUrl_ = L"https://www.quickscripttool.cloud/";
std::wstring AppBranding::contactInfo_ = L"24353623@qq.com";
std::wstring AppBranding::qqGroup_ = L"2163074732";
std::wstring AppBranding::copyrightText_;

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
