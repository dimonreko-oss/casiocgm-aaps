// AAPS link: sends the watch's steps and heart rate to the AAPS fork on the phone, and keeps the
// AAPS data it gets back (glucose, trend, delta, IOB, COB) for the face to show.
//
// It acts on no message from the phone. The one message it reads is the display message below,
// whose text it only copies for the face to draw, so nothing on the phone can make the watch do
// anything, and nothing on the watch can ask AAPS to dose.
//
// Message. The keys are fixed numbers, not package.json messageKeys, so adding this file to a face
// never renumbers the face's own keys.
//   AAPS_KEY_VERSION  uint8  AAPS_LINK_VERSION. A message without this key is the face's own.
//   AAPS_KEY_TIME     int32  when the snapshot was built, watch clock, UTC seconds. Also the
//                            snapshot id: a retry carries the same value.
//   AAPS_KEY_FLAGS    uint8  AAPS_FLAG_* bits below.
//   AAPS_KEY_MINUTES  bytes  minutes from the watch's own history, oldest first: the start of the
//                            first minute (uint32 UTC, little endian), then 2 bytes for each minute
//                            in a row: steps, bpm. Steps 0xFF: the watch has no record of that
//                            minute. Bpm 0: no heart-rate reading in it. Absent when there is no
//                            minute to send (see also AAPS_FLAG_TRUNCATED).
//
// Display message, from AAPS to the face (only to a face whose snapshot AAPS took). Text is ASCII
// only, at most 7 characters (the watch keeps 8 bytes with the end mark; longer text is cut): the
// system fonts have no arrows and no Cyrillic. A key that is absent or of another type
// reads as empty.
//   AAPS_KEY_DISPLAY_VERSION  uint8   AAPS_DISPLAY_VERSION; a message with another one is ignored.
//   AAPS_KEY_BG_TIME          int32   when the glucose was measured, UTC seconds; 0 when there is
//                                     none. The watch turns the glucose into a dash by its own clock
//                                     when it is older than DISPLAY_STALE_S.
//   AAPS_KEY_BG               cstring glucose in the user's units, e.g. "6.4" or "115"
//   AAPS_KEY_TREND            uint8   trend code: 0 unknown, 1 double up, 2 up, 3 forty-five up,
//                                     4 flat, 5 forty-five down, 6 down, 7 double down
//   AAPS_KEY_DELTA            cstring the AAPS delta (change per 5 minutes) in the user's units,
//                                     e.g. "+0.2"
//   AAPS_KEY_IOB              cstring insulin on board in units, e.g. "1.25"
//   AAPS_KEY_COB              cstring carbs on board, e.g. "45g"
//   AAPS_KEY_BG_MGDL          int32   the same glucose in whole mg/dL, 1 to 1000; 0 when there is
//                                     none. Optional: an older AAPS does not send it, and the face
//                                     then gets 0.
//
// What the AAPS side must do:
//   - Take the link from any watchapp UUID and know it by AAPS_KEY_VERSION. Every face that
//     carries the link has its own UUID.
//   - Answer at once and store the data afterwards. Until the phone answers, the face cannot send
//     its own messages.
//   - Refuse (NACK) every message, the face's own too, that does not come through the phone app
//     AAPS trusts (the Core Devices app). Answer each message that does by these rules, in this
//     order:
//     1. No AAPS_KEY_VERSION: the face's own message, meant for its JavaScript. Acknowledge (ACK)
//        and ignore it.
//     2. Refuse (NACK) a snapshot that does not fit, for any one of these: another version; Time or
//        Flags missing; Minutes not 4 + 2k bytes (k <= HISTORY_MIN); Minutes that do not start on a
//        whole minute, start more than HISTORY_MIN minutes before the minute of Time, or end after
//        Time; Time older than 3 minutes or more than 1 minute ahead. AAPS may also refuse for
//        reasons of its own, for example a snapshot from another watch within an hour of the last
//        one it took from the watch in use (see `PebbleIntake.onMessage` in the AAPS fork).
//     3. Acknowledge a snapshot whose Time was already taken from this watch: a retry after a lost
//        acknowledgement.
//     4. Refuse every other snapshot while the phone takes no watch data at all (in AAPS: the
//        Pebble plugin is off), so the face shows no link instead of a false "ok".
//     5. Otherwise take it and acknowledge it.
//     A refused snapshot was not taken, so its retry is checked again like a new one. Retries stop
//     at MAX_SEND_AGE_S, so a retry is never older than 3 minutes when it leaves the watch.
//   - Every snapshot carries the last HISTORY_MIN minutes again, so a lost snapshot loses nothing
//     unless AAPS_FLAG_TRUNCATED is set. Keep one value per minute and let a newer snapshot's
//     value win, except that a minute with no record (steps 0xFF) never replaces a value already
//     held: after a reset the watch can lose its newest minutes that were not saved yet.
//   - A bpm says only that the sensor had a reading in that minute. At rest the watch measures in
//     short windows every few minutes; it measures every second only during a walk or run it
//     detected itself, or in its Workout app (which replaces the face, so nothing is sent then).
//     How much a minute's heart rate counts is decided in AAPS.
//   - On a face with JavaScript, the phone's JavaScript runtime acknowledges every message as
//     well, usually first. There a NACK from AAPS rarely reaches the watch.

