/*
 * JSON helpers over jsmn; see json.c.
 */

#ifndef JSON_H
#define JSON_H

/*
 * Thin wrapper over jsmn. json_parse() remembers the buffer, so every
 * other call takes a token index and nothing else.
 */

int  json_parse(char *body);            /* token count, or <1 on failure */

/* Byte offset where the last parse failed, or -1 if it succeeded. */
int  json_fail_pos(void);

int  json_skip(int i);                    /* index of the next token not inside i */
int  json_get(int obj, const char *key);  /* value index for key, or -1 */

int  json_size(int i);

/*
 * A decimal number as thousandths: 23.976025 gives 23976.
 *
 * For frame rates, which have to reach the transcoder exactly - a rate that
 * arrives wrong makes ffmpeg refuse the encode outright.
 */
int  json_milli(int tok);
int  json_is_array(int i);
int  json_is_string(int i);

/* A nonnegative integer primitive within INT_MAX; 0 on invalid/missing. */
int  json_uint(int tok, int *out);

void json_str(int tok, char *out, int max);   /* unescapes, \uXXXX -> Latin-1 */
int  json_int(int tok);

#endif