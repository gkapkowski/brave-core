/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_rewards/browser/rewards_service_impl.h"

#include <stdint.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/containers/flat_map.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/i18n/time_formatting.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/json_string_value_serializer.h"
#include "base/time/time.h"
#include "base/logging.h"
#include "base/sequenced_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/task_runner_util.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "bat/ledger/ledger.h"
#include "bat/ledger/global_constants.h"
#include "bat/ledger/mojom_structs.h"
#include "bat/ledger/transactions_info.h"
#include "brave/base/containers/utils.h"
#include "brave/browser/ui/webui/brave_rewards_source.h"
#include "brave/components/brave_rewards/common/pref_names.h"
#include "brave/common/brave_channel_info.h"
#include "brave/components/brave_ads/browser/ads_service.h"
#include "brave/components/brave_ads/browser/ads_service_factory.h"
#include "brave/components/brave_ads/browser/buildflags/buildflags.h"
#include "brave/components/brave_ads/common/pref_names.h"
#include "brave/components/brave_rewards/browser/android_util.h"
#include "brave/components/brave_rewards/browser/auto_contribution_props.h"
#include "brave/components/brave_rewards/browser/balance_report.h"
#include "brave/components/brave_rewards/browser/content_site.h"
#include "brave/components/brave_rewards/browser/publisher_banner.h"
#include "brave/components/brave_rewards/browser/rewards_database.h"
#include "brave/components/brave_rewards/browser/rewards_notification_service.h"
#include "brave/components/brave_rewards/browser/rewards_notification_service_impl.h"
#include "brave/components/brave_rewards/browser/rewards_p3a.h"
#include "brave/browser/brave_rewards/rewards_service_factory.h"
#include "brave/components/brave_rewards/browser/rewards_service_observer.h"
#include "brave/components/brave_rewards/browser/static_values.h"
#include "brave/components/brave_rewards/browser/switches.h"
#include "brave/components/brave_rewards/browser/wallet_properties.h"
#include "brave/components/services/bat_ledger/public/cpp/ledger_client_mojo_proxy.h"
#include "chrome/browser/bitmap_fetcher/bitmap_fetcher_service_factory.h"
#include "chrome/browser/browser_process_impl.h"
#include "chrome/browser/favicon/favicon_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/country_codes/country_codes.h"
#include "components/favicon/core/favicon_service.h"
#include "components/favicon_base/favicon_types.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/url_data_source.h"
#include "content/public/common/service_manager_connection.h"
#include "net/base/escape.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/base/url_util.h"
#include "net/http/http_status_code.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/service_manager/public/cpp/connector.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/image/image.h"
#include "url/gurl.h"
#include "url/url_canon_stdstring.h"
#include "url/url_util.h"

#if defined(BRAVE_CHROMIUM_BUILD)
#include "brave/components/brave_rewards/resources/grit/brave_rewards_resources.h"
#include "components/grit/brave_components_resources.h"
#else
#include "components/grit/components_resources.h"
#endif

using net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES;
using std::placeholders::_1;
using std::placeholders::_2;

namespace brave_rewards {

static const unsigned int kRetriesCountOnNetworkChange = 1;

class LogStreamImpl : public ledger::LogStream {
 public:
  LogStreamImpl(const char* file,
                int line,
                const ledger::LogLevel log_level) {
    logging::LogSeverity severity;

    switch (log_level) {
      case ledger::LogLevel::LOG_INFO:
        severity = logging::LOG_INFO;
        break;
      case ledger::LogLevel::LOG_WARNING:
        severity = logging::LOG_WARNING;
        break;
      case ledger::LogLevel::LOG_ERROR:
        severity = logging::LOG_ERROR;
        break;
      default:
        severity = logging::LOG_VERBOSE;
        break;
    }

    log_message_ = std::make_unique<logging::LogMessage>(file, line, severity);
  }

  LogStreamImpl(const char* file,
                int line,
                int log_level) {
    // VLOG has negative log level
    log_message_ =
        std::make_unique<logging::LogMessage>(file, line, -log_level);
  }

  std::ostream& stream() override {
    return log_message_->stream();
  }

 private:
  std::unique_ptr<logging::LogMessage> log_message_;
  DISALLOW_COPY_AND_ASSIGN(LogStreamImpl);
};

namespace {

ContentSite PublisherInfoToContentSite(
    const ledger::PublisherInfo& publisher_info) {
  ContentSite content_site(publisher_info.id);
  content_site.percentage = publisher_info.percent;
  content_site.status = static_cast<uint32_t>(publisher_info.status);
  content_site.excluded = static_cast<int>(publisher_info.excluded);
  content_site.name = publisher_info.name;
  content_site.url = publisher_info.url;
  content_site.provider = publisher_info.provider;
  content_site.favicon_url = publisher_info.favicon_url;
  content_site.id = publisher_info.id;
  content_site.weight = publisher_info.weight;
  content_site.reconcile_stamp = publisher_info.reconcile_stamp;
  return content_site;
}

std::string URLMethodToRequestType(ledger::UrlMethod method) {
  switch (method) {
    case ledger::UrlMethod::GET:
      return "GET";
    case ledger::UrlMethod::POST:
      return "POST";
    case ledger::UrlMethod::PUT:
      return "PUT";
    case ledger::UrlMethod::PATCH:
      return "PATCH";
    default:
      NOTREACHED();
      return "GET";
  }
}

// Returns pair of string and its parsed counterpart. We parse it on the file
// thread for the performance sake. It's should be better to remove the string
// representation in the [far] future.
std::pair<std::string, base::Value> LoadStateOnFileTaskRunner(
    const base::FilePath& path) {
  std::string data;
  bool success = base::ReadFileToString(path, &data);

  // Make sure the file isn't empty.
  if (!success || data.empty()) {
    LOG(ERROR) << "Failed to read file: " << path.MaybeAsASCII();
    return {};
  }
  std::pair<std::string, base::Value> result;
  result.first = data;

  // Save deserialized version for recording P3A and future use.
  JSONStringValueDeserializer deserializer(data);
  int error_code = 0;
  std::string error_message;
  auto value = deserializer.Deserialize(&error_code, &error_message);
  if (!value) {
    LOG(ERROR) << "Cannot deserialize ledger state, error code: " << error_code
               << " message: " << error_message;
    return result;
  }

  const auto dict = base::DictionaryValue::From(std::move(value));
  if (!dict) {
    LOG(ERROR) << "Corrupted ledger state.";
    return result;
  }

  ExtractAndLogP3AStats(*dict);
  result.second = std::move(*dict);

  return result;
}

// `callback` has a WeakPtr so this won't crash if the file finishes
// writing after RewardsServiceImpl has been destroyed
void PostWriteCallback(
    const base::Callback<void(bool success)>& callback,
    scoped_refptr<base::SequencedTaskRunner> reply_task_runner,
    bool write_success) {
  // We can't run |callback| on the current thread. Bounce back to
  // the |reply_task_runner| which is the correct sequenced thread.
  reply_task_runner->PostTask(FROM_HERE,
                              base::Bind(callback, write_success));
}

time_t GetCurrentTimestamp() {
  return base::Time::NowFromSystemTime().ToTimeT();
}

std::string LoadOnFileTaskRunner(const base::FilePath& path) {
  std::string data;
  bool success = base::ReadFileToString(path, &data);

  // Make sure the file isn't empty.
  if (!success || data.empty()) {
    LOG(ERROR) << "Failed to read file: " << path.MaybeAsASCII();
    return std::string();
  }
  return data;
}

bool ResetOnFileTaskRunner(const base::FilePath& path) {
  return base::DeleteFile(path, false);
}

bool ResetOnFilesTaskRunner(const std::vector<base::FilePath>& paths) {
  bool res = true;
  for (size_t i = 0; i < paths.size(); i++) {
    if (!base::DeleteFileRecursively(paths[i])) {
      res = false;
    }
  }

  return res;
}

void EnsureRewardsBaseDirectoryExists(const base::FilePath& path) {
  if (!DirectoryExists(path))
    base::CreateDirectory(path);
}

net::NetworkTrafficAnnotationTag
GetNetworkTrafficAnnotationTagForFaviconFetch() {
  return net::DefineNetworkTrafficAnnotation(
      "brave_rewards_favicon_fetcher", R"(
      semantics {
        sender:
          "Brave Rewards Media Fetcher"
        description:
          "Fetches favicon for media publishers in Rewards."
        trigger:
          "User visits a media publisher content."
        data: "Favicon for media publisher."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: NO
        setting:
          "This feature cannot be disabled by settings."
        policy_exception_justification:
          "Not implemented."
      })");
}

net::NetworkTrafficAnnotationTag GetNetworkTrafficAnnotationTagForURLLoad() {
  return net::DefineNetworkTrafficAnnotation("rewards_service_impl", R"(
      semantics {
        sender: "Brave Rewards service"
        description:
          "This service lets users anonymously support the sites they visit by "
          "tallying the attention spent on visited sites and dividing up a "
          "monthly BAT contribution."
        trigger:
          "User-initiated for direct tipping or on a set interval while Brave "
          "is running for monthly contributions."
        data:
          "Publisher and contribution data."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: NO
        setting:
          "You can enable or disable this feature via the BAT icon in the URL "
          "bar or by visiting brave://rewards/."
        policy_exception_justification:
          "Not implemented."
      })");
}

const char pref_prefix[] = "brave.rewards.";

}  // namespace

bool IsMediaLink(const GURL& url,
                 const GURL& first_party_url,
                 const GURL& referrer) {
  return ledger::Ledger::IsMediaLink(url.spec(),
                                     first_party_url.spec(),
                                     referrer.spec());
}


// read comment about file pathes at src\base\files\file_path.h
#if defined(OS_WIN)
const base::FilePath::StringType kLedger_state(L"ledger_state");
const base::FilePath::StringType kPublisher_state(L"publisher_state");
const base::FilePath::StringType kPublisher_info_db(L"publisher_info_db");
const base::FilePath::StringType kPublishers_list(L"publishers_list");
const base::FilePath::StringType kRewardsStatePath(L"rewards_service");
#else
const base::FilePath::StringType kLedger_state("ledger_state");
const base::FilePath::StringType kPublisher_state("publisher_state");
const base::FilePath::StringType kPublisher_info_db("publisher_info_db");
const base::FilePath::StringType kPublishers_list("publishers_list");
const base::FilePath::StringType kRewardsStatePath("rewards_service");
#endif

RewardsServiceImpl::RewardsServiceImpl(Profile* profile)
    : profile_(profile),
      bat_ledger_client_binding_(new bat_ledger::LedgerClientMojoProxy(this)),
      file_task_runner_(base::CreateSequencedTaskRunner(
          {base::ThreadPool(), base::MayBlock(),
           base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})),
      ledger_state_path_(profile_->GetPath().Append(kLedger_state)),
      publisher_state_path_(profile_->GetPath().Append(kPublisher_state)),
      publisher_info_db_path_(profile->GetPath().Append(kPublisher_info_db)),
      publisher_list_path_(profile->GetPath().Append(kPublishers_list)),
      rewards_base_path_(profile_->GetPath().Append(kRewardsStatePath)),
      rewards_database_(new RewardsDatabase(publisher_info_db_path_)),
      notification_service_(new RewardsNotificationServiceImpl(profile)),
      next_timer_id_(0),
      reset_states_(false) {
  file_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&EnsureRewardsBaseDirectoryExists,
                                rewards_base_path_));
  // Set up the rewards data source
  content::URLDataSource::Add(profile_,
                              std::make_unique<BraveRewardsSource>(profile_));
}

RewardsServiceImpl::~RewardsServiceImpl() {
  file_task_runner_->DeleteSoon(FROM_HERE, rewards_database_.release());
  StopNotificationTimers();
}

void RewardsServiceImpl::ConnectionClosed() {
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(FROM_HERE,
      base::BindOnce(&RewardsServiceImpl::StartLedger, AsWeakPtr()),
      base::TimeDelta::FromSeconds(1));
}

void RewardsServiceImpl::Init(
    std::unique_ptr<RewardsServiceObserver> extension_observer,
    std::unique_ptr<RewardsServicePrivateObserver> private_observer,
    std::unique_ptr<RewardsNotificationServiceObserver> notification_observer) {
  notification_service_->Init(std::move(notification_observer));
  AddObserver(notification_service_.get());

  if (extension_observer) {
    extension_observer_ = std::move(extension_observer);
    AddObserver(extension_observer_.get());
  }

  if (private_observer) {
    private_observer_ = std::move(private_observer);
    private_observers_.AddObserver(private_observer_.get());
  }

  StartLedger();
}

void RewardsServiceImpl::StartLedger() {
  bat_ledger::mojom::BatLedgerClientAssociatedPtrInfo client_ptr_info;
  bat_ledger_client_binding_.Bind(mojo::MakeRequest(&client_ptr_info));

  content::ServiceManagerConnection* connection =
          content::ServiceManagerConnection::GetForProcess();
  if (!connection) {
    return;
  }

  if (bat_ledger_service_.is_bound()) {
    bat_ledger_service_.reset();
  }

  connection->GetConnector()->BindInterface(
      bat_ledger::mojom::kServiceName,
      bat_ledger_service_.BindNewPipeAndPassReceiver());
  bat_ledger_service_.set_disconnect_handler(
      base::Bind(&RewardsServiceImpl::ConnectionClosed, AsWeakPtr()));

  ledger::Environment environment = ledger::Environment::STAGING;
  // Environment
  #if defined(OFFICIAL_BUILD) && defined(OS_ANDROID)
    environment = GetServerEnvironmentForAndroid();
  #elif defined(OFFICIAL_BUILD)
    environment = ledger::Environment::PRODUCTION;
  #endif
  SetEnvironment(environment);

  SetDebug(false);

  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();

  if (command_line.HasSwitch(switches::kRewards)) {
    std::string options = command_line.GetSwitchValueASCII(switches::kRewards);

    if (!options.empty()) {
      HandleFlags(options);
    }
  }

  bat_ledger_service_->Create(std::move(client_ptr_info),
      MakeRequest(&bat_ledger_));

  auto callback = base::BindOnce(&RewardsServiceImpl::OnWalletInitialized,
      AsWeakPtr());

  bat_ledger_->Initialize(false, std::move(callback));
}