#include "aaps_link.h"

#define AAPS_LINK_VERSION 1

#define AAPS_KEY_VERSION 0x41415001
#define AAPS_KEY_TIME 0x41415002
#define AAPS_KEY_FLAGS 0x41415003
#define AAPS_KEY_MINUTES 0x41415004

#define AAPS_FLAG_ON_CHARGER (1 << 0)
// Heart-rate monitoring is switched off in the Health settings of the phone app (the watch reports
// no permission). "HR Monitoring: Disabled" in the watch menu is not reported: it shows only as
// bpm 0.
#define AAPS_FLAG_HR_OFF (1 << 1)
// The face's AppMessage outbox is too small for all minutes: Minutes has only the newest that fit,
// or is absent.
#define AAPS_FLAG_TRUNCATED (1 << 2)

#define AAPS_DISPLAY_VERSION 1
#define AAPS_KEY_DISPLAY_VERSION 0x41415010
#define AAPS_KEY_BG_TIME 0x41415011
#define AAPS_KEY_BG 0x41415012
#define AAPS_KEY_TREND 0x41415013
#define AAPS_KEY_DELTA 0x41415014
#define AAPS_KEY_IOB 0x41415015
#define AAPS_KEY_COB 0x41415016
#define AAPS_KEY_BG_MGDL 0x41415017
// Glucose older than this shows as a dash; IOB and COB too when no display message came for as long.
#define DISPLAY_STALE_S (15 * 60)
// Holds a display message (about 100 bytes); APP_MESSAGE_INBOX_SIZE_MINIMUM is always available.
#define INBOX_BYTES APP_MESSAGE_INBOX_SIZE_MINIMUM

#define HISTORY_MIN 60
#define MINUTES_BYTES (4 + 2 * HISTORY_MIN)
#define BUILD_EVERY_MIN 5
// The history is asked for in second :00. When PebbleOS reads it later in a minute, it fills in its
// own record of the running minute early: it clears the movement summed so far and takes a pending
// SpO2 reading, which changes the watch's own Health data. This is best effort: PebbleOS reads on
// its background task, which can be busy, so the read can still land after :00 (logged as a
// warning).
// The timer wakes the link in second :59 by whole seconds; from :55 it checks the clock every
// POLL_MS until the minute changes. The first time it is in second :00 of a new minute, however it
// got there, it takes one more step when that still fits in the second, so PebbleOS has closed the
// minute that just ended before the read. No timer aims at a point inside a second: on the emulator
// such a target drifted against the clock. There the milliseconds are a free-running counter, not
// the phase of the second, so the :00 step can only be checked on a real watch.
#define POLL_MS 250
#define EARLY_FROM_SECOND 55
// After this many minutes in a row when the app was still busy in second :00, the next minute reads
// the history late anyway: the data reaching AAPS matters more than a few seconds of the watch's
// movement sum.
#define MAX_BUILD_MISSES 2
// A snapshot is sent a few seconds later, away from the face's own messages, which often go out
// on its minute tick.
#define SEND_AFTER_READ_MS 5000
// AAPS drops a snapshot older than 3 minutes, so an unacknowledged one is retried each minute only
// while it is younger than this. After that the next build carries the same minutes.
#define MAX_SEND_AGE_S 150
// The firmware normally answers a send within 10 s; a stalled link can take longer. Longer than
// this means the callback went elsewhere or was lost.
#define NO_ANSWER_S 30

