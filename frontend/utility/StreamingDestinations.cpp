#include "StreamingDestinations.hpp"

#include <util/base.h>
#include <util/config-file.h>

#include <QString>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

constexpr const char *legacy_service_file = "service.json";
constexpr const char *legacy_encoder_file = "streamEncoder.json";
constexpr const char *legacy_stream_section = "Stream1";
constexpr const char *schema_version_key = "schema_version";
constexpr const char *video_tracks_key = "video_tracks";

bool idCharOk(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* The ini list keys hold comma-separated ids. Blank entries are ignored so a trailing comma in a
 * hand-edited basic.ini does not invent an empty destination. */
std::vector<std::string> splitList(const char *value)
{
	std::vector<std::string> out;
	if (!value)
		return out;

	const std::string text{value};
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t comma = text.find(',', pos);
		if (comma == std::string::npos)
			comma = text.size();

		std::string item = text.substr(pos, comma - pos);
		const auto not_space = [](unsigned char c) {
			return std::isspace(c) == 0;
		};
		item.erase(item.begin(), std::find_if(item.begin(), item.end(), not_space));
		item.erase(std::find_if(item.rbegin(), item.rend(), not_space).base(), item.end());
		if (!item.empty())
			out.push_back(std::move(item));

		if (comma == text.size())
			break;
		pos = comma + 1;
	}
	return out;
}

std::string joinList(const std::vector<std::string> &items)
{
	std::string out;
	for (size_t i = 0; i < items.size(); i++) {
		if (i != 0)
			out += ',';
		out += items[i];
	}
	return out;
}

/* Keeps the null case out of the callers. */
std::string getString(obs_data_t *data, const char *key)
{
	const char *value = data ? obs_data_get_string(data, key) : nullptr;
	return value ? value : "";
}

bool ieq(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	}
	return true;
}

/* The bitrate of one video track, from the encoder settings block - the same key the output handler
	 * reads ("bitrate", see AdvancedOutput.cpp). */
long long trackBitrate(obs_data_t *track)
{
	if (!track)
		return 0;

	OBSDataAutoRelease settings = obs_data_get_obj(track, "settings");
	if (!settings)
		return 0;

	const long long value = obs_data_get_int(settings, "bitrate");
	return value > 0 ? value : 0;
}

} // namespace

std::string StreamingDestination::serviceFileName() const
{
	return "service_" + id + ".json";
}

std::string StreamingDestination::tracksFileName() const
{
	return "stream_" + id + ".json";
}

std::string StreamingDestination::iniSection() const
{
	return "Stream-" + id;
}

bool StreamingDestinations::pageEnabled(config_t *profile_config)
{
	if (!profile_config)
		return false;
	if (config_get_bool(profile_config, page_flag_section, page_flag_key))
		return true;

	/* A profile that already lists destinations has the feature in everything but name - the list only
	 * exists because the page (or a hand edit) wrote it, and copying a profile carries the list without the
	 * flag. Hiding the page in that case leaves a profile streaming to several places with no way to see or
	 * change them, so the stored list counts as opting in. Flip the flag off in the profile to hide it again. */
	const char *list = config_get_string(profile_config, list_section, list_key);
	return list && *list;
}

void StreamingDestinations::Load(config_t *profile_config, const std::filesystem::path &profile_dir)
{
	config_ = profile_config;
	dir_ = profile_dir;
	destinations_.clear();
	retired_.clear();
	pending_removals_.clear();

	if (!config_)
		return;

	retired_ = splitList(config_get_string(config_, list_section, retired_key));

	for (const auto &id : splitList(config_get_string(config_, list_section, list_key))) {
		if (!validId(id)) {
			blog(LOG_WARNING,
			     "Streaming destination id '%s' ignored: ids are 1-32 characters from a-z, 0-9, '_' or '-',",
			     id.c_str());
			blog(LOG_WARNING, "starting with a letter or a digit");
			continue;
		}

		StreamingDestination dest;
		dest.id = id;
		const std::string section = dest.iniSection();

		const char *name = config_get_string(config_, section.c_str(), name_key);
		dest.name = (name && *name) ? name : id;
		dest.enabled = config_has_user_value(config_, section.c_str(), enabled_key)
				       ? config_get_bool(config_, section.c_str(), enabled_key)
				       : true;
		dest.inherits_legacy_section = config_has_user_value(config_, section.c_str(), inherits_key);

		destinations_.push_back(std::move(dest));
	}
}

