/* Exercise the real loaders and parser, replacing only HTTP and PS2 sleep. */
#include "jf.h"
#include "net.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char reply[HTTP_RAW_MAX];
static int total, requests, expected_start, omit_total, music;
static int raw_len, failures;
static const char *total_override;
enum {
	NORMAL, BAD_JSON, PART_JSON, TOKEN_LIMIT, RAW_FULL, TRANSPORT, TRUNCATED,
	RECOVER_JSON, RECOVER_TRANSPORT, NO_ITEMS, INVALID_ITEMS, REPEAT_PAGE,
	OVERLAP, WRONG_OFFSET, EXTRA_RECORD, EARLY_END, CHANGING_TOTAL,
	ALL_INVALID, MISSING_FIELDS, HTTP_ERROR
};
static int fault;

void net_say(const char *s) { (void)s; }
void net_snooze(int ms) { (void)ms; }
const char *net_last_info(void) { return "mock HTTP 200"; }
int net_last_raw_len(void) { return raw_len; }

int http_get(const char *host, int port, const char *path, const char *token,
             int *status, char **body)
{
	(void)host; (void)port; (void)token;
	*status = 200;
	*body = reply;
	raw_len = 100;
	if (strstr(path, "Fields=Overview")) {
		assert(strstr(path, "Ids=season"));
		strcpy(reply, "{\"Items\":[{\"Overview\":\"Season overview\"}]}");
		return 0;
	}
	const char *lp = strstr(path, "&Limit=");
	const char *sp = strstr(path, "&StartIndex=");
	assert(lp && sp); /* Regression: the old episode loader is unbounded. */
	int limit = atoi(lp + 7), start = atoi(sp + 12);
	assert(limit > 0 && limit <= 6);
	assert(start == expected_start);
	assert(++requests <= 260);
	if (music) {
		assert(strstr(path, "ParentId=album"));
	} else {
		assert(strstr(path, "/Shows/series/Episodes?userId=user"));
		assert(strstr(path, "&seasonId=season"));
		assert(strstr(path, "&Fields=RunTimeTicks"));
	}
	if (start >= 6) {
		if (fault == RAW_FULL) {
			raw_len = HTTP_RAW_MAX - 1;
			strcpy(reply, "{\"Items\":[]}");
			return 0; /* Even a parsable prefix at the buffer boundary is suspect. */
		}
		if (fault == TRANSPORT || fault == TRUNCATED ||
		    (fault == RECOVER_TRANSPORT && failures++ == 0)) {
			strcpy(reply, "{\"Items\":[");
			if (fault == TRANSPORT) *status = 0;
			return fault == TRANSPORT ? -1 : -6;
		}
		if (fault == BAD_JSON || (fault == RECOVER_JSON && failures++ == 0)) {
			strcpy(reply, "{\"Items\":[\"\\q\"]}");
			return 0;
		}
		if (fault == PART_JSON) {
			strcpy(reply, "{\"Items\":[");
			return 0;
		}
		if (fault == TOKEN_LIMIT) {
			strcpy(reply, "{\"Items\":[],\"Extra\":[");
			for (int i = 0; i < JSON_TOK_MAX; i++) strcat(reply, "0,");
			strcat(reply, "0]}");
			return 0;
		}
		if (fault == NO_ITEMS || fault == INVALID_ITEMS || fault == HTTP_ERROR) {
			strcpy(reply, fault == INVALID_ITEMS ? "{\"Items\":{}}" : "{}");
			if (fault == HTTP_ERROR) *status = 500;
			return 0;
		}
	}
	int count = total - start;
	if (count > limit) count = limit;
	if (count < 0) count = 0;
	if (start >= 6 && fault == EXTRA_RECORD) count = limit + 1;
	if (start >= 6 && fault == EARLY_END) count = 0;
	int used = snprintf(reply, sizeof(reply), "{\"Items\":[");
	for (int i = 0; i < count; i++) {
		int source = start + i;
		if (start >= 6 && fault == REPEAT_PAGE) source = i;
		if (start >= 6 && fault == OVERLAP) source--;
		const char *sep = i ? "," : "";
		if (fault == ALL_INVALID || (fault == MISSING_FIELDS && source >= 1 && source <= 5)) {
			const char *missing[] = {"{}", "{\"Name\":\"No ID\"}",
			    "{\"Id\":\"no-name\"}", "{\"Id\":false,\"Name\":null}",
			    "{\"Id\":\"\",\"Name\":\"Empty ID\"}",
			    "{\"Id\":\"0123456789012345678901234567890123456789\",\"Name\":\"Long ID\"}"};
			used += snprintf(reply + used, sizeof(reply) - used, "%s%s", sep,
			                 missing[fault == ALL_INVALID ? 0 : source]);
			continue;
		}
		used += snprintf(reply + used, sizeof(reply) - used,
		    "%s{\"Id\":\"id-%d\",\"Name\":\"Episode %d\","
		    "\"RunTimeTicks\":%s,\"Container\":\"mp3\","
		    "\"AlbumArtist\":\"Artist\"}",
		    sep, source, source, fault == MISSING_FIELDS ?
		    (source == 6 ? "null" : "99999999999999999999999") : "18000000000");
	}
	if (omit_total)
		snprintf(reply + used, sizeof(reply) - used, "]}");
	else if (total_override)
		snprintf(reply + used, sizeof(reply) - used,
		         "],\"TotalRecordCount\":%s}", total_override);
	else
		snprintf(reply + used, sizeof(reply) - used,
		         "],\"TotalRecordCount\":%d,\"StartIndex\":%d}",
		         total + (start >= 6 && fault == CHANGING_TOTAL),
		         start + (start >= 6 && fault == WRONG_OFFSET));
	if (fault != NORMAL && fault != MISSING_FIELDS && fault != RECOVER_JSON &&
	    fault != RECOVER_TRANSPORT && start >= 6) return 0;
	expected_start += count;
	return 0;
}