static bool s_started;
static bool s_build_due;
// Set once the timer has taken its one extra step into a new minute.
static bool s_stepped;
static uint8_t s_build_misses;
// The minute (UTC seconds / 60) already handled, so a timer that fires early does not handle it twice.
static time_t s_handled_minute;

// The last display message: copies, because a tuple lives only inside the inbox callback.
static int32_t s_bg_time;
static int32_t s_bg_mgdl;
static uint8_t s_trend;
static char s_bg[8];
static char s_delta[8];
static char s_iob[8];
static char s_cob[8];
static time_t s_display_at;
// The highest watch time seen since the stored values arrived: raised on every look (every minute,
// and whenever the face draws), and set to the message time, also lower, when a display message
// arrives, so a message that comes after a step back is kept.
static time_t s_clock_seen;
static AapsLinkDisplayHandler s_display_handler;

static uint8_t s_minutes[MINUTES_BYTES];
static uint16_t s_minute_count;
static time_t s_first_minute;
static int32_t s_snap_time;
static uint8_t s_snap_flags;
static bool s_pending;

// The snapshot on its way. A new snapshot can be built while the previous one still waits for its
// acknowledgement.
static bool s_in_flight;
static int32_t s_flight_time;
static time_t s_flight_started;
static time_t s_last_ack;

// The face's own AppMessage callbacks, if it has any. A message that is not ours goes to them.
static AppMessageOutboxSent s_face_sent;
static AppMessageOutboxFailed s_face_failed;
static AppMessageInboxReceived s_face_received;
// Set while a message is handed to the face. A face handler that hands it back to the link (as
// its previous callback) then stops here instead of calling round in a loop.
static bool s_forwarding;
static bool s_forwarding_inbox;

static void put_u32_le(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

static void build_snapshot(time_t now) {
  time_t open_minute = now / 60 * 60;
  time_t from = open_minute - HISTORY_MIN * 60;
  time_t to = open_minute;
  static HealthMinuteData data[HISTORY_MIN];
  uint32_t n = health_service_get_minute_history(data, HISTORY_MIN, &from, &to);
  int read_end_second = (int)(time(NULL) % 60);
  if (read_end_second != 0) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "history read ended in second :%02d", read_end_second);
  }
  // The history starts at its first record, which can be later than asked. Only closed minutes go.
  uint32_t closed = (n > 0 && from < open_minute) ? (uint32_t)((open_minute - from) / 60) : 0;
  if (n > closed) {
    n = closed;
  }

  s_minute_count = n;
  s_first_minute = from;
  for (uint32_t k = 0; k < n; k++) {
    uint8_t steps = data[k].steps;
    uint8_t bpm = data[k].heart_rate_bpm;
    if (data[k].is_invalid) {
      // A gap in the history reads 0xFF in every field.
      steps = 0xFF;
      bpm = 0;
    } else {
      if (steps == 0xFF) {
        steps = 0xFE;  // 0xFF means "no record"
      }
      if (bpm == 0xFF) {
        bpm = 0;
      }
    }
    s_minutes[4 + 2 * k] = steps;
    s_minutes[5 + 2 * k] = bpm;
    if (steps != 0xFF && (steps > 0 || bpm > 0)) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "  minute=%d steps=%d bpm=%d", (int)(from + (time_t)k * 60), (int)steps, (int)bpm);
    }
  }

  uint8_t flags = 0;
  if (battery_state_service_peek().is_plugged) {
    flags |= AAPS_FLAG_ON_CHARGER;
  }
  if (health_service_metric_accessible(HealthMetricHeartRateBPM, now - 3600, now) &
      HealthServiceAccessibilityMaskNoPermission) {
    flags |= AAPS_FLAG_HR_OFF;
  }

  s_snap_time = (int32_t)now;
  s_snap_flags = flags;
  s_pending = true;
  APP_LOG(APP_LOG_LEVEL_INFO, "snapshot t=%d minutes=%d flags=%d", (int)s_snap_time, (int)n, (int)flags);
}