bool StreamingDestinations::Save()
{
	if (!config_)
		return false;

	config_set_string(config_, list_section, list_key, joinList(ids()).c_str());
	config_set_string(config_, list_section, retired_key, joinList(retired_).c_str());

	for (const auto &dest : destinations_) {
		const std::string section = dest.iniSection();
		config_set_string(config_, section.c_str(), name_key, dest.name.c_str());
		config_set_bool(config_, section.c_str(), enabled_key, dest.enabled);
		if (dest.inherits_legacy_section)
			config_set_string(config_, section.c_str(), inherits_key, legacy_stream_section);
		else
			config_remove_value(config_, section.c_str(), inherits_key);
	}

	for (const auto &dest : destinations_)
		writeTracksFile(dest);

	for (const std::string &removed : pending_removals_) {
		StreamingDestination gone;
		gone.id = removed;
		removeDestinationFiles(gone);
	}
	pending_removals_.clear();

	return true;
}
StreamingDestination *StreamingDestinations::find(std::string_view id)
{
	auto it = std::find_if(destinations_.begin(), destinations_.end(),
			       [&](const StreamingDestination &d) { return d.id == id; });
	return it == destinations_.end() ? nullptr : &*it;
}

const StreamingDestination *StreamingDestinations::find(std::string_view id) const
{
	auto it = std::find_if(destinations_.begin(), destinations_.end(),
			       [&](const StreamingDestination &d) { return d.id == id; });
	return it == destinations_.end() ? nullptr : &*it;
}

bool StreamingDestinations::validId(std::string_view id)
{
	if (id.empty() || id.size() > 32)
		return false;
	if (std::isalnum(static_cast<unsigned char>(id.front())) == 0)
		return false;
	return std::all_of(id.begin(), id.end(), [](char c) { return idCharOk(c); });
}

std::string StreamingDestinations::makeId(std::string_view base) const
{
	std::string id;
	for (char c : base) {
		char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (idCharOk(lower))
			id += lower;
		else if (std::isspace(static_cast<unsigned char>(c)))
			id += '_';
		/* Anything else is dropped: the id ends up in a file name and an ini section name. */
	}

	const size_t first_alnum = id.find_first_of("abcdefghijklmnopqrstuvwxyz0123456789");
	if (first_alnum == std::string::npos)
		id.clear();
	else if (first_alnum > 0)
		id.erase(0, first_alnum);
	if (id.empty())
		id = "dest";
	if (id.size() > 32)
		id.resize(32);

	std::string candidate = id;
	for (int suffix = 2;; suffix++) {
		if (validId(candidate) && find(candidate) == nullptr && !idRetired(candidate))
			break;
		const std::string tail = "_" + std::to_string(suffix);
		candidate = id.substr(0, std::min<size_t>(id.size(), 32 - tail.size())) + tail;
	}

	return candidate;
}

StreamingDestination *StreamingDestinations::create(std::string_view base, const std::string &display_name,
						    QString *error)
{
	StreamingDestination dest;
	dest.id = makeId(base);
	dest.name = display_name.empty() ? dest.id : display_name;
	dest.enabled = true;

	destinations_.push_back(dest);

	if (error)
		error->clear();
	return find(dest.id);
}

bool StreamingDestinations::idTaken(std::string_view id) const
{
	return find(id) != nullptr;
}

bool StreamingDestinations::idUsable(std::string_view id) const
{
	return validId(id) && !idTaken(id) && !idRetired(id);
}