void RewardsServiceImpl::OnResult(
    ledger::ResultCallback callback,
    const ledger::Result result) {
  callback(result);
}

void RewardsServiceImpl::MaybeShowBackupNotification(uint64_t boot_stamp) {
  PrefService* pref_service = profile_->GetPrefs();
  bool user_has_funded = pref_service->GetBoolean(prefs::kRewardsUserHasFunded);
  bool backup_succeeded = pref_service->GetBoolean(
      prefs::kRewardsBackupSucceeded);
  if (user_has_funded && !backup_succeeded) {
    base::Time now = base::Time::Now();
    base::Time boot_timestamp = base::Time::FromDoubleT(boot_stamp);
    base::TimeDelta backup_notification_frequency =
        pref_service->GetTimeDelta(prefs::kRewardsBackupNotificationFrequency);
    base::TimeDelta backup_notification_interval =
        pref_service->GetTimeDelta(prefs::kRewardsBackupNotificationInterval);
    base::TimeDelta elapsed = now - boot_timestamp;
    if (elapsed > backup_notification_interval) {
      base::TimeDelta next_backup_notification_interval =
          backup_notification_interval + backup_notification_frequency;
      pref_service->SetTimeDelta(prefs::kRewardsBackupNotificationInterval,
                                 next_backup_notification_interval);
      RewardsNotificationService::RewardsNotificationArgs args;
      notification_service_->AddNotification(
          RewardsNotificationService::REWARDS_NOTIFICATION_BACKUP_WALLET, args,
          "rewards_notification_backup_wallet");
    }
  }
}

void RewardsServiceImpl::MaybeShowAddFundsNotification(
    uint64_t reconcile_stamp) {
  // Show add funds notification if reconciliation will occur in the
  // next 3 days and balance is too low.
  base::Time now = base::Time::Now();
  if (reconcile_stamp - now.ToDoubleT() <
      3 * base::Time::kHoursPerDay * base::Time::kSecondsPerHour) {
    if (ShouldShowNotificationAddFunds()) {
      MaybeShowNotificationAddFunds();
    }
  }
}

void RewardsServiceImpl::OnCreateWallet(
    CreateWalletCallback callback,
    ledger::Result result) {
  OnWalletInitialized(result);
  std::move(callback).Run(static_cast<int32_t>(result));
}

void RewardsServiceImpl::AddPrivateObserver(
    RewardsServicePrivateObserver* observer) {
  private_observers_.AddObserver(observer);
}

void RewardsServiceImpl::RemovePrivateObserver(
    RewardsServicePrivateObserver* observer) {
  private_observers_.RemoveObserver(observer);
}

void RewardsServiceImpl::CreateWallet(CreateWalletCallback callback) {
  if (ready().is_signaled()) {
    if (Connected()) {
      auto on_create = base::BindOnce(
          &RewardsServiceImpl::OnCreateWallet,
          AsWeakPtr(),
          std::move(callback));
#if !defined(OS_ANDROID)
      bat_ledger_->CreateWallet(std::move(on_create));
#else
      safetynet_check::ClientAttestationCallback attest_callback =
          base::BindOnce(&RewardsServiceImpl::CreateWalletAttestationResult,
              AsWeakPtr(),
              std::move(on_create));
      safetynet_check_runner_.performSafetynetCheck("",
          std::move(attest_callback), true);
#endif
    }
  } else {
    ready().Post(FROM_HERE,
        base::BindOnce(
            &brave_rewards::RewardsService::CreateWallet,
            AsWeakPtr(),
            std::move(callback)));
  }
}

#if defined(OS_ANDROID)
void RewardsServiceImpl::CreateWalletAttestationResult(
    bat_ledger::mojom::BatLedger::CreateWalletCallback callback,
    const bool token_received,
    const std::string& result_string,
    const bool attestation_passed) {
  if (!token_received) {
    LOG(ERROR) << "CreateWalletAttestationResult error: " << result_string;
    OnWalletInitialized(ledger::Result::LEDGER_ERROR);
    return;
  }
  if (!attestation_passed) {
    OnWalletInitialized(ledger::Result::SAFETYNET_ATTESTATION_FAILED);
    return;
  }
  bat_ledger_->CreateWallet(std::move(callback));
}
#endif

void RewardsServiceImpl::GetContentSiteList(
    uint32_t start,
    uint32_t limit,
    uint64_t min_visit_time,
    uint64_t reconcile_stamp,
    bool allow_non_verified,
    uint32_t min_visits,
    const GetContentSiteListCallback& callback) {
  auto filter = ledger::ActivityInfoFilter::New();
  filter->min_duration = min_visit_time;
  auto pair = ledger::ActivityInfoFilterOrderPair::New("ai.percent", false);
  filter->order_by.push_back(std::move(pair));
  filter->reconcile_stamp = reconcile_stamp;
  filter->excluded = ledger::ExcludeFilter::FILTER_ALL_EXCEPT_EXCLUDED;
  filter->percent = 1;
  filter->non_verified = allow_non_verified;
  filter->min_visits = min_visits;

  bat_ledger_->GetActivityInfoList(
      start,
      limit,
      std::move(filter),
      base::BindOnce(&RewardsServiceImpl::OnGetContentSiteList,
                     AsWeakPtr(),
                     callback));
}

void RewardsServiceImpl::GetExcludedList(
    const GetContentSiteListCallback& callback) {
  bat_ledger_->GetExcludedList(base::BindOnce(
      &RewardsServiceImpl::OnGetContentSiteList,
      AsWeakPtr(),
      callback));
}

void RewardsServiceImpl::OnGetContentSiteList(
    const GetContentSiteListCallback& callback,
    ledger::PublisherInfoList list) {
  std::unique_ptr<ContentSiteList> site_list(new ContentSiteList);

  for (auto &publisher : list) {
    site_list->push_back(PublisherInfoToContentSite(*publisher));
  }

  callback.Run(std::move(site_list));
}

void RewardsServiceImpl::OnLoad(SessionID tab_id, const GURL& url) {
  if (!Connected())
    return;

  auto origin = url.GetOrigin();
  const std::string baseDomain =
      GetDomainAndRegistry(origin.host(), INCLUDE_PRIVATE_REGISTRIES);

  if (baseDomain == "")
    return;

  const std::string publisher_url = origin.scheme() + "://" + baseDomain + "/";

  ledger::VisitDataPtr data = ledger::VisitData::New();
  data->tld = data->name = baseDomain;
  data->domain = origin.host(),
  data->path = url.path();
  data->tab_id = tab_id.id();
  data->url = publisher_url;
  bat_ledger_->OnLoad(std::move(data), GetCurrentTimestamp());
}

void RewardsServiceImpl::OnUnload(SessionID tab_id) {
  if (!Connected())
    return;

  bat_ledger_->OnUnload(tab_id.id(), GetCurrentTimestamp());
}

void RewardsServiceImpl::OnShow(SessionID tab_id) {
  if (!Connected())
    return;

  bat_ledger_->OnShow(tab_id.id(), GetCurrentTimestamp());
}

void RewardsServiceImpl::OnHide(SessionID tab_id) {
  if (!Connected())
    return;

  bat_ledger_->OnHide(tab_id.id(), GetCurrentTimestamp());
}

void RewardsServiceImpl::OnForeground(SessionID tab_id) {
  if (!Connected())
    return;

  bat_ledger_->OnForeground(tab_id.id(), GetCurrentTimestamp());
}

void RewardsServiceImpl::OnBackground(SessionID tab_id) {
  if (!Connected())
    return;

  bat_ledger_->OnBackground(tab_id.id(), GetCurrentTimestamp());
}

void RewardsServiceImpl::OnPostData(SessionID tab_id,
                                    const GURL& url,
                                    const GURL& first_party_url,
                                    const GURL& referrer,
                                    const std::string& post_data) {
  if (!Connected())
    return;

  std::string output;
  url::RawCanonOutputW<1024> canonOutput;
  url::DecodeURLEscapeSequences(post_data.c_str(),
                                post_data.length(),
                                url::DecodeURLMode::kUTF8OrIsomorphic,
                                &canonOutput);
  output = base::UTF16ToUTF8(base::StringPiece16(canonOutput.data(),
                                                 canonOutput.length()));

  if (output.empty())
    return;

  ledger::VisitDataPtr data = ledger::VisitData::New();
  data->path = url.spec(),
  data->tab_id = tab_id.id();

  bat_ledger_->OnPostData(url.spec(),
                          first_party_url.spec(),
                          referrer.spec(),
                          output,
                          std::move(data));
}

void RewardsServiceImpl::OnXHRLoad(SessionID tab_id,
                                   const GURL& url,
                                   const GURL& first_party_url,
                                   const GURL& referrer) {
  if (!Connected())
    return;

  std::map<std::string, std::string> parts;

  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    parts[it.GetKey()] = it.GetUnescapedValue();
  }

  ledger::VisitDataPtr data = ledger::VisitData::New();
  data->path = url.spec();
  data->tab_id = tab_id.id();

  bat_ledger_->OnXHRLoad(tab_id.id(),
                         url.spec(),
                         base::MapToFlatMap(parts),
                         first_party_url.spec(),
                         referrer.spec(),
                         std::move(data));
}

void RewardsServiceImpl::OnRestorePublishers(const ledger::Result result) {
  if (result != ledger::Result::LEDGER_OK) {
    return;
  }

  for (auto& observer : observers_) {
    observer.OnExcludedSitesChanged(
      this, "-1", static_cast<int>(ledger::PublisherExclude::ALL));
  }
}

void RewardsServiceImpl::RestorePublishers() {
  if (!Connected()) {
    return;
  }

  bat_ledger_->RestorePublishers(
    base::BindOnce(&RewardsServiceImpl::OnRestorePublishers,
                   AsWeakPtr()));
}

std::string RewardsServiceImpl::URIEncode(const std::string& value) {
  return net::EscapeQueryParamValue(value, false);
}

void RewardsServiceImpl::Shutdown() {
  RemoveObserver(notification_service_.get());

  if (extension_observer_) {
    RemoveObserver(extension_observer_.get());
  }

  if (private_observer_) {
    private_observers_.RemoveObserver(private_observer_.get());
  }

  BitmapFetcherService* image_service =
      BitmapFetcherServiceFactory::GetForBrowserContext(profile_);
  if (image_service) {
    for (auto mapping : current_media_fetchers_) {
      image_service->CancelRequest(mapping.second);
    }
  }

  for (auto* const loader : url_loaders_) {
    delete loader;
  }
  url_loaders_.clear();

  bat_ledger_.reset();
  RewardsService::Shutdown();
}

void RewardsServiceImpl::OnWalletInitialized(ledger::Result result) {
  if (result == ledger::Result::WALLET_CREATED ||
      result == ledger::Result::NO_LEDGER_STATE ||
      result == ledger::Result::LEDGER_OK) {
    is_wallet_initialized_ = true;
  }

  if (!ready_.is_signaled())
    ready_.Signal();

  if (result == ledger::Result::WALLET_CREATED) {
    SetRewardsMainEnabled(true);
    SetAutoContribute(true);
    StartNotificationTimers(true);

    // Record P3A:
    RecordWalletBalanceP3A(true, true, 0);
#if BUILDFLAG(BRAVE_ADS_ENABLED)
    const bool ads_enabled =
        profile_->GetPrefs()->GetBoolean(brave_ads::prefs::kEnabled);
    RecordAdsState(ads_enabled ? AdsP3AState::kAdsEnabled
                               : AdsP3AState::kAdsDisabled);
#endif
  }

  for (auto& observer : observers_) {
    observer.OnWalletInitialized(this, static_cast<int>(result));
  }
}

void RewardsServiceImpl::OnWalletProperties(
    const ledger::Result result,
    ledger::WalletPropertiesPtr properties) {
  std::unique_ptr<brave_rewards::WalletProperties> wallet_properties;
  for (auto& observer : observers_) {
    if (properties) {
      wallet_properties.reset(new brave_rewards::WalletProperties);
      wallet_properties->parameters_choices = properties->parameters_choices;
      wallet_properties->monthly_amount = properties->fee_amount;
      wallet_properties->default_tip_choices = properties->default_tip_choices;
      wallet_properties->default_monthly_tip_choices =
          properties->default_monthly_tip_choices;
    }
    // webui
    observer.OnWalletProperties(this,
                                static_cast<int>(result),
                                std::move(wallet_properties));
  }
}

void RewardsServiceImpl::OnGetAutoContributeProps(
    const GetAutoContributePropsCallback& callback,
    ledger::AutoContributePropsPtr props) {
  if (!props) {
    callback.Run(nullptr);
    return;
  }

  auto auto_contri_props =
    std::make_unique<brave_rewards::AutoContributeProps>();
  auto_contri_props->enabled_contribute = props->enabled_contribute;
  auto_contri_props->contribution_min_time = props->contribution_min_time;
  auto_contri_props->contribution_min_visits = props->contribution_min_visits;
  auto_contri_props->contribution_non_verified =
    props->contribution_non_verified;
  auto_contri_props->contribution_videos = props->contribution_videos;
  auto_contri_props->reconcile_stamp = props->reconcile_stamp;

  callback.Run(std::move(auto_contri_props));
}

