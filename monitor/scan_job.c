/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "scan_job.h"
#include "scan_control.h"

#include <cairo.h>
#include <cairo-pdf.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <errno.h>
#include <string.h>

#define SCAN_MAX_PAGES 500
#define SANE_NO_DOCUMENTS 7
#define BROTHER_NETWORK_CONFIG "/etc/opt/brother/scanner/brscan4/brsanenetdevice4.cfg"

/* Values advertised by brscan4, up to the model's maximum optical resolution. */
const unsigned int scan_resolutions[] = { 100, 150, 200, 300, 400, 600, 1200, 2400 };
const size_t scan_resolution_count = G_N_ELEMENTS(scan_resolutions);

void scan_settings_defaults(struct scan_settings *settings)
{
	const char *documents = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
	g_autofree char *directory = g_build_filename(documents ? documents :
						    g_get_home_dir(), "Scans", NULL);
	const char *host = g_get_host_name();
	const char *user = g_get_user_name();
	unsigned int i;

	memset(settings, 0, sizeof(*settings));
	g_strlcpy(settings->directory, directory, sizeof(settings->directory));
	/* Keep the first 15 characters of the complete display name. */
	g_snprintf(settings->name, sizeof(settings->name), "%s de %s", host, user);
	for (i = 0; settings->name[i]; i++) {
		if (!g_ascii_isalnum(settings->name[i]) && !strchr(" .-", settings->name[i]))
			settings->name[i] = '_';
	}
	g_strlcpy(settings->languages, "fra+eng", sizeof(settings->languages));
	settings->resolution = 300;
}

gboolean scan_settings_valid(const struct scan_settings *settings, GError **error)
{
	g_autoptr(GInetAddress) address = NULL;
	const char *p;
	size_t i;

	if (!settings->name[0])
		goto invalid_name;
	for (p = settings->name; *p; p++) {
		if (!g_ascii_isalnum(*p) && !strchr(" _.-", *p))
			goto invalid_name;
	}
	if (!g_path_is_absolute(settings->directory)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("Choose an absolute folder path for scanned documents."));
		return FALSE;
	}
	if (settings->address[0]) {
		address = g_inet_address_new_from_string(settings->address);
		if (!address || g_inet_address_get_family(address) != G_SOCKET_FAMILY_IPV4 ||
		    g_inet_address_get_is_any(address) || g_inet_address_get_is_multicast(address)) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				_("Enter a reachable IPv4 address for this PC, or leave it empty."));
			return FALSE;
		}
	}
	for (i = 0; i < scan_resolution_count; i++) {
		if (settings->resolution == scan_resolutions[i])
			break;
	}
	if (i == scan_resolution_count) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("Invalid scan resolution."));
		return FALSE;
	}
	if (!settings->languages[0])
		goto invalid_language;
	for (p = settings->languages; *p; p++) {
		if (!g_ascii_isalpha(*p) && *p != '_' && *p != '+')
			goto invalid_language;
	}
	return TRUE;
invalid_name:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		_("The PC name must contain 1–15 ASCII letters, digits, spaces, dots, hyphens or underscores."));
	return FALSE;
invalid_language:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
		_("Use OCR language codes separated by +, for example fra+eng."));
	return FALSE;
}

static void load_string(GKeyFile *file, const char *key, char *value, size_t size)
{
	g_autofree char *text = g_key_file_get_string(file, "Scan", key, NULL);

	if (text)
		g_strlcpy(value, text, size);
}

void scan_settings_load(struct scan_settings *settings, GKeyFile *file)
{
	scan_settings_defaults(settings);
	if (!g_key_file_has_group(file, "Scan"))
		return;
	settings->enabled = g_key_file_get_boolean(file, "Scan", "enabled", NULL);
	settings->grayscale = g_key_file_get_boolean(file, "Scan", "grayscale", NULL);
	settings->resolution = g_key_file_get_integer(file, "Scan", "resolution", NULL);
	load_string(file, "name", settings->name, sizeof(settings->name));
	load_string(file, "directory", settings->directory, sizeof(settings->directory));
	load_string(file, "address", settings->address, sizeof(settings->address));
	load_string(file, "languages", settings->languages, sizeof(settings->languages));
}

