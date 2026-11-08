#include "StreamingDestinations.hpp"

#include <util/base.h>
#include <util/config-file.h>

#include <OBSApp.hpp>

#include <QString>

#include <algorithm>
#include <cctype>

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

/* The endpoint described by one service object, whichever file or staged edit it came from: the type
 * plus the three settings fields the Output page edits. Everything else a service type carries
 * (protocol, auth, bearer token, ...) stays inside the object - the page and the duplicate check only
 * need to know where the stream goes, and createService() hands the whole object to libobs. */
void endpointFromServiceData(obs_data_t *data, StreamingDestinations::ServiceEndpoint &out)
{
	if (!data)
		return;

	/* Same key and default as OBSBasic::LoadService() uses for the profile's own service file. */
	obs_data_set_default_string(data, "type", "rtmp_common");
	out.type = getString(data, "type");

	OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
	if (settings) {
		out.provider = getString(settings, "service");
		out.server = getString(settings, "server");
		out.key = getString(settings, "key");
	}

	out.configured = !out.type.empty();
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
		/* Which file describes this destination's endpoint. Both keys default to false, so a
		 * destination added through the page and never given a server of its own has neither and
		 * simply is not configured yet. */
		dest.owns_service = config_get_bool(config_, section.c_str(), own_service_key);
		dest.follows_profile_service = config_get_bool(config_, section.c_str(), follows_profile_key);

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

		/* The endpoint state is the presence of a key: no OwnService key means the destination does
		 * not keep a service file of its own, no FollowsProfileService key means it does not follow
		 * the profile's default service. Writing the absence rather than "false" keeps a hand-edited
		 * basic.ini readable, and keeps the two states from ever being both true. */
		if (dest.owns_service)
			config_set_bool(config_, section.c_str(), own_service_key, true);
		else
			config_remove_value(config_, section.c_str(), own_service_key);

		if (dest.follows_profile_service)
			config_set_bool(config_, section.c_str(), follows_profile_key, true);
		else
			config_remove_value(config_, section.c_str(), follows_profile_key);
	}

	for (const auto &dest : destinations_)
		writeTracksFile(dest);

	/* The service edits the page staged. Written after the ini keys so the two always agree: the file
	 * a destination owns exists at the moment OwnService is in the profile, and it is gone again when
	 * the key is not. Reading a service file that the flags do not point at would put an old endpoint
	 * back into use, which is why the removal is done here and not at edit time. */
	for (auto &dest : destinations_) {
		switch (dest.pending_service) {
		case StreamingDestination::PendingService::Write:
			writeServiceFile(dest);
			break;
		case StreamingDestination::PendingService::Remove:
			removeServiceFile(dest);
			break;
		case StreamingDestination::PendingService::None:
			break;
		}
		dest.pending_service = StreamingDestination::PendingService::None;
		dest.service_type.clear();
		dest.service_settings.clear();
	}

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
			*error = QTStr("Basic.Settings.Stream.Destinations.Id.Invalid")
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

StreamingDestinations::ServiceLookup StreamingDestinations::serviceLookup(const StreamingDestination &dest) const
{
	ServiceLookup out;

	if (dest.pending_service == StreamingDestination::PendingService::Write) {
		out.source = ServiceSource::Staged;
		return out;
	}

	/* A staged removal means the destination no longer uploads where its own file says even though
	 * Save() has not deleted that file yet, so the own file is not an option from here on - only the
	 * profile's default service is. */
	if (dest.pending_service != StreamingDestination::PendingService::Remove && dest.owns_service) {
		out.source = ServiceSource::OwnFile;
		out.file = pathOf(dest.serviceFileName());
		return out;
	}

	if (dest.follows_profile_service || dest.inherits_legacy_section) {
		std::error_code ec;
		const std::filesystem::path profile_service = pathOf(legacy_service_file);
		if (std::filesystem::exists(profile_service, ec)) {
			out.source = ServiceSource::ProfileFile;
			out.file = profile_service;
			return out;
		}
	}

	/* Nothing describes it - a destination the page added and never gave an endpoint. The flags decide
	 * this rather than whether a service_<id>.json happens to sit in the profile: a file no key points
	 * at must not silently put an old stream key back into use. */
	return out;
}