void RewardsServiceImpl::OnGetRewardsInternalsInfo(
    GetRewardsInternalsInfoCallback callback,
    ledger::RewardsInternalsInfoPtr info) {
  if (!info) {
    std::move(callback).Run(nullptr);
    return;
  }

  auto rewards_internals_info =
      std::make_unique<brave_rewards::RewardsInternalsInfo>();
  rewards_internals_info->payment_id = info->payment_id;
  rewards_internals_info->is_key_info_seed_valid = info->is_key_info_seed_valid;
  rewards_internals_info->persona_id = info->persona_id;
  rewards_internals_info->user_id = info->user_id;
  rewards_internals_info->boot_stamp = info->boot_stamp;

  // TODO(https://github.com/brave/brave-browser/issues/8633)
  // add active contributions

  std::move(callback).Run(std::move(rewards_internals_info));
}

void RewardsServiceImpl::GetAutoContributeProps(
    const GetAutoContributePropsCallback& callback) {
  if (!Connected())
    return;

  bat_ledger_->GetAutoContributeProps(base::BindOnce(
        &RewardsServiceImpl::OnGetAutoContributeProps, AsWeakPtr(), callback));
}

void RewardsServiceImpl::OnRecoverWallet(
    ledger::Result result,
    double balance) {
  for (auto& observer : observers_) {
    observer.OnRecoverWallet(
      this,
      static_cast<int>(result),
      balance);
  }
}

void RewardsServiceImpl::OnReconcileComplete(
    const ledger::Result result,
    const std::string& contribution_id,
    const double amount,
    const ledger::RewardsType type) {
  if (result == ledger::Result::LEDGER_OK &&
      type == ledger::RewardsType::RECURRING_TIP) {
    MaybeShowNotificationTipsPaid();
  }

  for (auto& observer : observers_)
    observer.OnReconcileComplete(this,
                                 static_cast<int>(result),
                                 contribution_id,
                                 amount,
                                 static_cast<int>(type));
}

void RewardsServiceImpl::LoadLedgerState(
    ledger::OnLoadCallback callback) {
  base::PostTaskAndReplyWithResult(file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&LoadStateOnFileTaskRunner, ledger_state_path_),
      base::BindOnce(&RewardsServiceImpl::OnLedgerStateLoaded,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnLedgerStateLoaded(
    ledger::OnLoadCallback callback,
    std::pair<std::string, base::Value> state) {
  if (!Connected())
    return;

  if (state.second.is_dict()) {
    // Extract some properties from the parsed json.
    base::Value* auto_contribute = state.second.FindKey("auto_contribute");
    if (auto_contribute && auto_contribute->is_bool()) {
      auto_contributions_enabled_ = auto_contribute->GetBool();
    }
    // Record stats.
    RecordBackendP3AStats();
    MaybeRecordInitialAdsP3AState(profile_->GetPrefs());
  }
  if (state.first.empty()) {
    RecordNoWalletCreatedForAllMetrics();
  }

  // Run callbacks.
  const std::string& data = state.first;
  callback(data.empty() ? ledger::Result::NO_LEDGER_STATE
                        : ledger::Result::LEDGER_OK,
                        data);

  bat_ledger_->GetRewardsMainEnabled(
      base::BindOnce(&RewardsServiceImpl::StartNotificationTimers,
        AsWeakPtr()));
}

void RewardsServiceImpl::LoadPublisherState(
    ledger::OnLoadCallback callback) {
  if (!profile_->GetPrefs()->GetBoolean(prefs::kBraveRewardsEnabledMigrated)) {
    bat_ledger_->GetRewardsMainEnabled(
        base::BindOnce(&RewardsServiceImpl::SetRewardsMainEnabledPref,
          AsWeakPtr()));
  }
  base::PostTaskAndReplyWithResult(file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&LoadOnFileTaskRunner, publisher_state_path_),
      base::BindOnce(&RewardsServiceImpl::OnPublisherStateLoaded,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnPublisherStateLoaded(
    ledger::OnLoadCallback callback,
    const std::string& data) {
  if (!Connected())
    return;

  callback(
      data.empty() ? ledger::Result::NO_PUBLISHER_STATE
                   : ledger::Result::LEDGER_OK,
      data);
}

void RewardsServiceImpl::SaveLedgerState(
    const std::string& ledger_state,
    ledger::ResultCallback callback) {
  if (reset_states_) {
    return;
  }
  base::ImportantFileWriter writer(
      ledger_state_path_, file_task_runner_);

  writer.RegisterOnNextWriteCallbacks(
      base::Closure(),
      base::Bind(
        &PostWriteCallback,
        base::Bind(&RewardsServiceImpl::OnLedgerStateSaved,
            AsWeakPtr(),
            callback),
        base::SequencedTaskRunnerHandle::Get()));

  writer.WriteNow(std::make_unique<std::string>(ledger_state));
}

void RewardsServiceImpl::OnLedgerStateSaved(
    ledger::ResultCallback callback,
    bool success) {
  if (!Connected()) {
    return;
  }

  callback(success
           ? ledger::Result::LEDGER_OK
           : ledger::Result::NO_LEDGER_STATE);
}

void RewardsServiceImpl::SavePublisherState(
    const std::string& publisher_state,
    ledger::ResultCallback callback) {
  if (reset_states_) {
    return;
  }
  base::ImportantFileWriter writer(publisher_state_path_, file_task_runner_);

  writer.RegisterOnNextWriteCallbacks(
      base::Closure(),
      base::Bind(
        &PostWriteCallback,
        base::Bind(
            &RewardsServiceImpl::OnPublisherStateSaved,
            AsWeakPtr(),
            callback),
        base::SequencedTaskRunnerHandle::Get()));

  writer.WriteNow(std::make_unique<std::string>(publisher_state));
}

void RewardsServiceImpl::OnPublisherStateSaved(
    ledger::ResultCallback callback,
    bool success) {
  if (!Connected())
    return;

  callback(success
      ? ledger::Result::LEDGER_OK
      : ledger::Result::LEDGER_ERROR);
}

void RewardsServiceImpl::LoadNicewareList(
  ledger::GetNicewareListCallback callback) {
  if (!Connected())
    return;

  std::string data = ui::ResourceBundle::GetSharedInstance().GetRawDataResource(
      IDR_BRAVE_REWARDS_NICEWARE_LIST).as_string();

  if (data.empty()) {
    LOG(ERROR) << "Failed to read in niceware list";
  }
  callback(data.empty() ? ledger::Result::LEDGER_ERROR
                        : ledger::Result::LEDGER_OK, data);
}

void RewardsServiceImpl::LoadURL(
    const std::string& url,
    const std::vector<std::string>& headers,
    const std::string& content,
    const std::string& contentType,
    const ledger::UrlMethod method,
    ledger::LoadURLCallback callback) {

  if (url.empty()) {
    callback(net::HTTP_BAD_REQUEST, "", {});
    return;
  }

  GURL parsed_url(url);
  if (!parsed_url.is_valid()) {
    callback(net::HTTP_BAD_REQUEST, "", {});
    return;
  }

  if (test_response_callback_) {
    std::string test_response;
    std::map<std::string, std::string> test_headers;
    int response_status_code = net::HTTP_OK;
    test_response_callback_.Run(url,
                                static_cast<int>(method),
                                &response_status_code,
                                &test_response,
                                &test_headers);
    callback(response_status_code, test_response, test_headers);
    return;
  }

  const std::string request_method = URLMethodToRequestType(method);
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url);
  request->method = request_method;

  // Loading Twitter requires credentials
  if (request->url.DomainIs("twitter.com")) {
    request->credentials_mode = network::mojom::CredentialsMode::kInclude;

#if defined(OS_ANDROID)
    request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent, "DESKTOP");
#endif

  } else {
    request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  }

  for (size_t i = 0; i < headers.size(); i++)
    request->headers.AddHeaderFromString(headers[i]);
  network::SimpleURLLoader* loader = network::SimpleURLLoader::Create(
      std::move(request),
      GetNetworkTrafficAnnotationTagForURLLoad()).release();
  loader->SetAllowHttpErrorResults(true);
  url_loaders_.insert(loader);
  loader->SetRetryOptions(kRetriesCountOnNetworkChange,
      network::SimpleURLLoader::RetryMode::RETRY_ON_NETWORK_CHANGE);

  if (!content.empty())
    loader->AttachStringForUpload(content, contentType);

  if (VLOG_IS_ON(ledger::LogLevel::LOG_REQUEST)) {
    std::string headers_log = "";
    for (auto const& header : headers) {
      headers_log += "> headers: " + header + "\n";
    }
  }

  loader->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      content::BrowserContext::GetDefaultStoragePartition(profile_)
          ->GetURLLoaderFactoryForBrowserProcess().get(),
      base::BindOnce(&RewardsServiceImpl::OnURLLoaderComplete,
                     base::Unretained(this),
                     loader,
                     callback));
}

void RewardsServiceImpl::OnURLLoaderComplete(
    network::SimpleURLLoader* loader,
    ledger::LoadURLCallback callback,
    std::unique_ptr<std::string> response_body) {
  DCHECK(url_loaders_.find(loader) != url_loaders_.end());
  url_loaders_.erase(loader);
  std::unique_ptr<network::SimpleURLLoader> scoped_loader(loader);

  int response_code = -1;
  if (loader->ResponseInfo() && loader->ResponseInfo()->headers)
    response_code = loader->ResponseInfo()->headers->response_code();

  std::map<std::string, std::string> headers;
  if (loader->ResponseInfo()) {
    scoped_refptr<net::HttpResponseHeaders> headersList =
        loader->ResponseInfo()->headers;

    if (headersList) {
      size_t iter = 0;
      std::string key;
      std::string value;
      while (headersList->EnumerateHeaderLines(&iter, &key, &value)) {
        key = base::ToLowerASCII(key);
        headers[key] = value;
      }
    }
  }

  if (Connected()) {
    callback(response_code,
             response_body ? *response_body : std::string(),
             headers);
  }
}

void RewardsServiceImpl::OnFetchWalletProperties(
    const ledger::Result result,
    ledger::WalletPropertiesPtr properties) {
  OnWalletProperties(result, std::move(properties));
}

void RewardsServiceImpl::FetchWalletProperties() {
  if (ready().is_signaled()) {
    if (!Connected()) {
      return;
    }

    bat_ledger_->FetchWalletProperties(
        base::BindOnce(&RewardsServiceImpl::OnFetchWalletProperties,
                       AsWeakPtr()));
  } else {
    ready().Post(FROM_HERE,
        base::Bind(&brave_rewards::RewardsService::FetchWalletProperties,
            base::Unretained(this)));
  }
}

void RewardsServiceImpl::OnFetchPromotions(
    const ledger::Result result,
    ledger::PromotionList promotions) {
  std::vector<brave_rewards::Promotion> list;

  for (auto & promotion : promotions) {
    if (!promotion) {
      continue;
    }

    brave_rewards::Promotion properties;
    properties.promotion_id = promotion->id;
    properties.amount = promotion->approximate_value;
    properties.expires_at = promotion->expires_at;
    properties.type = static_cast<uint32_t>(promotion->type);
    properties.status = static_cast<uint32_t>(promotion->status);
    list.push_back(properties);
  }

  for (auto& observer : observers_)
    observer.OnFetchPromotions(this, static_cast<int>(result), list);
}

void RewardsServiceImpl::FetchPromotions() {
  if (!Connected()) {
    return;
  }

  bat_ledger_->FetchPromotions(base::BindOnce(
      &RewardsServiceImpl::OnFetchPromotions,
      AsWeakPtr()));
}

void ParseCaptchaResponse(
    const std::string& response,
    std::string* image,
    std::string* id,
    std::string* hint) {
  base::Optional<base::Value> value = base::JSONReader::Read(response);
  if (!value || !value->is_dict()) {
    return;
  }

  base::DictionaryValue* dictionary = nullptr;
  if (!value->GetAsDictionary(&dictionary)) {
    return;
  }

  auto* captcha_image = dictionary->FindKey("captchaImage");
  if (!captcha_image || !captcha_image->is_string()) {
    return;
  }

  auto* captcha_hint = dictionary->FindKey("hint");
  if (!captcha_hint || !captcha_hint->is_string()) {
    return;
  }

  auto* captcha_id = dictionary->FindKey("captchaId");
  if (!captcha_id || !captcha_id->is_string()) {
    return;
  }

  *image = captcha_image->GetString();
  *hint = captcha_hint->GetString();
  *id = captcha_id->GetString();
}

void RewardsServiceImpl::OnClaimPromotion(
    ClaimPromotionCallback callback,
    const ledger::Result result,
    const std::string& response) {
  std::string image;
  std::string hint;
  std::string id;

  if (result != ledger::Result::LEDGER_OK) {
    std::move(callback).Run(static_cast<int32_t>(result), image, hint, id);
    return;
  }

  ParseCaptchaResponse(response, &image, &id, &hint);

  if (image.empty() || hint.empty() || id.empty()) {
    std::move(callback).Run(static_cast<int32_t>(result), "", "", "");
    return;
  }

  std::move(callback).Run(
      static_cast<int32_t>(result),
      image,
      hint,
      id);
}

void RewardsServiceImpl::AttestationAndroid(
    const std::string& promotion_id,
    AttestPromotionCallback callback,
    const ledger::Result result,
    const std::string& response) {
  if (result != ledger::Result::LEDGER_OK) {
    std::move(callback).Run(static_cast<int32_t>(result), nullptr);
    return;
  }

  const std::string nonce = android_util::ParseClaimPromotionResponse(response);
  if (nonce.empty()) {
    std::move(callback).Run(
        static_cast<int32_t>(ledger::Result::LEDGER_ERROR),
        nullptr);
    return;
  }

  #if defined(OS_ANDROID)
    auto attest_callback =
        base::BindOnce(&RewardsServiceImpl::OnAttestationAndroid,
            AsWeakPtr(),
            promotion_id,
            std::move(callback),
            nonce);
    safetynet_check_runner_.performSafetynetCheck(
        nonce,
        std::move(attest_callback));
  #endif
}

void RewardsServiceImpl::OnAttestationAndroid(
    const std::string& promotion_id,
    AttestPromotionCallback callback,
    const std::string& nonce,
    const bool token_received,
    const std::string& token,
    const bool attestation_passed) {
  if (!token_received) {
    std::move(callback).Run(
        static_cast<int32_t>(ledger::Result::LEDGER_ERROR),
        nullptr);
  }

  base::Value solution(base::Value::Type::DICTIONARY);
  solution.SetStringKey("nonce", nonce);
  solution.SetStringKey("token", token);

  std::string json;
  base::JSONWriter::Write(solution, &json);

  bat_ledger_->AttestPromotion(
      promotion_id,
      json,
      base::BindOnce(&RewardsServiceImpl::OnAttestPromotion,
          AsWeakPtr(),
          std::move(callback)));
}

void RewardsServiceImpl::ClaimPromotion(
    ClaimPromotionCallback callback) {
  if (!Connected()) {
    return;
  }

  auto claim_callback = base::BindOnce(&RewardsServiceImpl::OnClaimPromotion,
      AsWeakPtr(),
      std::move(callback));

  bat_ledger_->ClaimPromotion("", std::move(claim_callback));
}

void RewardsServiceImpl::ClaimPromotion(
    const std::string& promotion_id,
    AttestPromotionCallback callback) {
  if (!Connected()) {
    return;
  }

  auto claim_callback = base::BindOnce(&RewardsServiceImpl::AttestationAndroid,
      AsWeakPtr(),
      promotion_id,
      std::move(callback));

  bat_ledger_->ClaimPromotion("", std::move(claim_callback));
}

void RewardsServiceImpl::GetWalletPassphrase(
    const GetWalletPassphraseCallback& callback) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->GetWalletPassphrase(callback);
}

