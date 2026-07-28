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
 * Two-stage fetch (issue #34): GitHub's full release objects embed a
 * complete asset sub-object per attached file (uploader, timestamps,
 * content_type, download_count, ...), roughly 1.3-1.6KB *per asset* alone --
 * a single release with a dozen assets (e.g. one binary per chip target plus
 * a from-scratch flash bundle) can already run ~19KB on its own, and that's
 * before accounting for every other release also being listed. Measured
 * against a real repo: fetching N releases in one call blew well past a
 * 16KB buffer on the very first attempt. So: list tags first (name only,
 * tens of bytes each, GET /repos/{repo}/tags) to populate the picker, then
 * fetch exactly one release's full data, including its assets, only once
 * the user has actually picked a tag (GET /repos/{repo}/releases/tags/{tag}).
 * Bounds memory to O(1 release) instead of O(N releases), which is what
 * actually matters here since assets-per-release, not release count, is the
 * unbounded axis.
 *
 * Sizes below are deliberately conservative (static allocation, see
 * net_ota.c) -- a picker over more than a handful of tags/assets isn't very
 * usable on this project's small screen anyway (no scrolling support in
 * net_ota.c's picker).
 */

#define NET_GITHUB_MAX_TAGS 6
#define NET_GITHUB_MAX_ASSETS_PER_RELEASE 16
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
    char names[NET_GITHUB_MAX_TAGS][NET_GITHUB_TAG_LEN];
    size_t count;
} net_github_tag_list_t;

/* GET /repos/{repo}/tags (repo = "owner/name"), newest-first (the API's own
 * default order) -- names only, capped at NET_GITHUB_MAX_TAGS via ?per_page=.
 * Requires WiFi already connected. Populates the "CHOOSE VERSION" picker
 * without pulling in any release/asset data yet. */
esp_err_t net_github_fetch_tags(const char *repo, net_github_tag_list_t *out);

/* GET /repos/{repo}/releases/tags/{tag} -- fetches exactly one release's
 * full data (tag name + its own assets), meant to be called once the user
 * has picked `tag` from a net_github_fetch_tags() list. Returns
 * ESP_ERR_NOT_FOUND if this tag has no associated GitHub Release (e.g. a
 * plain git tag with nothing attached) -- callers should show a clear error
 * rather than treat it as an empty-but-valid release. */
esp_err_t net_github_fetch_release_by_tag(const char *repo, const char *tag, net_github_release_t *out);

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