StreamingDestination *StreamingDestinations::createWithId(std::string_view requested_id,
							  const std::string &display_name, QString *error)
{
	if (!idUsable(requested_id)) {
		if (error)
			*error = QString("The id '%1' cannot be used: 1-32 characters from a-z, 0-9, '_' or '-', "
					 "starting with a letter or a digit, and never reused after a destination "
					 "was removed.")
					 .arg(QString::fromStdString(std::string(requested_id)));
		return nullptr;
	}

	StreamingDestination dest;
	dest.id = std::string(requested_id);
	dest.name = display_name.empty() ? dest.id : display_name;
	dest.enabled = true;

	destinations_.push_back(dest);

	if (error)
		error->clear();
	return find(dest.id);
}

bool StreamingDestinations::reorder(const std::vector<std::string> &ids)
{
	list_type reordered;
	reordered.reserve(destinations_.size());

	for (const auto &id : ids) {
		StreamingDestination *dest = find(id);
		if (dest)
			reordered.push_back(*dest);
	}

	/* Anything the caller did not mention keeps its place at the end, so a partial list can never drop
	 * a destination. */
	for (const auto &dest : destinations_) {
		auto it = std::find_if(reordered.begin(), reordered.end(),
				       [&](const StreamingDestination &d) { return d.id == dest.id; });
		if (it == reordered.end())
			reordered.push_back(dest);
	}

	if (reordered.size() != destinations_.size())
		return false;

	destinations_.swap(reordered);
	return true;
}

StreamingDestinations::BitrateBreakdown StreamingDestinations::bitrateFor(std::string_view id) const
{
	BitrateBreakdown info;
	const StreamingDestination *dest = find(id);
	if (!dest)
		return info;

	OBSDataAutoRelease data =
		obs_data_create_from_json_file_safe(pathOf(dest->tracksFileName()).u8string().c_str(), "bak");
	if (data) {
		OBSDataArrayAutoRelease tracks = obs_data_get_array(data, video_tracks_key);
		if (tracks) {
			for (size_t i = 0; i < obs_data_array_count(tracks); i++) {
				OBSDataAutoRelease track = obs_data_array_item(tracks, i);
				const long long bitrate = trackBitrate(track);
				if (bitrate > 0)
					info.videoKbps += bitrate;
			}
		}
	}

	/* No video tracks of its own: the destination is fed by the shared stream encoder, so its video bitrate
	 * is the one in the encoder file the advanced settings write - whether it came from the single-stream
	 * import or was added later. It is marked as shared so the page does not present it as a per-destination
	 * value. */
	if (info.videoKbps == 0) {
		OBSDataAutoRelease legacy =
			obs_data_create_from_json_file_safe(pathOf(legacy_encoder_file).u8string().c_str(), "bak");
		if (legacy) {
			const long long bitrate = obs_data_get_int(legacy, "bitrate");
			if (bitrate > 0) {
				info.videoKbps = bitrate;
				info.shared_video = true;
			}
		}
	}

	info.audioKbps = streamingAudioKbps(id);
	return info;
}

long long StreamingDestinations::streamingAudioKbps(std::string_view id) const
{
	if (!config_)
		return 0;

	auto mixBitrate = [this](int mix) -> long long {
		static const char *names[] = {"Track1Bitrate", "Track2Bitrate", "Track3Bitrate",
					      "Track4Bitrate", "Track5Bitrate", "Track6Bitrate"};
		if (mix < 1 || mix > 6)
			return 0;
		return config_get_int(config_, "AdvOut", names[mix - 1]);
	};

	/* Mirrors how the streaming output is wired up in AdvancedOutput::SetupStreaming()
	 * (AdvancedOutput.cpp:334,347-356,733-743): the mixes marked in the [AdvOut] StreamMultiTrackAudioMixes
	 * bitmask. A service that cannot take multitrack audio gets a single mix instead, which is why this number
	 * is what a multitrack-capable destination uploads - said out loud in the page's tooltip. */
	const unsigned mixes = static_cast<unsigned>(config_get_int(config_, "AdvOut", "StreamMultiTrackAudioMixes"));
	long long total = 0;
	if (mixes != 0) {
		for (int mix = 1; mix <= 6; mix++) {
			if ((mixes & (1u << (mix - 1))) != 0)
				total += mixBitrate(mix);
		}
	} else {
		total = mixBitrate(static_cast<int>(config_get_int(config_, "AdvOut", "TrackIndex")));
	}

	/* A VOD track is a second copy of one mix on the same connection, and only the services that support
	 * it get one. */
	if (usesVodTrack(id))
		total += mixBitrate(static_cast<int>(config_get_int(config_, "AdvOut", "VodTrackIndex")));

	return total;
}