void RewardsServiceImpl::RecoverWallet(const std::string& passPhrase) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->RecoverWallet(passPhrase, base::BindOnce(
      &RewardsServiceImpl::OnRecoverWallet,
      AsWeakPtr()));
}

void RewardsServiceImpl::AttestPromotion(
    const std::string& promotion_id,
    const std::string& solution,
    AttestPromotionCallback callback) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->AttestPromotion(
      promotion_id,
      solution,
      base::BindOnce(&RewardsServiceImpl::OnAttestPromotion,
          AsWeakPtr(),
          std::move(callback)));
}

void RewardsServiceImpl::OnAttestPromotion(
    AttestPromotionCallback callback,
    const ledger::Result result,
    ledger::PromotionPtr promotion) {
  const auto result_converted = static_cast<int>(result);
  if (result != ledger::Result::LEDGER_OK) {
    std::move(callback).Run(result_converted, nullptr);
    return;
  }

  brave_rewards::Promotion brave_promotion;
  brave_promotion.amount = promotion->approximate_value;
  brave_promotion.promotion_id = promotion->id;
  brave_promotion.expires_at = promotion->expires_at;
  brave_promotion.type = static_cast<int>(promotion->type);

  for (auto& observer : observers_) {
    observer.OnPromotionFinished(
        this,
        result_converted,
        brave_promotion);
  }

  auto brave_promotion_ptr =
      std::make_unique<brave_rewards::Promotion>(brave_promotion);
  std::move(callback).Run(result_converted, std::move(brave_promotion_ptr));
}

void RewardsServiceImpl::GetReconcileStamp(
    const GetReconcileStampCallback& callback)  {
  if (!Connected()) {
    return;
  }

  bat_ledger_->GetReconcileStamp(callback);
}

void RewardsServiceImpl::SetRewardsMainEnabled(bool enabled) {
  if (!Connected()) {
    return;
  }

  if (!enabled) {
    RecordRewardsDisabledForSomeMetrics();
  }
  SetRewardsMainEnabledPref(enabled);
  bat_ledger_->SetRewardsMainEnabled(enabled);
  TriggerOnRewardsMainEnabled(enabled);
}

void RewardsServiceImpl::GetRewardsMainEnabled(
    const GetRewardsMainEnabledCallback& callback) const {
  if (!Connected()) {
    return;
  }

  bat_ledger_->GetRewardsMainEnabled(callback);
}

void RewardsServiceImpl::SetRewardsMainEnabledPref(bool enabled) {
  profile_->GetPrefs()->SetBoolean(prefs::kBraveRewardsEnabled, enabled);
  SetRewardsMainEnabledMigratedPref(true);
}

void RewardsServiceImpl::SetRewardsMainEnabledMigratedPref(bool enabled) {
  profile_->GetPrefs()->SetBoolean(
      prefs::kBraveRewardsEnabledMigrated, true);
}

void RewardsServiceImpl::SetCatalogIssuers(const std::string& json) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->SetCatalogIssuers(json);
}

void RewardsServiceImpl::ConfirmAd(
    const std::string& json,
    const std::string& confirmation_type) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->ConfirmAd(json, confirmation_type);
}

void RewardsServiceImpl::ConfirmAction(
    const std::string& creative_instance_id,
    const std::string& creative_set_id,
    const std::string& confirmation_type) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->ConfirmAction(creative_instance_id, creative_set_id,
      confirmation_type);
}

void RewardsServiceImpl::SetConfirmationsIsReady(const bool is_ready) {
  auto* ads_service = brave_ads::AdsServiceFactory::GetForProfile(profile_);
  if (ads_service)
    ads_service->SetConfirmationsIsReady(is_ready);
}

void RewardsServiceImpl::ConfirmationsTransactionHistoryDidChange() {
    for (auto& observer : observers_)
    observer.OnTransactionHistoryChanged(this);
}

void RewardsServiceImpl::GetTransactionHistory(
    GetTransactionHistoryCallback callback) {
  if (!Connected()) {
    return;
  }
  bat_ledger_->GetTransactionHistory(
      base::BindOnce(&RewardsServiceImpl::OnGetTransactionHistory,
          AsWeakPtr(), std::move(callback)));
}

void RewardsServiceImpl::OnGetTransactionHistory(
    GetTransactionHistoryCallback callback,
    const std::string& json) {
  ledger::TransactionsInfo info;
  info.FromJson(json);

  std::move(callback).Run(info.estimated_pending_rewards,
      info.next_payment_date_in_seconds,
      info.ad_notifications_received_this_month);
}

void RewardsServiceImpl::SaveState(const std::string& name,
                                   const std::string& value,
                                   ledger::ResultCallback callback) {
  if (reset_states_) {
    return;
  }
  base::ImportantFileWriter writer(
      rewards_base_path_.AppendASCII(name), file_task_runner_);

  writer.RegisterOnNextWriteCallbacks(
      base::Closure(),
      base::Bind(&PostWriteCallback,
                 base::Bind(&RewardsServiceImpl::OnSavedState,
                            AsWeakPtr(),
                            std::move(callback)),
                 base::SequencedTaskRunnerHandle::Get()));

  writer.WriteNow(std::make_unique<std::string>(value));
}

void RewardsServiceImpl::LoadState(
    const std::string& name,
    ledger::OnLoadCallback callback) {
  base::PostTaskAndReplyWithResult(
      file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&LoadOnFileTaskRunner,
                     rewards_base_path_.AppendASCII(name)),
      base::BindOnce(&RewardsServiceImpl::OnLoadedState,
                     AsWeakPtr(), std::move(callback)));
}

void RewardsServiceImpl::ResetState(
    const std::string& name,
    ledger::ResultCallback callback) {
  base::PostTaskAndReplyWithResult(
      file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ResetOnFileTaskRunner,
                     rewards_base_path_.AppendASCII(name)),
      base::BindOnce(&RewardsServiceImpl::OnResetState,
                     AsWeakPtr(), std::move(callback)));
}

void RewardsServiceImpl::ResetTheWholeState(
    const base::Callback<void(bool)>& callback) {
  reset_states_ = true;
  notification_service_->DeleteAllNotifications();
  std::vector<base::FilePath> paths;
  paths.push_back(ledger_state_path_);
  paths.push_back(publisher_state_path_);
  paths.push_back(publisher_info_db_path_);
  paths.push_back(publisher_list_path_);
  paths.push_back(rewards_base_path_);

  base::PostTaskAndReplyWithResult(
      file_task_runner_.get(), FROM_HERE,
      base::BindOnce(&ResetOnFilesTaskRunner,
                     paths),
      base::BindOnce(&RewardsServiceImpl::OnResetTheWholeState,
                     AsWeakPtr(), std::move(callback)));
}

void RewardsServiceImpl::OnResetTheWholeState(
    base::Callback<void(bool)> callback,
    bool success) {
  callback.Run(success);
}

void RewardsServiceImpl::OnSavedState(
  ledger::ResultCallback callback, bool success) {
  if (!Connected())
    return;
  callback(success ? ledger::Result::LEDGER_OK : ledger::Result::LEDGER_ERROR);
}

void RewardsServiceImpl::OnLoadedState(
    ledger::OnLoadCallback callback,
    const std::string& value) {
  if (!Connected())
    return;
  if (value.empty()) {
    RecordNoWalletCreatedForAllMetrics();
    callback(ledger::Result::LEDGER_ERROR, value);
  } else {
    callback(ledger::Result::LEDGER_OK, value);
  }
}

void RewardsServiceImpl::SetBooleanState(const std::string& name, bool value) {
  profile_->GetPrefs()->SetBoolean(pref_prefix + name, value);
}

bool RewardsServiceImpl::GetBooleanState(const std::string& name) const {
  return profile_->GetPrefs()->GetBoolean(pref_prefix + name);
}

void RewardsServiceImpl::SetIntegerState(const std::string& name, int value) {
  profile_->GetPrefs()->SetInteger(pref_prefix + name, value);
}

int RewardsServiceImpl::GetIntegerState(const std::string& name) const {
  return profile_->GetPrefs()->GetInteger(pref_prefix + name);
}

void RewardsServiceImpl::SetDoubleState(const std::string& name, double value) {
  profile_->GetPrefs()->SetDouble(pref_prefix + name, value);
}

double RewardsServiceImpl::GetDoubleState(const std::string& name) const {
  return profile_->GetPrefs()->GetDouble(pref_prefix + name);
}

void RewardsServiceImpl::SetStringState(const std::string& name,
                                        const std::string& value) {
  profile_->GetPrefs()->SetString(pref_prefix + name, value);
}

std::string RewardsServiceImpl::GetStringState(const std::string& name) const {
  return profile_->GetPrefs()->GetString(pref_prefix + name);
}

void RewardsServiceImpl::SetInt64State(const std::string& name, int64_t value) {
  profile_->GetPrefs()->SetInt64(pref_prefix + name, value);
}

int64_t RewardsServiceImpl::GetInt64State(const std::string& name) const {
  return profile_->GetPrefs()->GetInt64(pref_prefix + name);
}

void RewardsServiceImpl::SetUint64State(const std::string& name,
                                        uint64_t value) {
  profile_->GetPrefs()->SetUint64(pref_prefix + name, value);
}

uint64_t RewardsServiceImpl::GetUint64State(const std::string& name) const {
  return profile_->GetPrefs()->GetUint64(pref_prefix + name);
}

void RewardsServiceImpl::ClearState(const std::string& name) {
  profile_->GetPrefs()->ClearPref(pref_prefix + name);
}

bool RewardsServiceImpl::GetBooleanOption(const std::string& name) const {
  DCHECK(!name.empty());

  const auto it = kBoolOptions.find(name);
  DCHECK(it != kBoolOptions.end());

  return kBoolOptions.at(name);
}

int RewardsServiceImpl::GetIntegerOption(const std::string& name) const {
  DCHECK(!name.empty());

  const auto it = kIntegerOptions.find(name);
  DCHECK(it != kIntegerOptions.end());

  return kIntegerOptions.at(name);
}

double RewardsServiceImpl::GetDoubleOption(const std::string& name) const {
  DCHECK(!name.empty());

  const auto it = kDoubleOptions.find(name);
  DCHECK(it != kDoubleOptions.end());

  return kDoubleOptions.at(name);
}

std::string RewardsServiceImpl::GetStringOption(const std::string& name) const {
  DCHECK(!name.empty());

  const auto it = kStringOptions.find(name);
  DCHECK(it != kStringOptions.end());

  return kStringOptions.at(name);
}

int64_t RewardsServiceImpl::GetInt64Option(const std::string& name) const {
  DCHECK(!name.empty());

  const auto it = kInt64Options.find(name);
  DCHECK(it != kInt64Options.end());

  return kInt64Options.at(name);
}

uint64_t RewardsServiceImpl::GetUint64Option(const std::string& name) const {
  DCHECK(!name.empty());

  const auto it = kUInt64Options.find(name);
  DCHECK(it != kUInt64Options.end());

  return kUInt64Options.at(name);
}

