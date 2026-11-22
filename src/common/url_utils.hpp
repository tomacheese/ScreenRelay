#pragma once
#include <string>

namespace url_utils {

/**
 * @brief Mask credentials in a URL so it is safe to log.
 *
 * Replaces userinfo with `***@`, the query with `?***`, and drops the fragment.
 * A string without `://` is returned unchanged.
 */
inline std::string redact_url(const std::string& url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;
    const size_t rest = scheme_end + 3;

    const std::string body = url.substr(0, url.find('#', rest));
    const size_t query = body.find('?', rest);
    const size_t region_end = (query == std::string::npos) ? body.size() : query;

    // The last '@' wins so an unencoded '@' or '/' in a password cannot leak a suffix.
    size_t at = std::string::npos;
    if (region_end > rest) {
        const size_t pos = body.rfind('@', region_end - 1);
        if (pos != std::string::npos && pos >= rest) at = pos;
    }

    std::string out = body.substr(0, rest);
    if (at != std::string::npos) {
        out += "***@";
        out += body.substr(at + 1, region_end - at - 1);
    } else {
        out += body.substr(rest, region_end - rest);
    }
    if (query != std::string::npos) out += "?***";
    return out;
}

}  // namespace url_utils
