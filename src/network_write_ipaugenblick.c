#include "network_backends.h"

#include "network.h"
#include "log.h"

#include "sys-socket.h"

#include <unistd.h>

#include <errno.h>
#include <string.h>
#include <ipaugenblick_api.h>

int network_ipaugenblick_file_chunk(server *srv, connection *con, int fd, chunkqueue *cq, off_t *p_max_bytes) {
	chunk* const c = cq->first;
	off_t offset, toSend, toSend2;
	ssize_t r;
	int rc;

	force_assert(NULL != c);
	force_assert(FILE_CHUNK == c->type);
	force_assert(c->offset >= 0 && c->offset <= c->file.length);

	offset = c->file.start + c->offset;
	toSend = c->file.length - c->offset;
	if (toSend > 64*1024) toSend = 64*1024; /* max read 64kb in one step */
	if (toSend > *p_max_bytes) toSend = *p_max_bytes;

	if (0 == toSend) {
		chunkqueue_remove_finished_chunks(cq);
		return 0;
	}

	if (0 != network_open_file_chunk(srv, con, cq)) return -1;
	int buffers_count = toSend / 1448 + ((toSend % 1448) ? 1 : 0);
	struct data_and_descriptor bufs_and_desc[buffers_count];
	int offsets[buffers_count];
	int lengths[buffers_count];
	if (0 != ipaugenblick_get_buffers_bulk(toSend, fd, buffers_count, bufs_and_desc)) {
		return -1;
	}

	if (-1 == lseek(c->file.fd, offset, SEEK_SET)) {
		log_error_write(srv, __FILE__, __LINE__, "ss", "lseek: ", strerror(errno));
		return -1;
	}
	int buf_idx = 0;
	toSend2 = toSend;
	while(toSend > 0) {
		int toRead = (toSend > 1448) ? 1448 : toSend;
		if (-1 == (toSend = read(c->file.fd, bufs_and_desc[buf_idx].pdata, toRead))) {
			log_error_write(srv, __FILE__, __LINE__, "ss", "read: ", strerror(errno));
			return -1;
		}
		offsets[buf_idx] = 0;
		lengths[buf_idx] = toRead;
		ipaugenblick_set_buffer_data_len(bufs_and_desc[buf_idx].pdesc, toRead);
		buf_idx++;
		toSend -= toRead;
	}

	rc = ipaugenblick_send_bulk(fd, bufs_and_desc, offsets, lengths, buffers_count);
	if (!rc)
		chunkqueue_mark_written(cq, toSend2 - toSend + 1);

	return rc;
}

int network_ipaugenblick_chunkqueue_write(server *srv, connection *con, int fd, chunkqueue *cq, off_t max_bytes) {
	int buffer_count = 0, i, rc;
	chunk* next;
	chunk* c = cq->first;

	while (max_bytes > 0 && c) {
		next = c->next;
		switch (/*cq->first*/c->type) {
		case MEM_CHUNK:	
			max_bytes -= c->mem->used;
			{
				int offsets[c->mem->buffers_count];
				int lengths[c->mem->buffers_count];
				for(i = 0; i < c->mem->buffers_count;i++) {
					offsets[i] = c->offset;
					lengths[i] = ipaugenblick_get_buffer_data_len(c->mem->bufs_and_desc[i].pdesc);
				}
				rc = ipaugenblick_send_bulk(fd, c->mem->bufs_and_desc, offsets, lengths, c->mem->buffers_count);
				if (rc)
					next = NULL;/* force exit loop */
				else {
					c->mem->buffers_count = 0;
					chunkqueue_mark_written(cq, c->mem->used);
				}
			}
			break;
		case FILE_CHUNK:
			rc = network_ipaugenblick_file_chunk(srv, con, fd, cq, &max_bytes);
			if (rc)
				next = NULL;/* force exit loop */
			break;
		}
		c = next;
	}
	while(ipaugenblick_socket_kick(fd) != 0);
	return (rc != 0) ? 1 : 0;
}

void network_ipaugenblick_readall(int fd)
{
	int len = 0;
	int segment_len = 0;
	void *pdesc = NULL;
	void *rxbuff = NULL;

	while(ipaugenblick_receive(fd, &rxbuff, &len, &segment_len,&pdesc) == 0) {
		ipaugenblick_release_rx_buffer(pdesc, fd);
		len = 0;
	}
}