void scan_settings_save(const struct scan_settings *settings, GKeyFile *file)
{
	g_key_file_set_boolean(file, "Scan", "enabled", settings->enabled);
	g_key_file_set_boolean(file, "Scan", "grayscale", settings->grayscale);
	/* Drop the obsolete source preference when updating an existing key file. */
	g_key_file_remove_key(file, "Scan", "source", NULL);
	g_key_file_set_integer(file, "Scan", "resolution", settings->resolution);
	g_key_file_set_string(file, "Scan", "name", settings->name);
	g_key_file_set_string(file, "Scan", "directory", settings->directory);
	g_key_file_set_string(file, "Scan", "address", settings->address);
	g_key_file_set_string(file, "Scan", "languages", settings->languages);
}

const char *scan_destination_name(enum scan_destination destination)
{
	const char *names[] = { N_("Image"), N_("Email"), N_("OCR"), N_("File") };

	return (unsigned int)destination < SCAN_DESTINATIONS ? _(names[destination]) : _("Unknown");
}

static void cancel_child(GCancellable *cancel, gpointer data)
{
	(void)cancel;
	g_subprocess_force_exit(G_SUBPROCESS(data));
}

/* No shell, commands or paths from a network datagram ever reach this function. */
static int run_program(const char *directory, const char *const *argv,
		       GCancellable *cancel, GString *log, char **output, GError **error)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		(output ? G_SUBPROCESS_FLAGS_STDOUT_PIPE : G_SUBPROCESS_FLAGS_STDOUT_SILENCE) |
		G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autofree char *diagnostic = NULL;
	gulong handler;
	gboolean ok;

	if (g_cancellable_set_error_if_cancelled(cancel, error))
		return -1;
	g_subprocess_launcher_set_cwd(launcher, directory);
	g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
	child = g_subprocess_launcher_spawnv(launcher, argv, error);
	if (!child)
		return -1;
	handler = g_cancellable_connect(cancel, G_CALLBACK(cancel_child), child, NULL);
	ok = g_subprocess_communicate_utf8(child, NULL, cancel, output, &diagnostic, error);
	g_cancellable_disconnect(cancel, handler);
	if (!ok) {
		g_subprocess_force_exit(child);
		g_subprocess_wait(child, NULL, NULL);
		return -1;
	}
	if (diagnostic)
		g_string_append(log, diagnostic);
	if (g_cancellable_set_error_if_cancelled(cancel, error))
		return -1;
	return g_subprocess_get_if_exited(child) ? g_subprocess_get_exit_status(child) : -1;
}

char *scan_device_for_printer(const char *config, const char *devices,
			      const char *printer, GError **error)
{
	g_auto(GStrv) records = g_strsplit(config, "\n", -1);
	g_auto(GStrv) scanners = g_strsplit(devices, "\n", -1);
	g_autoptr(GHashTable) names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autofree char *name = NULL;
	char *device = NULL;
	unsigned int matches = 0, devices_found = 0, i;

	/* DEVICE is Brother's friendly name, returned as SANE's model (%m).
	 * Enumeration order is reversed and may skip entries: never infer devN
	 * from a line number. Correlate the unique friendly name and exact IP.
	 */
	for (i = 0; records[i]; i++) {
		char *line = g_strstrip(records[i]);
		char *comma, *address, *end;
		unsigned int count;

		if (!g_str_has_prefix(line, "DEVICE="))
			continue;
		comma = strchr(line + 7, ',');
		if (!comma)
			continue;
		*comma = 0;
		line = g_strstrip(line + 7);
		count = GPOINTER_TO_UINT(g_hash_table_lookup(names, line));
		g_hash_table_replace(names, g_strdup(line), GUINT_TO_POINTER(count + 1));
		address = strstr(comma + 1, "IP-ADDRESS=");
		if (!address)
			continue;
		address = g_strstrip(address + strlen("IP-ADDRESS="));
		end = address + strcspn(address, " \t\r,#");
		*end = 0;
		if (!strcmp(address, printer)) {
			g_free(name);
			name = g_strdup(line);
			matches++;
		}
	}
	if (matches != 1 || GPOINTER_TO_UINT(g_hash_table_lookup(names, name)) != 1)
		goto unavailable;
	for (i = 0; scanners[i]; i++) {
		char *tab = strchr(scanners[i], '\t');
		const char *suffix;

		if (!tab)
			continue;
		*tab = 0;
		if (strcmp(tab + 1, name) || !g_str_has_prefix(scanners[i], "brother4:net1;dev"))
			continue;
		suffix = scanners[i] + strlen("brother4:net1;dev");
		if (!*suffix || strspn(suffix, "0123456789") != strlen(suffix))
			continue;
		g_free(device);
		device = g_strdup(scanners[i]);
		devices_found++;
	}
	if (devices_found == 1)
		return device;
	g_free(device);
unavailable:
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
		_("Configure one Brother brscan4 network scanner for %s, with a unique name and an IP address."), printer);
	return NULL;
}