StreamingDestinations::ServiceEndpoint
StreamingDestinations::fileServiceEndpoint(const StreamingDestination &dest) const
{
	ServiceEndpoint out;
	const ServiceLookup lookup = serviceLookup(dest);

	if (lookup.source == ServiceSource::Staged) {
		out.type = dest.service_type;
		OBSDataAutoRelease settings = obs_data_create_from_json(dest.service_settings.c_str());
		if (settings) {
			out.provider = getString(settings, "service");
			out.server = getString(settings, "server");
			out.key = getString(settings, "key");
		}
		out.configured = !out.type.empty();
		return out;
	}

	if (lookup.source == ServiceSource::None)
		return out;

	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(lookup.file.u8string().c_str(), "bak");
	endpointFromServiceData(data, out);
	return out;
}

StreamingDestinations::ServiceEndpoint StreamingDestinations::serviceEndpoint(std::string_view id) const
{
	const StreamingDestination *dest = find(id);
	if (!dest)
		return ServiceEndpoint();

	return fileServiceEndpoint(*dest);
}

bool StreamingDestinations::ownsService(std::string_view id) const
{
	const StreamingDestination *dest = find(id);
	if (!dest)
		return false;
	if (dest->pending_service == StreamingDestination::PendingService::Write)
		return true;
	if (dest->pending_service == StreamingDestination::PendingService::Remove)
		return false;

	return dest->owns_service;
}

bool StreamingDestinations::followsProfileService(std::string_view id) const
{
	const StreamingDestination *dest = find(id);
	if (!dest)
		return false;
	if (dest->pending_service == StreamingDestination::PendingService::Write)
		return false;

	/* The destination imported from the single-service layout *is* the profile's one service, so it
	 * follows service.json without a key of its own. */
	return dest->follows_profile_service || dest->inherits_legacy_section;
}

bool StreamingDestinations::profileServiceAvailable() const
{
	std::error_code ec;
	return std::filesystem::exists(pathOf(legacy_service_file), ec);
}

StreamingDestinations::ServiceEndpoint StreamingDestinations::profileServiceEndpoint() const
{
	ServiceEndpoint out;
	std::error_code ec;
	const std::filesystem::path file = pathOf(legacy_service_file);
	if (!std::filesystem::exists(file, ec))
		return out;

	OBSDataAutoRelease data = obs_data_create_from_json_file_safe(file.u8string().c_str(), "bak");
	endpointFromServiceData(data, out);
	return out;
}

std::string StreamingDestinations::serviceType(std::string_view id) const
{
	return serviceEndpoint(id).type;
}

OBSService StreamingDestinations::createService(std::string_view id) const
{
	const StreamingDestination *dest = find(id);
	if (!dest)
		return OBSService();

	/* Whatever describes the endpoint: the page's staged edit first, then the destination's own file,
	 * then the profile's default service for a destination that follows it. A destination with none of
	 * those cannot be started, and says so. */
	const ServiceLookup lookup = serviceLookup(*dest);
	OBSDataAutoRelease data = nullptr;
	const char *type = nullptr;

	if (lookup.source == ServiceSource::Staged) {
		data = obs_data_create();
		obs_data_set_string(data, "type", dest->service_type.c_str());
		OBSDataAutoRelease staged = obs_data_create_from_json(dest->service_settings.c_str());
		if (staged)
			obs_data_set_obj(data, "settings", staged);
		type = dest->service_type.c_str();
	} else if (lookup.source == ServiceSource::None) {
		blog(LOG_WARNING,
		     "streaming destination '%.*s' has no endpoint: no service of its own, and it follows no default",
		     static_cast<int>(id.size()), id.data());
		return OBSService();
	} else {
		data = obs_data_create_from_json_file_safe(lookup.file.u8string().c_str(), "bak");
		if (!data) {
			blog(LOG_WARNING, "streaming destination '%.*s' has no readable service file",
			     static_cast<int>(id.size()), id.data());
			return OBSService();
		}

		obs_data_set_default_string(data, "type", "rtmp_common");
		type = obs_data_get_string(data, "type");
	}

	OBSDataAutoRelease settings = obs_data_get_obj(data, "settings");
	OBSDataAutoRelease hotkeys = obs_data_get_obj(data, "hotkeys");

	OBSService service(obs_service_create(type, "stream_destination", settings, hotkeys));
	if (!service)
		blog(LOG_WARNING, "streaming destination '%.*s': service type '%s' is not available",
		     static_cast<int>(id.size()), id.data(), type ? type : "");
	/* The create reference belongs to nobody: OBSService's constructor took a reference of its own, so
	 * keeping the create one means the service outlives every owner and is still registered when
	 * obs_shutdown() has unloaded the plugins. */
	obs_service_release(service);
	return service;
}

