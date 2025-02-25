/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>

#include <sd-device.h>
#include <sd-messages.h>

#include "blkid-util.h"
#include "blockdev-util.h"
#include "build.h"
#include "chase.h"
#include "efi-loader.h"
#include "efivars.h"
#include "escape.h"
#include "fd-util.h"
#include "main-func.h"
#include "mountpoint-util.h"
#include "openssl-util.h"
#include "parse-argument.h"
#include "parse-util.h"
#include "pretty-print.h"
#include "tpm2-pcr.h"
#include "tpm2-util.h"
#include "varlink.h"
#include "varlink-io.systemd.PCRExtend.h"

static bool arg_graceful = false;
static char *arg_tpm2_device = NULL;
static char **arg_banks = NULL;
static char *arg_file_system = NULL;
static bool arg_machine_id = false;
static unsigned arg_pcr_index = UINT_MAX;
static bool arg_varlink = false;

STATIC_DESTRUCTOR_REGISTER(arg_banks, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_tpm2_device, freep);
STATIC_DESTRUCTOR_REGISTER(arg_file_system, freep);

#define EXTENSION_STRING_SAFE_LIMIT 1024

static int help(int argc, char *argv[], void *userdata) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-pcrextend", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s  [OPTIONS...] WORD\n"
               "%1$s  [OPTIONS...] --file-system=PATH\n"
               "%1$s  [OPTIONS...] --machine-id\n"
               "\n%5$sExtend a TPM2 PCR with boot phase, machine ID, or file system ID.%6$s\n"
               "\n%3$sOptions:%4$s\n"
               "  -h --help              Show this help\n"
               "     --version           Print version\n"
               "     --bank=DIGEST       Select TPM PCR bank (SHA1, SHA256)\n"
               "     --pcr=INDEX         Select TPM PCR index (0…23)\n"
               "     --tpm2-device=PATH  Use specified TPM2 device\n"
               "     --graceful          Exit gracefully if no TPM2 device is found\n"
               "     --file-system=PATH  Measure UUID/labels of file system into PCR 15\n"
               "     --machine-id        Measure machine ID into PCR 15\n"
               "\nSee the %2$s for details.\n",
               program_invocation_short_name,
               link,
               ansi_underline(),
               ansi_normal(),
               ansi_highlight(),
               ansi_normal());

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_BANK,
                ARG_PCR,
                ARG_TPM2_DEVICE,
                ARG_GRACEFUL,
                ARG_FILE_SYSTEM,
                ARG_MACHINE_ID,
        };

        static const struct option options[] = {
                { "help",        no_argument,       NULL, 'h'             },
                { "version",     no_argument,       NULL, ARG_VERSION     },
                { "bank",        required_argument, NULL, ARG_BANK        },
                { "pcr",         required_argument, NULL, ARG_PCR         },
                { "tpm2-device", required_argument, NULL, ARG_TPM2_DEVICE },
                { "graceful",    no_argument,       NULL, ARG_GRACEFUL    },
                { "file-system", required_argument, NULL, ARG_FILE_SYSTEM },
                { "machine-id",  no_argument,       NULL, ARG_MACHINE_ID  },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)
                switch (c) {

                case 'h':
                        help(0, NULL, NULL);
                        return 0;

                case ARG_VERSION:
                        return version();

                case ARG_BANK: {
                        const EVP_MD *implementation;

                        implementation = EVP_get_digestbyname(optarg);
                        if (!implementation)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Unknown bank '%s', refusing.", optarg);

                        if (strv_extend(&arg_banks, EVP_MD_name(implementation)) < 0)
                                return log_oom();

                        break;
                }

                case ARG_PCR:
                        r = tpm2_pcr_index_from_string(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse PCR index: %s", optarg);

                        arg_pcr_index = r;
                        break;

                case ARG_TPM2_DEVICE: {
                        _cleanup_free_ char *device = NULL;

                        if (streq(optarg, "list"))
                                return tpm2_list_devices();

                        if (!streq(optarg, "auto")) {
                                device = strdup(optarg);
                                if (!device)
                                        return log_oom();
                        }

                        free_and_replace(arg_tpm2_device, device);
                        break;
                }

                case ARG_GRACEFUL:
                        arg_graceful = true;
                        break;

                case ARG_FILE_SYSTEM:
                        r = parse_path_argument(optarg, /* suppress_root= */ false, &arg_file_system);
                        if (r < 0)
                                return r;

                        break;

                case ARG_MACHINE_ID:
                        arg_machine_id = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (arg_file_system && arg_machine_id)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "--file-system= and --machine-id may not be combined.");

        r = varlink_invocation(VARLINK_ALLOW_ACCEPT);
        if (r < 0)
                return log_error_errno(r, "Failed to check if invoked in Varlink mode: %m");
        if (r > 0)
                arg_varlink = true;
        else if (arg_pcr_index == UINT_MAX)
                arg_pcr_index = (arg_file_system || arg_machine_id) ?
                        TPM2_PCR_SYSTEM_IDENTITY : /* → PCR 15 */
                        TPM2_PCR_KERNEL_BOOT; /* → PCR 11 */

        return 1;
}

