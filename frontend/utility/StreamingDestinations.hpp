#pragma once

#include <obs.hpp>
#include <util/config-file.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

class QString;

/* A single streaming destination: one service (server + key), its own video tracks and, once the
 * engine supports it, its own multitrack-video session.
 *
 * The id is the stable handle: it names the per-destination files (service_<id>.json,
 * stream_<id>.json) and the ini section ([Stream-<id>]), and it is what other destinations use in
 * "shared_from"/"copied_from" references. It is generated once and never edited, which is what keeps
 * those references and file names valid; the display name is free-form text for the UI only. */
struct StreamingDestination {
	std::string id;
	std::string name;
	bool enabled = true;
	/* True for the destination imported from the previous single-stream layout: its non-video keys
	 * still live in [Stream1] and are only written into [Stream-<id>] when the user changes one. */
	bool inherits_legacy_section = false;

	std::string serviceFileName() const;
	std::string tracksFileName() const;
	std::string iniSection() const;
};

/* Owns the destination list of one profile. Pure data layer: it reads and writes profile files and
 * the profile ini, creates no outputs and reads no encoder state, so it is inert until the UI and the
 * output handler start using it. */
class StreamingDestinations {
public:
	/* Where the destination list itself lives in basic.ini. */
	static constexpr const char *list_section = "Streaming";
	static constexpr const char *list_key = "Destinations";
	static constexpr const char *retired_key = "Retired";

	/* Keys inside [Stream-<id>]. */
	static constexpr const char *name_key = "Name";
	static constexpr const char *enabled_key = "Enabled";
	static constexpr const char *inherits_key = "Inherits";
	/* Q14: a destination may reconnect differently from the rest of the profile. ReconnectOverride is
	 * the switch, so the three values below only mean anything when it is true; without it the
	 * destination follows the profile-wide [Output] values that the hidden Advanced dialog edits.
	 * Only the values differ per destination - the attempt counter, the backoff timer and the thread
	 * that waits on it already belong to one output each inside libobs. */
	static constexpr const char *reconnect_override_key = "ReconnectOverride";
	static constexpr const char *reconnect_key = "Reconnect";
	static constexpr const char *retry_delay_key = "RetryDelay";
	static constexpr const char *max_retries_key = "MaxRetries";
	/* The profile-wide streaming section those defaults live in. */
	static constexpr const char *profile_section = "Output";

	using list_type = std::vector<StreamingDestination>;

	void Load(config_t *profile_config, const std::filesystem::path &profile_dir);
	/* Write the model to disk: the ini keys, the JSON file of every listed destination, and the file
	 * removals recorded by destroy(). Structural edits (create/rename/enable/destroy) only touch memory,
	 * so the settings dialog can stage them behind Apply/OK and drop them on Cancel. */
	bool Save();

	const list_type &destinations() const { return destinations_; }
	StreamingDestination *find(std::string_view id);
	const StreamingDestination *find(std::string_view id) const;

	/* ids are ASCII, short and filesystem-safe: [a-z0-9_-], at most 32 characters. */
	static bool validId(std::string_view id);
	/* "twitch" -> "twitch", or "twitch_2" if taken. Also used to normalise a user-typed name. */
	std::string makeId(std::string_view base) const;
	/* Ids handed to the user before they are committed: taken ids (including retired ones) are refused,
	 * so a stale shared_from can never resolve to a newly created destination (Q22). */
	bool idTaken(std::string_view id) const;
	bool idRetired(std::string_view id) const;
	bool idUsable(std::string_view id) const;

	/* Creates an empty destination (no video tracks yet - a new destination is filled by fetching
	 * the service's own recommendation, not by copying another destination). An empty requested_id
	 * derives one from the display name. Nothing is written until Save() is called. */
	StreamingDestination *create(std::string_view base, const std::string &display_name, QString *error);
	StreamingDestination *createWithId(std::string_view requested_id, const std::string &display_name,
					   QString *error);
	bool setDisplayName(std::string_view id, const std::string &display_name);
	bool setEnabled(std::string_view id, bool enabled);
	/* Order of the list = order shown in the UI, and "first enabled destination" is what the legacy
	 * API and the websocket status report (Q42), so reordering is a real feature, not cosmetics. */
	bool reorder(const std::vector<std::string> &ids);
	/* Refuses while another destination references this one, so a shared encoder never ends up defined
	 * by a missing destination. The files are only deleted by the next Save(). */
	bool destroy(std::string_view id, QString *error);

