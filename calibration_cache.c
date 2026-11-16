/* Keep a complete factory calibration snapshot for each CUPS queue and device. */
#include "calibration.h"

#include <cups/http.h>
#include <cups/sidechannel.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum cache_limit {
	CACHE_KEY_BYTES = 4096,
	CACHE_NAME_BYTES = 96,
	CACHE_HEADER_BYTES = 20,
	CACHE_CREATE_ATTEMPTS = 16,
	CALIBRATION_QUERY_SECONDS = 1,
	CALIBRATION_TOTAL_SECONDS = 8,
};

static const unsigned char cache_magic[8] = {'B', 'R', 'C', 'L', 'B', '0', '0', '1'};

struct query_deadline {
	struct timespec started;
};

/* FNV-1a detects accidental cache corruption; the full key detects collisions. */
static uint64_t hash_bytes(uint64_t hash, const void *buffer, size_t size)
{
	const unsigned char *bytes = buffer;

	while (size--)
		hash = (hash ^ *bytes++) * UINT64_C(1099511628211);
	return hash;
}

static uint64_t snapshot_hash(const char *key, const struct printer_calibration *data)
{
	uint64_t hash = hash_bytes(UINT64_C(14695981039346656037), key, strlen(key));

	return hash_bytes(hash, data, sizeof(*data));
}

static bool make_cache_key(char key[CACHE_KEY_BYTES], const char *queue, const char *uri)
{
	char scheme[32], username[256], host[1024], resource[1024], identity[3072];
	int port, length;

	if (!queue || !*queue || !uri || !*uri || strchr(queue, '\n'))
		return false;
	if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme),
			    username, sizeof(username), host, sizeof(host), &port,
			    resource, sizeof(resource)) < HTTP_URI_STATUS_OK)
		return false;
	/* Printer credentials are not needed to associate a cache with a device. */
	if (httpAssembleURI(HTTP_URI_CODING_ALL, identity, sizeof(identity), scheme,
			    NULL, host, port, resource) < HTTP_URI_STATUS_OK)
		return false;
	length = snprintf(key, CACHE_KEY_BYTES, "%s\n%s", queue, identity);
	return length > 0 && length < CACHE_KEY_BYTES;
}

static void cache_name(char name[CACHE_NAME_BYTES], const char *key)
{
	uint64_t hash = hash_bytes(UINT64_C(14695981039346656037), key, strlen(key));

	snprintf(name, CACHE_NAME_BYTES, "mfcj5720dw-%016" PRIx64 ".calibration", hash);
}

static int open_cache_directory(void)
{
	const char *directory = getenv("CUPS_CACHEDIR");

	if (!directory || !*directory)
		directory = "/var/cache/cups";
	return open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
}

static bool read_exact(int descriptor, void *buffer, size_t size)
{
	unsigned char *cursor = buffer;

	while (size) {
		ssize_t count = read(descriptor, cursor, size);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return false;
		cursor += count;
		size -= count;
	}
	return true;
}

static bool write_exact(int descriptor, const void *buffer, size_t size)
{
	const unsigned char *cursor = buffer;

	while (size) {
		ssize_t count = write(descriptor, cursor, size);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return false;
		cursor += count;
		size -= count;
	}
	return true;
}

static bool load_cache(int directory, const char *key, struct printer_calibration *data)
{
	struct printer_calibration candidate;
	unsigned char header[CACHE_HEADER_BYTES];
	char name[CACHE_NAME_BYTES], stored_key[CACHE_KEY_BYTES];
	struct stat status;
	uint32_t encoded_size;
	uint64_t encoded_hash;
	size_t key_size = strlen(key);
	int descriptor;
	bool valid = false;

	cache_name(name, key);
	descriptor = openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (descriptor < 0)
		return false;
	if (fstat(descriptor, &status) || !S_ISREG(status.st_mode) ||
	    status.st_size != (off_t)(sizeof(header) + key_size + sizeof(candidate)) ||
	    !read_exact(descriptor, header, sizeof(header)) ||
	    memcmp(header, cache_magic, sizeof(cache_magic)))
		goto out;
	memcpy(&encoded_size, header + 8, sizeof(encoded_size));
	memcpy(&encoded_hash, header + 12, sizeof(encoded_hash));
	if (be32toh(encoded_size) != key_size ||
	    !read_exact(descriptor, stored_key, key_size) ||
	    memcmp(stored_key, key, key_size) ||
	    !read_exact(descriptor, &candidate, sizeof(candidate)) ||
	    snapshot_hash(key, &candidate) != be64toh(encoded_hash) ||
	    !calibration_valid(&candidate))
		goto out;
	*data = candidate;
	valid = true;
out:
	close(descriptor);
	return valid;
}