static int determine_banks(Tpm2Context *c, unsigned target_pcr_nr) {
        _cleanup_strv_free_ char **l = NULL;
        int r;

        assert(c);

        if (!strv_isempty(arg_banks)) /* Explicitly configured? Then use that */
                return 0;

        r = tpm2_get_good_pcr_banks_strv(c, UINT32_C(1) << target_pcr_nr, &l);
        if (r < 0)
                return log_error_errno(r, "Could not verify pcr banks: %m");

        strv_free_and_replace(arg_banks, l);
        return 0;
}

static int get_file_system_word(
                sd_device *d,
                const char *prefix,
                char **ret) {

        int r;

        assert(d);
        assert(prefix);
        assert(ret);

        _cleanup_close_ int block_fd = sd_device_open(d, O_RDONLY|O_CLOEXEC|O_NONBLOCK);
        if (block_fd < 0)
                return block_fd;

        _cleanup_(blkid_free_probep) blkid_probe b = blkid_new_probe();
        if (!b)
                return -ENOMEM;

        errno = 0;
        r = blkid_probe_set_device(b, block_fd, 0, 0);
        if (r != 0)
                return errno_or_else(ENOMEM);

        (void) blkid_probe_enable_superblocks(b, 1);
        (void) blkid_probe_set_superblocks_flags(b, BLKID_SUBLKS_TYPE|BLKID_SUBLKS_UUID|BLKID_SUBLKS_LABEL);
        (void) blkid_probe_enable_partitions(b, 1);
        (void) blkid_probe_set_partitions_flags(b, BLKID_PARTS_ENTRY_DETAILS);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == _BLKID_SAFEPROBE_ERROR)
                return errno_or_else(EIO);
        if (IN_SET(r, _BLKID_SAFEPROBE_AMBIGUOUS, _BLKID_SAFEPROBE_NOT_FOUND))
                return -ENOPKG;

        assert(r == _BLKID_SAFEPROBE_FOUND);

        _cleanup_strv_free_ char **l = strv_new(prefix);
        if (!l)
                return log_oom();

        FOREACH_STRING(field, "TYPE", "UUID", "LABEL", "PART_ENTRY_UUID", "PART_ENTRY_TYPE", "PART_ENTRY_NAME") {
                const char *v = NULL;

                (void) blkid_probe_lookup_value(b, field, &v, NULL);

                _cleanup_free_ char *escaped = xescape(strempty(v), ":"); /* Avoid ambiguity around ":" */
                if (!escaped)
                        return log_oom();

                r = strv_consume(&l, TAKE_PTR(escaped));
                if (r < 0)
                        return log_oom();

        }

        assert(strv_length(l) == 7); /* We always want 7 components, to avoid ambiguous strings */

        _cleanup_free_ char *word = strv_join(l, ":");
        if (!word)
                return log_oom();

        *ret = TAKE_PTR(word);
        return 0;
}