void RewardsServiceImpl::KillTimer(uint32_t timer_id) {
  if (timers_.find(timer_id) == timers_.end())
    return;

  timers_[timer_id]->Stop();
  timers_.erase(timer_id);
}

void RewardsServiceImpl::OnResetState(
  ledger::ResultCallback callback, bool success) {
  if (!Connected())
    return;
  callback(success ? ledger::Result::LEDGER_OK : ledger::Result::LEDGER_ERROR);
}

void RewardsServiceImpl::GetPublisherMinVisitTime(
    const GetPublisherMinVisitTimeCallback& callback) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->GetPublisherMinVisitTime(callback);
}

void RewardsServiceImpl::SetPublisherMinVisitTime(
    uint64_t duration_in_seconds) const {
  if (!Connected()) {
    return;
  }

  bat_ledger_->SetPublisherMinVisitTime(duration_in_seconds);
}

void RewardsServiceImpl::GetPublisherMinVisits(
    const GetPublisherMinVisitsCallback& callback) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->GetPublisherMinVisits(callback);
}

void RewardsServiceImpl::SetPublisherMinVisits(unsigned int visits) const {
  if (!Connected()) {
    return;
  }

  bat_ledger_->SetPublisherMinVisits(visits);
}

void RewardsServiceImpl::GetPublisherAllowNonVerified(
    const GetPublisherAllowNonVerifiedCallback& callback) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->GetPublisherAllowNonVerified(callback);
}

void RewardsServiceImpl::SetPublisherAllowNonVerified(bool allow) const {
  if (!Connected()) {
    return;
  }

  bat_ledger_->SetPublisherAllowNonVerified(allow);
}

void RewardsServiceImpl::GetPublisherAllowVideos(
    const GetPublisherAllowVideosCallback& callback) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->GetPublisherAllowVideos(callback);
}

void RewardsServiceImpl::SetPublisherAllowVideos(bool allow) const {
  if (!Connected()) {
    return;
  }

  bat_ledger_->SetPublisherAllowVideos(allow);
}

void RewardsServiceImpl::SetContributionAmount(const double amount) const {
  if (!Connected()) {
    return;
  }

  bat_ledger_->SetUserChangedContribution();
  bat_ledger_->SetContributionAmount(amount);
}

// TODO(brave): Remove me (and pure virtual definition)
// see https://github.com/brave/brave-core/commit/c4ef62c954a64fca18ae83ff8ffd611137323420#diff-aa3505dbf36b5d03d8ba0751e0c99904R385
// and https://github.com/brave-intl/bat-native-ledger/commit/27f3ceb471d61c84052737ff201fe18cb9a6af32#diff-e303122e010480b2226895b9470891a3R135
void RewardsServiceImpl::SetUserChangedContribution() const {
  if (!Connected()) {
    return;
  }

  bat_ledger_->SetUserChangedContribution();
}

void RewardsServiceImpl::GetAutoContribute(
    GetAutoContributeCallback callback) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->GetAutoContribute(std::move(callback));
}

void RewardsServiceImpl::SetAutoContribute(bool enabled) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->SetAutoContribute(enabled);
  auto_contributions_enabled_ = enabled;

  // Record stats.
  DCHECK(profile_->GetPrefs()->GetBoolean(prefs::kBraveRewardsEnabled));
  if (!enabled) {
    // Just record the disabled state.
    RecordAutoContributionsState(
        AutoContributionsP3AState::kWalletCreatedAutoContributeOff, 0);
  } else {
    // Need to query the DB for actual counts.
    RecordBackendP3AStats();
  }
}

void RewardsServiceImpl::OnAdsEnabled(
    bool ads_enabled) {
  for (auto& observer : observers_)
    observer.OnAdsEnabled(this, ads_enabled);
}

void RewardsServiceImpl::TriggerOnRewardsMainEnabled(
    bool rewards_main_enabled) {
  for (auto& observer : observers_)
    observer.OnRewardsMainEnabled(this, rewards_main_enabled);
}

void RewardsServiceImpl::SetTimer(uint64_t time_offset,
                                  uint32_t* timer_id) {
  if (next_timer_id_ == std::numeric_limits<uint32_t>::max())
    next_timer_id_ = 1;
  else
    ++next_timer_id_;

  *timer_id = next_timer_id_;

  timers_[next_timer_id_] = std::make_unique<base::OneShotTimer>();
  timers_[next_timer_id_]->Start(FROM_HERE,
      base::TimeDelta::FromSeconds(time_offset),
      base::BindOnce(
          &RewardsServiceImpl::OnTimer, AsWeakPtr(), next_timer_id_));
}

void RewardsServiceImpl::OnTimer(uint32_t timer_id) {
  if (!Connected()) {
    return;
  }

  timers_.erase(timer_id);
  bat_ledger_->OnTimer(timer_id);
}

void LedgerToServiceBalanceReport(
    ledger::BalanceReportInfoPtr report,
    brave_rewards::BalanceReport* new_report) {
  if (!report || !new_report) {
    return;
  }

  new_report->grants = report->grants;
  new_report->earning_from_ads = report->earning_from_ads;
  new_report->auto_contribute = report->auto_contribute;
  new_report->recurring_donation = report->recurring_donation;
  new_report->one_time_donation = report->one_time_donation;
}

void RewardsServiceImpl::OnGetBalanceReport(
    GetBalanceReportCallback callback,
    const ledger::Result result,
    ledger::BalanceReportInfoPtr report) {
  brave_rewards::BalanceReport newReport;
  const int32_t converted_result = static_cast<int32_t>(result);
  if (result == ledger::Result::LEDGER_ERROR || !report) {
    std::move(callback).Run(converted_result, newReport);
    return;
  }

  LedgerToServiceBalanceReport(std::move(report), &newReport);
  std::move(callback).Run(converted_result, newReport);
}

void RewardsServiceImpl::GetBalanceReport(
    const uint32_t month,
    const uint32_t year,
    GetBalanceReportCallback callback) {
  bat_ledger_->GetBalanceReport(
      static_cast<ledger::ActivityMonth>(month),
      year,
      base::BindOnce(&RewardsServiceImpl::OnGetBalanceReport,
        AsWeakPtr(),
        std::move(callback)));
}

void RewardsServiceImpl::IsWalletCreated(
    const IsWalletCreatedCallback& callback) {
  if (!Connected()) {
    callback.Run(false);
    return;
  }

  bat_ledger_->IsWalletCreated(callback);
}

void RewardsServiceImpl::GetPublisherActivityFromUrl(
    uint64_t windowId,
    const std::string& url,
    const std::string& favicon_url,
    const std::string& publisher_blob) {
  GURL parsedUrl(url);

  if (!parsedUrl.is_valid()) {
    return;
  }

  auto origin = parsedUrl.GetOrigin();
  std::string baseDomain =
      GetDomainAndRegistry(origin.host(), INCLUDE_PRIVATE_REGISTRIES);

  if (baseDomain == "") {
    ledger::PublisherInfoPtr info;
    OnPanelPublisherInfo(ledger::Result::NOT_FOUND, std::move(info), windowId);
    return;
  }

  if (!Connected())
    return;

  ledger::VisitDataPtr visit_data = ledger::VisitData::New();
  visit_data->domain = visit_data->name = baseDomain;
  visit_data->path = parsedUrl.PathForRequest();
  visit_data->url = origin.spec();
  visit_data->favicon_url = favicon_url;

  bat_ledger_->GetPublisherActivityFromUrl(
        windowId,
        std::move(visit_data),
        publisher_blob);
}

void RewardsServiceImpl::OnPanelPublisherInfo(
    const ledger::Result result,
    ledger::PublisherInfoPtr info,
    uint64_t windowId) {
  if (result != ledger::Result::LEDGER_OK &&
      result != ledger::Result::NOT_FOUND) {
    return;
  }

  for (auto& observer : private_observers_)
    observer.OnPanelPublisherInfo(this,
                                  static_cast<int>(result),
                                  info.get(),
                                  windowId);
}

void RewardsServiceImpl::GetContributionAmount(
    const GetContributionAmountCallback& callback) {
  if (!Connected())
    return;

  bat_ledger_->GetContributionAmount(callback);
}

void RewardsServiceImpl::FetchFavIcon(const std::string& url,
                                      const std::string& favicon_key,
                                      ledger::FetchIconCallback callback) {
  GURL parsedUrl(url);

  if (!parsedUrl.is_valid()) {
    return;
  }

  auto it = current_media_fetchers_.find(url);
  if (it != current_media_fetchers_.end()) {
    LOG(WARNING) << "Already fetching favicon: " << url;
    return;
  }

  BitmapFetcherService* image_service =
      BitmapFetcherServiceFactory::GetForBrowserContext(profile_);
  if (image_service) {
    current_media_fetchers_[url] = image_service->RequestImage(
        parsedUrl,
        base::Bind(&RewardsServiceImpl::OnFetchFavIconCompleted,
                   base::Unretained(this), callback, favicon_key, parsedUrl),
        GetNetworkTrafficAnnotationTagForFaviconFetch());
  }
}

void RewardsServiceImpl::OnFetchFavIconCompleted(
    ledger::FetchIconCallback callback,
    const std::string& favicon_key,
    const GURL& url,
    const SkBitmap& image) {
  GURL favicon_url(favicon_key);
  gfx::Image gfx_image = gfx::Image::CreateFrom1xBitmap(image);
  favicon::FaviconService* favicon_service =
          FaviconServiceFactory::GetForProfile(profile_,
              ServiceAccessType::EXPLICIT_ACCESS);
  favicon_service->SetOnDemandFavicons(
      favicon_url,
      url,
      favicon_base::IconType::kFavicon,
      gfx_image,
      base::BindOnce(&RewardsServiceImpl::OnSetOnDemandFaviconComplete,
          AsWeakPtr(), favicon_url.spec(), callback));

  auto it_url = current_media_fetchers_.find(url.spec());
  if (it_url != current_media_fetchers_.end()) {
    current_media_fetchers_.erase(it_url);
  }
}

void RewardsServiceImpl::OnSetOnDemandFaviconComplete(
    const std::string& favicon_url,
    ledger::FetchIconCallback callback, bool success) {
  if (!Connected())
    return;

  callback(success, favicon_url);
}