static bool save_cache(int directory, const char *key, const struct printer_calibration *data)
{
	unsigned char header[CACHE_HEADER_BYTES];
	char name[CACHE_NAME_BYTES], temporary[CACHE_NAME_BYTES + 64];
	uint32_t encoded_size = htobe32(strlen(key));
	uint64_t encoded_hash = htobe64(snapshot_hash(key, data));
	unsigned int attempt;
	int descriptor = -1;
	bool saved = false;

	cache_name(name, key);
	for (attempt = 0; attempt < CACHE_CREATE_ATTEMPTS; attempt++) {
		snprintf(temporary, sizeof(temporary), ".%s.%ld.%u", name, (long)getpid(), attempt);
		descriptor = openat(directory, temporary,
				    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (descriptor >= 0 || errno != EEXIST)
			break;
	}
	if (descriptor < 0)
		return false;
	memcpy(header, cache_magic, sizeof(cache_magic));
	memcpy(header + 8, &encoded_size, sizeof(encoded_size));
	memcpy(header + 12, &encoded_hash, sizeof(encoded_hash));
	if (!write_exact(descriptor, header, sizeof(header)) ||
	    !write_exact(descriptor, key, strlen(key)) ||
	    !write_exact(descriptor, data, sizeof(*data)) || fsync(descriptor))
		goto out;
	if (close(descriptor)) {
		descriptor = -1;
		goto out;
	}
	descriptor = -1;
	/* Readers see either the old complete snapshot or the new complete one. */
	if (!renameat(directory, temporary, directory, name))
		saved = true;
out:
	if (descriptor >= 0)
		close(descriptor);
	if (!saved)
		unlinkat(directory, temporary, 0);
	return saved;
}

static int query_calibration(void *context, const char *oid, char *reply, size_t *size)
{
	const struct query_deadline *deadline = context;
	struct timespec now;
	double remaining;
	int bytes = (int)*size;
	cups_sc_status_t status;

	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return 1;
	remaining = CALIBRATION_TOTAL_SECONDS -
		    (now.tv_sec - deadline->started.tv_sec) -
		    (now.tv_nsec - deadline->started.tv_nsec) / 1000000000.0;
	if (remaining <= 0)
		return 1;
	if (remaining > CALIBRATION_QUERY_SECONDS)
		remaining = CALIBRATION_QUERY_SECONDS;
	status = cupsSideChannelSNMPGet(oid, reply, &bytes, remaining);
	if (status != CUPS_SC_STATUS_OK || bytes < 0 || (size_t)bytes > *size)
		return 1;
	*size = bytes;
	return 0;
}

static bool side_channel_available(void)
{
	socklen_t size = sizeof(int);
	int type;

	return !getsockopt(CUPS_SC_FD, SOL_SOCKET, SO_TYPE, &type, &size) &&
	       type == SOCK_STREAM;
}

bool calibration_prepare(struct printer_calibration *data, const char *queue,
			 const char *device_uri, bool refresh)
{
	struct printer_calibration candidate;
	struct query_deadline deadline;
	char key[CACHE_KEY_BYTES];
	int directory = -1;
	bool cached = false, available = false;

	if (make_cache_key(key, queue, device_uri)) {
		directory = open_cache_directory();
		if (directory >= 0)
			cached = load_cache(directory, key, data);
	}
	if (cached && !refresh) {
		fputs("DEBUG: Using cached Brother factory calibration.\n", stderr);
		available = true;
		goto out;
	}
	if (!side_channel_available() || clock_gettime(CLOCK_MONOTONIC, &deadline.started) ||
	    calibration_fetch(&candidate, query_calibration, &deadline)) {
		fprintf(stderr, "DEBUG: Brother factory calibration unavailable; %s.\n",
			cached ? "keeping the cached calibration" : "using model defaults");
		available = cached;
		goto out;
	}
	*data = candidate;
	available = true;
	fputs("DEBUG: Retrieved complete Brother factory calibration (6033 bytes).\n", stderr);
	if (directory < 0 || !save_cache(directory, key, data))
		fputs("DEBUG: Cannot cache Brother calibration; using it for this job.\n", stderr);
out:
	if (directory >= 0)
		close(directory);
	return available;
}