long long StreamingDestinations::totalUploadKbps() const
{
	long long total = 0;
	for (const auto &dest : destinations_) {
		if (!dest.enabled)
			continue;
		const BitrateBreakdown info = bitrateFor(dest.id);
		total += info.videoKbps + info.audioKbps;
	}
	return total;
}

std::string StreamingDestinations::serviceType(std::string_view id) const
{
	const StreamingDestination *dest = find(id);
	if (!dest)
		return std::string();

	OBSDataAutoRelease data =
		obs_data_create_from_json_file_safe(pathOf(dest->serviceFileName()).u8string().c_str(), "bak");
	if (!data)
		return std::string();

	/* Same key and default as OBSBasic::LoadService() uses for the profile's own service file. */
	obs_data_set_default_string(data, "type", "rtmp_common");
	return obs_data_get_string(data, "type");
}

OBSService StreamingDestinations::createService(std::string_view id) const
{
	const StreamingDestination *dest = find(id);
	if (!dest)
		return OBSService();

	OBSDataAutoRelease data =
		obs_data_create_from_json_file_safe(pathOf(dest->serviceFileName()).u8string().c_str(), "bak");
	if (!data) {
		blog(LOG_WARNING, "streaming destination '%.*s' has no readable service file",
		     static_cast<int>(id.size()), id.data());
		return OBSService();
	}

	obs_data_set_default_string(data, "type", "rtmp_common");
	const char *type = obs_data_get_string(data, "type");

	OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
	OBSDataAutoRelease hotkeys = obs_data_get_obj(data, "hotkeys");

	OBSService service(obs_service_create(type, "stream_destination", settings, hotkeys));
	if (!service)
		blog(LOG_WARNING, "streaming destination '%.*s': service type '%s' is not available",
		     static_cast<int>(id.size()), id.data(), type ? type : "");
	return service;
}

bool StreamingDestinations::usesVodTrack(std::string_view id) const
{
	if (!config_ || !config_get_bool(config_, "AdvOut", "VodTrackEnabled"))
		return false;

	/* The same service list the output handler checks (ServiceSupportsVodTrack, BasicOutputHandler.hpp);
	 * duplicated on purpose, because the storage layer must not depend on the output handler. */
	static const char *vodTrackServices[] = {"Twitch"};
	const std::string type = serviceType(id);
	for (const char *service : vodTrackServices) {
		if (ieq(service, type))
			return true;
	}
	return false;
}

bool StreamingDestinations::setDisplayName(std::string_view id, const std::string &display_name)
{
	StreamingDestination *dest = find(id);
	if (!dest || display_name.empty())
		return false;

	dest->name = display_name;
	return true;
}