bool StreamingDestinations::setService(std::string_view id, const std::string &type, const std::string &settings_json,
				       std::string *conflicting_id, QString *error)
{
	StreamingDestination *dest = find(id);
	if (!dest) {
		if (error)
			*error = QTStr("Basic.Settings.Stream.Destinations.Service.Error.Gone");
		return false;
	}

	if (type.empty() || settings_json.empty()) {
		if (error)
			*error = QTStr("Basic.Settings.Stream.Destinations.Service.Error.Incomplete");
		return false;
	}

	OBSDataAutoRelease settings = obs_data_create_from_json(settings_json.c_str());
	if (!settings) {
		blog(LOG_WARNING, "streaming destination '%.*s': the service settings could not be read",
		     static_cast<int>(id.size()), id.data());
		if (error)
			*error = QTStr("Basic.Settings.Stream.Destinations.Service.Error.Incomplete");
		return false;
	}

	/* Q31 first: an endpoint is only usable when no other destination is already uploading there with
	 * this key. Checked against what the others actually use, staged edits included, because two
	 * destinations edited one after the other must not end up as one stream twice. */
	ServiceEndpoint candidate;
	candidate.type = type;
	candidate.provider = getString(settings, "service");
	candidate.server = getString(settings, "server");
	candidate.key = getString(settings, "key");
	candidate.configured = true;

	if (sameServiceAndKey(candidate, dest->id, conflicting_id))
		return false;

	dest->owns_service = true;
	dest->follows_profile_service = false;
	dest->pending_service = StreamingDestination::PendingService::Write;
	dest->service_type = type;
	dest->service_settings = settings_json;

	if (error)
		error->clear();
	return true;
}

