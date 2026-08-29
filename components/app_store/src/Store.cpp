/*
   Writable storage for user-authored appliance profiles.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "appstore/Store.h"

#include <esp_log.h>
#include <esp_spiffs.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace appstore {
namespace {

const char *const TAG = "appstore";

const char *const kBase = "/store";
const char *const kPartition = "storage";
const char *const kSuffix = ".json";

bool g_mounted = false;

std::string path_of(const std::string &slug) {
  return std::string(kBase) + "/" + slug + kSuffix;
}

std::string tmp_path() { return std::string(kBase) + "/.tmp"; }

}  // namespace

bool valid_slug(const std::string &slug) {
  if (slug.empty() || slug.size() > 24)
    return false;
  for (char c : slug)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  /* A leading or trailing dash reads as a typo and sorts oddly; refusing it
     costs nothing and keeps the list tidy. */
  return slug.front() != '-' && slug.back() != '-';
}

std::string slugify(const std::string &text) {
  std::string out;
  bool prev_dash = false;
  for (char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      out += static_cast<char>(std::tolower(u));
      prev_dash = false;
    } else if (!out.empty() && !prev_dash) {
      out += '-';
      prev_dash = true;
    }
    if (out.size() >= 24)
      break;
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out;
}

bool begin() {
  esp_vfs_spiffs_conf_t conf = {};
  conf.base_path = kBase;
  conf.partition_label = kPartition;
  conf.max_files = 4;
  /* A partition that has never been written has no filesystem on it, and that
     is the normal state of a freshly flashed board -- formatting once is the
     expected path, not a recovery from damage. */
  conf.format_if_mount_failed = true;

  const esp_err_t err = esp_vfs_spiffs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No profile storage: %s -- only built-in profiles will be available",
             esp_err_to_name(err));
    return false;
  }

  g_mounted = true;
  const Usage u = usage();
  ESP_LOGI(TAG, "Profile storage mounted: %u profiles, %u of %u bytes used",
           static_cast<unsigned>(u.count), static_cast<unsigned>(u.used),
           static_cast<unsigned>(u.total));
  return true;
}

bool available() { return g_mounted; }

std::vector<std::string> list() {
  std::vector<std::string> out;
  if (!g_mounted)
    return out;

  DIR *dir = opendir(kBase);
  if (!dir)
    return out;

  const size_t suffix_len = std::string(kSuffix).size();
  while (const dirent *e = readdir(dir)) {
    std::string name = e->d_name;
    if (name.size() <= suffix_len || name.compare(name.size() - suffix_len, suffix_len, kSuffix) != 0)
      continue;
    name.resize(name.size() - suffix_len);
    /* Anything that is not a slug we could have written is not ours to list. */
    if (valid_slug(name))
      out.push_back(name);
  }
  closedir(dir);

  std::sort(out.begin(), out.end());
  return out;
}

bool exists(const std::string &slug) {
  if (!g_mounted || !valid_slug(slug))
    return false;
  struct stat st;
  return stat(path_of(slug).c_str(), &st) == 0 && st.st_size > 0;
}

std::string load(const std::string &slug) {
  if (!g_mounted || !valid_slug(slug))
    return {};

  FILE *f = fopen(path_of(slug).c_str(), "rb");
  if (!f)
    return {};

  std::string out;
  char buf[512];
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
    out.append(buf, got);
    if (out.size() > kMaxProfileBytes) {
      /* Something wrote more than the save path would ever accept, so the file
         is not one of ours. Reporting nothing beats handing a truncated
         document to the parser and calling the failure a syntax error. */
      ESP_LOGE(TAG, "Stored profile \"%s\" is oversized; ignoring it", slug.c_str());
      fclose(f);
      return {};
    }
  }
  fclose(f);
  return out;
}

bool save(const std::string &slug, const std::string &json, std::string *error) {
  const auto fail = [&](const char *why) {
    if (error)
      *error = why;
    ESP_LOGE(TAG, "Save refused: %s", why);
    return false;
  };

  if (!g_mounted)
    return fail("no writable storage on this board");
  if (!valid_slug(slug))
    return fail("name must be 1-24 lowercase letters, digits or dashes");
  if (json.empty())
    return fail("empty profile");
  if (json.size() > kMaxProfileBytes)
    return fail("profile is too large");
  /* Checked only for a profile that does not exist yet, so the limit never
     blocks correcting one that is already stored. */
  if (!exists(slug) && list().size() >= kMaxProfiles)
    return fail("no room for another profile; delete one first");

  /* Written under a different name and moved into place only once it is
     complete and flushed. A crash before the rename leaves the old profile
     whole; a crash after leaves the new one whole. There is no moment at which
     the live file is half-written. */
  const std::string tmp = tmp_path();
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f)
    return fail("could not open storage for writing");

  const size_t written = fwrite(json.data(), 1, json.size(), f);
  const bool flushed = fflush(f) == 0 && fsync(fileno(f)) == 0;
  fclose(f);

  if (written != json.size() || !flushed) {
    unlink(tmp.c_str());
    return fail("storage full or write failed");
  }

  /* rename() does not replace an existing file on SPIFFS on every IDF version,
     so the old one goes first. The temporary file is already complete on flash
     at this point, so the window costs the previous version, never the new one. */
  const std::string dest = path_of(slug);
  unlink(dest.c_str());
  if (rename(tmp.c_str(), dest.c_str()) != 0) {
    unlink(tmp.c_str());
    return fail("could not commit the profile");
  }

  ESP_LOGI(TAG, "Stored profile \"%s\", %u bytes", slug.c_str(),
           static_cast<unsigned>(json.size()));
  return true;
}

bool erase(const std::string &slug) {
  if (!g_mounted || !valid_slug(slug))
    return false;
  const bool gone = unlink(path_of(slug).c_str()) == 0;
  if (gone)
    ESP_LOGI(TAG, "Erased profile \"%s\"", slug.c_str());
  return gone;
}

Usage usage() {
  Usage u;
  if (!g_mounted)
    return u;
  size_t total = 0, used = 0;
  if (esp_spiffs_info(kPartition, &total, &used) == ESP_OK) {
    u.total = total;
    u.used = used;
  }
  u.count = list().size();
  return u;
}

}  // namespace appstore
