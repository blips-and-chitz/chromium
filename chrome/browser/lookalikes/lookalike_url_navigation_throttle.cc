// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/lookalikes/lookalike_url_navigation_throttle.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/metrics/histogram_macros.h"
#include "base/stl_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/engagement/site_engagement_service.h"
#include "chrome/browser/lookalikes/lookalike_url_allowlist.h"
#include "chrome/browser/lookalikes/lookalike_url_controller_client.h"
#include "chrome/browser/lookalikes/lookalike_url_interstitial_page.h"
#include "chrome/browser/lookalikes/lookalike_url_service.h"
#include "chrome/browser/prerender/prerender_contents.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_features.h"
#include "components/security_interstitials/content/security_interstitial_tab_helper.h"
#include "components/ukm/content/source_url_recorder.h"
#include "components/url_formatter/top_domains/top500_domains.h"
#include "components/url_formatter/top_domains/top_domain_util.h"
#include "content/public/browser/navigation_handle.h"

namespace {

using lookalikes::LookalikeUrlNavigationThrottle;
using MatchType = LookalikeUrlInterstitialPage::MatchType;
using UserAction = LookalikeUrlInterstitialPage::UserAction;
using NavigationSuggestionEvent =
    lookalikes::LookalikeUrlNavigationThrottle::NavigationSuggestionEvent;
using DomainInfo = lookalikes::DomainInfo;

typedef content::NavigationThrottle::ThrottleCheckResult ThrottleCheckResult;

void RecordEvent(
    LookalikeUrlNavigationThrottle::NavigationSuggestionEvent event) {
  UMA_HISTOGRAM_ENUMERATION(LookalikeUrlNavigationThrottle::kHistogramName,
                            event);
}

bool SkeletonsMatch(const url_formatter::Skeletons& skeletons1,
                    const url_formatter::Skeletons& skeletons2) {
  DCHECK(!skeletons1.empty());
  DCHECK(!skeletons2.empty());
  for (const std::string& skeleton1 : skeletons1) {
    if (base::ContainsKey(skeletons2, skeleton1)) {
      return true;
    }
  }
  return false;
}

// Returns true if the domain given by |domain_info| is a top domain.
bool IsTopDomain(const DomainInfo& domain_info) {
  // Top domains are only accessible through their skeletons, so query the top
  // domains trie for each skeleton of this domain.
  for (const std::string& skeleton : domain_info.skeletons) {
    const std::string top_domain =
        url_formatter::LookupSkeletonInTopDomains(skeleton);
    if (domain_info.domain_and_registry == top_domain) {
      return true;
    }
  }
  return false;
}

// Returns a site that the user has used before that the eTLD+1 in
// |domain_and_registry| may be attempting to spoof, based on skeleton
// comparison.
std::string GetMatchingSiteEngagementDomain(
    const std::vector<DomainInfo>& engaged_sites,
    const DomainInfo& navigated_domain) {
  DCHECK(!navigated_domain.domain_and_registry.empty());
  for (const DomainInfo& engaged_site : engaged_sites) {
    DCHECK(!engaged_site.domain_and_registry.empty());
    DCHECK_NE(navigated_domain.domain_and_registry,
              engaged_site.domain_and_registry);
    if (SkeletonsMatch(navigated_domain.skeletons, engaged_site.skeletons)) {
      return engaged_site.full_domain;
    }
  }
  return std::string();
}

}  // namespace