bool StreamingDestinations::setEnabled(std::string_view id, bool enabled)
{
	StreamingDestination *dest = find(id);
	if (!dest)
		return false;

	dest->enabled = enabled;
	return true;
}
bool StreamingDestinations::destroy(std::string_view id, QString *error)
{
	StreamingDestination *dest = find(id);
	if (!dest)
		return false;

	for (const auto &other : destinations_) {
		if (other.id == dest->id)
			continue;
		if (references(other.id, dest->id)) {
			if (error)
				*error =
					QString("Destination '%1' still sends a track shared from '%2'. Give it its own "
						"track before removing '%2'.")
						.arg(QString::fromStdString(other.name))
						.arg(QString::fromStdString(dest->name));
			return false;
		}
	}

	/* Q22: a removed id is never handed out again, otherwise a stale reference would resolve to an
	 * unrelated destination. The [Stream-<id>] ini section is kept for the same reason - no later
	 * destination can pick it up, and deleting keys the user typed would lose data. */
	retired_.push_back(dest->id);
	pending_removals_.push_back(dest->id);
	destinations_.erase(std::find_if(destinations_.begin(), destinations_.end(),
					 [&](const StreamingDestination &d) { return d.id == dest->id; }));
	return true;
}

bool StreamingDestinations::sameServiceAndKey(const char *service_type, obs_data_t *settings,
					      std::string *conflicting_id) const
{
	if (!service_type || !settings)
		return false;

	const std::string key = getString(settings, "key");
	if (key.empty())
		return false; /* nothing configured yet, so nothing can collide */

	const std::string provider = getString(settings, "service");
	const std::string server = getString(settings, "server");

	for (const auto &dest : destinations_) {
		OBSDataAutoRelease data =
			obs_data_create_from_json_file_safe(pathOf(dest.serviceFileName()).u8string().c_str(), "bak");
		if (!data)
			continue;

		OBSDataAutoRelease other_settings = obs_data_get_obj(data, "settings");
		if (!other_settings)
			continue;

		const std::string other_key = getString(other_settings, "key");
		if (other_key.empty() || other_key != key)
			continue;

		const std::string other_type = getString(data, "type");
		bool same_target;
		if (strcmp(service_type, "rtmp_common") == 0 && other_type == "rtmp_common")
			same_target = ieq(getString(other_settings, "service"), provider);
		else
			same_target = other_type == service_type && ieq(server, getString(other_settings, "server"));

		if (same_target) {
			if (conflicting_id)
				*conflicting_id = dest.id;
			return true;
		}
	}

	return false;
}

std::vector<std::string> StreamingDestinations::profileFileNames() const
{
	std::vector<std::string> out;
	out.reserve(destinations_.size() * 2);
	for (const auto &dest : destinations_) {
		out.push_back(dest.serviceFileName());
		out.push_back(dest.tracksFileName());
	}
	return out;
}

std::string StreamingDestinations::serviceDescription(std::string_view id) const
{
	const StreamingDestination *dest = find(id);
	if (!dest)
		return {};

	OBSDataAutoRelease data =
		obs_data_create_from_json_file_safe(pathOf(dest->serviceFileName()).u8string().c_str(), "bak");
	if (!data)
		return {};

	std::string out = getString(data, "type");
	OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
	if (!settings)
		return out;

	const std::string provider = getString(settings, "service");
	const std::string server = getString(settings, "server");
	if (!provider.empty())
		out += " / " + provider;
	else if (!server.empty())
		out += " / " + server;

	if (!getString(settings, "key").empty())
		out += " (*)"; /* (*) = a stream key is set */

	return out;
}

size_t StreamingDestinations::trackCount(std::string_view id) const
{
	const StreamingDestination *dest = find(id);
	if (!dest)
		return 0;

	OBSDataAutoRelease data =
		obs_data_create_from_json_file_safe(pathOf(dest->tracksFileName()).u8string().c_str(), "bak");
	if (!data)
		return 0;

	OBSDataArrayAutoRelease tracks = obs_data_get_array(data, video_tracks_key);
	return tracks ? obs_data_array_count(tracks) : 0;
}

const char *StreamingDestinations::configString(std::string_view id, const char *key, const char *fallback) const
{
	const StreamingDestination *dest = find(id);
	if (!config_ || !dest)
		return fallback;

	const std::string section = dest->iniSection();
	if (config_has_user_value(config_, section.c_str(), key))
		return config_get_string(config_, section.c_str(), key);
	if (dest->inherits_legacy_section && config_has_user_value(config_, legacy_stream_section, key))
		return config_get_string(config_, legacy_stream_section, key);
	return fallback;
}