static char *find_scanner(const struct scan_job *job, const char *directory,
			 GCancellable *cancel, GString *log, GError **error)
{
	g_autofree char *config = NULL, *devices = NULL;
	const char *args[] = { "scanimage", "--formatted-device-list=%d\t%m%n", NULL };
	int status;

	if (!g_file_get_contents(BROTHER_NETWORK_CONFIG, &config, NULL, NULL)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
			_("Install and configure the Brother brscan4 64-bit scanner backend before receiving scans."));
		return NULL;
	}
	status = run_program(directory, args, cancel, log, &devices, error);
	if (status != 0) {
		if (!*error)
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
				_("Cannot list SANE scanners. Check scan.log and the brscan4 installation."));
		return NULL;
	}
	return scan_device_for_printer(config, devices, job->printer, error);
}

static unsigned int count_pages(const char *directory)
{
	unsigned int page;

	for (page = 1; page <= SCAN_MAX_PAGES; page++) {
		g_autofree char *name = g_strdup_printf("%s/page-%04u.png", directory, page);

		if (!g_file_test(name, G_FILE_TEST_IS_REGULAR))
			break;
	}
	return page - 1;
}

static int acquire(const struct scan_job *job, const char *directory,
		   const char *device, const char *source, GCancellable *cancel, GString *log, GError **error)
{
	g_autofree char *resolution = g_strdup_printf("--resolution=%u", job->settings.resolution);
	g_autofree char *input = g_strdup_printf("--source=%s", source);
	g_autofree char *limit = g_strdup_printf("--batch-count=%u", SCAN_MAX_PAGES);
	const char *args[] = { "scanimage", "--device-name", device,
		"--format=png", "--batch=page-%04d.png", resolution, input,
		job->settings.grayscale ? "--mode=True Gray" : "--mode=24bit Color[Fast]",
		limit, NULL };

	return run_program(directory, args, cancel, log, NULL, error);
}

static void remove_scan_images(const char *directory, GString *log, GError **error)
{
	g_autoptr(GError) cleanup_error = NULL;
	g_autoptr(GDir) folder = g_dir_open(directory, 0, &cleanup_error);
	const char *name;

	while (folder && (name = g_dir_read_name(folder))) {
		g_autofree char *path = NULL;

		/* Include incomplete scanimage output and interrupted rotations. */
		if (!g_str_has_suffix(name, ".png") &&
		    !g_str_has_suffix(name, ".png.part") &&
		    !g_str_has_suffix(name, ".png.rotated"))
			continue;
		path = g_build_filename(directory, name, NULL);
		if (g_unlink(path) < 0 && errno != ENOENT) {
			g_string_append_printf(log, "Cannot remove %s: %s\n", name, g_strerror(errno));
			if (!cleanup_error)
				g_set_error(&cleanup_error, G_FILE_ERROR, g_file_error_from_errno(errno),
					_("Cannot remove a temporary scan image: %s"), g_strerror(errno));
		}
	}
	if (cleanup_error) {
		g_string_append_printf(log, "%s\n", cleanup_error->message);
		if (!*error)
			*error = g_steal_pointer(&cleanup_error);
	}
}