static void send_pending(time_t now) {
  if (!s_pending || s_in_flight || now - s_snap_time > MAX_SEND_AGE_S) {
    return;
  }
  DictionaryIterator *it;
  AppMessageResult begin = app_message_outbox_begin(&it);
  if (begin != APP_MSG_OK) {
    // Busy is normal: the face's own message is on its way.
    if (begin != APP_MSG_BUSY) {
      APP_LOG(APP_LOG_LEVEL_WARNING, "outbox not usable: %d", (int)begin);
    }
    return;
  }
  // A face that opened AppMessage itself may have a smaller outbox; then only the newest minutes
  // that fit are sent. Whatever happens, the message is sent: an outbox begun and never sent would
  // block the face's own messages too.
  uint32_t room = (uint32_t)((const uint8_t *)it->end - (const uint8_t *)it->cursor);
  if (room < dict_calc_buffer_size(3, 1, 4, 1) - 1) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "outbox of %d bytes cannot carry the snapshot", (int)room + 1);
  }
  uint32_t fixed = dict_calc_buffer_size(4, 1, 4, 1, 4) - 1;
  uint32_t count = s_minute_count;
  uint32_t fits = room > fixed ? (room - fixed) / 2 : 0;
  uint8_t flags = s_snap_flags;
  if (count > fits) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "outbox fits %d of %d minutes", (int)fits, (int)count);
    count = fits;
    flags |= AAPS_FLAG_TRUNCATED;
  }
  dict_write_uint8(it, AAPS_KEY_VERSION, AAPS_LINK_VERSION);
  dict_write_int32(it, AAPS_KEY_TIME, s_snap_time);
  dict_write_uint8(it, AAPS_KEY_FLAGS, flags);
  if (count > 0) {
    static uint8_t out[MINUTES_BYTES];
    uint32_t skip = s_minute_count - count;
    put_u32_le(out, (uint32_t)(s_first_minute + (time_t)skip * 60));
    memcpy(out + 4, s_minutes + 4 + 2 * skip, 2 * count);
    dict_write_data(it, AAPS_KEY_MINUTES, out, 4 + 2 * count);
  }
  if (app_message_outbox_send() == APP_MSG_OK) {
    s_in_flight = true;
    s_flight_time = s_snap_time;
    s_flight_started = now;
    APP_LOG(APP_LOG_LEVEL_DEBUG, "send t=%d", (int)s_flight_time);
  }
}

static void outbox_sent(DictionaryIterator *it, void *context) {
  if (!s_in_flight) {
    if (s_face_sent && !s_forwarding) {
      s_forwarding = true;
      s_face_sent(it, context);
      s_forwarding = false;
    }
    return;
  }
  s_in_flight = false;
  s_last_ack = time(NULL);
  if (s_flight_time == s_snap_time) {
    s_pending = false;
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "acknowledged t=%d", (int)s_flight_time);
  // A snapshot built while this one was on its way goes now.
  send_pending(s_last_ack);
}

static void outbox_failed(DictionaryIterator *it, AppMessageResult reason, void *context) {
  if (!s_in_flight) {
    if (s_face_failed && !s_forwarding) {
      s_forwarding = true;
      s_face_failed(it, reason, context);
      s_forwarding = false;
    }
    return;
  }
  s_in_flight = false;
  APP_LOG(APP_LOG_LEVEL_WARNING, "snapshot t=%d not acknowledged: %d", (int)s_flight_time, (int)reason);
}

// Copies a text tuple; anything else, or an absent key, gives an empty string.
static void copy_text(DictionaryIterator *it, uint32_t key, char *buf, size_t size) {
  const Tuple *t = dict_find(it, key);
  if (t && t->type == TUPLE_CSTRING) {
    snprintf(buf, size, "%.*s", (int)t->length, t->value->cstring);
  } else {
    buf[0] = '\0';
  }
}