static void run(int count, int without_total, int is_music)
{
	total = count; omit_total = without_total; music = is_music;
	requests = expected_start = failures = 0;
	strcpy(jf_user_id, "user");
	int rc = music ? jf_load_tracks("album", "Album", 2000) :
	                 jf_load_episodes("series", "season", "Season");
	assert(rc == 0);
	int loaded = count < 256 ? count : 256;
	assert(jf_track_count == loaded);
	int pages = (loaded + 5) / 6;
	if (!count || (without_total && count < 256 && count % 6 == 0)) pages++;
	assert(requests == pages + (fault == RECOVER_JSON || fault == RECOVER_TRANSPORT));
	for (int i = 0; i < loaded; i++) {
		char id[40], name[112];
		snprintf(id, sizeof(id), "id-%d", i);
		snprintf(name, sizeof(name), "Episode %d", i);
		assert(strcmp(jf_tracks[i].id, id) == 0);
		assert(strcmp(jf_tracks[i].name, name) == 0);
		assert(jf_tracks[i].seconds == 1800);
		assert(strcmp(jf_tracks[i].container, music ? "mp3" : "") == 0);
	}
	if (!music) assert(strcmp(jf_album_desc, "Season overview") == 0);
	else assert(strcmp(jf_album_artist, "Artist") == 0);
}

static void expect_failure(int problem, const char *message)
{
	fault = problem;
	total = 13; omit_total = music = 0;
	requests = expected_start = failures = 0;
	assert(jf_load_episodes("series", "season", "Season") < 0);
	assert(jf_track_count == 0);
	if (!strstr(jf_err_body(), message)) {
		fprintf(stderr, "fault %d: expected '%s', got '%s'\n", problem, message, jf_err_body());
		abort();
	}
	assert(requests <= 1 + JSON_ATTEMPTS * 2);
}

int main(void)
{
	const int counts[] = {0, 1, 6, 7, 32, 33, 100, 256, 300};
	for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
		run(counts[i], 0, 0);
		run(counts[i], 1, 0);
	}
	run(33, 0, 1);
	run(300, 0, 1);
	fault = RECOVER_JSON; run(13, 0, 0);
	fault = RECOVER_TRANSPORT; run(13, 0, 0);
	expect_failure(BAD_JSON, "Malformed or incomplete JSON");
	expect_failure(PART_JSON, "Malformed or incomplete JSON");
	expect_failure(TOKEN_LIMIT, "JSON token limit");
	expect_failure(RAW_FULL, "HTTP response buffer");
	expect_failure(TRANSPORT, "HTTP transport failed (-1)");
	expect_failure(TRUNCATED, "HTTP transport failed (-6)");
	expect_failure(HTTP_ERROR, "HTTP request failed");
	expect_failure(NO_ITEMS, "Items array");
	expect_failure(INVALID_ITEMS, "Items array");
	expect_failure(REPEAT_PAGE, "Repeated episode ID");
	expect_failure(OVERLAP, "Repeated episode ID");
	expect_failure(WRONG_OFFSET, "Invalid count, total or StartIndex");
	expect_failure(EXTRA_RECORD, "Invalid count, total or StartIndex");
	expect_failure(EARLY_END, "ended before TotalRecordCount");
	expect_failure(CHANGING_TOTAL, "Invalid count, total or StartIndex");
	expect_failure(ALL_INVALID, "No usable episode IDs");
	const char *bad_totals[] = {"-1", "null", "\"13\"", "1.5", "2147483648", "999999999999999999999"};
	for (unsigned i = 0; i < sizeof(bad_totals) / sizeof(bad_totals[0]); i++) {
		total_override = bad_totals[i];
		expect_failure(NORMAL, "Invalid count, total or StartIndex");
	}
	total_override = NULL;
	fault = MISSING_FIELDS;
	total = 13; requests = expected_start = 0;
	assert(jf_load_episodes("series", "season", "Season") == 0);
	assert(jf_track_count == 8 && requests == 3 && expected_start == 13);
	assert(strcmp(jf_tracks[0].id, "id-0") == 0);
	assert(strcmp(jf_tracks[1].id, "id-6") == 0);
	assert(strcmp(jf_tracks[7].id, "id-12") == 0);
	for (int i = 0; i < jf_track_count; i++) assert(jf_tracks[i].seconds == 0);
	printf("episode pagination and music tests passed; JfTrack=%zu, array=%zu bytes\n",
	       sizeof(JfTrack), sizeof(jf_tracks));
	return 0;
}
