#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fetches release history directly from a target repo's GitHub Releases API
 * as an alternative to statically listing every downloadable version in this
 * launcher's own manifest.json (issue #29) -- a manifest.json entry opts in
 * by setting "github_repo": "owner/repo" instead of a fixed url/version (see
 * net_manifest.h). See docs/launcher-manifest.md for the release-side
 * launcher.manifest.json format this pairs with (lets a release declare
 * which asset is the right binary for this launcher's own chip target).
 *
 * Sizes below are deliberately conservative (static allocation, see
 * net_ota.c) -- a picker over more than a handful of releases/assets isn't
 * very usable on this project's small screen anyway (no scrolling support
 * in net_ota.c's picker).
 */

#define NET_GITHUB_MAX_RELEASES 6
#define NET_GITHUB_MAX_ASSETS_PER_RELEASE 6
#define NET_GITHUB_TAG_LEN 32
#define NET_GITHUB_ASSET_NAME_LEN 64
#define NET_GITHUB_URL_LEN 256

typedef struct {
    char name[NET_GITHUB_ASSET_NAME_LEN];
    char download_url[NET_GITHUB_URL_LEN];
    uint32_t size;
} net_github_asset_t;

typedef struct {
    char tag_name[NET_GITHUB_TAG_LEN];
    net_github_asset_t assets[NET_GITHUB_MAX_ASSETS_PER_RELEASE];
    size_t asset_count;
} net_github_release_t;

typedef struct {
    net_github_release_t releases[NET_GITHUB_MAX_RELEASES];
    size_t count;
} net_github_release_list_t;

/* GET /repos/{repo}/releases (repo = "owner/name"), newest-first (the API's
 * own default order) -- draft releases are skipped, capped at
 * NET_GITHUB_MAX_RELEASES via ?per_page=. Requires WiFi already connected. */
esp_err_t net_github_fetch_releases(const char *repo, net_github_release_list_t *out);

/* Looks for an asset literally named "launcher.manifest.json" among
 * release->assets, downloads and parses it, and resolves the asset name
 * declared under "targets" for CONFIG_IDF_TARGET (see
 * docs/launcher-manifest.md). On ESP_OK, *out_asset points at the matching
 * entry within release->assets (valid as long as `release` is). Returns
 * ESP_ERR_NOT_FOUND if there's no launcher.manifest.json asset, or it has no
 * entry for this chip target, or the entry names an asset not actually
 * present in this release -- callers should fall back to letting the user
 * pick an asset manually from release->assets in all of those cases. Any
 * other esp_err_t means a network/parse failure fetching the manifest
 * itself. */
esp_err_t net_github_resolve_target_asset(const net_github_release_t *release, const net_github_asset_t **out_asset);

#ifdef __cplusplus
}
#endif