// Only the display message is read here, and only copied: the watch acknowledges only after this
// returns, so it stays short.
static void inbox_received(DictionaryIterator *it, void *context) {
  const Tuple *version = dict_find(it, AAPS_KEY_DISPLAY_VERSION);
  if (!version) {
    if (s_face_received && !s_forwarding_inbox) {
      s_forwarding_inbox = true;
      s_face_received(it, context);
      s_forwarding_inbox = false;
    }
    return;
  }
  if (version->type != TUPLE_UINT || version->length != 1 || version->value->uint8 != AAPS_DISPLAY_VERSION) {
    return;
  }
  const Tuple *bg_time = dict_find(it, AAPS_KEY_BG_TIME);
  s_bg_time = (bg_time && bg_time->type == TUPLE_INT && bg_time->length == 4) ? bg_time->value->int32 : 0;
  const Tuple *mgdl = dict_find(it, AAPS_KEY_BG_MGDL);
  s_bg_mgdl = (mgdl && mgdl->type == TUPLE_INT && mgdl->length == 4 &&
               mgdl->value->int32 >= 1 && mgdl->value->int32 <= 1000) ? mgdl->value->int32 : 0;
  const Tuple *trend = dict_find(it, AAPS_KEY_TREND);
  s_trend = (trend && trend->type == TUPLE_UINT && trend->length == 1) ? trend->value->uint8 : 0;
  copy_text(it, AAPS_KEY_BG, s_bg, sizeof(s_bg));
  copy_text(it, AAPS_KEY_DELTA, s_delta, sizeof(s_delta));
  copy_text(it, AAPS_KEY_IOB, s_iob, sizeof(s_iob));
  copy_text(it, AAPS_KEY_COB, s_cob, sizeof(s_cob));
  s_display_at = time(NULL);
  // New values on the clock as it is now: a step back before this message does not drop them.
  s_clock_seen = s_display_at;
  if (s_display_handler) {
    s_display_handler();
  }
}

// Put the link's callbacks in front of the face's. Done again every minute, so a face that
// registers its own callbacks later is still chained.
static void hook_callbacks(void) {
  AppMessageOutboxSent sent = app_message_register_outbox_sent(outbox_sent);
  if (sent != outbox_sent) {
    s_face_sent = sent;
  }
  AppMessageOutboxFailed failed = app_message_register_outbox_failed(outbox_failed);
  if (failed != outbox_failed) {
    s_face_failed = failed;
  }
  AppMessageInboxReceived received = app_message_register_inbox_received(inbox_received);
  if (received != inbox_received) {
    s_face_received = received;
  }
}

static void send_cb(void *data) {
  // The face may have registered its own callbacks since the last minute.
  hook_callbacks();
  send_pending(time(NULL));
}

// The watch clock went back to more than a minute below that mark (the display time is of this
// clock): the stored values have no true age left, so they are dropped for good, not shown again
// once the clock catches up. The glucose time is of the phone clock and cannot tell this. The time
// is looked at at least once a minute, so a step back of up to about two minutes can be missed;
// then the values can stay, or show again, up to that much longer until the next display message.
static void check_clock(time_t now) {
  if (now < s_clock_seen - 60) {
    s_display_at = 0;
    s_bg_time = 0;
    s_clock_seen = now;
  } else if (now > s_clock_seen) {
    s_clock_seen = now;
  }
}

static void minute_cb(void *data);

// Wake in second :59 of this minute; minute_cb steps into the next minute from there.
static void schedule_next(time_t now) {
  app_timer_register((uint32_t)(59 - now % 60) * 1000, minute_cb, NULL);
}