static gboolean save_jpeg_pages(const char *directory, unsigned int pages,
				GCancellable *cancel, GError **error)
{
	unsigned int page;

	for (page = 1; page <= pages; page++) {
		g_autofree char *input = g_strdup_printf("%s/page-%04u.png", directory, page);
		g_autofree char *output = g_strdup_printf("%s/page-%04u.jpg", directory, page);
		g_autoptr(GdkPixbuf) image = NULL;

		if (g_cancellable_set_error_if_cancelled(cancel, error))
			return FALSE;
		image = gdk_pixbuf_new_from_file(input, error);
		if (!image)
			return FALSE;
		if (!gdk_pixbuf_save(image, output, "jpeg", error, "quality", "95", NULL)) {
			g_unlink(output);
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean rotate_back_pages(const char *directory, unsigned int pages,
				  GCancellable *cancel, GError **error)
{
	unsigned int page;

	/* The backend delivers both sides in long-edge orientation. Normalize
	 * short-edge backs before every destination, including image output and OCR.
	 */
	for (page = 2; page <= pages; page += 2) {
		g_autofree char *path = g_strdup_printf("%s/page-%04u.png", directory, page);
		g_autofree char *temporary = g_strconcat(path, ".rotated", NULL);
		cairo_surface_t *original, *rotated;
		cairo_status_t status;
		cairo_t *cr;
		int width, height;

		if (g_cancellable_set_error_if_cancelled(cancel, error))
			return FALSE;
		original = cairo_image_surface_create_from_png(path);
		width = cairo_image_surface_get_width(original);
		height = cairo_image_surface_get_height(original);
		rotated = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
		cr = cairo_create(rotated);
		cairo_translate(cr, width, height);
		cairo_scale(cr, -1, -1);
		cairo_set_source_surface(cr, original, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
		cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
		cairo_paint(cr);
		status = cairo_surface_status(original);
		if (status == CAIRO_STATUS_SUCCESS)
			status = cairo_status(cr);
		if (status == CAIRO_STATUS_SUCCESS)
			status = cairo_surface_write_to_png(rotated, temporary);
		cairo_destroy(cr);
		cairo_surface_destroy(rotated);
		cairo_surface_destroy(original);
		if (status != CAIRO_STATUS_SUCCESS) {
			g_unlink(temporary);
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
				_("Cannot orient the scanned back page: %s"), cairo_status_to_string(status));
			return FALSE;
		}
		if (g_rename(temporary, path) < 0) {
			int saved_errno = errno;

			g_unlink(temporary);
			g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
				_("Cannot save the oriented back page: %s"), g_strerror(saved_errno));
			return FALSE;
		}
	}
	return TRUE;
}

gboolean scan_write_pdf(const char *directory, unsigned int pages,
			unsigned int resolution, GCancellable *cancel, GError **error)
{
	g_autofree char *path = g_build_filename(directory, "document.pdf", NULL);
	cairo_surface_t *pdf = cairo_pdf_surface_create(path, 1, 1);
	cairo_t *cr = cairo_create(pdf);
	cairo_status_t status = CAIRO_STATUS_SUCCESS;
	unsigned int page;
	gboolean ok = FALSE;

	for (page = 1; page <= pages; page++) {
		g_autofree char *name = g_strdup_printf("%s/page-%04u.png", directory, page);
		cairo_surface_t *image;
		double width, height;

		if (g_cancellable_set_error_if_cancelled(cancel, error))
			goto out;
		image = cairo_image_surface_create_from_png(name);
		status = cairo_surface_status(image);
		if (status != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(image);
			goto failed;
		}
		width = cairo_image_surface_get_width(image) * 72.0 / resolution;
		height = cairo_image_surface_get_height(image) * 72.0 / resolution;
		cairo_pdf_surface_set_size(pdf, width, height);
		cairo_save(cr);
		cairo_scale(cr, 72.0 / resolution, 72.0 / resolution);
		cairo_set_source_surface(cr, image, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
		cairo_show_page(cr);
		cairo_surface_destroy(image);
	}
	cairo_surface_finish(pdf);
	status = cairo_status(cr);
	if (status == CAIRO_STATUS_SUCCESS)
		status = cairo_surface_status(pdf);
	if (status == CAIRO_STATUS_SUCCESS && pages) {
		ok = TRUE;
		goto out;
	}
failed:
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Cannot create PDF: %s"),
		    cairo_status_to_string(status));
out:
	cairo_destroy(cr);
	cairo_surface_destroy(pdf);
	if (!ok)
		g_unlink(path);
	return ok;
}

static gboolean recognize(const struct scan_job *job, struct scan_result *result,
			  GCancellable *cancel, GString *log)
{
	g_autoptr(GString) list = g_string_new(NULL);
	g_autofree char *path = g_build_filename(result->directory, "pages.txt", NULL);
	g_autofree char *dpi = g_strdup_printf("%u", job->settings.resolution);
	const char *args[] = { "tesseract", "pages.txt", "document", "-l",
		job->settings.languages, "--dpi", dpi, "pdf", NULL };
	unsigned int page;
	int status;

	for (page = 1; page <= result->pages; page++)
		g_string_append_printf(list, "page-%04u.png\n", page);
	if (!g_file_set_contents(path, list->str, list->len, &result->error))
		return FALSE;
	status = run_program(result->directory, args, cancel, log, NULL, &result->error);
	g_unlink(path);
	if (status != 0 && !result->error)
		g_set_error_literal(&result->error, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("OCR failed. Check the installed Tesseract languages and scan.log."));
	return !result->error;
}

struct scan_result *scan_job_run(const struct scan_job *job, GCancellable *cancel)
{
	struct scan_result *result = g_new0(struct scan_result, 1);
	g_autoptr(GString) log = g_string_new(NULL);
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	g_autofree char *stamp = g_date_time_format(now, "%Y-%m-%d_%H-%M-%S");
	g_autofree char *log_path = NULL;
	g_autofree char *device = NULL;
	const char *source;
	enum scan_sides sides;
	int status;

	if (!scan_settings_valid(&job->settings, &result->error))
		return result;
	if (g_mkdir_with_parents(job->settings.directory, 0700) < 0)
		goto file_error;
	result->directory = g_strdup_printf("%s/%s-XXXXXX", job->settings.directory, stamp);
	if (!g_mkdtemp_full(result->directory, 0700))
		goto file_error;
	if (!scan_control_query(job->printer, job->destination, &sides, cancel, log, &result->error))
		goto out;
	/* Feeder mode lets the printer select the loaded input and signal the
	 * end of the batch, including a single page from the glass.
	 */
	source = sides != SCAN_ONE_SIDED ? "Automatic Document Feeder(left aligned,Duplex)" :
		"Automatic Document Feeder(left aligned)";
	g_string_append_printf(log, "Requested SANE source: %s\nBack-page rotation: %u degrees\n",
			       source, sides == SCAN_SHORT_EDGE ? 180 : 0);
	device = find_scanner(job, result->directory, cancel, log, &result->error);
	if (!device)
		goto out;
	status = acquire(job, result->directory, device, source, cancel, log, &result->error);
	result->pages = count_pages(result->directory);
	if (result->error)
		goto out;
	if (!result->pages || (status != 0 && status != SANE_NO_DOCUMENTS)) {
		g_set_error(&result->error, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Scanning failed (status %d). See scan.log."), status);
		goto out;
	}
	if (result->pages == SCAN_MAX_PAGES) {
		g_set_error_literal(&result->error, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("The 500-page limit was reached. Check the feeder."));
		goto out;
	}
	if (sides == SCAN_SHORT_EDGE &&
	    !rotate_back_pages(result->directory, result->pages, cancel, &result->error))
		goto out;
	if (job->destination == SCAN_IMAGE) {
		if (!save_jpeg_pages(result->directory, result->pages, cancel, &result->error))
			goto out;
		result->document = g_build_filename(result->directory, "page-0001.jpg", NULL);
	} else if (job->destination == SCAN_OCR) {
		if (!recognize(job, result, cancel, log))
			goto out;
		result->document = g_build_filename(result->directory, "document.pdf", NULL);
	} else if (scan_write_pdf(result->directory, result->pages,
				  job->settings.resolution, cancel, &result->error)) {
		result->document = g_build_filename(result->directory, "document.pdf", NULL);
	}
out:
	/* Every path after acquisition converges here, including errors/cancellation. */
	remove_scan_images(result->directory, log, &result->error);
	log_path = g_build_filename(result->directory, "scan.log", NULL);
	g_file_set_contents(log_path, log->str, log->len, NULL);
	return result;
file_error:
	g_set_error(&result->error, G_FILE_ERROR, g_file_error_from_errno(errno),
		    _("Cannot create the scan folder: %s"), g_strerror(errno));
	return result;
}

void scan_result_free(struct scan_result *result)
{
	if (!result)
		return;
	g_free(result->directory);
	g_free(result->document);
	g_clear_error(&result->error);
	g_free(result);
}