static int extend_now(unsigned pcr, const void *data, size_t size, Tpm2UserspaceEventType event) {
        _cleanup_(tpm2_context_unrefp) Tpm2Context *c = NULL;
        int r;

        r = tpm2_context_new(arg_tpm2_device, &c);
        if (r < 0)
                return r;

        r = determine_banks(c, pcr);
        if (r < 0)
                return r;
        if (strv_isempty(arg_banks)) /* Still none? */
                return log_error_errno(SYNTHETIC_ERRNO(ENOENT), "Found a TPM2 without enabled PCR banks. Can't operate.");

        _cleanup_free_ char *joined_banks = NULL;
        joined_banks = strv_join(arg_banks, ", ");
        if (!joined_banks)
                return log_oom();

        _cleanup_free_ char *safe = NULL;
        if (size > EXTENSION_STRING_SAFE_LIMIT) {
                safe = cescape_length(data, EXTENSION_STRING_SAFE_LIMIT);
                if (!safe)
                        return log_oom();

                if (!strextend(&safe, "..."))
                        return log_oom();
        } else {
                safe = cescape_length(data, size);
                if (!safe)
                        return log_oom();
        }

        log_debug("Measuring '%s' into PCR index %u, banks %s.", safe, pcr, joined_banks);

        r = tpm2_extend_bytes(c, arg_banks, pcr, data, size, /* secret= */ NULL, /* secret_size= */ 0, event, safe);
        if (r < 0)
                return log_error_errno(r, "Could not extend PCR: %m");

        log_struct(LOG_INFO,
                   "MESSAGE_ID=" SD_MESSAGE_TPM_PCR_EXTEND_STR,
                   LOG_MESSAGE("Extended PCR index %u with '%s' (banks %s).", pcr, safe, joined_banks),
                   "MEASURING=%s", safe,
                   "PCR=%u", pcr,
                   "BANKS=%s", joined_banks);

        return 0;
}

typedef struct MethodExtendParameters {
        unsigned pcr;
        const char *text;
        void *data;
        size_t data_size;
} MethodExtendParameters;

static int json_dispatch_binary_data(const char *name, JsonVariant *variant, JsonDispatchFlags flags, void *userdata) {
        MethodExtendParameters *p = ASSERT_PTR(userdata);
        _cleanup_free_ void *d = NULL;
        size_t l;
        int r;

        r = json_variant_unbase64(variant, &d, &l);
        if (r < 0)
                return json_log(variant, flags, r, "JSON variant is not a base64 string.");

        free_and_replace(p->data, d);
        p->data_size = l;

        return 0;
}