static void minute_cb(void *data) {
  // One clock read for the second and its milliseconds, so the two cannot straddle a rollover.
  time_t now;
  uint16_t ms;
  time_ms(&now, &ms);
  check_clock(now);
  int second = (int)(now % 60);
  if (second >= EARLY_FROM_SECOND) {
    // A new :00 is still ahead, so the next arrival in it steps again.
    s_stepped = false;
    app_timer_register(POLL_MS, minute_cb, NULL);
    return;
  }
  if (second == 0 && !s_stepped && now / 60 != s_handled_minute) {
    // The minute has just changed, however the timer got here: one more step before the read, if it
    // still fits in this second. The milliseconds only decide that; no timer aims inside a second.
    if (ms < 1000 - 2 * POLL_MS) {
      s_stepped = true;
      app_timer_register(POLL_MS, minute_cb, NULL);
      return;
    }
  }
  s_stepped = false;
  if (now / 60 == s_handled_minute) {
    schedule_next(now);
    return;
  }
  // A build is due when a fifth minute was crossed since the last minute handled, even one that the
  // timer stepped over (a clock set forward, or the app blocked for a while).
  if (now / 60 / BUILD_EVERY_MIN != s_handled_minute / BUILD_EVERY_MIN) {
    s_build_due = true;
  }
  s_handled_minute = now / 60;

  hook_callbacks();
  if (s_in_flight && now - s_flight_started > NO_ANSWER_S) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "snapshot t=%d: no answer, the callback was lost", (int)s_flight_time);
    s_in_flight = false;
  }
  if (s_build_due) {
    if (second == 0 || s_build_misses >= MAX_BUILD_MISSES) {
      if (second != 0) {
        APP_LOG(APP_LOG_LEVEL_WARNING, "app busy in second :00 for %d minutes, reading the history late", (int)s_build_misses + 1);
      }
      build_snapshot(now);
      s_build_due = false;
      s_build_misses = 0;
    } else {
      s_build_misses++;
      APP_LOG(APP_LOG_LEVEL_DEBUG, "app busy in second :00, the build waits for the next minute");
    }
  }
  if (s_pending) {
    app_timer_register(SEND_AFTER_READ_MS, send_cb, NULL);
  }
  // The history read can block for a while, so the next wake-up is taken from the clock as it is now.
  time_t after = time(NULL);
  if (after / 60 != s_handled_minute) {
    app_timer_register(0, minute_cb, NULL);  // the read ran into the next minute: handle it at once
  } else {
    schedule_next(after);
  }
}

static void start_cb(void *data) {
  hook_callbacks();
  // A face that uses AppMessage has opened it already, and its sizes stay. Otherwise the inbox is
  // opened with room for one display message (INBOX_BYTES).
  AppMessageResult open = app_message_open(INBOX_BYTES, dict_calc_buffer_size(4, 1, 4, 1, MINUTES_BYTES));
  if (open != APP_MSG_OK && open != APP_MSG_INVALID_STATE) {
    APP_LOG(APP_LOG_LEVEL_WARNING, "AppMessage could not be opened: %d", (int)open);
  }
  // The first snapshot is built at the next minute, in second :00.
  time_t now = time(NULL);
  s_handled_minute = now / 60;
  s_build_due = true;
  schedule_next(now);
}

void aaps_link_init(void) {
  if (s_started) {
    return;
  }
  s_started = true;
  // Start after the face's init has finished, so the face's own AppMessage setup comes first
  // wherever in init this call sits.
  app_timer_register(0, start_cb, NULL);
}

time_t aaps_link_last_ack(void) {
  return s_last_ack;
}

void aaps_link_on_display(AapsLinkDisplayHandler handler) {
  s_display_handler = handler;
}

static const char *trend_text(uint8_t code) {
  switch (code) {
    case 1: return "^^";
    case 2: return "^";
    case 3: return "/";
    case 4: return "->";
    case 5: return "\\";
    case 6: return "v";
    case 7: return "vv";
    default: return "";
  }
}

void aaps_link_display(AapsLinkDisplay *out) {
  time_t now = time(NULL);
  check_clock(now);
  // A glucose up to a minute "ahead" is clock skew between phone and watch, not a future value.
  int32_t age_s = s_bg_time > 0 ? (int32_t)(now - s_bg_time) : -1;
  bool bg_fresh = s_bg_time > 0 && age_s >= -60 && age_s <= DISPLAY_STALE_S && s_bg[0] != '\0';
  bool display_fresh = s_display_at > 0 && now - s_display_at <= DISPLAY_STALE_S;
  out->age_min = bg_fresh ? (age_s < 0 ? 0 : age_s / 60) : -1;
  out->bg_time = bg_fresh ? (time_t)s_bg_time : 0;
  out->bg_mgdl = bg_fresh ? (int)s_bg_mgdl : 0;
  snprintf(out->bg, sizeof(out->bg), "%s", bg_fresh ? s_bg : "---");
  snprintf(out->trend, sizeof(out->trend), "%s", bg_fresh ? trend_text(s_trend) : "");
  snprintf(out->delta, sizeof(out->delta), "%s", bg_fresh ? s_delta : "");
  snprintf(out->iob, sizeof(out->iob), "%s", display_fresh && s_iob[0] ? s_iob : "--");
  snprintf(out->cob, sizeof(out->cob), "%s", display_fresh && s_cob[0] ? s_cob : "--");
}
