#include "recorder.h"
#include "bc-server.h"
#include "media_writer.h"

recorder::recorder(const bc_record *bc_rec)
	: stream_consumer("Recorder"), device_id(bc_rec->id), destroy_flag(false),
	  recording_type(BC_EVENT_CAM_T_CONTINUOUS), writer(0), current_event(0), event_snapshot_done(false)
{
}

recorder::~recorder()
{
	delete writer;
}

void recorder::set_recording_type(bc_event_cam_type_t type)
{
	recording_type = type;
}

void recorder::destroy()
{
	destroy_flag = true;
	buffer_wait.notify_all();
}

void recorder::run()
{
	std::shared_ptr<const stream_properties> saved_properties;

	std::unique_lock<std::mutex> l(lock);
	while (!destroy_flag)
	{
		if (buffer.empty()) {
			buffer_wait.wait(l);
			continue;
		}

		stream_packet packet = buffer.front();
		buffer.pop_front();
		l.unlock();

		if (!packet.data()) {
			/* Null packets force recording end */
			recording_end();
			l.lock();
			continue;
		}

		if (packet.properties() != saved_properties) {
			bc_log("recorder: Stream properties changed");
			saved_properties = packet.properties();
			recording_end();
		}

		bool schedule_recording = true;
		if (schedule_recording) {
			/* If on the motion schedule, and there is no motion, skip recording */
		//	if (!check_motion(bc_rec, packet))
		//		continue;

			if (current_event && packet.is_key_frame() && packet.is_video_frame() &&
					bc_event_media_length(current_event) > BC_MAX_RECORD_TIME) {
				recording_end();
			}

			/* Setup and write to recordings */
			if (!writer) {
#if 0
				if (bc_rec->sched_cur == 'M' && !bc_rec->prerecord_buffer.empty()) {
					/* Dump the prerecording buffer */
					if (recording_start(bc_rec, bc_rec->prerecord_buffer.front().ts_clock))
						goto error;
					bc_rec->output_pts_base = bc_rec->prerecord_buffer.front().pts;
					/* Skip the last packet; it's identical to the current packet,
					 * which we write in the normal path below. */
					for (auto it = bc_rec->prerecord_buffer.begin();
					     it != bc_rec->prerecord_buffer.end()-1; it++) {
						bc_output_packet_write(bc_rec, *it);
					}
				} else
#endif
				if (!packet.is_key_frame() || !packet.is_video_frame()) {
					/* Recordings must always start with a video keyframe; this should
					 * be ensured in most cases (continuous splits, initialization), but
					 * if absolutely necessary we'll drop packets. */
					goto end;
				} else {
					if (recording_start(0, packet)) {
						// XXX
						bc_log("W: recording_start failed! Bad!");
						goto end;
					}
				}
			}

			if (packet.is_video_frame() && packet.is_key_frame() && !event_snapshot_done) {
				//save_event_snapshot(bc_rec, packet);
				bc_log("XXX: Would do event snapshot here");
				event_snapshot_done = true;
				//event_trigger_notifications(bc_rec);
			}

			writer->write_packet(packet);
		}

end:
		l.lock();
	}

	bc_log("recorder destroying");
	l.unlock();
	delete this;
}

int recorder::recording_start(time_t start_ts, const stream_packet &first_packet)
{
	bc_event_cam_t nevent = NULL;

	if (!start_ts)
		start_ts = time(NULL);

	recording_end();
	std::string outfile = media_file_path(start_ts);
	if (outfile.empty()) {
		do_error_event(BC_EVENT_L_ALRM, BC_EVENT_CAM_T_NOT_FOUND);
		return -1;
	}

	writer = new media_writer;
	if (writer->open(outfile, *first_packet.properties().get()) != 0) {
		do_error_event(BC_EVENT_L_ALRM, BC_EVENT_CAM_T_NOT_FOUND);
		return -1;
	}

	bc_event_level_t level = (recording_type == BC_EVENT_CAM_T_MOTION) ? BC_EVENT_L_WARN : BC_EVENT_L_INFO;
	nevent = bc_event_cam_start(device_id, start_ts, level, recording_type, outfile.c_str());

	if (!nevent) {
		do_error_event(BC_EVENT_L_ALRM, BC_EVENT_CAM_T_NOT_FOUND);
		return -1;
	}

	bc_event_cam_end(&current_event);
	current_event = nevent;
	event_snapshot_done = false;

	return 0;
}

std::string recorder::media_file_path(time_t start_ts)
{
	struct tm tm;
	char date[12], mytime[10], dir[PATH_MAX];
	char stor[PATH_MAX];

	if (bc_get_media_loc(stor, sizeof(stor)) < 0)
		return std::string();

	/* XXX Need some way to reconcile time between media event and
	 * filename. They should match. */
	localtime_r(&start_ts, &tm);

	strftime(date, sizeof(date), "%Y/%m/%d", &tm);
	strftime(mytime, sizeof(mytime), "%H-%M-%S", &tm);
	if (snprintf(dir, sizeof(dir), "%s/%s/%06d", stor, date, device_id) >= (int)sizeof(dir))
		return std::string();
	if (bc_mkdir_recursive(dir) < 0) {
		bc_log("E: Cannot create media directory %s: %m", dir);
		return std::string();
	}

	std::string path = dir;
	path += "/" + std::string(mytime) + ".mkv";
	return path;
}

void recorder::recording_end()
{
	/* Close the media entry in the db */
	if (current_event && bc_event_has_media(current_event))
		bc_event_cam_end(&current_event);

	if (writer)
		writer->close();
	delete writer;
	writer = 0;
}

void recorder::do_error_event(bc_event_level_t level, bc_event_cam_type_t type)
{
	if (!current_event || current_event->level != level || current_event->type != type) {
		recording_end();
		bc_event_cam_end(&current_event);

		current_event = bc_event_cam_start(device_id, time(NULL), level, type, NULL);
	}
}

