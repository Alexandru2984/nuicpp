#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace nuigraph {

std::string renderLoginPage(const std::string& error, const std::string& assetVersion = "",
                            bool totpRequired = false);
std::string renderShareUnlockPage(const std::string& slug, const std::string& error,
                                  const std::string& assetVersion = "");
std::string renderEditorPage(const std::string& username, const std::string& baseUrl,
                             bool indexable = true, const std::string& assetVersion = "",
                             bool administrator = false,
                             const std::string& canonicalPath = "/",
                             const std::string& pageTitle = "",
                             const std::string& pageDescription = "",
                             bool softwareApplication = true);
std::string renderGalleryPage(const std::string& baseUrl,
                              const nlohmann::json& diagrams,
                              long total, long offset, long limit,
                              const std::string& assetVersion = "");
std::string renderAdminSessionsPage(const std::string& assetVersion = "");
std::string renderDocsPage(const std::string& baseUrl, const std::string& assetVersion = "");
std::string renderPrivacyPage(const std::string& baseUrl, int trashRetentionDays,
                              const std::string& assetVersion = "");

} // namespace nuigraph