void RewardsServiceImpl::GetPublisherBanner(
    const std::string& publisher_id,
    GetPublisherBannerCallback callback) {
  if (!Connected())
    return;

  bat_ledger_->GetPublisherBanner(publisher_id,
      base::BindOnce(&RewardsServiceImpl::OnPublisherBanner,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnPublisherBanner(
    GetPublisherBannerCallback callback,
    ledger::PublisherBannerPtr banner) {
  std::unique_ptr<brave_rewards::PublisherBanner> new_banner;
  new_banner.reset(new brave_rewards::PublisherBanner());

  if (!banner) {
    std::move(callback).Run(nullptr);
    return;
  }

  new_banner->publisher_key = banner->publisher_key;
  new_banner->title = banner->title;
  new_banner->name = banner->name;
  new_banner->description = banner->description;
  new_banner->background = banner->background;
  new_banner->logo = banner->logo;
  new_banner->amounts = banner->amounts;
  new_banner->links = base::FlatMapToMap(banner->links);
  new_banner->provider = banner->provider;
  new_banner->status = static_cast<uint32_t>(banner->status);

  std::move(callback).Run(std::move(new_banner));
}

void RewardsServiceImpl::OnSaveRecurringTip(
    SaveRecurringTipCallback callback,
    const ledger::Result result) {
  bool success = result == ledger::Result::LEDGER_OK;

  for (auto& observer : observers_) {
    observer.OnRecurringTipSaved(this, success);
  }

  std::move(callback).Run(success);
}

void RewardsServiceImpl::SaveRecurringTip(
    const std::string& publisher_key,
    const double amount,
    SaveRecurringTipCallback callback) {
  ledger::RecurringTipPtr info = ledger::RecurringTip::New();
  info->publisher_key = publisher_key;
  info->amount = amount;
  info->created_at = GetCurrentTimestamp();

  bat_ledger_->SaveRecurringTip(
      std::move(info),
      base::BindOnce(&RewardsServiceImpl::OnSaveRecurringTip,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnMediaInlineInfoSaved(
    SaveMediaInfoCallback callback,
    const ledger::Result result,
    ledger::PublisherInfoPtr publisher) {
  if (!Connected()) {
    std::move(callback).Run(nullptr);
    return;
  }

  std::unique_ptr<brave_rewards::ContentSite> site;

  if (result == ledger::Result::LEDGER_OK) {
    site = std::make_unique<brave_rewards::ContentSite>(
        PublisherInfoToContentSite(*publisher));
  }
  std::move(callback).Run(std::move(site));
}

void RewardsServiceImpl::SaveInlineMediaInfo(
    const std::string& media_type,
    const std::map<std::string, std::string>& args,
    SaveMediaInfoCallback callback) {
  bat_ledger_->SaveMediaInfo(
      media_type,
      base::MapToFlatMap(args),
      base::BindOnce(&RewardsServiceImpl::OnMediaInlineInfoSaved,
                    AsWeakPtr(),
                    std::move(callback)));
}

void RewardsServiceImpl::OnGetRecurringTips(
    GetRecurringTipsCallback callback,
    ledger::PublisherInfoList list) {
    std::unique_ptr<brave_rewards::ContentSiteList> new_list(
      new brave_rewards::ContentSiteList);

  for (auto& publisher : list) {
    brave_rewards::ContentSite site = PublisherInfoToContentSite(*publisher);
    site.percentage = publisher->weight;
    new_list->push_back(site);
  }

  std::move(callback).Run(std::move(new_list));
}

void RewardsServiceImpl::GetRecurringTips(
    GetRecurringTipsCallback callback) {
  bat_ledger_->GetRecurringTips(
      base::BindOnce(&RewardsServiceImpl::OnGetRecurringTips,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnGetOneTimeTips(
    GetRecurringTipsCallback callback,
    ledger::PublisherInfoList list) {
    std::unique_ptr<brave_rewards::ContentSiteList> new_list(
      new brave_rewards::ContentSiteList);

  for (auto& publisher : list) {
    brave_rewards::ContentSite site = PublisherInfoToContentSite(*publisher);
    site.percentage = publisher->weight;
    new_list->push_back(site);
  }

  std::move(callback).Run(std::move(new_list));
}

void RewardsServiceImpl::GetOneTimeTips(GetOneTimeTipsCallback callback) {
  bat_ledger_->GetOneTimeTips(
      base::BindOnce(&RewardsServiceImpl::OnGetOneTimeTips,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnRecurringTip(const ledger::Result result) {
  bool success = result == ledger::Result::LEDGER_OK;
  for (auto& observer : observers_) {
    observer.OnRecurringTipRemoved(this, success);
  }
}

void RewardsServiceImpl::RemoveRecurringTip(
    const std::string& publisher_key) {
  if (!Connected()) {
    return;
  }

  bat_ledger_->RemoveRecurringTip(
    publisher_key,
    base::Bind(&RewardsServiceImpl::OnRecurringTip,
               AsWeakPtr()));
}

void RewardsServiceImpl::UpdateAdsRewards() const {
  if (!Connected()) {
    return;
  }

  bat_ledger_->UpdateAdsRewards();
}

void RewardsServiceImpl::OnSetPublisherExclude(
    const std::string& publisher_key,
    const bool exclude,
    const ledger::Result result) {
  if (result != ledger::Result::LEDGER_OK) {
    return;
  }

  for (auto& observer : observers_) {
    observer.OnExcludedSitesChanged(this, publisher_key, exclude);
  }
}

void RewardsServiceImpl::SetPublisherExclude(
    const std::string& publisher_key,
    bool exclude) {
  if (!Connected())
    return;

  ledger::PublisherExclude status =
      exclude
      ? ledger::PublisherExclude::EXCLUDED
      : ledger::PublisherExclude::INCLUDED;

  bat_ledger_->SetPublisherExclude(
    publisher_key,
    status,
    base::BindOnce(&RewardsServiceImpl::OnSetPublisherExclude,
                   AsWeakPtr(),
                   publisher_key,
                   exclude));
}

RewardsNotificationService* RewardsServiceImpl::GetNotificationService() const {
  return notification_service_.get();
}

void RewardsServiceImpl::StartNotificationTimers(bool main_enabled) {
  if (!main_enabled) return;

  // Startup timer, begins after 30-second delay.
  PrefService* pref_service = profile_->GetPrefs();
  notification_startup_timer_ = std::make_unique<base::OneShotTimer>();
  notification_startup_timer_->Start(
      FROM_HERE,
      pref_service->GetTimeDelta(
        prefs::kRewardsNotificationStartupDelay),
      this,
      &RewardsServiceImpl::OnNotificationTimerFired);
  DCHECK(notification_startup_timer_->IsRunning());

  // Periodic timer, runs once per day by default.
  base::TimeDelta periodic_timer_interval =
      pref_service->GetTimeDelta(prefs::kRewardsNotificationTimerInterval);
  notification_periodic_timer_ = std::make_unique<base::RepeatingTimer>();
  notification_periodic_timer_->Start(
      FROM_HERE, periodic_timer_interval, this,
      &RewardsServiceImpl::OnNotificationTimerFired);
  DCHECK(notification_periodic_timer_->IsRunning());
}

void RewardsServiceImpl::StopNotificationTimers() {
  notification_startup_timer_.reset();
  notification_periodic_timer_.reset();
}

void RewardsServiceImpl::OnNotificationTimerFired() {
  if (!Connected())
    return;

  bat_ledger_->GetBootStamp(
      base::BindOnce(&RewardsServiceImpl::MaybeShowBackupNotification,
        AsWeakPtr()));
  GetReconcileStamp(
      base::Bind(&RewardsServiceImpl::MaybeShowAddFundsNotification,
        AsWeakPtr()));
  FetchPromotions();
}

void RewardsServiceImpl::MaybeShowNotificationAddFunds() {
  bat_ledger_->HasSufficientBalanceToReconcile(
      base::BindOnce(&RewardsServiceImpl::ShowNotificationAddFunds,
        AsWeakPtr()));
}

void RewardsServiceImpl::MaybeShowNotificationAddFundsForTesting(
    base::OnceCallback<void(bool)> callback) {
  bat_ledger_->HasSufficientBalanceToReconcile(
      base::BindOnce(
          &RewardsServiceImpl::OnMaybeShowNotificationAddFundsForTesting,
          AsWeakPtr(),
          std::move(callback)));
}

void RewardsServiceImpl::OnMaybeShowNotificationAddFundsForTesting(
    base::OnceCallback<void(bool)> callback,
    const bool sufficient) {
  ShowNotificationAddFunds(sufficient);
  std::move(callback).Run(sufficient);
}

bool RewardsServiceImpl::ShouldShowNotificationAddFunds() const {
  base::Time next_time =
      profile_->GetPrefs()->GetTime(prefs::kRewardsAddFundsNotification);
  return (next_time.is_null() || base::Time::Now() > next_time);
}

void RewardsServiceImpl::ShowNotificationAddFunds(bool sufficient) {
  if (sufficient) return;

  base::Time next_time = base::Time::Now() + base::TimeDelta::FromDays(3);
  profile_->GetPrefs()->SetTime(prefs::kRewardsAddFundsNotification, next_time);
  RewardsNotificationService::RewardsNotificationArgs args;
  notification_service_->AddNotification(
      RewardsNotificationService::REWARDS_NOTIFICATION_INSUFFICIENT_FUNDS, args,
      "rewards_notification_insufficient_funds");
}

void RewardsServiceImpl::MaybeShowNotificationTipsPaid() {
  GetAutoContribute(base::BindOnce(
      &RewardsServiceImpl::ShowNotificationTipsPaid,
      AsWeakPtr()));
}

void RewardsServiceImpl::ShowNotificationTipsPaid(bool ac_enabled) {
  if (ac_enabled)
    return;

  RewardsNotificationService::RewardsNotificationArgs args;
  notification_service_->AddNotification(
      RewardsNotificationService::REWARDS_NOTIFICATION_TIPS_PROCESSED, args,
      "rewards_notification_tips_processed");
}

std::unique_ptr<ledger::LogStream> RewardsServiceImpl::Log(
    const char* file,
    int line,
    const ledger::LogLevel log_level) const {
  return std::make_unique<LogStreamImpl>(file, line, log_level);
}

std::unique_ptr<ledger::LogStream> RewardsServiceImpl::VerboseLog(
                     const char* file,
                     int line,
                     int log_level) const {
  return std::make_unique<LogStreamImpl>(file, line, log_level);
}

// static
void RewardsServiceImpl::HandleFlags(const std::string& options) {
  std::vector<std::string> flags = base::SplitString(
      options, ",", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  for (const auto& flag : flags) {
    if (flag.empty()) {
      continue;
    }

    std::vector<std::string> values = base::SplitString(
      flag, "=", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

    if (values.size() != 2) {
      continue;
    }

    std::string name = base::ToLowerASCII(values[0]);
    std::string value = values[1];

    if (value.empty()) {
      continue;
    }

    if (name == "staging") {
      ledger::Environment environment;
      std::string lower = base::ToLowerASCII(value);

      if (lower == "true" || lower == "1") {
        environment = ledger::Environment::STAGING;
      } else {
        environment = ledger::Environment::PRODUCTION;
      }

      SetEnvironment(environment);
      continue;
    }

    if (name == "debug") {
      bool is_debug;
      std::string lower = base::ToLowerASCII(value);

      if (lower == "true" || lower == "1") {
        is_debug = true;
      } else {
        is_debug = false;
      }

      SetDebug(is_debug);
      continue;
    }

    if (name == "reconcile-interval") {
      int reconcile_int;
      bool success = base::StringToInt(value, &reconcile_int);

      if (success && reconcile_int > 0) {
        SetReconcileTime(reconcile_int);
      }

      continue;
    }

    if (name == "short-retries") {
      std::string lower = base::ToLowerASCII(value);
      bool short_retries;

      if (lower == "true" || lower == "1") {
        short_retries = true;
      } else {
        short_retries = false;
      }

      SetShortRetries(short_retries);
    }

    if (name == "uphold-token") {
      std::string token = base::ToLowerASCII(value);

      auto uphold = ledger::ExternalWallet::New();
      uphold->token = token;
      uphold->address = "c5fd7219-6586-4fe1-b947-0cbd25040ca8";
      uphold->status = ledger::WalletStatus::VERIFIED;
      uphold->one_time_string = "";
      uphold->user_name = "Brave Test";
      uphold->transferred = true;
      SaveExternalWallet(ledger::kWalletUphold, std::move(uphold));
      continue;
    }

    if (name == "development") {
      ledger::Environment environment;
      std::string lower = base::ToLowerASCII(value);

      if (lower == "true" || lower == "1") {
        environment = ledger::Environment::DEVELOPMENT;
        SetEnvironment(environment);
      }

      continue;
    }
  }
}

void RewardsServiceImpl::SetBackupCompleted() {
  profile_->GetPrefs()->SetBoolean(prefs::kRewardsBackupSucceeded, true);
}

void RewardsServiceImpl::GetRewardsInternalsInfo(
    GetRewardsInternalsInfoCallback callback) {
  bat_ledger_->GetRewardsInternalsInfo(
      base::BindOnce(&RewardsServiceImpl::OnGetRewardsInternalsInfo,
                     AsWeakPtr(), std::move(callback)));
}

void RewardsServiceImpl::OnTip(
    const std::string& publisher_key,
    const double amount,
    const bool recurring,
    std::unique_ptr<brave_rewards::ContentSite> site) {
  if (!site) {
    return;
  }

  auto info = ledger::PublisherInfo::New();
  info->id = publisher_key;
  info->name = site->name;
  info->url = site->url;
  info->provider = site->provider;
  info->favicon_url = site->favicon_url;

  bat_ledger_->SavePublisherInfo(
      std::move(info),
      base::BindOnce(&RewardsServiceImpl::OnTipPublisherSaved,
          AsWeakPtr(),
          publisher_key,
          amount,
          recurring));
}

void RewardsServiceImpl::OnTipPublisherSaved(
    const std::string& publisher_key,
    const double amount,
    const bool recurring,
    const ledger::Result result) {
  if (result != ledger::Result::LEDGER_OK) {
    return;
  }

  OnTip(publisher_key, amount, recurring);
}

void RewardsServiceImpl::OnTip(
    const std::string& publisher_key,
    const double amount,
    const bool recurring) {
  if (recurring) {
    SaveRecurringTip(publisher_key, amount, base::DoNothing());
    return;
  }

  bat_ledger_->OneTimeTip(publisher_key, amount, base::DoNothing());
}

bool RewardsServiceImpl::Connected() const {
  return bat_ledger_.is_bound();
}

void RewardsServiceImpl::SetLedgerEnvForTesting() {
  bat_ledger_service_->SetTesting();

  SetPublisherMinVisitTime(1);

  // this is needed because we are using braveledger_request_util::buildURL
  // directly in BraveRewardsBrowserTest
  #if defined(OFFICIAL_BUILD)
  ledger::_environment = ledger::Environment::PRODUCTION;
  #else
  ledger::_environment = ledger::Environment::STAGING;
  #endif
}

void RewardsServiceImpl::StartMonthlyContributionForTest() {
  bat_ledger_->StartMonthlyContribution();
}

void RewardsServiceImpl::CheckInsufficientFundsForTesting() {
  MaybeShowNotificationAddFunds();
}

ledger::TransferFeeList RewardsServiceImpl::GetTransferFeesForTesting(
    const std::string& wallet_type) {
  return GetTransferFees(wallet_type);
}

void RewardsServiceImpl::GetEnvironment(
    const GetEnvironmentCallback& callback) {
  bat_ledger_service_->GetEnvironment(callback);
}

void RewardsServiceImpl::GetDebug(const GetDebugCallback& callback) {
  bat_ledger_service_->GetDebug(callback);
}

void RewardsServiceImpl::GetReconcileTime(
    const GetReconcileTimeCallback& callback) {
  bat_ledger_service_->GetReconcileTime(callback);
}

void RewardsServiceImpl::GetShortRetries(
    const GetShortRetriesCallback& callback) {
  bat_ledger_service_->GetShortRetries(callback);
}

void RewardsServiceImpl::SetEnvironment(ledger::Environment environment) {
  bat_ledger_service_->SetEnvironment(environment);
}

void RewardsServiceImpl::SetDebug(bool debug) {
  bat_ledger_service_->SetDebug(debug);
}

void RewardsServiceImpl::SetReconcileTime(int32_t time) {
  bat_ledger_service_->SetReconcileTime(time);
}

void RewardsServiceImpl::SetShortRetries(bool short_retries) {
  bat_ledger_service_->SetShortRetries(short_retries);
}

void RewardsServiceImpl::GetPendingContributionsTotal(
    const GetPendingContributionsTotalCallback& callback) {
  bat_ledger_->GetPendingContributionsTotal(std::move(callback));
}

void RewardsServiceImpl::PublisherListNormalized(
    ledger::PublisherInfoList list) {
  ContentSiteList site_list;
  for (const auto& publisher : list) {
    if (publisher->percent >= 1) {
      site_list.push_back(PublisherInfoToContentSite(*publisher));
    }
  }

  for (auto& observer : observers_) {
    observer.OnPublisherListNormalized(this, site_list);
  }
}

void RewardsServiceImpl::RefreshPublisher(
    const std::string& publisher_key,
    RefreshPublisherCallback callback) {
  if (!Connected()) {
    std::move(callback).Run(
        static_cast<uint32_t>(ledger::PublisherStatus::NOT_VERIFIED),
        "");
    return;
  }
  bat_ledger_->RefreshPublisher(
      publisher_key,
      base::BindOnce(&RewardsServiceImpl::OnRefreshPublisher,
        AsWeakPtr(),
        std::move(callback),
        publisher_key));
}

void RewardsServiceImpl::OnRefreshPublisher(
    RefreshPublisherCallback callback,
    const std::string& publisher_key,
    ledger::PublisherStatus status) {
  std::move(callback).Run(static_cast<uint32_t>(status), publisher_key);
}

const RewardsNotificationService::RewardsNotificationsMap&
RewardsServiceImpl::GetAllNotifications() {
  return notification_service_->GetAllNotifications();
}

void RewardsServiceImpl::SetInlineTipSetting(const std::string& key,
                                             bool enabled) {
  bat_ledger_->SetInlineTipSetting(key, enabled);
}

void RewardsServiceImpl::GetInlineTipSetting(
      const std::string& key,
      GetInlineTipSettingCallback callback) {
  bat_ledger_->GetInlineTipSetting(
      key,
      base::BindOnce(&RewardsServiceImpl::OnInlineTipSetting,
          AsWeakPtr(),
          std::move(callback)));
}

void RewardsServiceImpl::OnInlineTipSetting(
    GetInlineTipSettingCallback callback,
    bool enabled) {
  std::move(callback).Run(enabled);
}

void RewardsServiceImpl::GetShareURL(
      const std::string& type,
      const std::map<std::string, std::string>& args,
      GetShareURLCallback callback) {
  bat_ledger_->GetShareURL(
      type,
      base::MapToFlatMap(args),
      base::BindOnce(&RewardsServiceImpl::OnShareURL,
          AsWeakPtr(),
          std::move(callback)));
}

void RewardsServiceImpl::OnShareURL(
    GetShareURLCallback callback,
    const std::string& url) {
  std::move(callback).Run(url);
}

PendingContributionInfo PendingContributionLedgerToRewards(
    const ledger::PendingContributionInfoPtr contribution) {
  PendingContributionInfo info;
  info.id = contribution->id;
  info.publisher_key = contribution->publisher_key;
  info.type = static_cast<int>(contribution->type);
  info.status = static_cast<uint32_t>(contribution->status);
  info.name = contribution->name;
  info.url = contribution->url;
  info.provider = contribution->provider;
  info.favicon_url = contribution->favicon_url;
  info.amount = contribution->amount;
  info.added_date = contribution->added_date;
  info.viewing_id = contribution->viewing_id;
  info.expiration_date = contribution->expiration_date;
  return info;
}

void RewardsServiceImpl::OnGetPendingContributions(
    GetPendingContributionsCallback callback,
    ledger::PendingContributionInfoList list) {
  std::unique_ptr<brave_rewards::PendingContributionInfoList> new_list(
      new brave_rewards::PendingContributionInfoList);
  for (auto &item : list) {
    brave_rewards::PendingContributionInfo new_contribution =
        PendingContributionLedgerToRewards(std::move(item));
    new_list->push_back(new_contribution);
  }

  std::move(callback).Run(std::move(new_list));
}

void RewardsServiceImpl::GetPendingContributions(
    GetPendingContributionsCallback callback) {
  bat_ledger_->GetPendingContributions(
      base::BindOnce(&RewardsServiceImpl::OnGetPendingContributions,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnPendingContributionRemoved(
  const ledger::Result result) {
  for (auto& observer : observers_) {
    observer.OnPendingContributionRemoved(this, static_cast<int>(result));
  }
}

void RewardsServiceImpl::RemovePendingContribution(const uint64_t id) {
  bat_ledger_->RemovePendingContribution(
      id,
      base::BindOnce(&RewardsServiceImpl::OnPendingContributionRemoved,
                     AsWeakPtr()));
}

void RewardsServiceImpl::OnRemoveAllPendingContributions(
  const ledger::Result result) {
  for (auto& observer : observers_) {
    observer.OnPendingContributionRemoved(this, static_cast<int>(result));
  }
}

void RewardsServiceImpl::RemoveAllPendingContributions() {
  bat_ledger_->RemoveAllPendingContributions(
      base::BindOnce(&RewardsServiceImpl::OnRemoveAllPendingContributions,
                     AsWeakPtr()));
}


void RewardsServiceImpl::OnContributeUnverifiedPublishers(
      ledger::Result result,
      const std::string& publisher_key,
      const std::string& publisher_name) {
  switch (result) {
    case ledger::Result::PENDING_NOT_ENOUGH_FUNDS:
    {
      RewardsNotificationService::RewardsNotificationArgs args;
      notification_service_->AddNotification(
          RewardsNotificationService::
          REWARDS_NOTIFICATION_PENDING_NOT_ENOUGH_FUNDS,
          args,
          "rewards_notification_not_enough_funds");
      break;
    }
    case ledger::Result::PENDING_PUBLISHER_REMOVED:
    {
      const auto result = static_cast<int>(ledger::Result::LEDGER_OK);
      for (auto& observer : observers_) {
        observer.OnPendingContributionRemoved(this, result);
      }
      break;
    }
    case ledger::Result::VERIFIED_PUBLISHER:
    {
      RewardsNotificationService::RewardsNotificationArgs args;
      args.push_back(publisher_name);
      notification_service_->AddNotification(
          RewardsNotificationService::REWARDS_NOTIFICATION_VERIFIED_PUBLISHER,
          args,
          "rewards_notification_verified_publisher_" + publisher_key);
      break;
    }
    default:
      break;
  }
}

void RewardsServiceImpl::OnFetchBalance(FetchBalanceCallback callback,
                                        const ledger::Result result,
                                        ledger::BalancePtr balance) {
  auto new_balance = std::make_unique<brave_rewards::Balance>();

  if (balance) {
    new_balance->total = balance->total;
    new_balance->rates = base::FlatMapToMap(balance->rates);
    new_balance->wallets = base::FlatMapToMap(balance->wallets);

    if (balance->total > 0) {
      profile_->GetPrefs()->SetBoolean(prefs::kRewardsUserHasFunded, true);
    }

    // Record stats.
    double balance_minus_grant = CalcWalletBalanceForP3A(balance->wallets,
                                                         balance->user_funds);
    RecordWalletBalanceP3A(true, true,
                           static_cast<size_t>(balance_minus_grant));
    RecordBackendP3AStats();
  }

  std::move(callback).Run(static_cast<int>(result), std::move(new_balance));
}

void RewardsServiceImpl::FetchBalance(FetchBalanceCallback callback) {
  bat_ledger_->FetchBalance(
      base::BindOnce(&RewardsServiceImpl::OnFetchBalance,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::SaveExternalWallet(const std::string& wallet_type,
                                            ledger::ExternalWalletPtr wallet) {
  auto* wallets =
      profile_->GetPrefs()->GetDictionary(prefs::kRewardsExternalWallets);

  base::Value new_wallets(wallets->Clone());

  auto* old_wallet = new_wallets.FindDictKey(wallet_type);
  base::Value new_wallet(base::Value::Type::DICTIONARY);
  if (old_wallet) {
    new_wallet = old_wallet->Clone();
  }

  new_wallet.SetStringKey("token", wallet->token);
  new_wallet.SetStringKey("address", wallet->address);
  new_wallet.SetIntKey("status", static_cast<int>(wallet->status));
  new_wallet.SetStringKey("one_time_string", wallet->one_time_string);
  new_wallet.SetStringKey("user_name", wallet->user_name);
  new_wallet.SetBoolKey("transferred", wallet->transferred);

  new_wallets.SetKey(wallet_type, std::move(new_wallet));

  profile_->GetPrefs()->Set(prefs::kRewardsExternalWallets, new_wallets);
}

void RewardsServiceImpl::GetExternalWallets(
    ledger::GetExternalWalletsCallback callback) {
  std::map<std::string, ledger::ExternalWalletPtr> wallets;

  auto* dict =
      profile_->GetPrefs()->GetDictionary(prefs::kRewardsExternalWallets);

  for (const auto& it : dict->DictItems()) {
    ledger::ExternalWalletPtr wallet = ledger::ExternalWallet::New();

    auto* token = it.second.FindKey("token");
    if (token && token->is_string()) {
      wallet->token = token->GetString();
    }

    auto* address = it.second.FindKey("address");
    if (address && address->is_string()) {
      wallet->address = address->GetString();
    }

    auto* one_time_string = it.second.FindKey("one_time_string");
    if (one_time_string && one_time_string->is_string()) {
      wallet->one_time_string = one_time_string->GetString();
    }

    auto* status = it.second.FindKey("status");
    if (status && status->is_int()) {
      wallet->status = static_cast<ledger::WalletStatus>(status->GetInt());
    }

    auto* user_name = it.second.FindKey("user_name");
    if (user_name && user_name->is_string()) {
      wallet->user_name = user_name->GetString();
    }

    auto* transferred = it.second.FindKey("transferred");
    if (transferred && transferred->is_bool()) {
      wallet->transferred = transferred->GetBool();
    }

    wallets.insert(std::make_pair(it.first, std::move(wallet)));
  }

  callback(std::move(wallets));
}

void RewardsServiceImpl::OnGetExternalWallet(
    const std::string& wallet_type,
    GetExternalWalletCallback callback,
    const ledger::Result result,
    ledger::ExternalWalletPtr wallet) {
  auto external =
      std::make_unique<brave_rewards::ExternalWallet>();

  if (wallet) {
    external->token = wallet->token;
    external->address = wallet->address;
    external->status = static_cast<int>(wallet->status);
    external->type = wallet_type;
    external->verify_url = wallet->verify_url;
    external->add_url = wallet->add_url;
    external->withdraw_url = wallet->withdraw_url;
    external->user_name = wallet->user_name;
    external->account_url = wallet->account_url;
  }

  std::move(callback).Run(static_cast<int>(result), std::move(external));
}

void RewardsServiceImpl::GetExternalWallet(const std::string& wallet_type,
                                           GetExternalWalletCallback callback) {
  bat_ledger_->GetExternalWallet(wallet_type,
      base::BindOnce(&RewardsServiceImpl::OnGetExternalWallet,
                     AsWeakPtr(),
                     wallet_type,
                     std::move(callback)));
}

void RewardsServiceImpl::OnExternalWalletAuthorization(
    const std::string& wallet_type,
    ExternalWalletAuthorizationCallback callback,
    const ledger::Result result,
    const base::flat_map<std::string, std::string>& args) {
  std::move(callback).Run(result, base::FlatMapToMap(args));
}

void RewardsServiceImpl::ExternalWalletAuthorization(
      const std::string& wallet_type,
      const std::map<std::string, std::string>& args,
      ExternalWalletAuthorizationCallback callback) {
  bat_ledger_->ExternalWalletAuthorization(
      wallet_type,
      base::MapToFlatMap(args),
      base::BindOnce(&RewardsServiceImpl::OnExternalWalletAuthorization,
                     AsWeakPtr(),
                     wallet_type,
                     std::move(callback)));
}

void RewardsServiceImpl::OnProcessExternalWalletAuthorization(
    const std::string& wallet_type,
    const std::string& action,
    ProcessRewardsPageUrlCallback callback,
    const ledger::Result result,
    const std::map<std::string, std::string>& args) {
  std::move(callback).Run(static_cast<int>(result), wallet_type, action, args);
}

void RewardsServiceImpl::ProcessRewardsPageUrl(
    const std::string& path,
    const std::string& query,
    ProcessRewardsPageUrlCallback callback) {
  auto path_items = base::SplitString(
      path,
      "/",
      base::TRIM_WHITESPACE,
      base::SPLIT_WANT_NONEMPTY);

  if (path_items.size() < 2) {
    const auto result = static_cast<int>(ledger::Result::LEDGER_ERROR);
    std::move(callback).Run(result, "", "", {});
    return;
  }

  const std::string action = path_items.at(1);
  const std::string wallet_type = path_items.at(0);

  std::map<std::string, std::string> query_map;

  const auto url = GURL("brave:/" + path + query);
  for (net::QueryIterator it(url); !it.IsAtEnd(); it.Advance()) {
    query_map[it.GetKey()] = it.GetUnescapedValue();
  }

  if (action == "authorization") {
    if (wallet_type == ledger::kWalletUphold) {
      ExternalWalletAuthorization(
          wallet_type,
          query_map,
          base::BindOnce(
              &RewardsServiceImpl::OnProcessExternalWalletAuthorization,
              AsWeakPtr(),
              wallet_type,
              action,
              std::move(callback)));
      return;
    }
  }

  const auto result = static_cast<int>(ledger::Result::LEDGER_ERROR);

  std::move(callback).Run(
      result,
      wallet_type,
      action,
      {});
}

void RewardsServiceImpl::OnDisconnectWallet(
    const std::string& wallet_type,
    const ledger::Result result) {
  for (auto& observer : observers_) {
    observer.OnDisconnectWallet(this, static_cast<int>(result), wallet_type);
  }
}

void RewardsServiceImpl::DisconnectWallet(const std::string& wallet_type) {
  bat_ledger_->DisconnectWallet(
      wallet_type,
      base::BindOnce(&RewardsServiceImpl::OnDisconnectWallet,
                     AsWeakPtr(),
                     wallet_type));
}

void RewardsServiceImpl::ShowNotification(
      const std::string& type,
      const std::vector<std::string>& args,
      ledger::ResultCallback callback) {
  if (type.empty()) {
    callback(ledger::Result::LEDGER_ERROR);
    return;
  }

  RewardsNotificationService::RewardsNotificationArgs notification_args;
  notification_args.push_back(type);
  notification_args.insert(notification_args.end(), args.begin(), args.end());
  notification_service_->AddNotification(
      RewardsNotificationService::REWARDS_NOTIFICATION_GENERAL_LEDGER,
      notification_args,
      "rewards_notification_general_ledger_" + type);
    callback(ledger::Result::LEDGER_OK);
}

void RewardsServiceImpl::SetTransferFee(
    const std::string& wallet_type,
    ledger::TransferFeePtr transfer_fee) {
  if (!transfer_fee) {
    return;
  }

  base::Value fee(base::Value::Type::DICTIONARY);
  fee.SetStringKey("id", transfer_fee->id);
  fee.SetDoubleKey("amount", transfer_fee->amount);
  fee.SetIntKey("execution_timestamp", transfer_fee->execution_timestamp);
  fee.SetIntKey("execution_id", transfer_fee->execution_id);

  auto* external_wallets =
      profile_->GetPrefs()->GetDictionary(prefs::kRewardsExternalWallets);

  base::Value new_external_wallets(external_wallets->Clone());

  auto* wallet = new_external_wallets.FindDictKey(wallet_type);
  base::Value new_wallet(base::Value::Type::DICTIONARY);
  if (wallet) {
    new_wallet = wallet->Clone();
  }

  auto* fees = wallet->FindDictKey("transfer_fees");
  base::Value new_fees(base::Value::Type::DICTIONARY);
  if (fees) {
    new_fees = fees->Clone();
  }
  new_fees.SetKey(transfer_fee->id, std::move(fee));

  new_wallet.SetKey("transfer_fees", std::move(new_fees));
  new_external_wallets.SetKey(wallet_type, std::move(new_wallet));

  profile_->GetPrefs()->Set(
      prefs::kRewardsExternalWallets,
      new_external_wallets);
}

ledger::TransferFeeList RewardsServiceImpl::GetTransferFees(
    const std::string& wallet_type) {
  ledger::TransferFeeList fees;

  auto* external_wallets =
      profile_->GetPrefs()->GetDictionary(prefs::kRewardsExternalWallets);

  auto* wallet_key = external_wallets->FindKey(wallet_type);

  const base::DictionaryValue* wallet;
  if (!wallet_key || !wallet_key->GetAsDictionary(&wallet)) {
    return fees;
  }

  auto* fees_key = wallet->FindKey("transfer_fees");
  const base::DictionaryValue* fee_dict;
  if (!fees_key || !fees_key->GetAsDictionary(&fee_dict)) {
    return fees;
  }

  for (auto& item : *fee_dict) {
    const base::DictionaryValue* fee_dict;
    if (!item.second || !item.second->GetAsDictionary(&fee_dict)) {
      continue;
    }

    auto fee = ledger::TransferFee::New();

    auto* id = fee_dict->FindKey("id");
    if (!id || !id->is_string()) {
      continue;
    }
    fee->id = id->GetString();

    auto* amount = fee_dict->FindKey("amount");
    if (!amount || !amount->is_double()) {
      continue;
    }
    fee->amount = amount->GetDouble();

    auto* timestamp = fee_dict->FindKey("execution_timestamp");
    if (!timestamp || !timestamp->is_int()) {
      continue;
    }
    fee->execution_timestamp = timestamp->GetInt();

    auto* execution_id = fee_dict->FindKey("execution_id");
    if (!execution_id || !execution_id->is_int()) {
      continue;
    }
    fee->execution_id = execution_id->GetInt();

    fees.insert(std::make_pair(fee->id, std::move(fee)));
  }

  return fees;
}

void RewardsServiceImpl::RemoveTransferFee(
    const std::string& wallet_type,
    const std::string& id) {
  auto* external_wallets =
      profile_->GetPrefs()->GetDictionary(prefs::kRewardsExternalWallets);

  base::Value new_external_wallets(external_wallets->Clone());

  const auto path = base::StringPrintf(
      "%s.transfer_fees.%s",
      wallet_type.c_str(),
      id.c_str());

  bool success = new_external_wallets.RemovePath(path);
  if (!success) {
    return;
  }

  profile_->GetPrefs()->Set(
      prefs::kRewardsExternalWallets,
      new_external_wallets);
}

bool RewardsServiceImpl::OnlyAnonWallet() {
  const int32_t current_country =
      country_codes::GetCountryIDFromPrefs(profile_->GetPrefs());

  for (const auto& country : kOnlyAnonWalletCountries) {
    if (country.length() != 2) {
      continue;
    }

    const int id = country_codes::CountryCharsToCountryID(
        country.at(0), country.at(1));

    if (id == current_country) {
      return true;
    }
  }

  return false;
}

void RewardsServiceImpl::RecordBackendP3AStats() {
  bat_ledger_->GetRecurringTips(
      base::BindOnce(&RewardsServiceImpl::OnRecordBackendP3AStatsRecurring,
          AsWeakPtr()));
}

void RewardsServiceImpl::OnRecordBackendP3AStatsRecurring(
    ledger::PublisherInfoList list) {
  bat_ledger_->GetAllContributions(
      base::BindOnce(&RewardsServiceImpl::OnRecordBackendP3AStatsContributions,
          AsWeakPtr(),
          list.size()));
}

void RewardsServiceImpl::OnRecordBackendP3AStatsContributions(
    const uint32_t recurring_donation_size,
    ledger::ContributionInfoList list) {
  int auto_contributions = 0;
  int tips = 0;
  int queued_recurring = 0;

  for (auto& contribution : list) {
    switch (contribution->type) {
    case ledger::RewardsType::AUTO_CONTRIBUTE: {
      auto_contributions += 1;
      break;
    }
    case ledger::RewardsType::ONE_TIME_TIP: {
      tips += 1;
      break;
    }
    case ledger::RewardsType::RECURRING_TIP: {
      queued_recurring += 1;
      break;
    }
    default:
      NOTREACHED();
    }
  }

  if (queued_recurring == 0) {
    queued_recurring = recurring_donation_size;
  }

  const auto auto_contributions_state = auto_contributions_enabled_ ?
        AutoContributionsP3AState::kAutoContributeOn :
        AutoContributionsP3AState::kWalletCreatedAutoContributeOff;
  RecordAutoContributionsState(auto_contributions_state, auto_contributions);
  RecordTipsState(true, true, tips, queued_recurring);
}

#if defined(OS_ANDROID)
ledger::Environment RewardsServiceImpl::GetServerEnvironmentForAndroid() {
  auto result = ledger::Environment::PRODUCTION;
  bool use_staging = false;
  if (profile_ && profile_->GetPrefs()) {
    use_staging =
        profile_->GetPrefs()->GetBoolean(prefs::kUseRewardsStagingServer);
  }

  if (use_staging) {
    result = ledger::Environment::STAGING;
  }

  return result;
}
#endif

ledger::ClientInfoPtr GetDesktopClientInfo() {
  auto info = ledger::ClientInfo::New();
  info->platform = ledger::Platform::DESKTOP;
  #if defined(OS_MACOSX)
    info->os = ledger::OperatingSystem::MACOS;
  #elif defined(OS_WIN)
    info->os = ledger::OperatingSystem::WINDOWS;
  #elif defined(OS_LINUX)
    info->os = ledger::OperatingSystem::LINUX;
  #else
    info->os = ledger::OperatingSystem::UNDEFINED;
  #endif

  info->channel = brave::GetChannelName();

  return info;
}

ledger::ClientInfoPtr RewardsServiceImpl::GetClientInfo() {
  #if defined(OS_ANDROID)
    return android_util::GetAndroidClientInfo();
  #else
    return GetDesktopClientInfo();
  #endif
}

void RewardsServiceImpl::UnblindedTokensReady() {
  for (auto& observer : observers_) {
    observer.OnUnblindedTokensReady(this);
  }
}

void RewardsServiceImpl::GetAnonWalletStatus(
    GetAnonWalletStatusCallback callback) {
  bat_ledger_->GetAnonWalletStatus(
      base::BindOnce(&RewardsServiceImpl::OnGetAnonWalletStatus,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnGetAnonWalletStatus(
    GetAnonWalletStatusCallback callback,
    const ledger::Result result) {
  std::move(callback).Run(static_cast<uint32_t>(result));
}

void RewardsServiceImpl::GetMonthlyReport(
    const uint32_t month,
    const uint32_t year,
    GetMonthlyReportCallback callback) {
  bat_ledger_->GetMonthlyReport(
      static_cast<ledger::ActivityMonth>(month),
      year,
      base::BindOnce(&RewardsServiceImpl::OnGetMonthlyReport,
          AsWeakPtr(),
          std::move(callback)));
}

void ConvertLedgerToServiceTransactionReportList(
    ledger::TransactionReportInfoList list,
    std::vector<TransactionReportInfo>* converted_list) {
  DCHECK(converted_list);

  TransactionReportInfo info;
  for (const auto& item : list) {
    info.amount = item->amount;
    info.type = static_cast<uint32_t>(item->type);
    info.processor = static_cast<uint32_t>(item->processor);
    info.created_at = item->created_at;

    converted_list->push_back(info);
  }
}

void ConvertLedgerToServiceContributionReportList(
    ledger::ContributionReportInfoList list,
    std::vector<ContributionReportInfo>* converted_list) {
  DCHECK(converted_list);

  ContributionReportInfo info;
  for (auto& item : list) {
    info.amount = item->amount;
    info.type = static_cast<uint32_t>(item->type);
    info.processor = static_cast<uint32_t>(item->processor);
    info.created_at = item->created_at;
    info.publishers = {};
    for (auto& publisher : item->publishers) {
      info.publishers.push_back(PublisherInfoToContentSite(*publisher));
    }

    converted_list->push_back(info);
  }
}

void RewardsServiceImpl::OnGetMonthlyReport(
    GetMonthlyReportCallback callback,
    const ledger::Result result,
    ledger::MonthlyReportInfoPtr report) {
  MonthlyReport monthly_report;

  if (result != ledger::Result::LEDGER_OK || !report) {
    std::move(callback).Run(monthly_report);
    return;
  }

  BalanceReport balance_report;
  LedgerToServiceBalanceReport(std::move(report->balance), &balance_report);
  monthly_report.balance = balance_report;

  std::vector<TransactionReportInfo> transaction_report;
  if (!report->transactions.empty()) {
    ConvertLedgerToServiceTransactionReportList(
        std::move(report->transactions),
        &transaction_report);
  }
  monthly_report.transactions = transaction_report;

  std::vector<ContributionReportInfo> contribution_report;
  if (!report->contributions.empty()) {
    ConvertLedgerToServiceContributionReportList(
        std::move(report->contributions),
        &contribution_report);
  }
  monthly_report.contributions = contribution_report;

  std::move(callback).Run(monthly_report);
}

void RewardsServiceImpl::ReconcileStampReset() {
  for (auto& observer : observers_) {
    observer.ReconcileStampReset();
  }
}

ledger::DBCommandResponsePtr RunDBTransactionOnFileTaskRunner(
    ledger::DBTransactionPtr transaction,
    RewardsDatabase* backend) {
  auto response = ledger::DBCommandResponse::New();
  if (!backend) {
    response->status = ledger::DBCommandResponse::Status::RESPONSE_ERROR;
  } else {
    backend->RunTransaction(std::move(transaction), response.get());
  }

  return response;
}

void RewardsServiceImpl::RunDBTransaction(
    ledger::DBTransactionPtr transaction,
    ledger::RunDBTransactionCallback callback) {
  base::PostTaskAndReplyWithResult(
      file_task_runner_.get(),
      FROM_HERE,
      base::BindOnce(&RunDBTransactionOnFileTaskRunner,
          base::Passed(std::move(transaction)),
          rewards_database_.get()),
      base::BindOnce(&RewardsServiceImpl::OnRunDBTransaction,
          AsWeakPtr(),
          std::move(callback)));
}

void RewardsServiceImpl::OnRunDBTransaction(
    ledger::RunDBTransactionCallback callback,
    ledger::DBCommandResponsePtr response) {
  callback(std::move(response));
}

void RewardsServiceImpl::GetCreateScript(
    ledger::GetCreateScriptCallback callback) {
  callback("", 0);
}

void RewardsServiceImpl::PendingContributionSaved(const ledger::Result result) {
  for (auto& observer : observers_) {
    observer.OnPendingContributionSaved(this, static_cast<int>(result));
  }
}

bool RewardsServiceImpl::IsWalletInitialized() {
  return is_wallet_initialized_;
}

void RewardsServiceImpl::ForTestingSetTestResponseCallback(
    GetTestResponseCallback callback) {
  test_response_callback_ = callback;
}

void RewardsServiceImpl::GetAllMonthlyReportIds(
      GetAllMonthlyReportIdsCallback callback) {
  bat_ledger_->GetAllMonthlyReportIds(
      base::BindOnce(&RewardsServiceImpl::OnGetAllMonthlyReportIds,
                     AsWeakPtr(),
                     std::move(callback)));
}

void RewardsServiceImpl::OnGetAllMonthlyReportIds(
    GetAllMonthlyReportIdsCallback callback,
    const std::vector<std::string>& ids) {
  std::move(callback).Run(ids);
}

}  // namespace brave_rewards