static int vl_method_extend(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {

        static const JsonDispatch dispatch_table[] = {
                { "pcr",  JSON_VARIANT_UNSIGNED, json_dispatch_uint,         offsetof(MethodExtendParameters, pcr),  JSON_MANDATORY },
                { "text", JSON_VARIANT_STRING,   json_dispatch_const_string, offsetof(MethodExtendParameters, text), 0              },
                { "data", JSON_VARIANT_STRING,   json_dispatch_binary_data,  0,                                      0              },
                {}
        };
        MethodExtendParameters p = {
                .pcr = UINT_MAX,
        };
        int r;

        assert(link);

        r = json_dispatch(parameters, dispatch_table, NULL, 0, &p);
        if (r < 0)
                return r;

        if (!TPM2_PCR_INDEX_VALID(p.pcr))
                return varlink_errorb(link, VARLINK_ERROR_INVALID_PARAMETER, JSON_BUILD_OBJECT(JSON_BUILD_PAIR_STRING("parameter", "pcr")));

        if (p.text) {
                /* Specifying both the text string and the binary data is not allowed */
                if (p.data)
                        return varlink_errorb(link, VARLINK_ERROR_INVALID_PARAMETER, JSON_BUILD_OBJECT(JSON_BUILD_PAIR_STRING("parameter", "data")));

                r = extend_now(p.pcr, p.text, strlen(p.text), _TPM2_USERSPACE_EVENT_TYPE_INVALID);
        } else if (p.data)
                r = extend_now(p.pcr, p.data, p.data_size, _TPM2_USERSPACE_EVENT_TYPE_INVALID);
        else
                return varlink_errorb(link, VARLINK_ERROR_INVALID_PARAMETER, JSON_BUILD_OBJECT(JSON_BUILD_PAIR_STRING("parameter", "text")));
        if (r < 0)
                return r;

        return varlink_reply(link, NULL);
}

static int run(int argc, char *argv[]) {
        _cleanup_free_ char *word = NULL;
        Tpm2UserspaceEventType event;
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        if (arg_varlink) {
                _cleanup_(varlink_server_unrefp) VarlinkServer *varlink_server = NULL;

                /* Invocation as Varlink service */

                r = varlink_server_new(&varlink_server, VARLINK_SERVER_ROOT_ONLY);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate Varlink server: %m");

                r = varlink_server_add_interface(varlink_server, &vl_interface_io_systemd_PCRExtend);
                if (r < 0)
                        return log_error_errno(r, "Failed to add Varlink interface: %m");

                r = varlink_server_bind_method(varlink_server, "io.systemd.PCRExtend.Extend", vl_method_extend);
                if (r < 0)
                        return log_error_errno(r, "Failed to bind Varlink method: %m");

                r = varlink_server_loop_auto(varlink_server);
                if (r < 0)
                        return log_error_errno(r, "Failed to run Varlink event loop: %m");

                return EXIT_SUCCESS;
        }

        if (arg_file_system) {
                _cleanup_free_ char *normalized = NULL, *normalized_escaped = NULL;
                _cleanup_(sd_device_unrefp) sd_device *d = NULL;
                _cleanup_close_ int dfd = -EBADF;

                if (optind != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expected no argument.");

                dfd = chase_and_open(arg_file_system, NULL, 0, O_DIRECTORY|O_CLOEXEC, &normalized);
                if (dfd < 0)
                        return log_error_errno(dfd, "Failed to open path '%s': %m", arg_file_system);

                r = fd_is_mount_point(dfd, NULL, 0);
                if (r < 0)
                        return log_error_errno(r, "Failed to determine if path '%s' is mount point: %m", normalized);
                if (r == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTDIR), "Specified path '%s' is not a mount point, refusing: %m", normalized);

                normalized_escaped = xescape(normalized, ":"); /* Avoid ambiguity around ":" */
                if (!normalized_escaped)
                        return log_oom();

                _cleanup_free_ char* prefix = strjoin("file-system:", normalized_escaped);
                if (!prefix)
                        return log_oom();

                r = block_device_new_from_fd(dfd, BLOCK_DEVICE_LOOKUP_BACKING, &d);
                if (r < 0) {
                        log_notice_errno(r, "Unable to determine backing block device of '%s', measuring generic fallback file system identity string: %m", arg_file_system);

                        word = strjoin(prefix, "::::::");
                        if (!word)
                                return log_oom();
                } else {
                        r = get_file_system_word(d, prefix, &word);
                        if (r < 0)
                                return log_error_errno(r, "Failed to get file system identifier string for '%s': %m", arg_file_system);
                }

                event = TPM2_EVENT_FILESYSTEM;

        } else if (arg_machine_id) {
                sd_id128_t mid;

                if (optind != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expected no argument.");

                r = sd_id128_get_machine(&mid);
                if (r < 0)
                        return log_error_errno(r, "Failed to acquire machine ID: %m");

                word = strjoin("machine-id:", SD_ID128_TO_STRING(mid));
                if (!word)
                        return log_oom();

                event = TPM2_EVENT_MACHINE_ID;

        } else {
                if (optind+1 != argc)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Expected a single argument.");

                word = strdup(argv[optind]);
                if (!word)
                        return log_oom();

                /* Refuse to measure an empty word. We want to be able to write the series of measured words
                 * separated by colons, where multiple separating colons are collapsed. Thus it makes sense to
                 * disallow an empty word to avoid ambiguities. */
                if (isempty(word))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "String to measure cannot be empty, refusing.");

                event = TPM2_EVENT_PHASE;
        }

        if (arg_graceful && tpm2_support() != TPM2_SUPPORT_FULL) {
                log_notice("No complete TPM2 support detected, exiting gracefully.");
                return EXIT_SUCCESS;
        }

        /* Skip logic if sd-stub is not used, after all PCR 11 might have a very different purpose then. */
        r = efi_measured_uki(LOG_ERR);
        if (r < 0)
                return r;
        if (r == 0) {
                log_info("Kernel stub did not measure kernel image into PCR %i, skipping userspace measurement, too.", TPM2_PCR_KERNEL_BOOT);
                return EXIT_SUCCESS;
        }

        r = extend_now(arg_pcr_index, word, strlen(word), event);
        if (r < 0)
                return log_error_errno(r, "Failed to create TPM2 context: %m");

        return EXIT_SUCCESS;
}

DEFINE_MAIN_FUNCTION(run);