	/* Q31: two destinations may use the same provider, but never the same stream key. */
	bool sameServiceAndKey(const char *service_type, obs_data_t *settings, std::string *conflicting_id) const;

	/* Files this profile needs for clone/export/import, beside the always-present ones. */
	std::vector<std::string> profileFileNames() const;

	/* Short human-readable summaries for the settings page: "<service type> / <provider or server>"
	 * (empty when nothing is configured yet) and the number of video tracks in the JSON file. */
	std::string serviceDescription(std::string_view id) const;
	size_t trackCount(std::string_view id) const;

	/* What one destination puts on the wire, in kbps, so the page can show it. Video comes from the
	 * destination's own video tracks; the destination imported from the single-stream layout has none of
	 * its own and gets the number from the shared advanced-mode encoder file, and a profile still in
	 * simple mode keeps it in [SimpleOutput] - both are reported with shared_video so the value is not
	 * mistaken for a per-destination setting. Audio is the profile's streaming audio, because
	 * per-destination audio tracks are not stored yet. Each destination uploads its own copy: sharing an
	 * encoder saves encoding time, not bandwidth. */
	struct BitrateBreakdown {
		long long videoKbps = 0;
		long long audioKbps = 0;
		bool shared_video = false;
	};
	BitrateBreakdown bitrateFor(std::string_view id) const;
	/* Sum over the enabled destinations - a disabled destination uploads nothing. */
	long long totalUploadKbps() const;
	/* The audio a destination's stream carries: the profile's selected mixes, plus the VOD track copy
	 * for a service that uses one. */
	long long streamingAudioKbps(std::string_view id) const;
	std::string serviceType(std::string_view id) const;
	/* The service object of a destination, built from its own service_<id>.json. Every destination needs
	 * one to start, and the settings page needs one before the engine exists. Null when the file is
	 * missing or its service type is not registered. */
	OBSService createService(std::string_view id) const;

	/* Resolved reconnect policy of one destination: the profile-wide [Output] values, replaced by the
	 * destination's own while ReconnectOverride is set. The defaults are registered by OBSBasic on every
	 * profile, so reading [Output] without a user value still gives what the output would use. */
	struct ReconnectPolicy {
		bool overridden = false;
		bool enabled = true;
		long long retryDelaySec = 2;
		long long maxRetries = 25;
	};
	ReconnectPolicy reconnectPolicy(std::string_view id) const;
	/* The profile-wide values an override replaces - what every destination without an override uses. */
	ReconnectPolicy profileReconnectPolicy() const;
	void setReconnectPolicy(std::string_view id, const ReconnectPolicy &policy);

	/* Non-video settings access, with the [Stream1] fallback for the imported destination. */
	const char *configString(std::string_view id, const char *key, const char *fallback = nullptr) const;
	bool configBool(std::string_view id, const char *key, bool fallback = false) const;
	long long configInt(std::string_view id, const char *key, long long fallback = 0) const;
	void setConfigString(std::string_view id, const char *key, const char *value);
	void setConfigBool(std::string_view id, const char *key, bool value);
	void setConfigInt(std::string_view id, const char *key, long long value);

	/* One-time import of the pre-multi-destination layout: copies service.json to
	 * service_<id>.json and marks the destination as inheriting [Stream1]. The old files are left
	 * in place and unread, matching how [SimpleOutput] is handled when simple mode is dropped. */
	bool importLegacyLayout();

private:
	std::vector<std::string> ids() const;
	/* Is this profile using advanced output? Decides where the audio bitrates live. */
	/* Does this destination's service take the extra VOD audio track? */
	bool usesVodTrack(std::string_view id) const;
	/* Does the tracks file of 'from' reference 'target' (shared_from / copied_from)? */
	bool references(std::string_view from, std::string_view target) const;
	void writeTracksFile(const StreamingDestination &dest) const;
	void removeDestinationFiles(const StreamingDestination &dest) const;
	std::filesystem::path pathOf(const std::string &file_name) const;
	/* Which service file describes one destination. A destination that still inherits the pre-multi-destination
	 * layout *is* the profile's single service, so the Stream settings page's service.json describes it - the
	 * service_<id>.json copy made at import only takes over once the destination is detached from [Stream1].
	 * Same fallback idea as configString()/configBool()/configInt() reading the [Stream1] section. */
	std::filesystem::path serviceFileOf(const StreamingDestination &dest) const;

	config_t *config_ = nullptr;
	std::filesystem::path dir_;
	list_type destinations_;
	std::vector<std::string> retired_;
	/* Destinations whose files destroy() has to delete when the change set is committed. */
	std::vector<std::string> pending_removals_;
};