bool StreamingDestinations::clearService(std::string_view id, bool follow_profile_service, std::string *conflicting_id,
					 QString *error)
{
	StreamingDestination *dest = find(id);
	if (!dest) {
		if (error)
			*error = QTStr("Basic.Settings.Stream.Destinations.Service.Error.Gone");
		return false;
	}

	if (follow_profile_service) {
		if (!profileServiceAvailable()) {
			if (error)
				*error = QTStr("Basic.Settings.Stream.Destinations.Service.Error.NoProfileService");
			return false;
		}

		/* Same rule as setService(): a destination put back onto the profile's default service must
		 * not become the second connection to a stream another destination already makes. */
		if (sameServiceAndKey(profileServiceEndpoint(), dest->id, conflicting_id)) {
			if (error && (conflicting_id == nullptr || conflicting_id->empty()))
				*error = QTStr("Basic.Settings.Stream.Destinations.Service.Error.Duplicate");
			return false;
		}
	}

	dest->owns_service = false;
	dest->follows_profile_service = follow_profile_service;
	dest->pending_service = StreamingDestination::PendingService::Remove;
	dest->service_type.clear();
	dest->service_settings.clear();

	if (error)
		error->clear();
	return true;
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
				*error = QTStr("Basic.Settings.Stream.Destinations.Remove.Referenced")
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

bool StreamingDestinations::sameServiceAndKey(const ServiceEndpoint &candidate, std::string_view except_id,
					      std::string *conflicting_id) const
{
	if (!candidate.configured || candidate.key.empty())
		return false; /* nothing configured yet, so nothing can collide */

	for (const auto &dest : destinations_) {
		if (dest.id == except_id)
			continue;

		/* Through fileServiceEndpoint(): a destination that follows the profile's default service is
		 * compared with that service, and one the page holds an edit for is compared with what it is
		 * about to become - otherwise two cards could each pass this check against a state the other
		 * one is about to leave. */
		const ServiceEndpoint other = fileServiceEndpoint(dest);
		if (other.key.empty() || other.key != candidate.key)
			continue;

		/* Same key at the same place. Two destinations may use one provider with different keys -
		 * that is what streaming to several places is for - and one key at two ingests is nobody's
		 * business, but one key twice at one ingest is a second connection the ingest refuses. For
		 * rtmp_common the provider names its own ingest, so the provider is the place. */
		bool same_target;
		if (candidate.type == "rtmp_common" && other.type == "rtmp_common")
			same_target = ieq(other.provider, candidate.provider);
		else
			same_target = other.type == candidate.type && ieq(other.server, candidate.server);

		if (same_target) {
			if (conflicting_id)
				*conflicting_id = dest.id;
			return true;
		}
	}

	return false;
}

/* The shape of a destination's own service file is the shape of the profile's service.json: a type plus a
 * settings object, so libobs reads both the same way. Written by Save() from the staged edit, which is what
 * keeps a Cancelled dialog from leaving an endpoint behind. Destination services carry no hotkeys of their
 * own - the profile service's hotkeys stay where they are. */
void StreamingDestinations::writeServiceFile(const StreamingDestination &dest) const
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "type", dest.service_type.c_str());

	OBSDataAutoRelease settings = obs_data_create_from_json(dest.service_settings.c_str());
	if (settings)
		obs_data_set_obj(data, "settings", settings);

	const std::filesystem::path path = pathOf(dest.serviceFileName());
	if (!obs_data_save_json_safe(data, path.u8string().c_str(), "tmp", "bak"))
		blog(LOG_WARNING, "Failed to write the service file of streaming destination '%s'", dest.id.c_str());
}

void StreamingDestinations::removeServiceFile(const StreamingDestination &dest) const
{
	std::error_code ec;
	const std::filesystem::path path = pathOf(dest.serviceFileName());
	std::filesystem::remove(path, ec);
	if (ec)
		blog(LOG_WARNING, "Failed to remove the service file of streaming destination '%s': %s",
		     dest.id.c_str(), ec.message().c_str());

	/* obs_data_save_json_safe() keeps a backup and may leave a temp file behind. */
	std::filesystem::remove(std::filesystem::path(path.u8string() + ".bak"), ec);
	std::filesystem::remove(std::filesystem::path(path.u8string() + ".tmp"), ec);
}

std::vector<std::string> StreamingDestinations::profileFileNames() const
{
	std::vector<std::string> out;
	out.reserve(destinations_.size() * 2);
	for (const auto &dest : destinations_) {
		/* Only the service files a destination actually owns: a profile that copies its files must not
		 * carry a service_<id>.json that its own ini says is unused - that file would then win again
		 * after an import, once the flags are read on the other side. */
		if (dest.owns_service)
			out.push_back(dest.serviceFileName());
		out.push_back(dest.tracksFileName());
	}
	return out;
}

std::string StreamingDestinations::serviceDescription(std::string_view id) const
{
	/* Through serviceEndpoint(): whatever describes this destination right now, so the summary line of a
	 * card says where the connection would actually go - its own endpoint, the profile's default, or a
	 * change the page has staged but not written yet. */
	const ServiceEndpoint endpoint = serviceEndpoint(id);
	if (!endpoint.configured)
		return {};

	std::string out = endpoint.type;
	if (!endpoint.provider.empty())
		out += " / " + endpoint.provider;
	else if (!endpoint.server.empty())
		out += " / " + endpoint.server;

	if (!endpoint.key.empty())
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
	/* The destination this import creates *is* the profile's single service, so it says so through the
	 * endpoint state instead of through a copy of the file: what it uploads to stays whatever the Stream
	 * settings page writes into service.json until the Output page gives it an endpoint of its own.
	 * A service_<id>.json left over in a profile by an earlier build of this fork is deliberately
	 * ignored - no OwnService key points at it, and a file the flags do not name must not silently put
	 * an old stream key back into use. */
	dest.follows_profile_service = true;
	destinations_.push_back(dest);

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