namespace lookalikes {

// static
const char LookalikeUrlNavigationThrottle::kHistogramName[] =
    "NavigationSuggestion.Event";

LookalikeUrlNavigationThrottle::LookalikeUrlNavigationThrottle(
    content::NavigationHandle* navigation_handle)
    : content::NavigationThrottle(navigation_handle),
      interstitials_enabled_(base::FeatureList::IsEnabled(
          features::kLookalikeUrlNavigationSuggestionsUI)),
      profile_(Profile::FromBrowserContext(
          navigation_handle->GetWebContents()->GetBrowserContext())),
      weak_factory_(this) {}

LookalikeUrlNavigationThrottle::~LookalikeUrlNavigationThrottle() {}

ThrottleCheckResult LookalikeUrlNavigationThrottle::HandleThrottleRequest(
    const GURL& url) {
  content::NavigationHandle* handle = navigation_handle();

  // Ignore subframe and same document navigations.
  if (!handle->IsInMainFrame() || handle->IsSameDocument()) {
    return content::NavigationThrottle::PROCEED;
  }

  if (!url.SchemeIsHTTPOrHTTPS()) {
    return content::NavigationThrottle::PROCEED;
  }

  // If the URL is in the allowlist, don't show any warning.
  LookalikeUrlAllowlist* allowlist =
      LookalikeUrlAllowlist::GetOrCreateAllowlist(handle->GetWebContents());
  if (allowlist->IsDomainInList(url.host())) {
    return content::NavigationThrottle::PROCEED;
  }

  const DomainInfo navigated_domain = GetDomainInfo(url);
  if (navigated_domain.domain_and_registry.empty() ||
      IsTopDomain(navigated_domain)) {
    return content::NavigationThrottle::PROCEED;
  }

  LookalikeUrlService* service = LookalikeUrlService::Get(profile_);
  if (service->UpdateEngagedSites(
          base::BindOnce(&LookalikeUrlNavigationThrottle::PerformChecksDeferred,
                         weak_factory_.GetWeakPtr(), url, navigated_domain))) {
    // If we're not going to show an interstitial, there's no reason to delay
    // the navigation any further.
    if (!interstitials_enabled_) {
      return content::NavigationThrottle::PROCEED;
    }
    return content::NavigationThrottle::DEFER;
  }

  return PerformChecks(url, navigated_domain, service->GetLatestEngagedSites());
}

ThrottleCheckResult LookalikeUrlNavigationThrottle::WillStartRequest() {
  return HandleThrottleRequest(navigation_handle()->GetURL());
}

ThrottleCheckResult LookalikeUrlNavigationThrottle::WillRedirectRequest() {
  return HandleThrottleRequest(navigation_handle()->GetURL());
}

const char* LookalikeUrlNavigationThrottle::GetNameForLogging() {
  return "LookalikeUrlNavigationThrottle";
}

ThrottleCheckResult LookalikeUrlNavigationThrottle::ShowInterstitial(
    const GURL& safe_url,
    const GURL& url,
    ukm::SourceId source_id,
    MatchType match_type) {
  content::NavigationHandle* handle = navigation_handle();
  content::WebContents* web_contents = handle->GetWebContents();

  auto controller = std::make_unique<LookalikeUrlControllerClient>(
      web_contents, url, safe_url);

  std::unique_ptr<LookalikeUrlInterstitialPage> blocking_page(
      new LookalikeUrlInterstitialPage(web_contents, safe_url, source_id,
                                       match_type, std::move(controller)));

  base::Optional<std::string> error_page_contents =
      blocking_page->GetHTMLContents();

  security_interstitials::SecurityInterstitialTabHelper::AssociateBlockingPage(
      web_contents, handle->GetNavigationId(), std::move(blocking_page));

  return ThrottleCheckResult(content::NavigationThrottle::CANCEL,
                             net::ERR_BLOCKED_BY_CLIENT, error_page_contents);
}

std::unique_ptr<LookalikeUrlNavigationThrottle>
LookalikeUrlNavigationThrottle::MaybeCreateNavigationThrottle(
    content::NavigationHandle* navigation_handle) {
  // If the tab is being prerendered, stop here before it breaks metrics
  content::WebContents* web_contents = navigation_handle->GetWebContents();
  if (prerender::PrerenderContents::FromWebContents(web_contents)) {
    return nullptr;
  }

  // Otherwise, always insert the throttle for metrics recording.
  return std::make_unique<LookalikeUrlNavigationThrottle>(navigation_handle);
}

void LookalikeUrlNavigationThrottle::PerformChecksDeferred(
    const GURL& url,
    const DomainInfo& navigated_domain,
    const std::vector<DomainInfo>& engaged_sites) {
  ThrottleCheckResult result = LookalikeUrlNavigationThrottle::PerformChecks(
      url, navigated_domain, engaged_sites);

  if (!interstitials_enabled_) {
    return;
  }

  if (result.action() == content::NavigationThrottle::PROCEED) {
    Resume();
    return;
  }

  CancelDeferredNavigation(result);
}

ThrottleCheckResult LookalikeUrlNavigationThrottle::PerformChecks(
    const GURL& url,
    const DomainInfo& navigated_domain,
    const std::vector<DomainInfo>& engaged_sites) {
  std::string matched_domain;
  MatchType match_type;

  // Ensure that this URL is not already engaged. We can't use the synchronous
  // SiteEngagementService::IsEngagementAtLeast as it has side effects. We check
  // in PerformChecks to ensure we have up-to-date engaged_sites.
  // This check ignores the scheme which is okay since it's more conservative:
  // If the user is engaged with http://domain.test, not showing the warning on
  // https://domain.test is acceptable.
  const auto already_engaged =
      std::find_if(engaged_sites.begin(), engaged_sites.end(),
                   [navigated_domain](const DomainInfo& engaged_domain) {
                     return (navigated_domain.domain_and_registry ==
                             engaged_domain.domain_and_registry);
                   });
  if (already_engaged != engaged_sites.end()) {
    return content::NavigationThrottle::PROCEED;
  }

  if (!GetMatchingDomain(navigated_domain, engaged_sites, &matched_domain,
                         &match_type)) {
    return content::NavigationThrottle::PROCEED;
  }

  DCHECK(!matched_domain.empty());

  GURL::Replacements replace_host;
  replace_host.SetHostStr(matched_domain);
  const GURL suggested_url = url.ReplaceComponents(replace_host);

  ukm::SourceId source_id = ukm::ConvertToSourceId(
      navigation_handle()->GetNavigationId(), ukm::SourceIdType::NAVIGATION_ID);

  if (interstitials_enabled_ && match_type != MatchType::kEditDistance) {
    return ShowInterstitial(suggested_url, url, source_id, match_type);
  }

  // Interstitial normally records UKM, but still record when it's not shown.
  LookalikeUrlInterstitialPage::RecordUkmEvent(
      source_id, match_type, UserAction::kInterstitialNotShown);

  return content::NavigationThrottle::PROCEED;
}

bool LookalikeUrlNavigationThrottle::GetMatchingDomain(
    const DomainInfo& navigated_domain,
    const std::vector<DomainInfo>& engaged_sites,
    std::string* matched_domain,
    MatchType* match_type) {
  DCHECK(!navigated_domain.domain_and_registry.empty());

  if (navigated_domain.idn_result.has_idn_component) {
    // If the navigated domain is IDN, check its skeleton against engaged sites
    // and top domains.
    const std::string matched_engaged_domain =
        GetMatchingSiteEngagementDomain(engaged_sites, navigated_domain);
    if (!matched_engaged_domain.empty()) {
      RecordEvent(NavigationSuggestionEvent::kMatchSiteEngagement);
      *matched_domain = matched_engaged_domain;
      *match_type = MatchType::kSiteEngagement;
      return true;
    }

    if (!navigated_domain.idn_result.matching_top_domain.empty()) {
      // In practice, this is not possible since the top domain list does not
      // contain IDNs, so domain_and_registry can't both have IDN and be a top
      // domain. Still, sanity check in case the top domain list changes in the
      // future.
      // At this point, navigated domain should not be a top domain.
      DCHECK_NE(navigated_domain.domain_and_registry,
                navigated_domain.idn_result.matching_top_domain);
      RecordEvent(NavigationSuggestionEvent::kMatchTopSite);
      *matched_domain = navigated_domain.idn_result.matching_top_domain;
      *match_type = MatchType::kTopSite;
      return true;
    }
  }

  // If we can't find an exact top domain or an engaged site, try to find a top
  // domain within an edit distance of one.
  const std::string similar_domain =
      GetSimilarDomainFromTop500(navigated_domain);
  if (!similar_domain.empty() &&
      navigated_domain.domain_and_registry != similar_domain) {
    RecordEvent(NavigationSuggestionEvent::kMatchEditDistance);
    *matched_domain = similar_domain;
    *match_type = MatchType::kEditDistance;
    return true;
  }
  return false;
}

// static
bool LookalikeUrlNavigationThrottle::IsEditDistanceAtMostOne(
    const base::string16& str1,
    const base::string16& str2) {
  if (str1.size() > str2.size() + 1 || str2.size() > str1.size() + 1) {
    return false;
  }
  base::string16::const_iterator i = str1.begin();
  base::string16::const_iterator j = str2.begin();
  size_t edit_count = 0;
  while (i != str1.end() && j != str2.end()) {
    if (*i == *j) {
      i++;
      j++;
    } else {
      edit_count++;
      if (edit_count > 1) {
        return false;
      }

      if (str1.size() > str2.size()) {
        // First string is longer than the second. This can only happen if the
        // first string has an extra character.
        i++;
      } else if (str2.size() > str1.size()) {
        // Second string is longer than the first. This can only happen if the
        // second string has an extra character.
        j++;
      } else {
        // Both strings are the same length. This can only happen if the two
        // strings differ by a single character.
        i++;
        j++;
      }
    }
  }
  if (i != str1.end() || j != str2.end()) {
    // A character at the end did not match.
    edit_count++;
  }
  return edit_count <= 1;
}

// static
std::string LookalikeUrlNavigationThrottle::GetSimilarDomainFromTop500(
    const DomainInfo& navigated_domain) {
  if (!url_formatter::top_domains::IsEditDistanceCandidate(
          navigated_domain.domain_and_registry)) {
    return std::string();
  }
  const std::string domain_without_registry =
      url_formatter::top_domains::HostnameWithoutRegistry(
          navigated_domain.domain_and_registry);

  for (const std::string& navigated_skeleton : navigated_domain.skeletons) {
    for (const char* const top_domain_skeleton : top500_domains::kTop500) {
      if (IsEditDistanceAtMostOne(base::UTF8ToUTF16(navigated_skeleton),
                                  base::UTF8ToUTF16(top_domain_skeleton))) {
        const std::string top_domain =
            url_formatter::LookupSkeletonInTopDomains(top_domain_skeleton);
        DCHECK(!top_domain.empty());
        // If the only difference between the navigated and top
        // domains is the registry part, this is unlikely to be a spoofing
        // attempt. Ignore this match and continue. E.g. If the navigated domain
        // is google.com.tw and the top domain is google.com.tr, this won't
        // produce a match.
        const std::string top_domain_without_registry =
            url_formatter::top_domains::HostnameWithoutRegistry(top_domain);
        if (domain_without_registry != top_domain_without_registry) {
          return top_domain;
        }
      }
    }
  }
  return std::string();
}

}  // namespace lookalikes
