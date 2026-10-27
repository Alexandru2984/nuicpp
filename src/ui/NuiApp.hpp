#pragma once

#include <string>

namespace nuigraph {

std::string renderLoginPage(const std::string& error, const std::string& assetVersion = "",
                            bool totpRequired = false);
std::string renderShareUnlockPage(const std::string& slug, const std::string& error,
                                  const std::string& assetVersion = "");
std::string renderEditorPage(const std::string& username, const std::string& baseUrl,
                             bool indexable = true, const std::string& assetVersion = "",
                             bool administrator = false);
std::string renderAdminSessionsPage(const std::string& assetVersion = "");
std::string renderDocsPage(const std::string& baseUrl, const std::string& assetVersion = "");
std::string renderPrivacyPage(const std::string& baseUrl, int trashRetentionDays,
                              const std::string& assetVersion = "");

} // namespace nuigraph
