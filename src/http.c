#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "http.h"
#include "logging.h"

#define BUFFER_STEP (1 << 18)
#define BUFFER_INIT_RATIO (1)

struct http_message_t *http_message_new()
{
	struct http_message_t *msg = calloc(1, sizeof(*msg));
	if (msg == NULL) {
		ERR("failed to alloc space for http message");
		return NULL;
	}


	size_t capacity = BUFFER_STEP;
	msg->spare_buffer = calloc(capacity, sizeof(*(msg->spare_buffer)));
	if (msg->spare_buffer == NULL) {
		ERR("failed to alloc buffer for http message");
		free(msg);
		return NULL;
	}

	msg->spare_capacity = capacity;

	return msg;
}

void message_free(struct http_message_t *msg)
{
	free(msg->spare_buffer);
	free(msg);
}


static int doesMatch(const char *matcher, size_t matcher_len,
                     const uint8_t *matchy,  size_t matchy_len)
{
	for (size_t i = 0; i < matcher_len; i++)
		if (i >= matchy_len || matcher[i] != matchy[i])
			return 0;
	return 1;
}

static int inspect_header_field(struct http_packet_t *pkt, size_t header_size,
                                char *key, size_t key_size)
{
	// Find key
	uint8_t *pos = memmem(pkt->buffer, header_size, key, key_size);
	if (pos == NULL)
		return -1;

	// Find first digit
	size_t number_pos = (pos - pkt->buffer) + key_size;
	while (number_pos < pkt->filled_size && !isdigit(pkt->buffer[number_pos]))
		++number_pos;

	// Find next non-digit
	size_t number_end = number_pos;
	while (number_end < pkt->filled_size && isdigit(pkt->buffer[number_end]))
		++number_end;

	// Failed to find next non-digit
	// number may have been at end of buffer
	if (number_end >= pkt->filled_size)
		return -1;

	// Temporary stringification of buffer for atoi()
	char original_char = pkt->buffer[number_end];
	pkt->buffer[number_end] = '\0';
	int val = atoi((const char *)(pkt->buffer + number_pos));
	pkt->buffer[number_end] = original_char;
	return val;
}

static long long packet_get_header_size(struct http_packet_t *pkt)
{
	// Find header
	for (size_t i = 0; i < pkt->filled_size; i++) {
		// two \r\n pairs
		if ((i + 3) < pkt->filled_size &&
		    '\r' == pkt->buffer[i] &&
		    '\n' == pkt->buffer[i + 1] &&
		    '\r' == pkt->buffer[i + 2] &&
		    '\n' == pkt->buffer[i + 3]) {
				return i + 4;
		}

		// two \n pairs
		if ((i + 1) < pkt->filled_size &&
		    '\n' == pkt->buffer[i] &&
		    '\n' == pkt->buffer[i + 1]) {
				return i + 2;
		}
	}

	return -1;
}

enum http_request_t packet_find_type(struct http_packet_t *pkt)
{
	enum http_request_t type = HTTP_UNKNOWN;
	int size = -1;
	/* Valid methods for determining http request
	 * size are defined by W3 in RFC2616 section 4.4
	 * link: http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4
	 */

	/* This function attempts to find what method this
	 * packet would use. This is only possible in specific case:
	 * 1. if the request uses method 1 we can check the http 
	 *    request type. We must be called on a packet which
	 *    has the full header.
	 * 2. if the request uses method 2 we need the full header
	 *    but a simple network-byte-order-aware string search
	 *    works. This function does not work if called with
	 *    a chunked transport's sub-packet.
	 * 3. if the request uses method 3 we again perform the
	 *    string search.
	 * 
	 * All cases require the packat to contain the full header.
	 */

	/* RFC2616 recomends we match newline on \n despite full
	 * complience requires the message to use only \r\n
	 * http://www.w3.org/Protocols/rfc2616/rfc2616-sec19.html#sec19.3
	 */

	long long header_size = packet_get_header_size(pkt);
	if (header_size < 0) {
		// We don't have the header yet
		// TODO: make UNSET mean no header
		// and UNKNOWN mean no known size
		goto do_ret;
	}

	// Try Transfer-Encoding Chunked
	char xfer_encode_str[] = "Transfer-Encoding: chunked";
	int xfer_encode_str_size = sizeof(xfer_encode_str) - 1;
	uint8_t *xfer_encode_pos = memmem(pkt->buffer, header_size,
	                                  xfer_encode_str,
	                                  xfer_encode_str_size);
	if (xfer_encode_pos != NULL) {
		size = 0;
		type = HTTP_CHUNKED;
		goto do_ret;
	}

	// Try Content-Length
	char content_length_str[] = "Content-Length: ";
	size = inspect_header_field(pkt, header_size, content_length_str,
	                            sizeof(content_length_str) - 1);
	if (size >= 0) {
		size += header_size;
		type = HTTP_CONTENT_LENGTH;
		goto do_ret;
	} 

	// Get requests
	if (doesMatch("GET", 3, pkt->buffer, pkt->filled_size)) {
		size = pkt->filled_size;
		type = HTTP_HEADER_ONLY;
		goto do_ret;
	}


	// Note: if we got here then either the packet did not contain
	// the full header or the client intends to close the connection
	// to signal end of message. We let the caller decide which it is.
    
do_ret:
	pkt->parent_message->claimed_size = size;
	pkt->parent_message->type = type;
	return type;
}

void packet_mark_received(struct http_packet_t *pkt, size_t received)
{
	pkt->filled_size += received;

	struct http_message_t *msg = pkt->parent_message;
	msg->received_size += received;
}

struct http_packet_t *packet_new(struct http_message_t *parent_msg)
{
	struct http_packet_t *pkt = NULL;
	uint8_t              *buf = NULL;
	size_t const capacity = BUFFER_STEP * BUFFER_INIT_RATIO;

	assert(parent_msg != NULL);

	buf = calloc(capacity, sizeof(*buf));
	pkt = calloc(1, sizeof(*pkt));
	if (buf == NULL || pkt == NULL) {
		ERR("failed to alloc space for packet's buffer or space for packet");
		free(pkt);
		free(buf);
		return NULL;
	}
	
	// Assemble packet
	pkt->buffer = buf;
	pkt->buffer_capacity = capacity;
	pkt->filled_size = 0;
	pkt->parent_message = parent_msg;

	return pkt;
}

void packet_free(struct http_packet_t *pkt)
{
	free(pkt->buffer);
	free(pkt);
}