bool StreamingDestinations::configBool(std::string_view id, const char *key, bool fallback) const
{
	const StreamingDestination *dest = find(id);
	if (!config_ || !dest)
		return fallback;

	const std::string section = dest->iniSection();
	if (config_has_user_value(config_, section.c_str(), key))
		return config_get_bool(config_, section.c_str(), key);
	if (dest->inherits_legacy_section && config_has_user_value(config_, legacy_stream_section, key))
		return config_get_bool(config_, legacy_stream_section, key);
	return fallback;
}

long long StreamingDestinations::configInt(std::string_view id, const char *key, long long fallback) const
{
	const StreamingDestination *dest = find(id);
	if (!config_ || !dest)
		return fallback;

	const std::string section = dest->iniSection();
	if (config_has_user_value(config_, section.c_str(), key))
		return config_get_int(config_, section.c_str(), key);
	if (dest->inherits_legacy_section && config_has_user_value(config_, legacy_stream_section, key))
		return config_get_int(config_, legacy_stream_section, key);
	return fallback;
}

void StreamingDestinations::setConfigString(std::string_view id, const char *key, const char *value)
{
	const StreamingDestination *dest = find(id);
	if (!config_ || !dest)
		return;
	const std::string section = dest->iniSection();
	config_set_string(config_, section.c_str(), key, value);
}

void StreamingDestinations::setConfigBool(std::string_view id, const char *key, bool value)
{
	const StreamingDestination *dest = find(id);
	if (!config_ || !dest)
		return;
	const std::string section = dest->iniSection();
	config_set_bool(config_, section.c_str(), key, value);
}

void StreamingDestinations::setConfigInt(std::string_view id, const char *key, long long value)
{
	const StreamingDestination *dest = find(id);
	if (!config_ || !dest)
		return;
	const std::string section = dest->iniSection();
	config_set_int(config_, section.c_str(), key, value);
}

StreamingDestinations::ReconnectPolicy StreamingDestinations::reconnectPolicy(std::string_view id) const
{
	ReconnectPolicy policy = profileReconnectPolicy();

	policy.overridden = configBool(id, reconnect_override_key, false);
	if (policy.overridden) {
		policy.enabled = configBool(id, reconnect_key, policy.enabled);
		policy.retryDelaySec = configInt(id, retry_delay_key, policy.retryDelaySec);
		policy.maxRetries = configInt(id, max_retries_key, policy.maxRetries);
	}
	return policy;
}

StreamingDestinations::ReconnectPolicy StreamingDestinations::profileReconnectPolicy() const
{
	ReconnectPolicy policy;
	if (!config_)
		return policy;

	policy.enabled = config_get_bool(config_, profile_section, reconnect_key);
	policy.retryDelaySec = config_get_int(config_, profile_section, retry_delay_key);
	policy.maxRetries = config_get_int(config_, profile_section, max_retries_key);
	return policy;
}

void StreamingDestinations::setReconnectPolicy(std::string_view id, const ReconnectPolicy &policy)
{
	setConfigBool(id, reconnect_override_key, policy.overridden);
	if (!policy.overridden)
		return;

	setConfigBool(id, reconnect_key, policy.enabled);
	setConfigInt(id, retry_delay_key, policy.retryDelaySec);
	setConfigInt(id, max_retries_key, policy.maxRetries);
}

bool StreamingDestinations::importLegacyLayout()
{
	if (!config_ || dir_.empty() || !destinations_.empty())
		return false;

	std::error_code ec;
	const std::filesystem::path legacy = pathOf(legacy_service_file);
	if (!std::filesystem::exists(legacy, ec))
		return false;

	std::string base = "stream";
	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(legacy.u8string().c_str(), "bak");
	if (data) {
		OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
		if (settings) {
			const std::string provider = getString(settings, "service");
			const std::string server = getString(settings, "server");
			if (!provider.empty())
				base = provider;
			else if (!server.empty())
				base = server;
		}
	}

	StreamingDestination dest;
	dest.id = makeId(base);
	dest.name = dest.id;
	dest.enabled = true;
	dest.inherits_legacy_section = true;
	destinations_.push_back(dest);

	/* service.json is left in place: the engine still loads its single service from there until the
	 * output handler starts using the per-destination copies, and the original stays as the fallback
	 * if this feature is switched off again. */
	std::filesystem::copy(legacy, pathOf(dest.serviceFileName()), std::filesystem::copy_options::skip_existing, ec);
	if (ec)
		blog(LOG_WARNING, "Streaming destination import: failed to copy '%s' to '%s': %s", legacy_service_file,
		     dest.serviceFileName().c_str(), ec.message().c_str());

	Save();
	blog(LOG_INFO, "Imported the single-service configuration as streaming destination '%s'", dest.id.c_str());
	return true;
}

std::vector<std::string> StreamingDestinations::ids() const
{
	std::vector<std::string> out;
	out.reserve(destinations_.size());
	for (const auto &dest : destinations_)
		out.push_back(dest.id);
	return out;
}

bool StreamingDestinations::idRetired(std::string_view id) const
{
	return std::find(retired_.begin(), retired_.end(), id) != retired_.end();
}

bool StreamingDestinations::references(std::string_view from, std::string_view target) const
{
	const StreamingDestination *owner = find(from);
	if (!owner)
		return false;

	OBSDataAutoRelease data =
		obs_data_create_from_json_file_safe(pathOf(owner->tracksFileName()).u8string().c_str(), "bak");
	if (!data)
		return false;

	OBSDataArrayAutoRelease tracks = obs_data_get_array(data, video_tracks_key);
	if (!tracks)
		return false;

	for (size_t i = 0; i < obs_data_array_count(tracks); i++) {
		OBSDataAutoRelease track = obs_data_array_item(tracks, i);
		if (!track)
			continue;
		for (const char *ref_key : {"shared_from", "copied_from"}) {
			OBSDataAutoRelease ref = obs_data_get_obj(track, ref_key);
			if (ref && getString(ref, "destination") == target)
				return true;
		}
	}

	return false;
}

void StreamingDestinations::writeTracksFile(const StreamingDestination &dest) const
{
	const std::filesystem::path path = pathOf(dest.tracksFileName());

	std::error_code ec;
	if (std::filesystem::exists(path, ec))
		return;

	OBSDataAutoRelease data = obs_data_create();
	OBSDataArrayAutoRelease tracks = obs_data_array_create();
	obs_data_set_string(data, schema_version_key, "1");
	obs_data_set_array(data, video_tracks_key, tracks);

	if (!obs_data_save_json_safe(data, path.u8string().c_str(), "tmp", "bak"))
		blog(LOG_WARNING, "Failed to create the video-track file of streaming destination '%s'",
		     dest.id.c_str());
}

void StreamingDestinations::removeDestinationFiles(const StreamingDestination &dest) const
{
	std::error_code ec;
	for (const std::string &file : {dest.serviceFileName(), dest.tracksFileName()}) {
		const std::filesystem::path path = pathOf(file);
		std::filesystem::remove(path, ec);
		/* obs_data_save_json_safe() keeps a backup and may leave a temp file behind. */
		std::filesystem::remove(std::filesystem::path(path.u8string() + ".bak"), ec);
		std::filesystem::remove(std::filesystem::path(path.u8string() + ".tmp"), ec);
	}
}

std::filesystem::path StreamingDestinations::pathOf(const std::string &file_name) const
{
	/* Ids are restricted to ASCII, so no encoding conversion is required here (and
	 * std::filesystem::u8path() is deprecated in C++20). */
	return dir_ / file_name;
}
